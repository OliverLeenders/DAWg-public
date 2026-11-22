#include "dawg/Commands.h"

#include "dawg/ClipOps.h"
#include "dawg/DrumSynth.h"
#include "dawg/DrumVoices.h"
#include "dawg/FaustNode.h"
#include "dawg/GraphCompiler.h"
#include "dawg/MidiEffects.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ParamLinks.h"
#include "dawg/PluginNode.h"
#include "dawg/ProjectFile.h"
#include "dawg/Sampler.h"
#include "dawg/Scales.h"
#include "dawg/StepSequencer.h"
#include "dawg/VarUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace dawg
{

CommandError::CommandError (const juce::String& message)
    : std::runtime_error (message.toStdString())
{
}

namespace
{
// ---------------------------------------------------------------------------------------------------------
// Argument parsing

[[nodiscard]] bool isNumber (const juce::var& value) noexcept
{
    return value.isDouble() || value.isInt() || value.isInt64();
}

[[nodiscard]] double requireNumber (const juce::var& args, const char* key)
{
    const auto& value = args[key];

    if (! isNumber (value))
        throw CommandError (juce::String ("Missing or non-numeric argument '") + key + "'");

    const auto number = static_cast<double> (value);

    if (! std::isfinite (number))
        throw CommandError (juce::String ("Argument '") + key + "' must be finite");

    return number;
}

[[nodiscard]] double numberOr (const juce::var& args, const char* key, double fallback)
{
    return args.hasProperty (key) ? requireNumber (args, key) : fallback;
}

[[nodiscard]] ObjectId requireId (const juce::var& args, const char* key)
{
    return static_cast<ObjectId> (requireNumber (args, key));
}

[[nodiscard]] Ticks requireTicks (const juce::var& args, const char* key, Ticks minValue, Ticks maxValue)
{
    const auto value = std::llround (requireNumber (args, key));

    if (value < minValue || value > maxValue)
        throw CommandError (juce::String ("Argument '") + key + "' is out of range");

    return value;
}

[[nodiscard]] Ticks requireTicksOr (const juce::var& args, const char* key, Ticks fallback)
{
    return args.hasProperty (key) ? requireTicks (args, key, 0, limits::maxPosition) : fallback;
}

[[nodiscard]] int requireInt (const juce::var& args, const char* key)
{
    return static_cast<int> (std::lround (requireNumber (args, key)));
}

// The timeline ticks from start that last the given seconds (docs/design/tempo-map.md, TM8).
[[nodiscard]] Ticks timelineLength (const TempoMap& map, Ticks start, double seconds)
{
    const auto from = static_cast<double> (start);
    return std::max (Ticks { 1 },
                     static_cast<Ticks> (std::llround (map.tickAt (map.secondsAt (from) + seconds) - from)));
}

// The seconds an audio clip's content ticks last: they run at the tempo where the clip starts (TM8).
[[nodiscard]] double contentSeconds (const TempoMap& map, Ticks start, Ticks content)
{
    return static_cast<double> (content) * 60.0 / (map.bpmAt (static_cast<double> (start)) * ticksPerBeat);
}

[[nodiscard]] const juce::Array<juce::var>& requireArray (const juce::var& args, const char* key)
{
    const auto* array = args[key].getArray();

    if (array == nullptr)
        throw CommandError (juce::String ("Argument '") + key + "' must be an array");

    return *array;
}

[[nodiscard]] Curve parseCurve (const juce::var& value, Lane lane)
{
    const auto& info = laneInfos()[static_cast<size_t> (lane)];
    return curveFromVar (value, info.minValue, info.maxValue);
}

// An optional field of a curve point: `fallback` when the point is shorter, else a number clamped to low..high.
[[nodiscard]] double optionalCurveField (const juce::Array<juce::var>& fields, int index, double fallback, double low,
                                         double high, const char* error)
{
    if (fields.size() <= index)
        return fallback;

    if (! isNumber (fields[index]))
        throw CommandError (error);

    return std::clamp (static_cast<double> (fields[index]), low, high);
}

// One point of a curve: [time, value, shape?, tension?, handleTime?, handleValue?].
[[nodiscard]] CurvePoint curvePointFromVar (const juce::var& pointVar, double minValue, double maxValue)
{
    const auto* fields = pointVar.getArray();

    if (fields == nullptr || fields->size() < 2 || ! isNumber ((*fields)[0]) || ! isNumber ((*fields)[1]))
        throw CommandError ("A curve point must be [time, value, shape?, tension?, handleTime?, handleValue?]");

    CurvePoint point;
    point.time = std::llround (static_cast<double> ((*fields)[0]));
    point.value = std::clamp (static_cast<double> ((*fields)[1]), minValue, maxValue);

    if (fields->size() > 2)
    {
        if (! isNumber ((*fields)[2]))
            throw CommandError ("A curve shape must be a number");

        const auto shape = static_cast<int> ((*fields)[2]);

        if (shape < 0 || shape > static_cast<int> (CurveShape::rounded))
            throw CommandError ("Unknown curve shape");

        point.shape = static_cast<CurveShape> (shape);
    }

    point.tension = optionalCurveField (*fields, 3, 0.0, -maxTension, maxTension, "A curve tension must be a number");
    point.handleTime =
        optionalCurveField (*fields, 4, point.handleTime, 0.0, 1.0, "A free handle time must be a number");
    point.handleValue =
        optionalCurveField (*fields, 5, point.handleValue, 0.0, 1.0, "A free handle value must be a number");

    if (point.time < 0 || point.time > limits::maxPosition || ! std::isfinite (point.value))
        throw CommandError ("A curve point is out of range");

    return point;
}
} // namespace

Curve curveFromVar (const juce::var& value, double minValue, double maxValue)
{
    const auto* array = value.getArray();

    if (array == nullptr)
        throw CommandError ("A curve must be an array of points");

    if (static_cast<size_t> (array->size()) > limits::maxCurvePoints)
        throw CommandError ("A curve has too many points");

    Curve curve;

    for (const auto& pointVar : *array)
        curve.push_back (curvePointFromVar (pointVar, minValue, maxValue));

    std::ranges::sort (curve, {}, &CurvePoint::time);

    if (std::ranges::adjacent_find (curve, {}, &CurvePoint::time) != curve.end())
        throw CommandError ("Curve points must have distinct times");

    return curve;
}

namespace
{
constexpr double maxShapeSemitones = 48.0;

// A note's frames from { leadStart, start, end }, checked against the file's length.
void readNoteFrames (const juce::var& value, juce::int64 fileFrames, PitchNote& note)
{
    note.start = std::llround (requireNumber (value, "start"));
    note.end = std::llround (requireNumber (value, "end"));
    note.leadStart = value.hasProperty ("leadStart") ? std::llround (requireNumber (value, "leadStart")) : note.start;

    if (note.leadStart < 0 || note.leadStart > note.start || note.start >= note.end
        || (fileFrames > 0 && note.end > fileFrames))
        throw CommandError ("A pitch note's frames are out of order or outside the file");
}

// Makes a pitch shape what the editor expects (section 4.4): rounded throughout, since a step or a corner in sung pitch
// is never wanted and a segment kept from elsewhere (a detected shape's last point) would otherwise bring one in; and
// with its first and last nodes on the note's edges, so the curve spans the note and never holds flat inside it.
void normalisePitchShape (Curve& shape, juce::int64 length)
{
    if (shape.empty())
        return;

    if (shape.size() == 1)
        shape.push_back (shape.front());

    // The end nodes move onto the edges; inner nodes stay strictly inside.
    Curve pinned { shape.front() };
    pinned.front().time = 0;

    for (size_t i = 1; i + 1 < shape.size(); ++i)
        if (shape[i].time > 0 && shape[i].time < length)
            pinned.push_back (shape[i]);

    pinned.push_back (shape.back());
    pinned.back().time = length;
    shape = std::move (pinned);

    for (auto& point : shape)
    {
        point.shape = CurveShape::rounded;
        point.tension = 0.0;
    }
}

[[nodiscard]] Curve noteShapeFromVar (const juce::var& value, const PitchNote& note)
{
    if (value.isVoid())
        return {};

    auto shape = curveFromVar (value, -maxShapeSemitones, maxShapeSemitones);

    if (! shape.empty() && (shape.front().time < 0 || shape.back().time > note.end - note.start))
        throw CommandError ("A pitch note's shape reaches outside the note");

    normalisePitchShape (shape, note.end - note.start);
    return shape;
}

void checkNoteOrder (const std::vector<PitchNote>& notes)
{
    for (size_t i = 1; i < notes.size(); ++i)
        if (notes[i].leadStart < notes[i - 1].end)
            throw CommandError ("Pitch notes must be in order and not overlap");
}
} // namespace

FilePitch filePitchFromVar (const juce::var& value, juce::int64 fileFrames)
{
    FilePitch pitch;

    if (! value.isObject())
        return pitch;

    pitch.analyser = value["analyser"].toString();

    if (const auto* notes = value["notes"].getArray())
    {
        for (const auto& n : *notes)
        {
            PitchNote note;
            note.id = std::llround (requireNumber (n, "id"));
            readNoteFrames (n, fileFrames, note);
            note.detectedCentre = std::clamp (requireNumber (n, "detectedCentre"), 0.0, 127.0);
            note.targetCentre = std::clamp (requireNumber (n, "targetCentre"), 0.0, 127.0);
            note.detectedShape = noteShapeFromVar (n["detectedShape"], note);
            note.shape = noteShapeFromVar (n["shape"], note);

            if (const auto* pivots = n["pivots"].getArray())
                for (const auto& pivot : *pivots)
                    note.pivots.push_back (std::llround (static_cast<double> (pivot)));

            note.vibrato = std::clamp (numberOr (n, "vibrato", 1.0), 0.0, 2.0);
            note.transitionMs = std::clamp (numberOr (n, "transitionMs", 20.0), 0.0, 500.0);
            pitch.notes.push_back (std::move (note));
        }
    }

    std::ranges::sort (pitch.notes, {}, &PitchNote::start);
    checkNoteOrder (pitch.notes);
    return pitch;
}

namespace
{

void parseExpression (const juce::var& value, Expression& expression)
{
    const auto* object = value.getDynamicObject();

    if (object == nullptr)
        throw CommandError ("'expression' must be an object of lanes");

    for (const auto& property : object->getProperties())
    {
        Lane lane {};

        if (! laneFromId (property.name.toString(), lane))
            throw CommandError ("Unknown expression lane '" + property.name.toString() + "'");

        expression[static_cast<size_t> (lane)] = parseCurve (property.value, lane);
    }
}

// Applies the vibrato fields present in `value` (an object) to `vibrato`; absent fields keep their values.
void applyVibratoFields (const juce::var& value, Vibrato& vibrato)
{
    if (value.getDynamicObject() == nullptr)
        throw CommandError ("'vibrato' must be an object");

    vibrato.depth =
        std::clamp (numberOr (value, "depth", vibrato.depth), limits::minVibratoDepth, limits::maxVibratoDepth);
    vibrato.rateHz =
        std::clamp (numberOr (value, "rateHz", vibrato.rateHz), limits::minVibratoRate, limits::maxVibratoRate);
    vibrato.syncBeats = std::clamp (numberOr (value, "syncBeats", vibrato.syncBeats), 0.0, limits::maxVibratoSyncBeats);

    if (value.hasProperty ("shape"))
    {
        const auto shape = requireInt (value, "shape");

        if (shape < 0 || shape >= numVibratoShapes)
            throw CommandError ("Unknown vibrato shape");

        vibrato.shape = static_cast<VibratoShape> (shape);
    }

    if (value.hasProperty ("delay"))
        vibrato.delay = requireTicks (value, "delay", 0, limits::maxPosition);

    if (value.hasProperty ("fadeIn"))
        vibrato.fadeIn = requireTicks (value, "fadeIn", 0, limits::maxPosition);
}

// MPE lanes always start with a point; the note end has one only if the user adds it
// (docs/design/phase3-piano-roll.md, section 6).
constexpr std::array mpeLanes { Lane::pitch, Lane::pressure, Lane::timbre };

void anchorExpression (Note& note)
{
    for (const auto lane : mpeLanes)
        anchorCurve (note.expression[static_cast<size_t> (lane)], laneInfos()[static_cast<size_t> (lane)].defaultValue);
}

// Applies the note fields present in `fields` to `note`; with `requireAll`, start/length/pitch must be given.

void applyNoteFields (const juce::var& fields, Note& note, bool requireAll)
{
    const auto oldLength = note.length;

    if (requireAll || fields.hasProperty ("start"))
        note.start = requireTicks (fields, "start", 0, limits::maxPosition);

    if (requireAll || fields.hasProperty ("length"))
        note.length = requireTicks (fields, "length", 1, limits::maxPosition);

    if (requireAll || fields.hasProperty ("pitch"))
        note.pitch = std::clamp (requireNumber (fields, "pitch"), limits::minPitch, limits::maxPitch);

    note.velocity = std::clamp (numberOr (fields, "velocity", note.velocity), 0.0, 1.0);
    note.releaseVelocity = std::clamp (numberOr (fields, "releaseVelocity", note.releaseVelocity), 0.0, 1.0);

    if (fields.hasProperty ("expression"))
        parseExpression (fields["expression"], note.expression);

    if (fields.hasProperty ("vibrato"))
        applyVibratoFields (fields["vibrato"], note.vibrato);

    // Resizing moves the end anchor along with the note's end (unless new curves came with the change).
    if (! requireAll && note.length != oldLength && ! fields.hasProperty ("expression"))
    {
        for (auto& curve : note.expression)
        {
            const auto end = std::ranges::find (curve, oldLength, &CurvePoint::time);

            if (end != curve.end()
                && std::ranges::none_of (curve, [&] (const CurvePoint& p) { return p.time == note.length; }))
            {
                end->time = note.length;
                std::ranges::sort (curve, {}, &CurvePoint::time);
            }
        }
    }

    anchorExpression (note);
}

// ---------------------------------------------------------------------------------------------------------
// Copy-on-write helpers

template <typename UpdateFn>
[[nodiscard]] Project updateTrack (const Project& project, ObjectId trackId, UpdateFn&& update)
{
    const auto index = project.indexOfTrack (trackId);

    if (index < 0)
        throw CommandError ("No track with id " + juce::String (trackId));

    auto result = project;
    auto track = *project.tracks[static_cast<size_t> (index)];
    std::forward<UpdateFn> (update) (track, result);
    result.tracks[static_cast<size_t> (index)] = std::make_shared<const Track> (std::move (track));
    return result;
}

// Like updateTrack, but also accepts the master track's id (mixing and chain commands).
template <typename UpdateFn>
[[nodiscard]] Project updateTrackOrMaster (const Project& project, ObjectId trackId, UpdateFn&& update)
{
    if (project.master == nullptr || project.master->id != trackId)
        return updateTrack (project, trackId, std::forward<UpdateFn> (update));

    auto result = project;
    auto master = *project.master;
    std::forward<UpdateFn> (update) (master, result);
    result.master = std::make_shared<const Track> (std::move (master));
    return result;
}

struct ClipLocation
{
    size_t track;
    size_t clip;
};

[[nodiscard]] ClipLocation requireClip (const Project& project, ObjectId clipId)
{
    for (size_t t = 0; t < project.tracks.size(); ++t)
        if (const auto c = project.tracks[t]->indexOfClip (clipId); c >= 0)
            return { t, static_cast<size_t> (c) };

    throw CommandError ("No clip with id " + juce::String (clipId));
}

void sortClips (std::vector<ClipPtr>& clips)
{
    std::ranges::sort (clips, [] (const ClipPtr& a, const ClipPtr& b)
                       { return a->start != b->start ? a->start < b->start : a->id < b->id; });
}

void sortNotes (std::vector<NotePtr>& notes)
{
    std::ranges::sort (notes, [] (const NotePtr& a, const NotePtr& b)
                       { return a->start != b->start ? a->start < b->start : a->id < b->id; });
}

template <typename UpdateFn>
[[nodiscard]] Project updateClip (const Project& project, ObjectId clipId, UpdateFn&& update)
{
    const auto location = requireClip (project, clipId);
    return updateTrack (project, project.tracks[location.track]->id,
                        [&] (Track& track, Project& result)
                        {
                            auto clip = *track.clips[location.clip];
                            std::forward<UpdateFn> (update) (clip, result);
                            track.clips[location.clip] = std::make_shared<const NoteClip> (std::move (clip));
                            sortClips (track.clips);
                        });
}

[[nodiscard]] const NodeDescriptor& requireDescriptor (const juce::String& type)
{
    const auto* descriptor = NodeRegistry::builtin().find (type);

    if (descriptor == nullptr)
        throw CommandError ("Unknown node type '" + type + "'");

    return *descriptor;
}

// A node's description: a Faust node's comes from its compiled script (docs/design/phase11-faust-panels.md, F3)
// and is kept alive by the returned pointer; nullptr for a Faust node whose script hasn't compiled.
[[nodiscard]] std::shared_ptr<const NodeDescriptor> descriptorOf (const GraphNode& node)
{
    if (node.type == nodetype::faust)
    {
        const auto program = compiledFaustProgram (node.code);
        return program != nullptr ? std::shared_ptr<const NodeDescriptor> (program, &program->descriptor()) : nullptr;
    }

    return { std::shared_ptr<const void>(), &requireDescriptor (node.type) }; // static storage, not owned
}

// What a node's ports carry, by port. Everything that checks a cable or a slot asks here, as a node's ports aren't always its type's
// (docs/design/component-nodes.md, N2).
struct NodePorts
{
    std::vector<PortType> inputs;
    std::vector<PortType> outputs;

    [[nodiscard]] bool isInput (int port, PortType type) const noexcept
    {
        return port >= 0 && static_cast<size_t> (port) < inputs.size() && inputs[static_cast<size_t> (port)] == type;
    }
};

// A compiled Faust node has its script's ports; an uncompiled one has one stereo input and output. A component has the ports it was given,
// and its Input and Output have them the other way round, which takes the component they're in.
[[nodiscard]] NodePorts portsOf (const GraphNode& node, const ComponentGraph* owner = nullptr)
{
    if (node.component != nullptr || node.type == nodetype::componentInput || node.type == nodetype::componentOutput)
    {
        const auto typesOf = [] (const std::vector<ComponentPort>& ports)
        {
            std::vector<PortType> types;
            std::ranges::transform (ports, std::back_inserter (types), &ComponentPort::type);
            return types;
        };

        if (node.component != nullptr)
            return { typesOf (node.component->inputs), typesOf (node.component->outputs) };

        if (owner == nullptr)
            return {};

        return node.type == nodetype::componentInput ? NodePorts { {}, typesOf (owner->inputs) }
                                                     : NodePorts { typesOf (owner->outputs), {} };
    }

    const auto compiled = descriptorOf (node);
    const auto& descriptor = compiled != nullptr ? *compiled : faustDescriptor();
    NodePorts ports;
    std::ranges::transform (descriptor.inputs, std::back_inserter (ports.inputs), &PortInfo::type);
    std::ranges::transform (descriptor.outputs, std::back_inserter (ports.outputs), &PortInfo::type);
    return ports;
}

void requireModulationPort (const GraphNode& node, int port)
{
    if (! portsOf (node).isInput (port, PortType::modulation))
        throw CommandError ("Not a modulation input");
}

// Looks up a node's parameter. A Faust node whose script hasn't compiled accepts any parameter unclamped: its
// values are applied once the script compiles (unknown paths are ignored then).
[[nodiscard]] std::optional<ParamInfo> paramOf (const GraphNode& node, const juce::String& paramId)
{
    const auto descriptor = descriptorOf (node);

    if (descriptor == nullptr)
    {
        ParamInfo open;
        open.minValue = std::numeric_limits<float>::lowest();
        open.maxValue = std::numeric_limits<float>::max();
        return open;
    }

    const auto index = descriptor->indexOfParam (paramId);

    if (index < 0)
        return std::nullopt;

    return descriptor->params[static_cast<size_t> (index)];
}

// Whether a slot can drive a parameter from a port; throws if not. A plugin's parameters aren't known here.
void checkModulationTarget (const GraphNode& node, int port, const juce::String& paramId)
{
    requireModulationPort (node, port);

    if (nodetype::isPlugin (node.type))
        return;

    const auto info = paramOf (node, paramId);

    if (! info.has_value())
        throw CommandError ("Unknown parameter '" + paramId + "'");

    if (! info->acceptsModulation())
        throw CommandError ("'" + paramId + "' can't be modulated");
}

[[nodiscard]] size_t requireNodeIndex (const TrackGraph& graph, ObjectId nodeId)
{
    const auto index = graph.indexOfNode (nodeId);

    if (index < 0)
        throw CommandError ("No node with id " + juce::String (nodeId));

    return static_cast<size_t> (index);
}

[[nodiscard]] GraphNode& requireNode (TrackGraph& graph, ObjectId nodeId)
{
    return graph.nodes[requireNodeIndex (graph, nodeId)];
}

[[nodiscard]] const GraphNode& requireNode (const TrackGraph& graph, ObjectId nodeId)
{
    return graph.nodes[requireNodeIndex (graph, nodeId)];
}

[[nodiscard]] Connection parseConnection (const juce::var& args)
{
    return { requireId (args, "fromNode"), requireInt (args, "fromPort"), requireId (args, "toNode"),
             requireInt (args, "toPort") };
}

// True if adding from -> to would close a cycle with no Feedback node in it, which is the only kind a loop can close through
// (docs/design/feedback-routing.md, R1).
[[nodiscard]] bool createsCycle (const TrackGraph& graph, ObjectId from, ObjectId to)
{
    const auto isFeedback = [&] (ObjectId id)
    {
        return std::ranges::any_of (graph.nodes,
                                    [&] (const GraphNode& n) { return n.id == id && n.type == nodetype::feedback; });
    };

    if (isFeedback (from) || isFeedback (to))
        return false;

    std::vector<ObjectId> stack { to };
    std::set<ObjectId> seen;

    while (! stack.empty())
    {
        const auto current = stack.back();
        stack.pop_back();

        if (current == from)
            return true;

        if (! seen.insert (current).second || isFeedback (current))
            continue;

        for (const auto& c : graph.connections)
            if (c.fromNode == current)
                stack.push_back (c.toNode);
    }

    return false;
}

[[nodiscard]] GraphNode makeNode (Project& project, const char* type, float x, float y)
{
    GraphNode node;
    node.id = project.nextId++;
    node.type = type;
    node.x = x;
    node.y = y;

    // A new Drum Synth plays the default kit (docs/design/drum-synth.md, DR1).
    if (node.type == nodetype::drumSynth)
        node.drumPads = defaultDrumPads();

    return node;
}

// A new Voice Instrument's voice graph (docs/design/phase12-voice-scope.md, V1), a playable synth: Note Expression's
// pitch drives the oscillator, and an ADSR opens the gain from 0 % (docs/design/modulation.md, M2).
[[nodiscard]] std::shared_ptr<const TrackGraph> makeDefaultVoiceGraph (Project& project)
{
    constexpr float column = 220.0f;
    constexpr float row = 120.0f;
    TrackGraph graph;
    const auto add = [&] (const char* type, float x, float y) -> GraphNode&
    { return graph.nodes.emplace_back (makeNode (project, type, x, y)); };

    const auto input = add (nodetype::voiceInput, 0.0f, 0.0f).id;
    const auto expression = add (nodetype::noteExpression, column, -row).id;
    auto& oscillator = add (nodetype::oscillator, 2.0f * column, -row);
    oscillator.modulations[0] = { "pitch", 1.0, true };
    const auto oscillatorId = oscillator.id;
    const auto adsr = add (nodetype::adsr, column, row).id;
    auto& gain = add (nodetype::gain, 3.0f * column, 0.0f);
    gain.params["amplitude"] = 0.0;
    gain.modulations[1] = { "amplitude", 0.5, true }; // +100 % of the 200 % range
    const auto gainId = gain.id;
    const auto output = add (nodetype::voiceOutput, 4.0f * column, 0.0f).id;

    graph.connections = { { input, 0, expression, 0 },    { input, 0, adsr, 0 },  { expression, 0, oscillatorId, 0 },
                          { oscillatorId, 0, gainId, 0 }, { adsr, 0, gainId, 1 }, { gainId, 0, output, 0 } };
    return std::make_shared<const TrackGraph> (std::move (graph));
}

// The voice graph of a Sampler converted to a Voice Instrument (docs/design/sampler-midi-delay-limiter.md, section 4): it
// keeps its zones and settings, Note Expression's pitch drives it, and it takes the notes itself to choose its zones.
[[nodiscard]] std::shared_ptr<const TrackGraph> makeSamplerVoiceGraph (Project& project, const GraphNode& source)
{
    constexpr float column = 220.0f;
    constexpr float row = 120.0f;
    TrackGraph graph;
    const auto add = [&] (const char* type, float x, float y) -> GraphNode&
    { return graph.nodes.emplace_back (makeNode (project, type, x, y)); };

    const auto input = add (nodetype::voiceInput, 0.0f, 0.0f).id;
    const auto expression = add (nodetype::noteExpression, column, -row).id;
    auto& sampler = add (nodetype::sampleVoice, 2.0f * column, 0.0f);
    sampler.params = source.params;
    sampler.sampleZones = source.sampleZones;
    sampler.modulations[1] = { "pitch", 1.0, true };
    const auto samplerId = sampler.id;
    const auto output = add (nodetype::voiceOutput, 3.0f * column, 0.0f).id;

    graph.connections = { { input, 0, expression, 0 },
                          { input, 0, samplerId, 0 },
                          { expression, 0, samplerId, 1 },
                          { samplerId, 0, output, 0 } };
    return std::make_shared<const TrackGraph> (std::move (graph));
}

// Gives every node of a graph a new id, those in voice graphs and components too; `ids` maps old to new.
void renumberGraph (TrackGraph& graph, Project& project, std::map<ObjectId, ObjectId>& ids)
{
    for (auto& node : graph.nodes)
    {
        ids[node.id] = project.nextId;
        node.id = project.nextId++;

        if (node.voiceGraph != nullptr)
        {
            auto voice = *node.voiceGraph;
            renumberGraph (voice, project, ids);
            node.voiceGraph = std::make_shared<const TrackGraph> (std::move (voice));
        }

        if (node.component != nullptr)
        {
            auto inside = *node.component;
            renumberGraph (inside.graph, project, ids);
            for (auto& param : inside.exposed)
                param.nodeId = ids.at (param.nodeId);

            node.component = std::make_shared<const ComponentGraph> (std::move (inside));
        }
    }

    for (auto& connection : graph.connections)
    {
        connection.fromNode = ids.at (connection.fromNode);
        connection.toNode = ids.at (connection.toNode);
    }
}

// A node's id and the ids of everything in it.
[[nodiscard]] std::set<ObjectId> idsWithin (const GraphNode& node)
{
    std::set<ObjectId> ids { node.id };
    const auto add = [&ids] (const GraphNode& inner, bool) { ids.insert (inner.id); };

    if (node.voiceGraph != nullptr)
        forEachNode (*node.voiceGraph, add);

    if (node.component != nullptr)
        forEachNode (node.component->graph, add);

    return ids;
}

[[nodiscard]] const juce::StringArray& trackColours()
{
    // The first six are the original track colours; the rest complete the 12-swatch palette (ui/src/colours.ts).
    static const juce::StringArray colours { "#7c8cff", "#ff7ca8", "#5fd4b0", "#ffb86b", "#c58bff", "#6bc8ff",
                                             "#ff6b6b", "#f5d76e", "#9be36b", "#4fd1c5", "#8f9bb3", "#e0a3ff" };
    return colours;
}

// ---------------------------------------------------------------------------------------------------------
// Project commands

// The tempo map (docs/design/tempo-map.md, TM5). Audio clips are fitted to a changed map in applyCommand().

[[nodiscard]] Project withTempo (const Project& project, std::vector<TempoPoint> points)
{
    auto result = project;
    result.tempoMap = TempoMap (std::move (points), project.tempoMap.signatures());
    return result;
}

[[nodiscard]] Project withSignatures (const Project& project, std::vector<TimeSignature> signatures)
{
    auto result = project;
    result.tempoMap = TempoMap (project.tempoMap.points(), std::move (signatures));
    return result;
}

[[nodiscard]] size_t requirePointIndex (const Project& project, const juce::var& args)
{
    const auto index = requireInt (args, "index");

    if (index < 0 || index >= static_cast<int> (project.tempoMap.points().size()))
        throw CommandError ("No tempo point " + juce::String (index));

    return static_cast<size_t> (index);
}

// Sets the tempo in force at a position (the last point at or before it; TM11), or the starting tempo.
[[nodiscard]] Project setTempo (const Project& project, const juce::var& args)
{
    const auto at = args.hasProperty ("at") ? requireTicks (args, "at", 0, limits::maxPosition) : Ticks { 0 };
    auto points = project.tempoMap.points();
    auto index = points.size() - 1;

    while (index > 0 && points[index].tick > at)
        --index;

    points[index].bpm = requireNumber (args, "bpm");
    return withTempo (project, std::move (points));
}

[[nodiscard]] Project addTempoPoint (const Project& project, const juce::var& args)
{
    auto points = project.tempoMap.points();
    const auto tick = requireTicks (args, "tick", 0, limits::maxPosition);
    std::erase_if (points, [tick] (const TempoPoint& p) { return p.tick == tick; });
    points.push_back ({ tick, requireNumber (args, "bpm"), static_cast<bool> (args["ramp"]) });
    return withTempo (project, std::move (points));
}

// Changes a point. It stays between its neighbours, and the first point stays at tick 0.
[[nodiscard]] Project setTempoPoint (const Project& project, const juce::var& args)
{
    auto points = project.tempoMap.points();
    const auto index = requirePointIndex (project, args);
    auto& point = points[index];

    if (args.hasProperty ("tick") && index > 0)
    {
        const auto last = index + 1 < points.size() ? points[index + 1].tick - 1 : limits::maxPosition;
        point.tick = std::clamp (requireTicks (args, "tick", 0, limits::maxPosition), points[index - 1].tick + 1, last);
    }

    if (args.hasProperty ("bpm"))
        point.bpm = requireNumber (args, "bpm");

    if (args.hasProperty ("ramp"))
        point.ramp = static_cast<bool> (args["ramp"]);

    return withTempo (project, std::move (points));
}

[[nodiscard]] Project removeTempoPoint (const Project& project, const juce::var& args)
{
    const auto index = requirePointIndex (project, args);

    if (index == 0)
        throw CommandError ("The first tempo point can't be removed");

    auto points = project.tempoMap.points();
    points.erase (points.begin() + static_cast<std::ptrdiff_t> (index));
    return withTempo (project, std::move (points));
}

[[nodiscard]] int requireBar (const juce::var& args, const char* key)
{
    const auto bar = requireInt (args, key);

    if (bar < 0 || bar > limits::maxBar)
        throw CommandError ("'" + juce::String (key) + "' is out of range");

    return bar;
}

// Adds a time signature on a bar, or replaces the one there.
[[nodiscard]] Project setSignature (const Project& project, const juce::var& args)
{
    auto signatures = project.tempoMap.signatures();
    const auto bar = requireBar (args, "bar");
    std::erase_if (signatures, [bar] (const TimeSignature& s) { return s.bar == bar; });
    signatures.push_back ({ bar, requireInt (args, "numerator"), requireInt (args, "denominator") });
    return withSignatures (project, std::move (signatures));
}

// Moves a time signature to another bar (not the one on bar 0), replacing one there.
[[nodiscard]] Project moveSignature (const Project& project, const juce::var& args)
{
    const auto from = requireBar (args, "from");
    const auto to = requireBar (args, "to");
    auto signatures = project.tempoMap.signatures();
    const auto it = std::ranges::find_if (signatures, [from] (const TimeSignature& s) { return s.bar == from; });

    if (from == 0 || to == 0 || it == signatures.end())
        throw CommandError ("No time signature to move on bar " + juce::String (from + 1));

    auto moved = *it;
    moved.bar = to;
    signatures.erase (it);
    std::erase_if (signatures, [to] (const TimeSignature& s) { return s.bar == to; });
    signatures.push_back (moved);
    return withSignatures (project, std::move (signatures));
}

[[nodiscard]] Project removeSignature (const Project& project, const juce::var& args)
{
    const auto bar = requireBar (args, "bar");

    if (bar == 0)
        throw CommandError ("The first time signature can't be removed");

    auto signatures = project.tempoMap.signatures();
    std::erase_if (signatures, [bar] (const TimeSignature& s) { return s.bar == bar; });
    return withSignatures (project, std::move (signatures));
}

[[nodiscard]] Project setMetronome (const Project& project, const juce::var& args)
{
    auto result = project;
    result.metronome = static_cast<bool> (args["enabled"]);
    return result;
}

// The length of automatic crossfades (docs/design/audio-features.md, AU7), 0 for none.
[[nodiscard]] Project setAutoCrossfade (const Project& project, const juce::var& args)
{
    auto result = project;
    result.autoCrossfadeMs = std::clamp (requireNumber (args, "ms"), 0.0, limits::maxCrossfadeMs);
    return result;
}

// The key on a bar (docs/design/scale-highlighting.md, SH1, SH9): edits the key change there, or adds one starting
// from the key in effect. Each argument left out keeps its value, so turning the key back on restores its root and
// scale. The bar defaults to 0, the first key.
[[nodiscard]] Project setKey (const Project& project, const juce::var& args)
{
    const auto bar = args.hasProperty ("bar") ? requireBar (args, "bar") : 0;
    auto result = project;
    auto it = std::ranges::find (result.keys, bar, &KeyChange::bar);

    if (it == result.keys.end())
    {
        const auto inEffect = project.keyAtBar (bar);
        it = result.keys.insert (std::ranges::upper_bound (result.keys, bar, {}, &KeyChange::bar),
                                 KeyChange { bar, inEffect });
    }

    auto& key = it->key;

    if (args.hasProperty ("enabled"))
        key.enabled = static_cast<bool> (args["enabled"]);

    if (args.hasProperty ("root"))
        key.root = std::clamp (static_cast<int> (std::lround (requireNumber (args, "root"))), 0, 11);

    if (args.hasProperty ("scale"))
        key.scale = std::clamp (static_cast<int> (std::lround (requireNumber (args, "scale"))), 0,
                                static_cast<int> (scales.size()) - 1);

    return result;
}

// Moves a key change to another bar (not the one on bar 0), replacing one there.
[[nodiscard]] Project moveKey (const Project& project, const juce::var& args)
{
    const auto from = requireBar (args, "from");
    const auto to = requireBar (args, "to");
    auto result = project;
    const auto it = std::ranges::find (result.keys, from, &KeyChange::bar);

    if (from == 0 || to == 0 || it == result.keys.end())
        throw CommandError ("No key change to move on bar " + juce::String (from + 1));

    auto moved = *it;
    moved.bar = to;
    result.keys.erase (it);
    std::erase_if (result.keys, [to] (const KeyChange& k) { return k.bar == to; });
    result.keys.insert (std::ranges::upper_bound (result.keys, to, {}, &KeyChange::bar), moved);
    return result;
}

[[nodiscard]] Project removeKey (const Project& project, const juce::var& args)
{
    const auto bar = requireBar (args, "bar");

    if (bar == 0)
        throw CommandError ("The first key can't be removed");

    auto result = project;
    std::erase_if (result.keys, [bar] (const KeyChange& k) { return k.bar == bar; });
    return result;
}

// ---------------------------------------------------------------------------------------------------------
// Chain view of a track graph (docs/design/phase4-plugin-hosting.md, P2)
//
// A track graph is a chain when it reads Sequencer -> [instrument] -> [effects...] -> [Gain] -> Track Output. The
// Gain is optional: a graph whose Gain was removed is still a chain. Commands that insert, reorder or remove
// instruments and effects rewrite the connections from this view.

struct Chain
{
    ObjectId sequencer = 0;
    std::vector<ObjectId> midiEffects; // between the sequencer and the instrument (docs/design/midi-effects.md, ME2)
    ObjectId instrument = 0;           // 0 when the track has none
    std::vector<ObjectId> effects;
    ObjectId gain = 0; // 0 when the track has none
    ObjectId output = 0;

    [[nodiscard]] bool contains (ObjectId id) const
    {
        return id == instrument || std::ranges::find (effects, id) != effects.end()
               || std::ranges::find (midiEffects, id) != midiEffects.end();
    }
};

// The connection into a node's input port 0, if it is the node's only incoming connection there. Other inputs
// (a key input or a modulation pool) don't affect the chain view.
[[nodiscard]] const Connection* soleInput (const TrackGraph& graph, ObjectId nodeId)
{
    const Connection* found = nullptr;

    for (const auto& c : graph.connections)
    {
        if (c.toNode != nodeId || c.toPort != 0)
            continue;

        if (found != nullptr || c.fromPort != 0)
            return nullptr;

        found = &c;
    }

    return found;
}

// Walks backwards from the instrument's note input through MIDI effects to the sequencer (docs/design/midi-effects.md,
// ME2). Returns false if something else feeds them.
[[nodiscard]] bool walkMidiEffects (const TrackGraph& graph, Chain& chain, const Connection* input)
{
    std::set<ObjectId> seen;

    for (; input != nullptr; input = soleInput (graph, input->fromNode))
    {
        if (input->fromNode == chain.sequencer)
            return true;

        const auto& node = requireNode (graph, input->fromNode);

        if (! nodetype::isMidiEffect (node.type) || ! seen.insert (node.id).second)
            return false;

        chain.midiEffects.insert (chain.midiEffects.begin(), node.id);
    }

    // Nothing feeds the first: fine only without MIDI effects, for an instrument on a track without a sequencer.
    return chain.midiEffects.empty();
}

// Without an instrument, the MIDI effects hang off the sequencer in a row, each feeding only the next.
void collectDanglingMidiEffects (const TrackGraph& graph, Chain& chain)
{
    for (auto from = chain.sequencer; from != 0;)
    {
        const Connection* next = nullptr;
        auto count = 0;

        for (const auto& c : graph.connections)
        {
            if (c.fromNode == from && c.fromPort == 0 && c.toPort == 0
                && nodetype::isMidiEffect (requireNode (graph, c.toNode).type))
            {
                next = &c;
                ++count;
            }
        }

        if (count != 1 || std::ranges::find (chain.midiEffects, next->toNode) != chain.midiEffects.end())
            return;

        chain.midiEffects.push_back (next->toNode);
        from = next->toNode;
    }
}

// Walks backwards from the gain (or from the output, without one) to the instrument (or to a source without input, or
// to a node without input), collecting the effects. Returns false if the graph isn't a chain there.
[[nodiscard]] bool walkBackToSource (const TrackGraph& graph, Chain& chain)
{
    std::set<ObjectId> seen;

    for (const auto* input = soleInput (graph, chain.gain != 0 ? chain.gain : chain.output); input != nullptr;)
    {
        const auto from = input->fromNode;

        if ((chain.sequencer != 0 && from == chain.sequencer) || ! seen.insert (from).second)
            return false;

        const auto& node = requireNode (graph, from);
        const auto* descriptor = NodeRegistry::builtin().find (node.type);

        if (descriptor == nullptr)
            return false;

        if (portsOf (node).inputs.empty())
        {
            // A source without inputs (the audio player) heads the chain.
            chain.instrument = from;
            return true;
        }

        // A component taking notes first counts as an instrument (docs/design/component-nodes.md, N3).
        if (isInstrument (*descriptor) || (node.component != nullptr && portsOf (node).isInput (0, PortType::events)))
        {
            chain.instrument = from;
            return walkMidiEffects (graph, chain, soleInput (graph, from));
        }

        chain.effects.insert (chain.effects.begin(), from);
        input = soleInput (graph, from);
    }

    return true;
}

[[nodiscard]] std::optional<Chain> findChain (const TrackGraph& graph)
{
    const auto byType = [&] (const char* type) -> const GraphNode*
    {
        const auto it = std::ranges::find_if (graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
        return it != graph.nodes.end() ? &*it : nullptr;
    };

    // Audio tracks have no sequencer: their chain starts with the audio player (phase 7, T1). The master
    // track's starts with the mix input.
    const auto* sequencer = byType (nodetype::sequencer);
    const auto* output = byType (nodetype::trackOutput);

    if (output == nullptr
        || (sequencer == nullptr && byType (nodetype::audioPlayer) == nullptr
            && byType (nodetype::mixInput) == nullptr))
        return std::nullopt;

    // Nothing feeding the output is an empty chain (once the gain is gone, removing the instrument leaves that).
    const auto* intoOutput = soleInput (graph, output->id);
    const auto outputFed = std::ranges::any_of (graph.connections, [&] (const Connection& c)
                                                { return c.toNode == output->id && c.toPort == 0; });

    if (intoOutput == nullptr && outputFed)
        return std::nullopt;

    Chain chain;
    chain.sequencer = sequencer != nullptr ? sequencer->id : 0;
    chain.output = output->id;

    if (intoOutput != nullptr && requireNode (graph, intoOutput->fromNode).type == nodetype::gain)
        chain.gain = intoOutput->fromNode;

    if (! walkBackToSource (graph, chain))
        return std::nullopt;

    if (chain.instrument == 0 && chain.sequencer != 0)
        collectDanglingMidiEffects (graph, chain);

    // Every audio node must be part of the chain; modulation sources are extra nodes outside it, and so are Output to
    // track nodes fed straight from the instrument (docs/design/multi-output-instruments.md, MO3). The sequencer may
    // only feed the instrument or a modulation source's note input.
    const size_t fixedNodes = 1u + (chain.sequencer != 0 ? 1u : 0u) + (chain.gain != 0 ? 1u : 0u);
    const auto extra = [&] (const GraphNode& n)
    {
        return nodetype::isModulationSource (n.type)
               || (n.type == nodetype::busOutput && chain.instrument != 0
                   && std::ranges::all_of (graph.connections, [&] (const Connection& c)
                                           { return c.toNode != n.id || c.fromNode == chain.instrument; }));
    };

    if (graph.nodes.size() - static_cast<size_t> (std::ranges::count_if (graph.nodes, extra))
        != fixedNodes + chain.midiEffects.size() + chain.effects.size() + (chain.instrument != 0 ? 1 : 0))
        return std::nullopt;

    const auto afterSequencer = chain.midiEffects.empty() ? chain.instrument : chain.midiEffects.front();

    for (const auto& c : graph.connections)
    {
        if (chain.sequencer != 0 && c.fromNode == chain.sequencer && c.toNode != afterSequencer)
        {
            const auto& target = requireNode (graph, c.toNode);

            if (! nodetype::isModulationSource (target.type))
                return std::nullopt;
        }
    }

    return chain;
}

[[nodiscard]] Chain requireChain (const TrackGraph& graph)
{
    auto chain = findChain (graph);

    if (! chain.has_value())
        throw CommandError ("This track's graph is not a simple chain");

    return *chain;
}

// A modulation slot only exists while something feeds its port; dropping the last connection drops the slot. A macro
// slot is fed by its knob, without a cable (docs/design/macros-and-modulation-column.md, MC7).
void pruneModulationSlots (TrackGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        for (auto it = node.modulations.begin(); it != node.modulations.end();)
        {
            const auto port = it->first;
            const auto connected =
                it->second.macro >= 0
                || std::ranges::any_of (graph.connections, [&] (const Connection& connection)
                                        { return connection.toNode == node.id && connection.toPort == port; });
            it = connected ? std::next (it) : node.modulations.erase (it);
        }
    }
}

void removeNodeAndConnections (TrackGraph& graph, ObjectId nodeId)
{
    std::erase_if (graph.nodes, [&] (const GraphNode& n) { return n.id == nodeId; });
    std::erase_if (graph.connections, [&] (const Connection& c) { return c.fromNode == nodeId || c.toNode == nodeId; });
    pruneModulationSlots (graph);
}

// Whether a connection is outside the simple chain and must survive a rewire: a modulation edge, a key input, or
// anything touching a modulation source (for example the sequencer feeding its note input).
[[nodiscard]] bool isExtraConnection (const TrackGraph& graph, const Connection& connection)
{
    if (connection.fromPort != 0 || connection.toPort != 0)
        return true;

    const auto& from = requireNode (graph, connection.fromNode);
    const auto& to = requireNode (graph, connection.toNode);

    if (nodetype::isModulationSource (from.type) || nodetype::isModulationSource (to.type))
        return true;

    return portsOf (to).isInput (connection.toPort, PortType::modulation);
}

// Rebuilds the connections of a chain and lays its nodes out in a row.
void rewire (TrackGraph& graph, const Chain& chain)
{
    std::vector<ObjectId> order;
    std::vector<Connection> extra;

    for (const auto& connection : graph.connections)
        if (isExtraConnection (graph, connection))
            extra.push_back (connection);

    if (chain.sequencer != 0)
        order.push_back (chain.sequencer);

    order.insert (order.end(), chain.midiEffects.begin(), chain.midiEffects.end());

    if (chain.instrument != 0)
        order.push_back (chain.instrument);

    order.insert (order.end(), chain.effects.begin(), chain.effects.end());

    if (chain.gain != 0)
        order.push_back (chain.gain);

    order.push_back (chain.output);

    graph.connections.clear();

    for (size_t i = 0; i + 1 < order.size(); ++i)
    {
        // Without an instrument the notes go only through the MIDI effects: the audio effects start from silence.
        const auto isMidi = [&] (ObjectId id)
        { return std::ranges::find (chain.midiEffects, id) != chain.midiEffects.end(); };

        if (chain.sequencer != 0 && chain.instrument == 0 && (order[i] == chain.sequencer || isMidi (order[i]))
            && ! isMidi (order[i + 1]))
            continue;

        graph.connections.push_back ({ order[i], 0, order[i + 1], 0 });
    }

    graph.connections.insert (graph.connections.end(), extra.begin(), extra.end());
    pruneModulationSlots (graph);

    for (size_t i = 0; i < order.size(); ++i)
    {
        auto& node = requireNode (graph, order[i]);
        node.x = 220.0f * static_cast<float> (i);
        node.y = 0.0f;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Track commands

// A new track's chain: its source nodes, then the default inserts, an EQ and a compressor that starts bypassed
// (docs/design/channel-strip.md, C2), then Gain -> Track Output, connected in a row.
void makeChainGraph (Track& track, Project& result, std::initializer_list<const char*> sources)
{
    std::vector<const char*> types (sources);
    types.insert (types.end(), { nodetype::eq, nodetype::compressor, nodetype::gain, nodetype::trackOutput });
    track.graph.nodes.clear();
    track.graph.connections.clear();

    for (const auto* type : types)
    {
        auto node = makeNode (result, type, 220.0f * static_cast<float> (track.graph.nodes.size()), 0.0f);
        node.bypassed = juce::String (type) == nodetype::compressor;

        if (! track.graph.nodes.empty())
            track.graph.connections.push_back ({ track.graph.nodes.back().id, 0, node.id, 0 });

        track.graph.nodes.push_back (std::move (node));
    }
}

// A bus's graph, as the master track has, with the default inserts: Mix Input -> EQ -> Compressor -> Gain ->
// Track Output (docs/design/groups-and-sends.md, R1).
void makeBusGraph (Track& track, Project& result)
{
    makeChainGraph (track, result, { nodetype::mixInput });
}

// A new group named `name` (or "Group n"), with an empty chain.
[[nodiscard]] Track makeGroup (const juce::String& name, Project& result)
{
    const auto groups =
        std::ranges::count_if (result.tracks, [] (const TrackPtr& t) { return t->kind == TrackKind::group; });
    const auto& colours = trackColours();

    Track group;
    group.id = result.nextId++;
    group.kind = TrackKind::group;
    group.name = name.trim().isNotEmpty() ? name.trim() : "Group " + juce::String (groups + 1);
    group.colour = colours[static_cast<int> (static_cast<size_t> (groups) % static_cast<size_t> (colours.size()))];
    makeBusGraph (group, result);
    return group;
}

// The index of the first return track (they come last, R10), or the end of the list.
[[nodiscard]] size_t firstReturn (const Project& project)
{
    const auto it =
        std::ranges::find_if (project.tracks, [] (const TrackPtr& t) { return t->kind == TrackKind::returnTrack; });
    return static_cast<size_t> (it - project.tracks.begin());
}

// Where a new track goes: at the end of its group's members, or at the end of the top level (before the returns).
[[nodiscard]] size_t endOfGroup (const Project& project, ObjectId parent)
{
    if (parent == 0)
        return firstReturn (project);

    const auto index = project.indexOfTrack (parent);

    if (index < 0 || project.tracks[static_cast<size_t> (index)]->kind != TrackKind::group)
        throw CommandError ("No group with id " + juce::String (parent));

    return subtreeEnd (project.tracks, static_cast<size_t> (index));
}

// Where a track placed after the track at `index` goes: past it, everything inside it, and the output tracks of those
// that still sit right after them (docs/design/multi-output-instruments.md, MO4).
[[nodiscard]] size_t endAfter (const std::vector<TrackPtr>& tracks, size_t index)
{
    std::set<ObjectId> block;
    auto end = subtreeEnd (tracks, index);

    for (auto i = index; i < end; ++i)
        block.insert (tracks[i]->id);

    while (end < tracks.size() && tracks[end]->kind == TrackKind::output && block.contains (tracks[end]->source))
        block.insert (tracks[end++]->id);

    return end;
}

[[nodiscard]] Project addTrack (const Project& project, const juce::var& args)
{
    auto result = project;
    const auto& colours = trackColours();
    const auto parent = static_cast<ObjectId> (args.getProperty ("parent", 0));
    const auto at = endOfGroup (project, parent);

    if (args["kind"].toString() == "return")
    {
        // A return track (docs/design/groups-and-sends.md, R10): a bus fed by sends, after every other track.
        const auto returns = std::ranges::count_if (project.tracks, [] (const TrackPtr& t)
                                                    { return t->kind == TrackKind::returnTrack; });
        Track track;
        track.id = result.nextId++;
        track.kind = TrackKind::returnTrack;
        track.name = args["name"].toString().trim().isNotEmpty() ? args["name"].toString().trim()
                                                                 : "Return " + juce::String (returns + 1);
        track.colour =
            colours[static_cast<int> (static_cast<size_t> (returns + 5) % static_cast<size_t> (colours.size()))];
        makeBusGraph (track, result);
        const auto returnId = track.id;
        result.tracks.push_back (std::make_shared<const Track> (std::move (track)));

        // A new return for a track's send: one step (the inspector's "New return track").
        if (args.hasProperty ("sendFrom"))
            return applyCommand (result, "addSend",
                                 makeObject ({ { "trackId", args["sendFrom"] }, { "target", returnId } }));

        return result;
    }

    if (args["kind"].toString() == "output")
    {
        // An output track (docs/design/multi-output-instruments.md, MO4): a bus fed by an instrument's Output to track
        // nodes. It starts right after the instrument and its other output tracks there, in its group.
        const auto sourceIndex = project.indexOfTrack (requireId (args, "source"));

        if (sourceIndex < 0)
            throw CommandError ("No track with id " + args["source"].toString());

        const auto& source = *project.tracks[static_cast<size_t> (sourceIndex)];

        if (source.kind != TrackKind::instrument && source.kind != TrackKind::audio)
            throw CommandError ("Only instrument and audio tracks have output tracks");

        Track track;
        track.id = result.nextId++;
        track.kind = TrackKind::output;
        track.source = source.id;
        track.parent = source.parent;
        track.colour = source.colour;
        track.name = args["name"].toString().trim().isNotEmpty() ? args["name"].toString().trim()
                                                                 : source.name + " " + juce::String (2);
        makeBusGraph (track, result);
        result.tracks.insert (
            result.tracks.begin()
                + static_cast<std::ptrdiff_t> (endAfter (result.tracks, static_cast<size_t> (sourceIndex))),
            std::make_shared<const Track> (std::move (track)));
        normaliseGroups (result);
        return result;
    }

    if (args["kind"].toString() == "group")
    {
        auto group = makeGroup (args["name"].toString(), result);
        group.parent = parent;
        result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (at),
                              std::make_shared<const Track> (std::move (group)));
        return result;
    }

    Track track;
    track.id = result.nextId++;
    track.parent = parent;
    track.name = args["name"].toString().trim();

    if (track.name.isEmpty())
        track.name = "Track " + juce::String (project.tracks.size() + 1);

    track.colour = colours[static_cast<int> (project.tracks.size() % static_cast<size_t> (colours.size()))];

    if (args["kind"].toString() == "audio")
    {
        // Audio track: Audio player -> [inserts] -> Gain -> Track Output (phase 7, T1).
        track.kind = TrackKind::audio;
        makeChainGraph (track, result, { nodetype::audioPlayer });
    }
    else
    {
        // Default graph: Sequencer -> MPE Synth -> [inserts] -> Gain -> Track Output.
        makeChainGraph (track, result, { nodetype::sequencer, nodetype::mpeSynth });
    }

    const auto trackId = track.id;
    result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (at),
                          std::make_shared<const Track> (std::move (track)));

    // A device dropped from the browser where there is no track: the new track holds it, an instrument in the MPE
    // Synth's place and an effect after the inserts, in the same undo step. "device" takes insertNode's arguments.
    if (args["device"].isObject())
    {
        auto insert = args["device"].clone();
        insert.getDynamicObject()->setProperty ("trackId", static_cast<juce::int64> (trackId));
        return applyCommand (result, "insertNode", insert);
    }

    return result;
}

// Removes a track's send and the automation lanes of its level.
void eraseSend (Track& track, ObjectId sendId)
{
    std::erase_if (track.sends, [sendId] (const Send& s) { return s.id == sendId; });
    std::erase_if (
        track.automation, [sendId] (const AutomationLane& lane)
        { return lane.target.kind == AutomationTargetKind::send && lane.target.param == juce::String (sendId); });
}

// Removes the key sends to nodes that are gone (K2).
void removeKeySendsTo (Project& project, ObjectId trackId, const std::set<ObjectId>& nodes)
{
    for (auto& track : project.tracks)
    {
        const auto keysRemoved = [&] (const Send& s) { return s.target == trackId && nodes.contains (s.targetNode); };

        if (std::ranges::none_of (track->sends, keysRemoved))
            continue;

        auto changed = *track;

        for (const auto& send : track->sends)
            if (keysRemoved (send))
                eraseSend (changed, send.id);

        track = std::make_shared<const Track> (std::move (changed));
    }
}

// Removes the sends to tracks that are gone (a removed return takes its sends along).
void removeSendsTo (Project& project, const std::set<ObjectId>& removed)
{
    for (auto& track : project.tracks)
    {
        if (std::ranges::none_of (track->sends, [&] (const Send& s) { return removed.contains (s.target); }))
            continue;

        auto changed = *track;

        for (const auto& send : track->sends)
            if (removed.contains (send.target))
                eraseSend (changed, send.id);

        track = std::make_shared<const Track> (std::move (changed));
    }
}

// Removes the Output to track nodes that fed tracks now gone, and puts the Sampler zones whose output then leads
// nowhere back on Main (docs/design/multi-output-instruments.md, MO5).
void detachOutputTracks (Project& project, const std::set<ObjectId>& removed)
{
    for (auto& track : project.tracks)
    {
        const auto feedsRemoved = [&] (const GraphNode& node)
        { return node.type == nodetype::busOutput && removed.contains (node.target); };

        if (std::ranges::none_of (track->graph.nodes, feedsRemoved))
            continue;

        auto changed = *track;
        auto& graph = changed.graph;
        std::set<ObjectId> gone;

        for (const auto& node : graph.nodes)
            if (feedsRemoved (node))
                gone.insert (node.id);

        std::erase_if (graph.nodes, [&] (const GraphNode& node) { return gone.contains (node.id); });
        std::erase_if (graph.connections,
                       [&] (const Connection& c) { return gone.contains (c.fromNode) || gone.contains (c.toNode); });

        const auto unwired = [&] (const GraphNode& node, int output)
        {
            return output > 1
                   && std::ranges::none_of (graph.connections, [&] (const Connection& c)
                                            { return c.fromNode == node.id && c.fromPort == output - 1; });
        };

        for (auto& node : graph.nodes)
        {
            if (node.type == nodetype::sampler)
                for (auto& zone : node.sampleZones)
                    if (unwired (node, zone.output))
                        zone.output = 1;

            if (node.type == nodetype::drumSynth)
                for (auto& pad : node.drumPads)
                    if (unwired (node, pad.output))
                        pad.output = 1;
        }

        track = std::make_shared<const Track> (std::move (changed));
    }
}

[[nodiscard]] Project removeTrack (const Project& project, const juce::var& args)
{
    const auto index = project.indexOfTrack (requireId (args, "id"));

    if (index < 0)
        throw CommandError ("No track with id " + args["id"].toString());

    // A group goes with everything inside it (docs/design/groups-and-sends.md, section 8), and a track with its output
    // tracks, wherever they are (docs/design/multi-output-instruments.md, MO11).
    auto result = project;
    std::set<ObjectId> removed;

    for (auto i = static_cast<size_t> (index); i < subtreeEnd (result.tracks, static_cast<size_t> (index)); ++i)
        removed.insert (result.tracks[i]->id);

    for (const auto& track : result.tracks)
        if (track->kind == TrackKind::output && removed.contains (track->source))
            removed.insert (track->id);

    std::erase_if (result.tracks, [&] (const TrackPtr& t) { return removed.contains (t->id); });
    std::erase_if (result.midiMappings, [&] (const MidiMapping& m) { return removed.contains (m.trackId); });
    removeSendsTo (result, removed);
    detachOutputTracks (result, removed);
    return result;
}

[[nodiscard]] NoteClip copyWithNewIds (const NoteClip& clip, Project& result); // with the clip commands
[[nodiscard]] std::vector<ObjectId> requireIds (const juce::var& args, const char* key);

// Duplicates a track right below it: the same settings and graph with new node ids (a plugin loads from the same
// saved state), clips with new ids that don't link to the originals, and automation lanes pointing at the copied
// nodes. MIDI mappings stay with the original.
// Gives the plugin nodes named in `states` (by node id, as base64) those states, inside components and voice graphs too:
// what each saved for a copy of itself (DuplicateStateSource), which the app fills in before duplicating.
void setDuplicateStates (TrackGraph& graph, const juce::var& states)
{
    for (auto& node : graph.nodes)
    {
        if (node.plugin.has_value())
            if (const auto state = states[juce::Identifier (juce::String (node.id))]; state.isString())
            {
                auto block = std::make_shared<juce::MemoryBlock>();

                if (block->fromBase64Encoding (state.toString()))
                    node.pluginState = std::move (block);
            }

        if (node.voiceGraph != nullptr)
        {
            auto voice = *node.voiceGraph;
            setDuplicateStates (voice, states);
            node.voiceGraph = std::make_shared<const TrackGraph> (std::move (voice));
        }

        if (node.component != nullptr)
        {
            auto inside = *node.component;
            setDuplicateStates (inside.graph, states);
            node.component = std::make_shared<const ComponentGraph> (std::move (inside));
        }
    }
}

[[nodiscard]] Project duplicateTrack (const Project& project, const juce::var& args)
{
    const auto index = project.indexOfTrack (requireId (args, "id"));

    if (index < 0)
        throw CommandError ("No track with id " + args["id"].toString());

    // A group is copied with everything inside it; the copies of nested groups hold the copies of their members.
    auto result = project;
    const auto first = static_cast<size_t> (index);
    const auto end = subtreeEnd (project.tracks, first);
    std::map<ObjectId, ObjectId> trackIds;
    std::map<ObjectId, std::map<ObjectId, ObjectId>> nodeIdsOf; // by original track

    const auto copyOf = [&] (const Track& original)
    {
        auto track = original;
        track.id = result.nextId++;
        trackIds[original.id] = track.id;

        if (args["pluginStates"].isObject())
            setDuplicateStates (track.graph, args["pluginStates"]);

        auto& nodeIds = nodeIdsOf[original.id];
        renumberGraph (track.graph, result, nodeIds);

        for (auto& clip : track.clips)
            clip = std::make_shared<const NoteClip> (copyWithNewIds (*clip, result));

        std::map<juce::String, juce::String> sendIds;

        for (auto& send : track.sends)
        {
            const auto id = result.nextId++;
            sendIds[juce::String (send.id)] = juce::String (id);
            send.id = id;
        }

        for (auto& lane : track.automation)
        {
            lane.id = result.nextId++;

            if (const auto node = nodeIds.find (lane.target.nodeId); node != nodeIds.end())
                lane.target.nodeId = node->second;

            if (lane.target.kind == AutomationTargetKind::send)
                lane.target.param = sendIds[lane.target.param];
        }

        return track;
    };

    std::vector<Track> copies;

    for (auto i = first; i < end; ++i)
    {
        auto track = copyOf (*project.tracks[i]);

        // Members go into the copied group, and an output track inside it belongs to the copy of its track.
        if (i > first)
        {
            if (const auto parent = trackIds.find (track.parent); parent != trackIds.end())
                track.parent = parent->second;

            if (const auto source = trackIds.find (track.source); source != trackIds.end())
                track.source = source->second;
        }

        copies.push_back (std::move (track));
    }

    // Output tracks of the copied tracks that lie elsewhere (moved or grouped apart, MO4) get copies too, right after
    // the copy of their track and in its group.
    std::map<ObjectId, std::vector<Track>> outputCopies;

    for (size_t i = 0; i < project.tracks.size(); ++i)
    {
        const auto& track = *project.tracks[i];

        if ((i < first || i >= end) && track.kind == TrackKind::output && trackIds.contains (track.source))
        {
            const auto source = trackIds[track.source];
            auto copy = copyOf (track);
            copy.source = source;
            copy.parent = std::ranges::find (copies, source, &Track::id)->parent;
            outputCopies[source].push_back (std::move (copy));
        }
    }

    // The copies' Output to track nodes feed the copies of their output tracks (MO11).
    std::vector<TrackPtr> shared;
    const auto share = [&] (Track copy)
    {
        for (auto& node : copy.graph.nodes)
            if (const auto target = trackIds.find (node.target);
                node.type == nodetype::busOutput && target != trackIds.end())
                node.target = target->second;

        shared.push_back (std::make_shared<const Track> (std::move (copy)));
    };

    for (auto& copy : copies)
    {
        const auto id = copy.id;
        share (std::move (copy));

        for (auto& output : outputCopies[id])
            share (std::move (output));
    }

    const auto at = endAfter (project.tracks, first);
    result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (at), shared.begin(), shared.end());

    // Links with both ends in the copies are copied with them; links to anything else aren't
    // (docs/design/parameter-linking.md, L14).
    const auto copied = [&] (const ParamAddress& address) -> std::optional<ParamAddress>
    {
        const auto nodes = nodeIdsOf.find (address.trackId);
        const auto node = nodes != nodeIdsOf.end() ? nodes->second.find (address.nodeId)
                                                   : std::map<ObjectId, ObjectId>::const_iterator {};

        if (nodes == nodeIdsOf.end() || node == nodes->second.end())
            return std::nullopt;

        return ParamAddress { trackIds.at (address.trackId), node->second, address.param };
    };

    for (const auto& link : project.links)
    {
        const auto source = copied (link.source);
        const auto target = copied (link.target);

        if (source.has_value() && target.has_value())
        {
            auto copy = link;
            copy.id = result.nextId++;
            copy.source = *source;
            copy.target = *target;
            result.links.push_back (std::move (copy));
        }
    }

    return result;
}

// The tracks named by `ids` that aren't inside another of them (those move with it), in track order.
[[nodiscard]] std::vector<size_t> topmostIn (const Project& project, const std::vector<ObjectId>& ids)
{
    const std::set<ObjectId> wanted (ids.begin(), ids.end());
    std::vector<size_t> result;
    size_t skipUntil = 0;

    for (size_t i = 0; i < project.tracks.size(); ++i)
    {
        if (i < skipUntil || ! wanted.contains (project.tracks[i]->id))
            continue;

        result.push_back (i);
        skipUntil = subtreeEnd (project.tracks, i);
    }

    if (result.empty() || std::ranges::any_of (ids, [&] (ObjectId id) { return project.indexOfTrack (id) < 0; }))
        throw CommandError ("Some of the tracks don't exist");

    return result;
}

// Takes the blocks starting at `indices` (each a track and everything inside it) out of the track list.
[[nodiscard]] std::vector<TrackPtr> takeBlocks (std::vector<TrackPtr>& tracks, const std::vector<size_t>& indices)
{
    std::vector<TrackPtr> taken;
    std::vector<bool> take (tracks.size(), false);

    for (const auto index : indices)
        for (auto i = index; i < subtreeEnd (tracks, index); ++i)
            take[i] = true;

    std::vector<TrackPtr> kept;

    for (size_t i = 0; i < tracks.size(); ++i)
        (take[i] ? taken : kept).push_back (tracks[i]);

    tracks = std::move (kept);
    return taken;
}

[[nodiscard]] TrackPtr withParent (const TrackPtr& track, ObjectId parent)
{
    if (track->parent == parent)
        return track;

    auto changed = *track;
    changed.parent = parent;
    return std::make_shared<const Track> (std::move (changed));
}

// Groups tracks (Ctrl+G): a new group where the first of them is, holding them in their order
// (docs/design/groups-and-sends.md, section 5). It joins the group the first track was in, so groups nest.
[[nodiscard]] Project groupTracks (const Project& project, const juce::var& args)
{
    const auto roots = topmostIn (project, requireIds (args, "ids"));

    if (std::ranges::any_of (roots, [&] (size_t i) { return project.tracks[i]->kind == TrackKind::returnTrack; }))
        throw CommandError ("Return tracks can't be grouped");

    auto result = project;
    auto group = makeGroup (args["name"].toString(), result);
    group.parent = project.tracks[roots.front()]->parent;

    // Where the group goes: before the first track, counted among the tracks that stay.
    const auto position = roots.front();
    auto moved = takeBlocks (result.tracks, roots);
    const std::set<ObjectId> rootIds = [&]
    {
        std::set<ObjectId> ids;
        for (const auto index : roots)
            ids.insert (project.tracks[index]->id);
        return ids;
    }();

    for (auto& track : moved)
        if (rootIds.contains (track->id))
            track = withParent (track, group.id);

    moved.insert (moved.begin(), std::make_shared<const Track> (std::move (group)));
    result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (position), moved.begin(), moved.end());
    normaliseGroups (result);
    return result;
}

// Ungroups (Ctrl+Shift+G): the members move up to the group's own group, and the group goes.
[[nodiscard]] Project ungroup (const Project& project, const juce::var& args)
{
    const auto index = project.indexOfTrack (requireId (args, "id"));

    if (index < 0 || project.tracks[static_cast<size_t> (index)]->kind != TrackKind::group)
        throw CommandError ("Not a group");

    auto result = project;
    const auto& group = *project.tracks[static_cast<size_t> (index)];

    for (auto& track : result.tracks)
        if (track->parent == group.id)
            track = withParent (track, group.parent);

    result.tracks.erase (result.tracks.begin() + index);
    std::erase_if (result.midiMappings, [&] (const MidiMapping& m) { return m.trackId == group.id; });
    return result;
}

// Moves tracks (each with everything inside it) into `parent` (0: the top level), before the member `before`
// (0: after its last member).
[[nodiscard]] Project moveTracks (const Project& project, const juce::var& args)
{
    const auto roots = topmostIn (project, requireIds (args, "ids"));
    const auto parent = static_cast<ObjectId> (args.getProperty ("parent", 0));
    const auto before = static_cast<ObjectId> (args.getProperty ("before", 0));
    const auto isReturn = [&] (size_t i) { return project.tracks[i]->kind == TrackKind::returnTrack; };
    const auto returns = std::ranges::count_if (roots, isReturn);

    // Returns move only within their section, other tracks never into it (R10).
    if (returns != 0 && (returns != static_cast<std::ptrdiff_t> (roots.size()) || parent != 0))
        throw CommandError ("Return tracks stay in the returns section");

    if (const auto* target = project.findTrack (before);
        target != nullptr && (target->kind == TrackKind::returnTrack) != (returns != 0))
        throw CommandError ("Return tracks stay in the returns section");

    auto result = project;
    auto moved = takeBlocks (result.tracks, roots);

    // The destination must still be there: a track can't move into itself or before a track that moves.
    if (parent != 0)
    {
        const auto at = result.indexOfTrack (parent);
        if (at < 0 || result.tracks[static_cast<size_t> (at)]->kind != TrackKind::group)
            throw CommandError ("Tracks can only move into a group they're not part of");
    }

    size_t position = 0;

    if (before != 0)
    {
        const auto at = result.indexOfTrack (before);

        if (at < 0 || result.tracks[static_cast<size_t> (at)]->parent != parent)
            throw CommandError ("The tracks can't go there");

        position = static_cast<size_t> (at);
    }
    else
    {
        position = returns != 0 ? result.tracks.size() : endOfGroup (result, parent);
    }

    for (const auto index : roots)
        for (auto& track : moved)
            if (track->id == project.tracks[index]->id)
                track = withParent (track, parent);

    result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (position), moved.begin(), moved.end());
    normaliseGroups (result);
    return result;
}

// Whether `from` reaches `to` through sends (only returns send onwards, so this follows returns).
[[nodiscard]] bool sendsReach (const Project& project, ObjectId from, ObjectId to, std::set<ObjectId>& seen)
{
    if (from == to)
        return true;

    if (! seen.insert (from).second)
        return false;

    const auto* track = project.findTrack (from);

    return track != nullptr
           && std::ranges::any_of (track->sends,
                                   [&] (const Send& s) { return sendsReach (project, s.target, to, seen); });
}

// Whether a node can be keyed: its second input is audio (docs/design/sidechain.md, K1).
[[nodiscard]] bool isKeyable (const GraphNode& node)
{
    return NodeRegistry::builtin().find (node.type) != nullptr && portsOf (node).isInput (1, PortType::audio);
}

// Whether track `from` runs before track `to` (so `to` depends on it): through group membership (members run before
// their group) or sends (senders run before the tracks they feed, returns and keyed tracks alike; K3).
[[nodiscard]] bool runsBefore (const Project& project, ObjectId from, ObjectId to)
{
    std::set<ObjectId> seen;
    std::vector<ObjectId> pending { from };

    while (! pending.empty())
    {
        const auto id = pending.back();
        pending.pop_back();

        if (id == to)
            return true;

        if (! seen.insert (id).second)
            continue;

        if (const auto* track = project.findTrack (id))
        {
            if (track->parent != 0)
                pending.push_back (track->parent);

            for (const auto& send : track->sends)
                pending.push_back (send.target);
        }
    }

    return false;
}

[[nodiscard]] Send& requireSend (Track& track, ObjectId sendId)
{
    const auto it = std::ranges::find (track.sends, sendId, &Send::id);

    if (it == track.sends.end())
        throw CommandError ("No send with id " + juce::String (sendId));

    return *it;
}

void applySendFields (const juce::var& args, Send& send)
{
    if (args.hasProperty ("levelDb"))
        send.levelDb = std::clamp (requireNumber (args, "levelDb"), limits::minGainDb, limits::maxGainDb);

    if (args.hasProperty ("preFader"))
        send.preFader = static_cast<bool> (args["preFader"]);

    if (args.hasProperty ("active"))
        send.active = static_cast<bool> (args["active"]);
}

// A key send (K2, K3): to a keyable node on another track, once per node, without closing a loop.
[[nodiscard]] Project addKeySend (const Project& project, const juce::var& args, ObjectId targetId, ObjectId targetNode)
{
    const auto* target = project.findTrack (targetId);
    const auto sourceId = requireId (args, "trackId");
    const auto* keyed = target != nullptr ? target->findTrackNode (targetNode) : nullptr;

    if (keyed == nullptr || ! isKeyable (*keyed))
        throw CommandError (
            "Keys go to a compressor, gate, expander or plugin with a sidechain input on another track");

    if (targetId == sourceId)
        throw CommandError ("A track keys its own nodes in the graph, not through a send");

    if (runsBefore (project, targetId, sourceId))
        throw CommandError ("That key would feed the track back into itself");

    return updateTrack (project, sourceId,
                        [&] (Track& track, Project& result)
                        {
                            if (std::ranges::any_of (track.sends, [&] (const Send& s)
                                                     { return s.target == targetId && s.targetNode == targetNode; }))
                                throw CommandError ("The track already keys that node");

                            if (track.sends.size() >= limits::maxSends)
                                throw CommandError ("A track has at most " + juce::String (limits::maxSends)
                                                    + " sends");

                            Send send;
                            send.id = result.nextId++;
                            send.target = targetId;
                            send.targetNode = targetNode;
                            send.preFader = true;
                            applySendFields (args, send);
                            track.sends.push_back (send);
                        });
}

// Adds a send to a return track (docs/design/groups-and-sends.md, R4): post-fader at 0 dB unless told otherwise. With
// a targetNode, the send keys that node on another track instead (docs/design/sidechain.md, K2): pre-fader unless
// told otherwise.
[[nodiscard]] Project addSend (const Project& project, const juce::var& args)
{
    const auto targetId = requireId (args, "target");
    const auto* target = project.findTrack (targetId);
    const auto targetNode = args.hasProperty ("targetNode") ? requireId (args, "targetNode") : ObjectId { 0 };

    if (targetNode != 0)
        return addKeySend (project, args, targetId, targetNode);

    if (target == nullptr || target->kind != TrackKind::returnTrack)
        throw CommandError ("Sends go to return tracks");

    return updateTrack (project, requireId (args, "trackId"),
                        [&] (Track& track, Project& result)
                        {
                            std::set<ObjectId> seen;

                            if (track.id == targetId || sendsReach (project, targetId, track.id, seen))
                                throw CommandError ("That send would feed the track back into itself");

                            if (std::ranges::any_of (track.sends, [&] (const Send& s)
                                                     { return s.target == targetId && s.targetNode == 0; }))
                                throw CommandError ("The track already sends there");

                            if (track.sends.size() >= limits::maxSends)
                                throw CommandError ("A track has at most " + juce::String (limits::maxSends)
                                                    + " sends");

                            Send send;
                            send.id = result.nextId++;
                            send.target = targetId;
                            applySendFields (args, send);
                            track.sends.push_back (send);
                        });
}

[[nodiscard]] Project setSend (const Project& project, const juce::var& args)
{
    return updateTrack (project, requireId (args, "trackId"), [&] (Track& track, Project&)
                        { applySendFields (args, requireSend (track, requireId (args, "sendId"))); });
}

[[nodiscard]] Project removeSend (const Project& project, const juce::var& args)
{
    return updateTrack (project, requireId (args, "trackId"),
                        [&] (Track& track, Project&)
                        {
                            const auto sendId = requireId (args, "sendId");
                            std::ignore = requireSend (track, sendId);
                            eraseSend (track, sendId);
                        });
}

[[nodiscard]] Project setTrackFolded (const Project& project, const juce::var& args)
{
    return updateTrack (project, requireId (args, "id"),
                        [&] (Track& track, Project&)
                        {
                            if (track.kind != TrackKind::group)
                                throw CommandError ("Only groups fold");

                            track.folded = static_cast<bool> (args["folded"]);
                        });
}

[[nodiscard]] Project renameTrack (const Project& project, const juce::var& args)
{
    const auto newName = args["name"].toString().trim();

    if (newName.isEmpty())
        throw CommandError ("Track name must not be empty");

    return updateTrack (project, requireId (args, "id"), [&] (Track& t, Project&) { t.name = newName; });
}

[[nodiscard]] Project setTrackGain (const Project& project, const juce::var& args)
{
    const auto gain = std::clamp (requireNumber (args, "gainDb"), limits::minGainDb, limits::maxGainDb);
    return updateTrackOrMaster (project, requireId (args, "id"), [&] (Track& t, Project&) { t.gainDb = gain; });
}

[[nodiscard]] Project setTrackPan (const Project& project, const juce::var& args)
{
    const auto pan = std::clamp (requireNumber (args, "pan"), limits::minPan, limits::maxPan);
    return updateTrackOrMaster (project, requireId (args, "id"), [&] (Track& t, Project&) { t.pan = pan; });
}

// Adds a MIDI file's parts as new instrument tracks (docs/design/sampler-midi-delay-limiter.md, section 3), optionally
// with its tempo and time signatures, as what parseMidiFile() returns.
[[nodiscard]] Project importMidi (const Project& project, const juce::var& args)
{
    auto result = project;

    if (args.hasProperty ("tempo"))
    {
        std::vector<TempoPoint> points;

        for (const auto& point : requireArray (args, "tempo"))
            points.push_back (
                { requireTicks (point, "tick", 0, limits::maxPosition), requireNumber (point, "bpm"), false });

        result = withTempo (result, std::move (points));
    }

    if (args.hasProperty ("signatures"))
    {
        std::vector<TimeSignature> signatures;

        for (const auto& signature : requireArray (args, "signatures"))
            signatures.push_back ({ requireBar (signature, "bar"), requireInt (signature, "numerator"),
                                    requireInt (signature, "denominator") });

        result = withSignatures (result, std::move (signatures));
    }

    const auto start = requireTicksOr (args, "start", 0);

    for (const auto& part : requireArray (args, "tracks"))
    {
        const auto trackId = result.nextId;
        result = applyCommand (result, "addTrack", makeObject ({ { "kind", "instrument" }, { "name", part["name"] } }));

        if (static_cast<bool> (part["drums"]))
            result = applyCommand (result, "setTrackDrumMode", makeObject ({ { "id", trackId }, { "mode", "drums" } }));

        result = applyCommand (result, "addClip",
                               makeObject ({ { "trackId", trackId },
                                             { "start", start },
                                             { "length", part["length"] },
                                             { "name", part["name"] },
                                             { "notes", part["notes"] },
                                             { "controllers", part["controllers"] } }));
    }

    return result;
}

[[nodiscard]] Project setTrackDrumMode (const Project& project, const juce::var& args)
{
    const auto mode = args["mode"].toString();

    if (mode != "auto" && mode != "drums" && mode != "melodic")
        throw CommandError ("Unknown drum mode '" + mode + "'");

    return updateTrack (project, requireId (args, "id"), [&] (Track& t, Project&) { t.drumMode = mode; });
}

[[nodiscard]] Project setTrackOffset (const Project& project, const juce::var& args)
{
    const auto offset = std::clamp (requireNumber (args, "offsetMs"), limits::minOffsetMs, limits::maxOffsetMs);
    const auto id = requireId (args, "id");

    if (project.master != nullptr && project.master->id == id)
        throw CommandError ("The master track has no timing offset");

    return updateTrack (project, id, [&] (Track& t, Project&) { t.offsetMs = offset; });
}

// ---------------------------------------------------------------------------------------------------------
// Clip commands (docs/design/phase6-arrangement.md)

[[nodiscard]] Ticks clipEnd (const NoteClip& clip) noexcept
{
    return clip.start + clip.length;
}

// Wraps a content position into a looped clip's loop range; leaves it alone for clips without a loop.
[[nodiscard]] Ticks wrapIntoLoop (const NoteClip& clip, Ticks position) noexcept
{
    if (clip.loopLength <= 0)
        return position;

    const auto relative = (position - clip.loopStart) % clip.loopLength;
    return clip.loopStart + (relative < 0 ? relative + clip.loopLength : relative);
}

// Moves the left edge to newStart, keeping the content in place on the timeline (A2).
void trimStart (NoteClip& clip, Ticks newStart) noexcept
{
    const auto delta = newStart - clip.start;
    clip.length -= delta;
    clip.offset = clip.loopLength > 0 ? wrapIntoLoop (clip, clip.offset + delta) : clip.offset + delta;
    clip.start = newStart;
}

// A copy of a clip with new ids for the clip and its notes (note ids are unique outside linked clips).
[[nodiscard]] NoteClip copyWithNewIds (const NoteClip& clip, Project& result)
{
    auto copy = clip;
    copy.id = result.nextId++;
    copy.linkId = 0;
    std::map<ObjectId, ObjectId> ids;

    for (auto& note : copy.notes)
    {
        auto renamed = *note;
        renamed.id = result.nextId++;
        ids[note->id] = renamed.id;
        note = std::make_shared<const Note> (std::move (renamed));
    }

    // Links follow the notes' new ids.
    for (auto& note : copy.notes)
    {
        if (note->link.has_value())
        {
            auto relinked = *note;
            auto link = *note->link;

            if (const auto target = ids.find (link.to); target != ids.end())
            {
                link.to = target->second;
                relinked.link = link;
            }
            else
            {
                relinked.link.reset();
            }

            note = std::make_shared<const Note> (std::move (relinked));
        }
    }

    return copy;
}

// A linked copy: a new clip id, the same notes (and note ids) and link id.
[[nodiscard]] NoteClip linkedCopy (const NoteClip& clip, Project& result)
{
    auto copy = clip;
    copy.id = result.nextId++;
    return copy;
}

void setLinkId (Track& track, ObjectId clipId, ObjectId linkId)
{
    for (auto& clipPtr : track.clips)
    {
        if (clipPtr->id == clipId)
        {
            auto clip = *clipPtr;
            clip.linkId = linkId;
            clipPtr = std::make_shared<const NoteClip> (std::move (clip));
        }
    }
}

// The clip to place for a move: the clip itself, a copy with new ids, or a linked copy.
[[nodiscard]] NoteClip takeOrCopy (NoteClip& clip, bool copy, bool linked, Project& result)
{
    if (linked)
        return linkedCopy (clip, result);

    return copy ? copyWithNewIds (clip, result) : std::move (clip);
}

// Splits a clip at a timeline position strictly inside it (A4). Notes go to the part they start in; a looped
// clip keeps its loop in both parts (the right part is a copy), so both sound exactly as before.
[[nodiscard]] std::pair<NoteClip, NoteClip> splitClip (const NoteClip& clip, Ticks at, Project& result)
{
    auto left = clip;
    left.length = at - clip.start;

    NoteClip right;

    if (clip.loopLength > 0 || clip.linkId != 0)
    {
        // A looped clip keeps its loop in both parts; a linked clip stays linked on the left and the right
        // becomes a unique copy.
        right = copyWithNewIds (clip, result);
        right.linkId = 0;
    }
    else
    {
        const auto contentAt = clip.offset + (at - clip.start);
        right = clip;
        right.id = result.nextId++;
        std::erase_if (left.notes, [&] (const NotePtr& n) { return n->start >= contentAt; });
        std::erase_if (right.notes, [&] (const NotePtr& n) { return n->start < contentAt; });
    }

    trimStart (right, at);
    return { std::move (left), std::move (right) };
}

// Replaces whatever covers [from, to) in one of a track's takes lanes, except the clips in `keep` (A6, per lane since
// docs/design/takes-lanes.md, T2): covered clips are removed, trimmed or split around the range.
void clearRange (Track& track, Ticks from, Ticks to, Project& result, const std::set<ObjectId>& keep, int lane)
{
    std::vector<ClipPtr> clips;

    for (const auto& clipPtr : track.clips)
    {
        const auto& clip = *clipPtr;
        const auto end = clipEnd (clip);

        if (keep.contains (clip.id) || clip.lane != lane || end <= from || clip.start >= to)
        {
            clips.push_back (clipPtr);
        }
        else if (clip.start < from && end > to)
        {
            auto [left, rest] = splitClip (clip, from, result);
            auto [middle, right] = splitClip (rest, to, result);
            clips.push_back (std::make_shared<const NoteClip> (std::move (left)));
            clips.push_back (std::make_shared<const NoteClip> (std::move (right)));
        }
        else if (clip.start < from)
        {
            auto trimmed = clip;
            trimmed.length = from - clip.start;
            clips.push_back (std::make_shared<const NoteClip> (std::move (trimmed)));
        }
        else if (end > to)
        {
            auto trimmed = clip;
            trimStart (trimmed, to);
            clips.push_back (std::make_shared<const NoteClip> (std::move (trimmed)));
        }
        // else: completely covered, removed
    }

    track.clips = std::move (clips);
}

// The order for a clip placed on a track now: above every clip there (docs/design/takes-lanes.md, T1).
[[nodiscard]] std::int64_t topOrder (const Track& track)
{
    std::int64_t top = 0;

    for (const auto& clip : track.clips)
        top = std::max (top, clip->order + 1);

    return top;
}

// Whether a lane holds nothing over [from, to), besides the clips in `keep`.
[[nodiscard]] bool laneFree (const Track& track, int lane, Ticks from, Ticks to, const std::set<ObjectId>& keep)
{
    return std::ranges::none_of (
        track.clips, [&] (const ClipPtr& c)
        { return c->lane == lane && ! keep.contains (c->id) && c->start < to && clipEnd (*c) > from; });
}

// A new lane on top of a track's takes lanes (T3): the others move down a row. False when the track has as many as
// it can hold.
bool addTopLane (Track& track)
{
    if (track.lanes >= limits::maxLanes)
        return false;

    for (auto& clipPtr : track.clips)
    {
        auto clip = *clipPtr;
        ++clip.lane;
        clipPtr = std::make_shared<const NoteClip> (std::move (clip));
    }

    ++track.lanes;
    return true;
}

// Takes lanes left empty go, so a track keeps only the lanes that hold something (and always one).
void compactLanes (Track& track)
{
    std::vector<int> used (static_cast<size_t> (track.lanes), 0);

    for (const auto& clip : track.clips)
        used[static_cast<size_t> (std::clamp (clip->lane, 0, track.lanes - 1))] = 1;

    std::vector<int> renumbered (used.size(), 0);
    int lanes = 0;

    for (size_t i = 0; i < used.size(); ++i)
        renumbered[i] = used[i] != 0 ? lanes++ : lanes;

    if (lanes == track.lanes || (lanes == 0 && track.lanes == 1))
        return;

    for (auto& clipPtr : track.clips)
    {
        if (renumbered[static_cast<size_t> (clipPtr->lane)] == clipPtr->lane)
            continue;

        auto clip = *clipPtr;
        clip.lane = renumbered[static_cast<size_t> (clip.lane)];
        clipPtr = std::make_shared<const NoteClip> (std::move (clip));
    }

    track.lanes = std::max (1, lanes);

    if (track.lanes == 1)
        track.lanesExpanded = false;
}

// Where a placed clip goes among the takes lanes.
enum class LanePolicy
{
    stack,  // its own lane if that's free, else the first free lane from the top, else a new lane on top (T6)
    record, // the top lane if that's free, else a new lane on top (T3)
};

// Puts a clip on a track on top of the stack (T1), in a lane where it overlaps nothing (T2): covered clips stay as
// they are, underneath. Only when the track holds as many lanes as it can does the clip replace what it covers in
// its lane (A6).
void placeClip (Track& track, NoteClip clip, Project& result, std::set<ObjectId> keep = {},
                LanePolicy policy = LanePolicy::stack)
{
    keep.insert (clip.id);
    const auto from = clip.start;
    const auto to = clipEnd (clip);
    clip.lane = std::clamp (clip.lane, 0, track.lanes - 1);
    clip.order = topOrder (track);

    if (policy == LanePolicy::record)
        clip.lane = 0;

    if (! laneFree (track, clip.lane, from, to, keep))
    {
        std::optional<int> free;

        if (policy == LanePolicy::stack)
            for (int lane = 0; lane < track.lanes && ! free.has_value(); ++lane)
                if (laneFree (track, lane, from, to, keep))
                    free = lane;

        if (free.has_value())
            clip.lane = *free;
        else if (addTopLane (track))
            clip.lane = 0;
        else
            clearRange (track, from, to, result, keep, clip.lane);
    }

    track.clips.push_back (std::make_shared<const NoteClip> (std::move (clip)));
    sortClips (track.clips);
    compactLanes (track);
}

[[nodiscard]] std::vector<ObjectId> requireIds (const juce::var& args, const char* key)
{
    std::vector<ObjectId> ids;

    for (const auto& id : requireArray (args, key))
    {
        if (! isNumber (id))
            throw CommandError (juce::String ("'") + key + "' must hold ids");

        ids.push_back (static_cast<ObjectId> (id));
    }

    if (ids.empty())
        throw CommandError (juce::String ("'") + key + "' is empty");

    return ids;
}

// Applies an edit to working copies of all tracks and builds the resulting project.
template <typename EditFn>
[[nodiscard]] Project editTracks (const Project& project, EditFn&& edit)
{
    auto result = project;
    std::vector<Track> tracks;
    tracks.reserve (project.tracks.size());

    for (const auto& track : project.tracks)
        tracks.push_back (*track);

    std::forward<EditFn> (edit) (tracks, result);

    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].clips != project.tracks[i]->clips)
            result.tracks[i] = std::make_shared<const Track> (std::move (tracks[i]));

    return result;
}

struct FoundClip
{
    size_t track;
    NoteClip clip;
};

// Takes the clips with the given ids out of the working tracks (or copies them, leaving the originals).
[[nodiscard]] std::vector<FoundClip> takeClips (std::vector<Track>& tracks, const std::vector<ObjectId>& ids,
                                                bool remove)
{
    std::vector<FoundClip> found;

    for (const auto id : ids)
    {
        bool present = false;

        for (size_t t = 0; t < tracks.size() && ! present; ++t)
        {
            const auto index = tracks[t].indexOfClip (id);

            if (index < 0)
                continue;

            found.push_back ({ t, *tracks[t].clips[static_cast<size_t> (index)] });
            present = true;

            if (remove)
                tracks[t].clips.erase (tracks[t].clips.begin() + index);
        }

        if (! present)
            throw CommandError ("No clip with id " + juce::String (id));
    }

    return found;
}

[[nodiscard]] Project addClip (const Project& project, const juce::var& args)
{
    const auto start = requireTicks (args, "start", 0, limits::maxPosition);
    const auto length = requireTicks (args, "length", 1, limits::maxPosition);

    return updateTrack (
        project, requireId (args, "trackId"),
        [&] (Track& track, Project& result)
        {
            if (track.kind != TrackKind::instrument)
                throw CommandError ("Note clips belong on instrument tracks");

            NoteClip clip;
            clip.id = result.nextId++;
            clip.start = start;
            clip.length = length;
            clip.name = args["name"].toString();
            clip.colour = args["colour"].toString();
            clip.offset = requireTicksOr (args, "offset", 0);
            clip.loopStart = requireTicksOr (args, "loopStart", 0);
            clip.loopLength = requireTicksOr (args, "loopLength", 0);

            if (clip.loopLength != 0 && clip.loopLength < limits::minLoopLength)
                throw CommandError ("A clip loop must be at least a 1/16 note long");

            // A sequencer clip (docs/design/step-sequencer.md, SS2): no notes, a pattern and a start step.
            if (const auto& sequencer = args["sequencer"]; sequencer.isObject())
            {
                clip.sequencerPattern = std::clamp (static_cast<int> (sequencer.getProperty ("pattern", 0)), 0,
                                                    StepSequence::maxPatterns - 1);
                clip.sequencerStartStep = std::clamp (static_cast<int> (sequencer.getProperty ("startStep", 0)), 0,
                                                      StepSequence::maxSteps - 1);
                clip.loopLength = 0;
            }

            // Recorded controllers (docs/design/phase10-automation.md, A2).
            if (const auto* controllers = args["controllers"].getDynamicObject())
                for (const auto& property : controllers->getProperties())
                    clip.controllers[std::clamp (property.name.toString().getIntValue(), 0, controllerKeyPressure)] =
                        curveFromVar (property.value, 0.0, 1.0);

            if (args.hasProperty ("notes"))
            {
                for (const auto& fields : requireArray (args, "notes"))
                {
                    Note note;
                    note.id = result.nextId++;
                    applyNoteFields (fields, note, true);
                    clip.notes.push_back (std::make_shared<const Note> (std::move (note)));
                }

                sortNotes (clip.notes);
            }

            placeClip (track, std::move (clip), result, {},
                       static_cast<bool> (args["recorded"]) ? LanePolicy::record : LanePolicy::stack);
        });
}

[[nodiscard]] Project removeClip (const Project& project, const juce::var& args)
{
    const auto location = requireClip (project, requireId (args, "id"));
    return updateTrack (project, project.tracks[location.track]->id,
                        [&] (Track& track, Project&)
                        {
                            track.clips.erase (track.clips.begin() + static_cast<std::ptrdiff_t> (location.clip));
                            compactLanes (track);
                        });
}

[[nodiscard]] Project deleteClips (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "ids");
    return editTracks (project,
                       [&] (std::vector<Track>& tracks, Project&)
                       {
                           std::ignore = takeClips (tracks, ids, true);

                           for (auto& track : tracks)
                               compactLanes (track);
                       });
}

// Puts a clip, or the part of it between "from" and "to", on top of its track's stack, so it plays there
// (docs/design/takes-lanes.md, T6). A part is split off first; the rest of the clip stays where it was in the stack.
[[nodiscard]] Project bringToTop (const Project& project, const juce::var& args)
{
    const auto location = requireClip (project, requireId (args, "id"));
    const auto clip = *project.tracks[location.track]->clips[location.clip];
    const auto from = std::max (
        clip.start, args.hasProperty ("from") ? requireTicks (args, "from", 0, limits::maxPosition) : clip.start);
    const auto to = std::min (
        clipEnd (clip), args.hasProperty ("to") ? requireTicks (args, "to", 0, limits::maxPosition) : clipEnd (clip));

    if (from >= to)
        throw CommandError ("The range is outside the clip");

    return updateTrack (project, project.tracks[location.track]->id,
                        [&] (Track& track, Project& result)
                        {
                            const auto top = topOrder (track);
                            std::erase_if (track.clips, [&] (const ClipPtr& c) { return c->id == clip.id; });
                            auto rest = clip;
                            std::vector<NoteClip> parts;

                            if (from > rest.start)
                            {
                                auto [left, right] = splitClip (rest, from, result);
                                parts.push_back (std::move (left));
                                rest = std::move (right);
                            }

                            if (to < clipEnd (rest))
                            {
                                auto [middle, right] = splitClip (rest, to, result);
                                parts.push_back (std::move (right));
                                rest = std::move (middle);
                            }

                            rest.order = top;
                            parts.push_back (std::move (rest));

                            for (auto& part : parts)
                                track.clips.push_back (std::make_shared<const NoteClip> (std::move (part)));

                            sortClips (track.clips);
                        });
}

// Keeps what plays and drops what doesn't (T7): covered parts are cut away and every clip goes into one lane.
[[nodiscard]] Project flattenTakes (const Project& project, const juce::var& args)
{
    return updateTrack (project, requireId (args, "trackId"),
                        [&] (Track& track, Project& result)
                        {
                            // The played parts, by the clip they come from, in time order.
                            std::map<ObjectId, std::vector<std::pair<Ticks, Ticks>>> kept;

                            for (const auto& part : playedClips (track.clips))
                                kept[part->id].emplace_back (part->start, clipEnd (*part));

                            std::vector<ClipPtr> clips;

                            for (const auto& clipPtr : track.clips)
                            {
                                auto rest = *clipPtr;

                                for (const auto& [from, to] : kept[clipPtr->id])
                                {
                                    if (from > rest.start)
                                        rest = splitClip (rest, from, result).second;

                                    auto piece = rest;

                                    if (to < clipEnd (rest))
                                    {
                                        auto [left, right] = splitClip (rest, to, result);
                                        piece = std::move (left);
                                        rest = std::move (right);
                                    }

                                    piece.lane = 0;
                                    clips.push_back (std::make_shared<const NoteClip> (std::move (piece)));
                                }
                            }

                            track.clips = std::move (clips);
                            sortClips (track.clips);
                            track.lanes = 1;
                            track.lanesExpanded = false;
                        });
}

// Removes a takes lane and the clips in it (T5); the lanes below move up.
[[nodiscard]] Project removeLane (const Project& project, const juce::var& args)
{
    return updateTrack (project, requireId (args, "trackId"),
                        [&] (Track& track, Project&)
                        {
                            const auto lane = requireInt (args, "lane");

                            if (lane < 0 || lane >= track.lanes)
                                throw CommandError ("No such takes lane");

                            std::erase_if (track.clips, [&] (const ClipPtr& c) { return c->lane == lane; });

                            for (auto& clipPtr : track.clips)
                            {
                                if (clipPtr->lane < lane)
                                    continue;

                                auto moved = *clipPtr;
                                --moved.lane;
                                clipPtr = std::make_shared<const NoteClip> (std::move (moved));
                            }

                            track.lanes = std::max (1, track.lanes - 1);

                            if (track.lanes == 1)
                                track.lanesExpanded = false;
                        });
}

[[nodiscard]] Project setTrackLanesExpanded (const Project& project, const juce::var& args)
{
    return updateTrack (project, requireId (args, "id"), [&] (Track& track, Project&)
                        { track.lanesExpanded = static_cast<bool> (args["expanded"]) && track.lanes > 1; });
}

// The track a moved clip lands on: the track `trackIds` names for it, else `trackDelta` tracks on. The
// arrangement names tracks when groups lie between (docs/design/groups-and-sends.md).
[[nodiscard]] int targetTrackOf (const Project& project, const juce::var& args, int trackDelta, const FoundClip& m)
{
    if (const auto named = args["trackIds"][juce::Identifier (juce::String (m.clip.id))]; ! named.isVoid())
        return project.indexOfTrack (static_cast<ObjectId> (named));

    return static_cast<int> (m.track) + trackDelta;
}

// Checks every destination before anything moves: a clip can't leave the track list or change kind (A6).
void checkMoveTargets (const std::vector<Track>& tracks, const std::vector<FoundClip>& moved,
                       const std::vector<int>& targets, bool linked)
{
    for (size_t i = 0; i < moved.size(); ++i)
    {
        const auto target = targets[i];

        if (target < 0 || target >= static_cast<int> (tracks.size()))
            throw CommandError ("There is no track to move the clips to");

        if (tracks[static_cast<size_t> (target)].kind != tracks[moved[i].track].kind)
            throw CommandError ("Audio and note clips can't move between track kinds");

        if (linked && moved[i].clip.fileId != 0)
            throw CommandError ("Audio clips can't be linked");
    }
}

// Linked copies share the original's link (a new one if it had none).
void linkMovedClips (std::vector<Track>& tracks, std::vector<FoundClip>& moved, Project& result)
{
    for (auto& m : moved)
    {
        if (m.clip.linkId != 0)
            continue;

        m.clip.linkId = result.nextId++;
        setLinkId (tracks[m.track], m.clip.id, m.clip.linkId);
    }
}

// Puts each moved clip on its target track, replacing what it covers (A6).
void placeMovedClips (std::vector<Track>& tracks, std::vector<FoundClip>& moved, const std::vector<int>& targets,
                      Project& result, Ticks delta, bool copy, bool linked)
{
    std::set<ObjectId> placed;

    for (size_t i = 0; i < moved.size(); ++i)
    {
        auto clip = takeOrCopy (moved[i].clip, copy, linked, result);
        clip.start = std::min (clip.start + delta, limits::maxPosition);
        placed.insert (clip.id);
        placeClip (tracks[static_cast<size_t> (targets[i])], std::move (clip), result, placed);
    }
}

// The body of moveClips, on the working copies of the tracks (see editTracks).
void moveClipBatch (std::vector<Track>& tracks, Project& result, const Project& project, const juce::var& args,
                    const std::vector<ObjectId>& ids, Ticks delta, int trackDelta, bool copy, bool linked)
{
    auto moved = takeClips (tracks, ids, ! copy);
    std::vector<int> targets;
    targets.reserve (moved.size());
    auto earliest = limits::maxPosition;

    for (const auto& m : moved)
    {
        earliest = std::min (earliest, m.clip.start);
        targets.push_back (targetTrackOf (project, args, trackDelta, m));
    }

    checkMoveTargets (tracks, moved, targets, linked);
    delta = std::max (delta, -earliest); // nothing moves before the start

    if (linked)
        linkMovedClips (tracks, moved, result);

    placeMovedClips (tracks, moved, targets, result, delta, copy, linked);
}

// Moves (or, with copy, duplicates) clips by a time offset and a number of tracks, keeping their relative
// positions. Clips land replacing what they cover (A6).
[[nodiscard]] Project moveClips (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "ids");
    const auto delta = static_cast<Ticks> (std::llround (requireNumber (args, "delta")));
    const auto trackDelta = static_cast<int> (std::lround (numberOr (args, "trackDelta", 0)));
    const auto copy = static_cast<bool> (args["copy"]) || static_cast<bool> (args["linked"]);
    const auto linked = static_cast<bool> (args["linked"]);

    return editTracks (project, [&] (std::vector<Track>& tracks, Project& result)
                       { moveClipBatch (tracks, result, project, args, ids, delta, trackDelta, copy, linked); });
}

// Legacy single-clip move, kept for existing callers.
[[nodiscard]] Project moveClip (const Project& project, const juce::var& args)
{
    const auto clipId = requireId (args, "id");
    const auto start = requireTicks (args, "start", 0, limits::maxPosition);
    const auto location = requireClip (project, clipId);
    const auto& clip = *project.tracks[location.track]->clips[location.clip];
    auto trackDelta = 0;

    if (args.hasProperty ("trackId"))
    {
        const auto target = project.indexOfTrack (requireId (args, "trackId"));

        if (target < 0)
            throw CommandError ("No track with id " + args["trackId"].toString());

        trackDelta = target - static_cast<int> (location.track);
    }

    return moveClips (project, makeObject ({ { "ids", juce::Array<juce::var> { clipId } },
                                             { "delta", start - clip.start },
                                             { "trackDelta", trackDelta } }));
}

// Moves one edge of a clip. The start edge keeps the content in place (A2). The legacy form { id, length }
// moves the end edge.
[[nodiscard]] Project resizeClip (const Project& project, const juce::var& args)
{
    const auto clipId = requireId (args, "id");
    const auto location = requireClip (project, clipId);
    auto clip = *project.tracks[location.track]->clips[location.clip];
    const auto end = clipEnd (clip);
    const auto edge = args.hasProperty ("length") ? juce::String ("end") : args["edge"].toString();

    if (edge == "end")
    {
        const auto to = args.hasProperty ("length") ? clip.start + requireTicks (args, "length", 1, limits::maxPosition)
                                                    : requireTicks (args, "to", 0, limits::maxPosition);

        if (to <= clip.start)
            throw CommandError ("A clip must be longer than zero");

        clip.length = to - clip.start;
    }
    else if (edge == "start")
    {
        auto to = requireTicks (args, "to", 0, limits::maxPosition);

        if (to >= end)
            throw CommandError ("A clip must be longer than zero");

        // Without a loop, the content can't be revealed before its beginning.
        if (clip.loopLength <= 0)
            to = std::max (to, clip.start - clip.offset);

        trimStart (clip, to);
    }
    else
    {
        throw CommandError ("'edge' must be \"start\" or \"end\"");
    }

    return updateTrack (project, project.tracks[location.track]->id,
                        [&] (Track& track, Project& result)
                        {
                            std::erase_if (track.clips, [&] (const ClipPtr& c) { return c->id == clipId; });
                            placeClip (track, std::move (clip), result);
                        });
}

// Duplicates the selection right after itself, on the same tracks.
[[nodiscard]] Project duplicateClips (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "ids");
    Ticks from = limits::maxPosition;
    Ticks to = 0;

    for (const auto id : ids)
    {
        const auto location = requireClip (project, id);
        const auto& clip = *project.tracks[location.track]->clips[location.clip];
        from = std::min (from, clip.start);
        to = std::max (to, clipEnd (clip));
    }

    return moveClips (project, makeObject ({ { "ids", args["ids"] },
                                             { "delta", to - from },
                                             { "copy", true },
                                             { "linked", static_cast<bool> (args["linked"]) } }));
}

[[nodiscard]] Project splitClips (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "ids");
    const auto at = requireTicks (args, "at", 0, limits::maxPosition);

    return editTracks (project,
                       [&] (std::vector<Track>& tracks, Project& result)
                       {
                           for (auto& m : takeClips (tracks, ids, true))
                           {
                               auto& clips = tracks[m.track].clips;

                               if (at <= m.clip.start || at >= clipEnd (m.clip))
                               {
                                   clips.push_back (std::make_shared<const NoteClip> (std::move (m.clip)));
                               }
                               else
                               {
                                   auto [left, right] = splitClip (m.clip, at, result);
                                   clips.push_back (std::make_shared<const NoteClip> (std::move (left)));
                                   clips.push_back (std::make_shared<const NoteClip> (std::move (right)));
                               }

                               sortClips (clips);
                           }
                       });
}

[[nodiscard]] Project setClipLoop (const Project& project, const juce::var& args)
{
    return updateClip (project, requireId (args, "id"),
                       [&] (NoteClip& clip, Project&)
                       {
                           const auto length = requireTicks (args, "loopLength", 0, limits::maxPosition);

                           if (length != 0 && length < limits::minLoopLength)
                               throw CommandError ("A clip loop must be at least a 1/16 note long");

                           clip.loopLength = length;
                           clip.loopStart = args.hasProperty ("loopStart")
                                                ? requireTicks (args, "loopStart", 0, limits::maxPosition)
                                                : clip.offset;

                           if (length > 0)
                               clip.offset = wrapIntoLoop (clip, clip.offset);
                       });
}

[[nodiscard]] Project setClipProps (const Project& project, const juce::var& args)
{
    return updateClip (
        project, requireId (args, "id"),
        [&] (NoteClip& clip, Project&)
        {
            if (args.hasProperty ("name"))
                clip.name = args["name"].toString().trim();

            if (args.hasProperty ("colour"))
                clip.colour = args["colour"].toString().trim();

            if (clip.fileId == 0)
                return;

            if (args.hasProperty ("gainDb"))
                clip.gainDb = std::clamp (requireNumber (args, "gainDb"), limits::minGainDb, limits::maxGainDb);

            // Fades stay inside the clip and never overlap each other.
            if (args.hasProperty ("fadeIn"))
                clip.fadeIn = std::clamp (requireTicks (args, "fadeIn", 0, limits::maxPosition), Ticks { 0 },
                                          clip.length - clip.fadeOut);

            if (args.hasProperty ("fadeOut"))
                clip.fadeOut = std::clamp (requireTicks (args, "fadeOut", 0, limits::maxPosition), Ticks { 0 },
                                           clip.length - clip.fadeIn);

            if (args.hasProperty ("fadeInShape"))
                clip.fadeInShape = static_cast<FadeShape> (std::clamp (requireInt (args, "fadeInShape"), 0, 3));

            if (args.hasProperty ("fadeOutShape"))
                clip.fadeOutShape = static_cast<FadeShape> (std::clamp (requireInt (args, "fadeOutShape"), 0, 3));
        });
}

// Merges clips on one track into a single clip without loop or offset; what was audible stays audible.
[[nodiscard]] Project glueClips (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "ids");

    return editTracks (
        project,
        [&] (std::vector<Track>& tracks, Project& result)
        {
            const auto parts = takeClips (tracks, ids, true);

            if (std::ranges::any_of (parts, [] (const FoundClip& p) { return p.clip.fileId != 0; }))
                throw CommandError ("Audio clips can't be glued yet");

            if (std::ranges::any_of (parts, [&] (const FoundClip& p) { return p.track != parts[0].track; }))
                throw CommandError ("Only clips on one track can be glued");

            NoteClip glued;
            glued.id = result.nextId++;
            glued.start = limits::maxPosition;
            Ticks end = 0;

            for (const auto& part : parts)
            {
                glued.start = std::min (glued.start, part.clip.start);
                end = std::max (end, clipEnd (part.clip));
            }

            glued.length = end - glued.start;
            glued.name = parts[0].clip.name;
            glued.colour = parts[0].clip.colour;

            for (const auto& part : parts)
            {
                forEachAudibleNote (part.clip,
                                    [&] (const Note& note, Ticks noteStart, Ticks noteEnd, int)
                                    {
                                        auto copy = note;
                                        copy.id = result.nextId++;
                                        copy.link.reset();
                                        copy.start = noteStart - glued.start;
                                        copy.length = noteEnd - noteStart;
                                        glued.notes.push_back (std::make_shared<const Note> (std::move (copy)));
                                    });
            }

            sortNotes (glued.notes);
            placeClip (tracks[parts[0].track], std::move (glued), result);
        });
}

// ---------------------------------------------------------------------------------------------------------
// Markers and loop range (A8)

void sortMarkers (std::vector<Marker>& markers)
{
    std::ranges::sort (markers, [] (const Marker& a, const Marker& b)
                       { return a.position != b.position ? a.position < b.position : a.id < b.id; });
}

[[nodiscard]] Marker& requireMarker (Project& project, ObjectId id)
{
    const auto it = std::ranges::find_if (project.markers, [id] (const Marker& m) { return m.id == id; });

    if (it == project.markers.end())
        throw CommandError ("No marker with id " + juce::String (id));

    return *it;
}

[[nodiscard]] Project addMarker (const Project& project, const juce::var& args)
{
    auto result = project;
    Marker marker;
    marker.id = result.nextId++;
    marker.position = requireTicks (args, "position", 0, limits::maxPosition);
    marker.name = args["name"].toString().trim();

    if (marker.name.isEmpty())
        marker.name = "Marker " + juce::String (project.markers.size() + 1);

    result.markers.push_back (marker);
    sortMarkers (result.markers);
    return result;
}

[[nodiscard]] Project moveMarker (const Project& project, const juce::var& args)
{
    auto result = project;
    requireMarker (result, requireId (args, "id")).position = requireTicks (args, "position", 0, limits::maxPosition);
    sortMarkers (result.markers);
    return result;
}

[[nodiscard]] Project renameMarker (const Project& project, const juce::var& args)
{
    auto result = project;
    const auto name = args["name"].toString().trim();

    if (name.isEmpty())
        throw CommandError ("A marker needs a name");

    requireMarker (result, requireId (args, "id")).name = name;
    return result;
}

[[nodiscard]] Project removeMarker (const Project& project, const juce::var& args)
{
    auto result = project;
    const auto id = requireId (args, "id");
    std::ignore = requireMarker (result, id);
    std::erase_if (result.markers, [id] (const Marker& m) { return m.id == id; });
    return result;
}

[[nodiscard]] Project setLoop (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& loop = result.loop;

    if (args.hasProperty ("enabled"))
        loop.enabled = static_cast<bool> (args["enabled"]);

    if (args.hasProperty ("start"))
        loop.start = requireTicks (args, "start", 0, limits::maxPosition);

    if (args.hasProperty ("end"))
        loop.end = requireTicks (args, "end", 1, limits::maxPosition);

    if (loop.end - loop.start < limits::minLoopLength)
        throw CommandError ("The loop range must be at least a 1/16 note long");

    return result;
}

// The punch range (docs/design/audio-features.md, AU10), as setLoop for the loop range.
[[nodiscard]] Project setPunch (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& punch = result.punch;

    if (args.hasProperty ("enabled"))
        punch.enabled = static_cast<bool> (args["enabled"]);

    if (args.hasProperty ("start"))
        punch.start = requireTicks (args, "start", 0, limits::maxPosition);

    if (args.hasProperty ("end"))
        punch.end = requireTicks (args, "end", 1, limits::maxPosition);

    if (punch.end - punch.start < limits::minLoopLength)
        throw CommandError ("The punch range must be at least a 1/16 note long");

    return result;
}

// A link with a single member left is no link: clears that member's link id.
void dissolveSingleLink (std::vector<Track>& tracks, ObjectId link)
{
    std::vector<std::pair<Track*, ObjectId>> members;

    for (auto& track : tracks)
        for (const auto& clip : track.clips)
            if (clip->linkId == link)
                members.emplace_back (&track, clip->id);

    if (members.size() == 1)
        setLinkId (*members[0].first, members[0].second, 0);
}

// Finds a file in the audio pool by path, or adds it. Returns its id.
ObjectId poolAudioFile (Project& result, const juce::var& f)
{
    auto file = audioFile (0, f["path"].toString(), f["name"].toString(), requireNumber (f, "sampleRate"),
                           requireInt (f, "channels"), static_cast<juce::int64> (requireNumber (f, "frames")));

    if (file.path.isEmpty() || file.sampleRate <= 0 || file.channels < 1 || file.frames < 1)
        throw CommandError ("Invalid audio file");

    const auto existing =
        std::ranges::find_if (result.audioFiles, [&] (const AudioFile& a) { return a.path == file.path; });

    if (existing != result.audioFiles.end())
        return existing->id;

    file.id = result.nextId++;
    result.audioFiles.push_back (file);
    return file.id;
}

// Places an audio clip, adding the file to the pool first if it isn't there (by path).
[[nodiscard]] Project addAudioClip (const Project& project, const juce::var& args)
{
    const auto start = requireTicks (args, "start", 0, limits::maxPosition);
    auto result = project;
    ObjectId fileId = 0;

    if (args.hasProperty ("fileId"))
    {
        fileId = requireId (args, "fileId");

        if (project.findAudioFile (fileId) == nullptr)
            throw CommandError ("No audio file with id " + juce::String (fileId));
    }
    else
    {
        fileId = poolAudioFile (result, args["file"]);
    }

    const auto& file = *result.findAudioFile (fileId);
    const auto seconds = static_cast<double> (file.frames) / file.sampleRate;
    // Copied because `result` is replaced below, which invalidates `file` (it points into result.audioFiles).
    const auto fileName = file.name;
    auto trackId = args.hasProperty ("trackId") ? requireId (args, "trackId") : ObjectId { 0 };

    // Dropping a file below the last track: a new audio track named after it. addTrack puts it before the
    // returns (R10), so remember where it lands instead of assuming it is last.
    if (static_cast<bool> (args["newTrack"]))
    {
        const auto index = endOfGroup (result, 0);
        result = addTrack (result, makeObject ({ { "kind", "audio" }, { "name", fileName } }));
        trackId = result.tracks[index]->id;
    }
    // A new pool file gets a source tempo suggestion; recordings need none, their tempo is the map's
    // (docs/design/time-stretching.md, TS11).
    if (! args.hasProperty ("fileId") && ! static_cast<bool> (args["recorded"]))
    {
        auto& pooled =
            *std::ranges::find_if (result.audioFiles, [fileId] (const AudioFile& f) { return f.id == fileId; });

        if (pooled.suggestedBpm <= 0.0)
        {
            const auto& map = result.tempoMap;
            const auto& signature = map.signatureAt (static_cast<double> (start));
            const auto quarters = static_cast<double> (signature.barTicks()) / static_cast<double> (ticksPerBeat);
            pooled.suggestedBpm =
                suggestTempo (static_cast<double> (args["file"].getProperty ("metadataBpm", 0.0)),
                              juce::File::createFileWithoutCheckingPath (pooled.path).getFileNameWithoutExtension()
                                  + " " + pooled.name,
                              seconds, quarters, map.bpmAt (static_cast<double> (start)));
        }
    }

    const auto& placedFile = *result.findAudioFile (fileId);
    const auto musical = placedFile.timing.musical();
    // The file's length in content ticks, through its timing map (docs/design/time-stretching.md, TS2): its content
    // seconds at the tempo where the clip starts for one-shot files (docs/design/tempo-map.md, TM8).
    const auto content =
        TimingMap (placedFile.timing, placedFile.sampleRate).tickAt (static_cast<double> (placedFile.frames));
    const auto length =
        std::max (Ticks { 1 },
                  musical ? static_cast<Ticks> (std::ceil (content))
                          : static_cast<Ticks> (std::llround (
                                content * result.tempoMap.bpmAt (static_cast<double> (start)) / 60.0 * ticksPerBeat)));

    return updateTrack (result, trackId,
                        [&] (Track& track, Project& r)
                        {
                            if (track.kind != TrackKind::audio)
                                throw CommandError ("Audio clips belong on audio tracks");

                            NoteClip clip;
                            clip.id = r.nextId++;
                            clip.start = start;
                            clip.offset =
                                args.hasProperty ("offset") ? requireTicks (args, "offset", 0, length - 1) : 0;
                            if (args.hasProperty ("length"))
                                clip.length = requireTicks (args, "length", 1, length);
                            else if (musical)
                                clip.length = length - clip.offset;
                            else
                                clip.length = timelineLength (r.tempoMap, start,
                                                              contentSeconds (r.tempoMap, start, length - clip.offset));
                            clip.fileId = fileId;
                            clip.name = placedFile.name;

                            // A recording goes into the top lane if it's free there, else a new lane on top
                            // (docs/design/takes-lanes.md, T3).
                            placeClip (track, std::move (clip), r, {},
                                       static_cast<bool> (args["recorded"]) ? LanePolicy::record : LanePolicy::stack);
                        });
}

[[nodiscard]] MidiMapping parseMidiTarget (const juce::var& args)
{
    MidiMapping mapping;
    mapping.trackId = requireId (args, "trackId");
    mapping.nodeId = args.hasProperty ("nodeId") ? static_cast<ObjectId> (args["nodeId"]) : 0;
    mapping.param = args["param"].toString();

    if (mapping.param.isEmpty())
        throw CommandError ("Missing param");

    return mapping;
}

[[nodiscard]] bool sameTarget (const MidiMapping& a, const MidiMapping& b) noexcept
{
    return a.trackId == b.trackId && a.nodeId == b.nodeId && a.param == b.param;
}

[[nodiscard]] Project setMidiMapping (const Project& project, const juce::var& args)
{
    auto mapping = parseMidiTarget (args);
    mapping.device = args["device"].toString();
    mapping.channel = std::clamp (requireInt (args, "channel"), 1, 16);
    mapping.controller = std::clamp (requireInt (args, "controller"), 0, 127);

    const auto* track = project.findTrackOrMaster (mapping.trackId);

    if (track == nullptr)
        throw CommandError ("No track with id " + juce::String (mapping.trackId));

    if (mapping.nodeId == 0 ? mapping.param != "gain" && mapping.param != "pan"
                            : track->findNode (mapping.nodeId) == nullptr
                                  || ! paramOf (*track->findNode (mapping.nodeId), mapping.param).has_value())
        throw CommandError ("Unknown parameter '" + mapping.param + "'");

    // One controller per parameter, one parameter per controller.
    auto result = project;
    std::erase_if (result.midiMappings,
                   [&] (const MidiMapping& m)
                   {
                       return sameTarget (m, mapping)
                              || (m.device == mapping.device && m.channel == mapping.channel
                                  && m.controller == mapping.controller);
                   });
    result.midiMappings.push_back (mapping);
    return result;
}

[[nodiscard]] Project removeMidiMapping (const Project& project, const juce::var& args)
{
    const auto target = parseMidiTarget (args);
    auto result = project;

    if (std::erase_if (result.midiMappings, [&] (const MidiMapping& m) { return sameTarget (m, target); }) == 0)
        throw CommandError ("This parameter has no MIDI mapping");

    return result;
}

[[nodiscard]] Project relinkAudioFile (const Project& project, const juce::var& args)
{
    auto result = project;
    const auto id = requireId (args, "id");
    const auto it = std::ranges::find_if (result.audioFiles, [id] (const AudioFile& f) { return f.id == id; });

    if (it == result.audioFiles.end())
        throw CommandError ("No audio file with id " + juce::String (id));

    it->path = args["path"].toString();
    return result;
}

[[nodiscard]] Project setTrackSwitches (const Project& project, const juce::var& args)
{
    return updateTrack (project, requireId (args, "id"),
                        [&] (Track& track, Project&)
                        {
                            if (args.hasProperty ("mute"))
                                track.mute = static_cast<bool> (args["mute"]);

                            if (args.hasProperty ("solo"))
                                track.solo = static_cast<bool> (args["solo"]);

                            if (args.hasProperty ("monitor"))
                                track.monitor =
                                    static_cast<MonitorMode> (std::clamp (requireInt (args, "monitor"), 0, 2));

                            if (args.hasProperty ("inputChannel"))
                                track.inputChannel = std::max (-1, requireInt (args, "inputChannel"));

                            if (args.hasProperty ("inputStereo"))
                                track.inputStereo = static_cast<bool> (args["inputStereo"]);
                        });
}

// Gives the selected linked clips their own copies of the notes.
[[nodiscard]] Project makeUnique (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "ids");

    return editTracks (project,
                       [&] (std::vector<Track>& tracks, Project& result)
                       {
                           std::set<ObjectId> touchedLinks;

                           for (auto& track : tracks)
                           {
                               for (auto& clipPtr : track.clips)
                               {
                                   if (std::ranges::find (ids, clipPtr->id) == ids.end() || clipPtr->linkId == 0)
                                       continue;

                                   touchedLinks.insert (clipPtr->linkId);
                                   auto unique = copyWithNewIds (*clipPtr, result);
                                   unique.id = clipPtr->id;
                                   clipPtr = std::make_shared<const NoteClip> (std::move (unique));
                               }
                           }

                           for (const auto link : touchedLinks)
                               dissolveSingleLink (tracks, link);
                       });
}

[[nodiscard]] Project setTrackColour (const Project& project, const juce::var& args)
{
    const auto colour = args["colour"].toString().trim();

    if (! colour.startsWith ("#") || colour.length() != 7)
        throw CommandError ("A colour must look like #rrggbb");

    return updateTrack (project, requireId (args, "id"), [&] (Track& track, Project&) { track.colour = colour; });
}

[[nodiscard]] Project setTrackHeight (const Project& project, const juce::var& args)
{
    const auto height = std::clamp (requireInt (args, "height"), limits::minTrackHeight, limits::maxTrackHeight);
    return updateTrack (project, requireId (args, "id"), [&] (Track& track, Project&) { track.height = height; });
}

// ---------------------------------------------------------------------------------------------------------
// Note commands

[[nodiscard]] Project addNotes (const Project& project, const juce::var& args)
{
    const auto& notes = requireArray (args, "notes");

    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project& result)
                       {
                           std::vector<Note> added;

                           for (const auto& fields : notes)
                           {
                               Note note;
                               note.id = result.nextId++;
                               applyNoteFields (fields, note, true);
                               added.push_back (std::move (note));
                           }

                           // Pasted notes link to each other by their place in this call, having no ids yet. A
                           // target is one note's only (the UI's copies are), and must start later.
                           std::set<int> targets;

                           for (int i = 0; i < notes.size(); ++i)
                           {
                               const auto& link = notes.getReference (i)["link"];

                               if (! link.isObject())
                                   continue;

                               const auto to = requireInt (link, "toIndex");
                               auto& from = added[static_cast<size_t> (i)];

                               if (to >= 0 && to < notes.size() && targets.insert (to).second
                                   && added[static_cast<size_t> (to)].start > from.start)
                                   from.link = NoteLink { added[static_cast<size_t> (to)].id,
                                                          std::clamp (requireNumber (link, "glideMs"), 0.0, 10000.0) };
                           }

                           for (auto& note : added)
                               clip.notes.push_back (std::make_shared<const Note> (std::move (note)));

                           sortNotes (clip.notes);
                       });
}

[[nodiscard]] Project updateNotes (const Project& project, const juce::var& args)
{
    const auto& notes = requireArray (args, "notes");

    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project&)
                       {
                           for (const auto& fields : notes)
                           {
                               const auto noteId = requireId (fields, "id");
                               const auto index = clip.indexOfNote (noteId);

                               if (index < 0)
                                   throw CommandError ("No note with id " + juce::String (noteId));

                               auto note = *clip.notes[static_cast<size_t> (index)];
                               applyNoteFields (fields, note, false);
                               clip.notes[static_cast<size_t> (index)] =
                                   std::make_shared<const Note> (std::move (note));
                           }

                           sortNotes (clip.notes);
                       });
}

[[nodiscard]] Project removeNotes (const Project& project, const juce::var& args)
{
    const auto& ids = requireArray (args, "ids");
    std::set<ObjectId> toRemove;

    for (const auto& id : ids)
    {
        if (! isNumber (id))
            throw CommandError ("Note ids must be numbers");

        toRemove.insert (static_cast<ObjectId> (static_cast<double> (id)));
    }

    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project&)
                       {
                           const auto before = clip.notes.size();
                           std::erase_if (clip.notes, [&] (const NotePtr& n) { return toRemove.contains (n->id); });

                           if (before - clip.notes.size() != toRemove.size())
                               throw CommandError ("Some notes to remove do not exist in the clip");

                           for (auto& note : clip.notes)
                           {
                               if (note->link.has_value() && toRemove.contains (note->link->to))
                               {
                                   auto unlinked = *note;
                                   unlinked.link.reset();
                                   note = std::make_shared<const Note> (std::move (unlinked));
                               }
                           }
                       });
}

// Links and unlinks notes (docs/design/linked-notes.md). A note links to one note and is linked from one: what a new link
// takes over from an older link, that link gives up.
[[nodiscard]] Project setNoteLinks (const Project& project, const juce::var& args)
{
    const auto& links = requireArray (args, "links");

    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project&)
                       {
                           const auto noteAt = [&clip] (ObjectId id) -> NotePtr&
                           {
                               const auto index = clip.indexOfNote (id);

                               if (index < 0)
                                   throw CommandError ("No note with id " + juce::String (id));

                               return clip.notes[static_cast<size_t> (index)];
                           };

                           for (const auto& fields : links)
                           {
                               const auto from = requireId (fields, "from");
                               auto source = *noteAt (from);

                               if (! fields.hasProperty ("to") || fields["to"].isVoid())
                               {
                                   source.link.reset();
                                   noteAt (from) = std::make_shared<const Note> (std::move (source));
                                   continue;
                               }

                               const auto to = requireId (fields, "to");

                               if (noteAt (to)->start <= source.start)
                                   throw CommandError ("A note can only link to a note that starts later");

                               auto link = source.link.value_or (NoteLink {});
                               link.to = to;

                               if (fields.hasProperty ("glideMs"))
                                   link.glideMs = std::clamp (requireNumber (fields, "glideMs"), 0.0, 10000.0);

                               for (auto& note : clip.notes)
                               {
                                   if (note->id != from && note->link.has_value() && note->link->to == to)
                                   {
                                       auto other = *note;
                                       other.link.reset();
                                       note = std::make_shared<const Note> (std::move (other));
                                   }
                               }

                               source.link = link;
                               noteAt (from) = std::make_shared<const Note> (std::move (source));
                           }
                       });
}

// Splits notes at a clip position strictly inside them. The left part keeps the note's id; the right part is a
// new note with the same pitch and velocities, and the rest of every curve and of the vibrato.
[[nodiscard]] Project splitNotes (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "ids");
    const auto at = requireTicks (args, "at", 0, limits::maxPosition);

    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project& result)
                       {
                           std::vector<NotePtr> added;

                           for (const auto noteId : ids)
                           {
                               const auto index = clip.indexOfNote (noteId);

                               if (index < 0)
                                   throw CommandError ("No note with id " + juce::String (noteId));

                               const auto& note = *clip.notes[static_cast<size_t> (index)];
                               const auto offset = at - note.start;

                               if (offset <= 0 || offset >= note.length)
                                   continue;

                               auto left = note;
                               auto right = note;
                               left.length = offset;
                               left.link.reset(); // the right half ends where the note ended, so it hands on the voice
                               right.id = result.nextId++;
                               right.start = at;
                               right.length = note.length - offset;

                               for (size_t lane = 0; lane < numLanes; ++lane)
                                   std::tie (left.expression[lane], right.expression[lane]) =
                                       splitCurve (note.expression[lane], offset, laneInfos()[lane].defaultValue);

                               // The vibrato carries on where it was: what is left of its delay and fade-in.
                               const auto faded = std::max (Ticks { 0 }, offset - note.vibrato.delay);
                               right.vibrato.delay = std::max (Ticks { 0 }, note.vibrato.delay - offset);
                               right.vibrato.fadeIn = std::max (Ticks { 0 }, note.vibrato.fadeIn - faded);

                               anchorExpression (left);
                               anchorExpression (right);
                               clip.notes[static_cast<size_t> (index)] =
                                   std::make_shared<const Note> (std::move (left));
                               added.push_back (std::make_shared<const Note> (std::move (right)));
                           }

                           if (added.empty())
                               throw CommandError ("The split position is not inside the notes");

                           clip.notes.insert (clip.notes.end(), added.begin(), added.end());
                           sortNotes (clip.notes);
                       });
}

[[nodiscard]] Project setCurve (const Project& project, const juce::var& args)
{
    Lane lane {};

    if (! laneFromId (args["lane"].toString(), lane))
        throw CommandError ("Unknown expression lane '" + args["lane"].toString() + "'");

    const auto curve = parseCurve (args["points"], lane);
    const auto noteId = requireId (args, "noteId");

    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project&)
                       {
                           const auto index = clip.indexOfNote (noteId);

                           if (index < 0)
                               throw CommandError ("No note with id " + juce::String (noteId));

                           auto note = *clip.notes[static_cast<size_t> (index)];
                           note.expression[static_cast<size_t> (lane)] = curve;
                           anchorExpression (note);
                           clip.notes[static_cast<size_t> (index)] = std::make_shared<const Note> (std::move (note));
                       });
}

// ---------------------------------------------------------------------------------------------------------
// Graph commands

[[nodiscard]] PluginRef parsePluginRef (const juce::var& value)
{
    PluginRef ref;
    ref.identifier = value["identifier"].toString();
    ref.name = value["name"].toString();
    ref.manufacturer = value["manufacturer"].toString();
    ref.format = value["format"].toString();

    if (ref.identifier.isEmpty())
        throw CommandError ("Missing plugin identifier");

    if (ref.name.isEmpty())
        ref.name = ref.identifier;

    return ref;
}

void validateConnection (const TrackGraph& graph, const Connection& connection, const ComponentGraph* owner = nullptr);

// The graph a graph command edits (docs/design/phase12-voice-scope.md, section 6; component-nodes.md, N7): the track's, or, when the
// command names a "voiceNodeId", what that Voice Instrument or component holds, at any depth.
struct GraphScope
{
    TrackGraph& graph;
    Track& track;
    bool inVoice = false;
    const ComponentGraph* component = nullptr;
    ObjectId owner = 0;

    [[nodiscard]] bool voice() const noexcept { return inVoice; }
    [[nodiscard]] bool nested() const noexcept { return owner != 0; }
};

// Runs `update (scope, result)` on the graph the command's "trackId" and optional "voiceNodeId" name.
// Without a "voiceNodeId", the node a command names decides: one inside a voice graph or component edits that graph, so MIDI
// learn, automation writing and the UI's controls work on those nodes as on track nodes (node ids are unique).
[[nodiscard]] ObjectId graphScopeOf (const Track& track, const juce::var& args)
{
    if (args.hasProperty ("voiceNodeId"))
        return requireId (args, "voiceNodeId");

    for (const auto* key : { "nodeId", "fromNode" })
        if (args.hasProperty (key))
            return track.graphOwnerOf (static_cast<ObjectId> (static_cast<juce::int64> (args[key])));

    if (const auto* ids = args["nodeIds"].getArray(); ids != nullptr && ! ids->isEmpty())
        return track.graphOwnerOf (static_cast<ObjectId> (static_cast<juce::int64> (ids->getFirst())));

    return 0;
}

// Finds the node holding the graph to edit, copying what leads to it, and calls `edit (graph, inVoice, component)` on that graph.
template <typename EditFn>
[[nodiscard]] bool editOwnedGraph (TrackGraph& graph, ObjectId owner, bool inVoice, EditFn& edit)
{
    for (auto& node : graph.nodes)
    {
        if (node.voiceGraph != nullptr && idsWithin (node).contains (owner))
        {
            auto inner = *node.voiceGraph;

            if (node.id == owner)
                edit (inner, true, static_cast<const ComponentGraph*> (nullptr));
            else if (! editOwnedGraph (inner, owner, true, edit))
                continue;

            node.voiceGraph = std::make_shared<const TrackGraph> (std::move (inner));
            return true;
        }

        if (node.component != nullptr && idsWithin (node).contains (owner))
        {
            auto inner = *node.component;

            if (node.id == owner)
                edit (inner.graph, inVoice, static_cast<const ComponentGraph*> (&inner));
            else if (! editOwnedGraph (inner.graph, owner, inVoice, edit))
                continue;

            node.component = std::make_shared<const ComponentGraph> (std::move (inner));
            return true;
        }
    }

    return false;
}

// Forgets what's shown on components for nodes no longer in them.
void pruneExposedParams (TrackGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        if (node.voiceGraph != nullptr)
        {
            auto voice = *node.voiceGraph;
            pruneExposedParams (voice);
            node.voiceGraph = std::make_shared<const TrackGraph> (std::move (voice));
        }

        if (node.component == nullptr)
            continue;

        auto inside = *node.component;
        pruneExposedParams (inside.graph);
        std::set<ObjectId> within;
        forEachNode (inside.graph, [&within] (const GraphNode& inner, bool) { within.insert (inner.id); });
        std::erase_if (inside.exposed,
                       [&within] (const ExposedParam& param) { return ! within.contains (param.nodeId); });
        node.component = std::make_shared<const ComponentGraph> (std::move (inside));
    }
}

// Drops the macro slots of a graph and what it holds, once the track has no Macros device to feed them
// (docs/design/macros-and-modulation-column.md, MC7).
void dropMacroSlots (TrackGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        std::erase_if (node.modulations, [] (const auto& entry) { return entry.second.macro >= 0; });

        if (node.voiceGraph != nullptr)
        {
            auto voice = *node.voiceGraph;
            dropMacroSlots (voice);
            node.voiceGraph = std::make_shared<const TrackGraph> (std::move (voice));
        }

        if (node.component != nullptr)
        {
            auto inside = *node.component;
            dropMacroSlots (inside.graph);
            node.component = std::make_shared<const ComponentGraph> (std::move (inside));
        }
    }
}

// Whether any node of a graph, at any depth, has a macro slot.
[[nodiscard]] bool hasMacroSlots (const TrackGraph& graph)
{
    auto any = false;
    forEachNode (graph,
                 [&any] (const GraphNode& node, bool)
                 {
                     any = any
                           || std::ranges::any_of (node.modulations,
                                                   [] (const auto& entry) { return entry.second.macro >= 0; });
                 });
    return any;
}

// After an edit: without a Macros device, no macro slots.
void pruneMacroSlots (Track& track)
{
    if (std::ranges::none_of (track.graph.nodes, [] (const GraphNode& n) { return n.type == nodetype::macros; })
        && hasMacroSlots (track.graph))
        dropMacroSlots (track.graph);
}

// What a track's components may hold, checked after every edit of its graphs rather than at each way in, as a paste can bring a whole
// tree (docs/design/component-nodes.md, N3, N4). Loops are looked for with the components flattened away, as they may cross their edges.
void requireValidComponents (const Track& track)
{
    auto any = false;
    forEachNode (track.graph, [&any] (const GraphNode& node, bool) { any = any || node.component != nullptr; });

    if (! any)
        return;

    const auto depthOf = [] (const TrackGraph& graph, const auto& self) -> int
    {
        int deepest = 0;

        for (const auto& node : graph.nodes)
            if (node.component != nullptr)
                deepest = std::max (deepest, 1 + self (node.component->graph, self));

        return deepest;
    };

    if (depthOf (track.graph, depthOf) > limits::maxComponentDepth)
        throw CommandError ("Components can only go " + juce::String (limits::maxComponentDepth) + " levels deep");

    const auto requireNoLoop = [] (const TrackGraph& graph)
    {
        auto wireLoop = false;
        const auto flat = flattenComponents (graph, &wireLoop);

        if (wireLoop)
            throw CommandError ("This connection would create a feedback loop");

        for (const auto& c : flat.connections)
            if (createsCycle (flat, c.fromNode, c.toNode))
                throw CommandError ("This connection would create a feedback loop");
    };

    requireNoLoop (track.graph);

    forEachNode (track.graph,
                 [&] (const GraphNode& node, bool inVoice)
                 {
                     if (inVoice && ! nodetype::allowedInVoiceGraph (node.type))
                         throw CommandError (juce::String ("A ") + requireDescriptor (node.type).displayName
                                             + " can't run in a voice graph");

                     if (node.voiceGraph != nullptr)
                         requireNoLoop (*node.voiceGraph);

                     if (node.component != nullptr)
                         for (const auto& inner : node.component->graph.nodes)
                             if (! nodetype::allowedInComponent (inner.type))
                                 throw CommandError (juce::String ("A ") + requireDescriptor (inner.type).displayName
                                                     + " can't go in a component");
                 });
}

template <typename UpdateFn>
[[nodiscard]] Project updateGraph (const Project& project, const juce::var& args, UpdateFn&& update)
{
    return updateTrackOrMaster (project, requireId (args, "trackId"),
                                [&] (Track& track, Project& result)
                                {
                                    const auto owner = graphScopeOf (track, args);

                                    if (owner == 0)
                                    {
                                        GraphScope scope { track.graph, track };
                                        update (scope, result);
                                        pruneExposedParams (track.graph);
                                        pruneMacroSlots (track);
                                        requireValidComponents (track);
                                        return;
                                    }

                                    auto edit = [&] (TrackGraph& graph, bool inVoice, const ComponentGraph* component)
                                    {
                                        GraphScope scope { graph, track, inVoice, component, owner };
                                        update (scope, result);
                                    };

                                    // The owner's own graph, which is what "has no graph" is about.
                                    const auto* found = track.findNode (owner);

                                    if (found == nullptr
                                        || (found->voiceGraph == nullptr && found->component == nullptr)
                                        || ! editOwnedGraph (track.graph, owner, false, edit))
                                        throw CommandError ("Node " + juce::String (owner) + " holds no graph");

                                    pruneExposedParams (track.graph);
                                    requireValidComponents (track);
                                });
}

// For the commands that only work on a track's graph (the chain tools and plugin settings).
void requireTrackScope (const juce::var& args)
{
    if (args.hasProperty ("voiceNodeId"))
        throw CommandError ("This only works on a track's graph");
}

// A pasted or saved component brings what's in it, with new ids; a new one passes its audio straight through.
void fillComponent (GraphNode& node, Project& result, const juce::var& spec)
{
    if (spec["component"].isObject())
    {
        auto inside = *projectfile::componentFromVar (spec["component"]);
        std::map<ObjectId, ObjectId> ids;
        renumberGraph (inside.graph, result, ids);

        for (auto& param : inside.exposed)
            param.nodeId = ids.at (param.nodeId);

        node.component = std::make_shared<const ComponentGraph> (std::move (inside));
        return;
    }

    ComponentGraph inside;
    const auto input = makeNode (result, nodetype::componentInput, 0.0f, 0.0f);
    const auto output = makeNode (result, nodetype::componentOutput, 440.0f, 0.0f);
    inside.graph.connections = { { input.id, 0, output.id, 0 } };
    inside.graph.nodes = { input, output };
    inside.inputs = { { PortType::audio, "In" } };
    inside.outputs = { { PortType::audio, "Out" } };
    node.component = std::make_shared<const ComponentGraph> (std::move (inside));
}

// A node a user adds in graph view (docs/design/phase8-node-graph.md, G5): any user-editable type, plugins
// with their plugin reference. Instruments only go on instrument tracks; a voice graph takes only the types that can
// run per voice (docs/design/phase12-voice-scope.md, V6). A Voice Instrument starts with the default voice graph.
// Whether a node of a type may go where it's being added.
void checkUserNodePlace (const juce::String& type, const NodeDescriptor& descriptor, const GraphScope& scope)
{
    if (! descriptor.userEditable)
        throw CommandError (juce::String ("Nodes of type '") + descriptor.type + "' cannot be added");

    if (scope.voice() && ! nodetype::allowedInVoiceGraph (type))
        throw CommandError (juce::String ("A ") + descriptor.displayName + " can't run in a voice graph");

    if (! scope.voice() && type == nodetype::sampleVoice)
        throw CommandError ("A Sample Voice only runs in a voice graph");

    if (! scope.voice() && isInstrument (descriptor) && scope.track.kind != TrackKind::instrument)
        throw CommandError ("Only instrument tracks can hold instruments");

    // The macros belong to the track: one Macros device, in its own graph (docs/design/macros-and-modulation-column.md,
    // MC5).
    if (type == nodetype::macros)
    {
        if (scope.nested())
            throw CommandError ("Macros belong to the track's own graph");

        if (std::ranges::any_of (scope.graph.nodes, [] (const GraphNode& n) { return n.type == nodetype::macros; }))
            throw CommandError ("This track has its macros already");
    }

    // One step sequencer per track, in its own graph (docs/design/step-sequencer.md, SS1), however it's added.
    if (type == nodetype::stepSequencer)
    {
        if (scope.nested())
            throw CommandError ("A step sequencer belongs to the track's own graph");

        if (std::ranges::any_of (scope.graph.nodes,
                                 [] (const GraphNode& n) { return n.type == nodetype::stepSequencer; }))
            throw CommandError ("This track already has a step sequencer");
    }
}

// Pasted nodes bring their modulation assignments too. Keys are modulation input ports. A Faust node whose script
// hasn't compiled can't be checked; its assignments wait for the script, like its parameter values.
void pasteModulations (GraphNode& node, const juce::var& spec)
{
    if (const auto* slots = spec["modulations"].getDynamicObject())
    {
        const auto pastedDescriptor = descriptorOf (node);

        for (const auto& property : slots->getProperties())
        {
            const auto key = property.name.toString();

            if (key.isEmpty() || ! key.containsOnly ("0123456789") || key.length() > 4)
                throw CommandError ("Bad modulation port '" + key + "'");

            const auto& fields = property.value;
            ModulationSlot slot;
            slot.param = fields["param"].toString();

            if (pastedDescriptor != nullptr)
                checkModulationTarget (node, key.getIntValue(), slot.param);

            slot.depth = std::clamp (numberOr (fields, "depth", 0.0), -1.0, 1.0);
            slot.active = static_cast<bool> (fields.getProperty ("active", true));
            slot.macro = projectfile::macroOf (fields);
            node.modulations[key.getIntValue()] = std::move (slot);
        }
    }
}

// Pasted nodes bring their parameter values (unknown parameters are ignored).
void pasteParams (GraphNode& node, const juce::var& spec)
{
    if (const auto* values = spec["params"].getDynamicObject())
    {
        for (const auto& property : values->getProperties())
        {
            const auto info = paramOf (node, property.name.toString());
            const auto& value = property.value;

            if (! info.has_value() || ! (value.isDouble() || value.isInt() || value.isInt64()))
                continue;

            node.params[property.name.toString()] =
                std::clamp (static_cast<double> (property.value), static_cast<double> (info->minValue),
                            static_cast<double> (info->maxValue));
        }
    }
}

[[nodiscard]] std::shared_ptr<const StepSequence> defaultSteps (const Track& track, const Project& project);

[[nodiscard]] GraphNode makeUserNode (Project& result, const GraphScope& scope, const juce::var& spec)
{
    const auto type = spec["type"].toString(); // a named value: GCC flags references bound via temporaries
    const auto& descriptor = requireDescriptor (type);

    checkUserNodePlace (type, descriptor, scope);

    auto node = makeNode (result, descriptor.type, static_cast<float> (numberOr (spec, "x", 0.0)),
                          static_cast<float> (numberOr (spec, "y", 0.0)));

    if (nodetype::isPlugin (type))
        node.plugin = parsePluginRef (spec["plugin"]);

    if (type == nodetype::faust)
        node.code = spec["code"].isString() ? spec["code"].toString() : juce::String (defaultFaustCode);

    // Split nodes come with their settings: "Left/right split" is a Stereo Split in that mode (split-nodes.md, SP4).
    if (type == nodetype::bandSplit && spec.hasProperty ("bands"))
        node.bands = std::clamp (static_cast<int> (spec["bands"]), 2, 4);

    if ((type == nodetype::stereoSplit || type == nodetype::stereoMerge) && spec["mode"].toString() == "leftRight")
        node.stereoMode = StereoMode::leftRight;

    pasteModulations (node, spec);

    if (type == nodetype::macros)
        node.macroNames = projectfile::macroNamesOf (spec);

    // A pasted LFO keeps its shape and a pasted Waveshaper its drawn curve; other node types have none.
    if ((type == nodetype::lfo || type == nodetype::waveshaper) && spec["modulationShape"].isArray())
    {
        node.modulationShape = curveFromVar (spec["modulationShape"], -1.0, 1.0);

        if (! node.modulationShape.empty()
            && (node.modulationShape.front().time < 0 || node.modulationShape.back().time > modulationShapeTicks))
            throw CommandError ("Shape points must be inside one cycle");
    }

    // A pasted plugin starts from the state its source saved for a copy (the app fills it in).
    if (nodetype::isPlugin (type) && spec["pluginState"].isString())
    {
        auto state = std::make_shared<juce::MemoryBlock>();

        if (state->fromBase64Encoding (spec["pluginState"].toString()))
            node.pluginState = std::move (state);
    }

    // A pasted plugin instrument keeps how it receives notes.
    if (nodetype::isPlugin (type) && spec["midi"].isObject())
    {
        const auto& midi = spec["midi"];
        node.midi.mpe = static_cast<bool> (midi.getProperty ("mpe", node.midi.mpe));
        node.midi.bendRange = std::clamp (
            static_cast<int> (midi.getProperty ("bendRange", MidiOutputSettings::defaultBendRange (node.midi.mpe))),
            limits::minBendRange, limits::maxBendRange);
    }

    pasteParams (node, spec);
    node.bypassed = static_cast<bool> (spec["bypassed"]);

    if (type == nodetype::component)
        fillComponent (node, result, spec);

    // A pasted Voice Instrument brings its voice graph, with new ids; a new one gets the default.
    if (type == nodetype::voiceInstrument && spec["voice"].isObject())
    {
        auto voice = *projectfile::voiceGraphFromVar (spec["voice"]);
        std::map<ObjectId, ObjectId> ids;
        renumberGraph (voice, result, ids);
        node.voiceGraph = std::make_shared<const TrackGraph> (std::move (voice));
    }
    else if (type == nodetype::voiceInstrument)
    {
        node.voiceGraph = makeDefaultVoiceGraph (result);
    }

    if (nodetype::playsSamples (type))
        node.sampleZones = projectfile::sampleZonesFromVar (spec["zones"]);

    // A pasted Drum Synth keeps its pads; a new one has the default kit from makeNode().
    if (type == nodetype::drumSynth && spec["pads"].isArray())
        node.drumPads = projectfile::drumPadsFromVar (spec["pads"]);

    // A pasted step sequencer keeps its rows and patterns; a new one starts with the track's drum rows, as when it's
    // inserted.
    if (type == nodetype::stepSequencer)
        node.steps =
            spec["steps"].isObject() ? projectfile::stepsFromVar (spec["steps"]) : defaultSteps (scope.track, result);

    return node;
}

[[nodiscard]] Project addNode (const Project& project, const juce::var& args)
{
    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project& result)
                        {
                            scope.graph.nodes.push_back (makeUserNode (result, scope, args));

                            // Dropped on a cable (docs/design/phase8-node-graph.md, section 7): into its path.
                            const auto& into = args["insertInto"];

                            if (! into.isObject())
                                return;

                            const auto id = scope.graph.nodes.back().id;
                            const auto cable = parseConnection (into);
                            auto& connections = scope.graph.connections;
                            const auto found = std::ranges::find (connections, cable);

                            if (found == connections.end())
                                throw CommandError ("No such connection");

                            connections.erase (found);

                            for (const auto& connection :
                                 { Connection { cable.fromNode, cable.fromPort, id, requireInt (into, "input") },
                                   Connection { id, requireInt (into, "output"), cable.toNode, cable.toPort } })
                            {
                                validateConnection (scope.graph, connection, scope.component);
                                connections.push_back (connection);
                            }
                        });
}

[[nodiscard]] Project addNodes (const Project& project, const juce::var& args)
{
    const auto* specs = args["nodes"].getArray();

    if (specs == nullptr || specs->isEmpty())
        throw CommandError ("Missing nodes");

    return updateGraph (
        project, args,
        [&] (GraphScope& scope, Project& result)
        {
            std::vector<ObjectId> ids;

            for (const auto& spec : *specs)
            {
                scope.graph.nodes.push_back (makeUserNode (result, scope, spec));
                ids.push_back (scope.graph.nodes.back().id);
            }

            // Connections between the new nodes, by index into nodes.
            if (const auto* connections = args["connections"].getArray())
            {
                for (const auto& c : *connections)
                {
                    const auto from = requireInt (c, "from");
                    const auto to = requireInt (c, "to");

                    if (from < 0 || to < 0 || static_cast<size_t> (from) >= ids.size()
                        || static_cast<size_t> (to) >= ids.size())
                        throw CommandError ("Connection refers to a node that is not being added");

                    const Connection connection { ids[static_cast<size_t> (from)], requireInt (c, "fromPort"),
                                                  ids[static_cast<size_t> (to)], requireInt (c, "toPort") };
                    validateConnection (scope.graph, connection, scope.component);
                    scope.graph.connections.push_back (connection);
                }
            }
        });
}

void requireRemovable (const GraphNode& node)
{
    if (! requireDescriptor (node.type).userEditable)
        throw CommandError ("This node cannot be removed");
}

[[nodiscard]] Project removeNodeOnly (const Project& project, const juce::var& args, ObjectId nodeId);

[[nodiscard]] Project removeNode (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto trackId = requireId (args, "trackId");
    auto result = removeNodeOnly (project, args, nodeId);
    removeKeySendsTo (result, trackId, { nodeId });
    return result;
}

// Forgets the MIDI mappings and automation lanes of a node, and of a Voice Instrument's voice graph.
void forgetNode (const GraphNode& node, Track& track, Project& result)
{
    const auto ids = idsWithin (node);
    std::erase_if (result.midiMappings, [&] (const MidiMapping& m) { return ids.contains (m.nodeId); });
    std::erase_if (track.automation, [&] (const AutomationLane& l) { return ids.contains (l.target.nodeId); });
}

[[nodiscard]] Project removeNodeOnly (const Project& project, const juce::var& args, ObjectId nodeId)
{
    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project& result)
                        {
                            const auto& node = requireNode (scope.graph, nodeId);
                            requireRemovable (node);
                            forgetNode (node, scope.track, result);
                            auto chain = scope.nested() ? std::nullopt : findChain (scope.graph);
                            removeNodeAndConnections (scope.graph, nodeId);

                            if (chain.has_value() && (chain->contains (nodeId) || chain->gain == nodeId))
                            {
                                // Keep the chain intact: the neighbours are joined up again. The gain is
                                // optional, so the graph stays a chain without it (G4).
                                if (chain->instrument == nodeId)
                                    chain->instrument = 0;

                                if (chain->gain == nodeId)
                                    chain->gain = 0;

                                std::erase (chain->effects, nodeId);
                                std::erase (chain->midiEffects, nodeId);
                                rewire (scope.graph, *chain);
                            }
                        });
}

[[nodiscard]] Project removeNodes (const Project& project, const juce::var& args)
{
    const auto* ids = args["nodeIds"].getArray();

    if (ids == nullptr)
        throw CommandError ("Missing nodeIds");

    std::set<ObjectId> removed;

    for (const auto& id : *ids)
        removed.insert (static_cast<ObjectId> (id));

    auto edited = updateGraph (project, args,
                               [&] (GraphScope& scope, Project& result)
                               {
                                   for (const auto& id : *ids)
                                   {
                                       const auto nodeId = static_cast<ObjectId> (id);
                                       const auto& node = requireNode (scope.graph, nodeId);
                                       requireRemovable (node);
                                       forgetNode (node, scope.track, result);
                                       removeNodeAndConnections (scope.graph, nodeId);
                                   }
                               });
    removeKeySendsTo (edited, requireId (args, "trackId"), removed);
    return edited;
}

// ---------------------------------------------------------------------------------------------------------
// Step sequencer (docs/design/step-sequencer.md)

// A new sequencer's rows (SS5): one per key of the track's Sampler zones or Drum Synth pads, up to 16, named after the
// first zone's sample on that key or after the pad, or else a General MIDI drum kit.
[[nodiscard]] std::shared_ptr<const StepSequence> defaultSteps (const Track& track, const Project& project)
{
    StepSequence steps;
    std::map<int, juce::String> keys;

    for (const auto& node : track.graph.nodes)
    {
        // A Drum Synth's pads that play, by their names (docs/design/drum-synth.md, DR10).
        if (node.type == nodetype::drumSynth)
        {
            const auto pads = drumPadsOf (node);

            for (int i = 0; i < drumPadCount; ++i)
                if (const auto& pad = pads[static_cast<size_t> (i)]; pad.model != DrumModel::off && keys.size() < 16)
                    keys.emplace (firstDrumKey + i,
                                  pad.name.isNotEmpty() ? pad.name : juce::String (drumModelName (pad.model)));

            continue;
        }

        if (! nodetype::playsSamples (node.type))
            continue;

        for (const auto& zone : node.sampleZones)
        {
            const auto key = zone.lowKey == zone.highKey ? zone.lowKey : zone.rootKey;

            if (keys.size() >= 16 || keys.contains (key))
                continue;

            const auto* file = project.findAudioFile (zone.fileId);
            // The file's name without folders or extension (paths may be relative, so no juce::File).
            juce::String name;

            if (file != nullptr)
                name = file->name.isNotEmpty() ? file->name : file->path;

            name = name.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false);
            keys[key] = name.containsChar ('.') ? name.upToLastOccurrenceOf (".", false, false) : name;
        }
    }

    if (! keys.empty())
    {
        // Highest first, as pads read top to bottom.
        for (auto it = keys.rbegin(); it != keys.rend(); ++it)
            steps.rows.push_back ({ it->first, it->second });
    }
    else
    {
        steps.rows = { { 49, "Crash" }, { 46, "Open hat" }, { 42, "Closed hat" }, { 39, "Clap" },
                       { 38, "Snare" }, { 45, "Low tom" },  { 36, "Kick" } };
    }

    return std::make_shared<const StepSequence> (projectfile::normalisedSteps (std::move (steps)));
}

// Edits the steps of the sequencer a command names.
template <typename Edit>
[[nodiscard]] Project editSteps (const Project& project, const juce::var& args, Edit&& edit)
{
    const auto nodeId = requireId (args, "nodeId");
    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::stepSequencer || node.steps == nullptr)
                                throw CommandError ("This node isn't a step sequencer");

                            auto steps = *node.steps;
                            std::forward<Edit> (edit) (steps);
                            node.steps =
                                std::make_shared<const StepSequence> (projectfile::normalisedSteps (std::move (steps)));
                        });
}

[[nodiscard]] int requireIndex (const juce::var& args, const char* key, size_t count)
{
    const auto index = requireInt (args, key);

    if (index < 0 || std::cmp_greater_equal (index, count))
        throw CommandError ("'" + juce::String (key) + "' is out of range");

    return index;
}

// A step: on or off, velocity, length, chance and ratchet; fields left out keep their values.
[[nodiscard]] Project setStepCell (const Project& project, const juce::var& args)
{
    return editSteps (project, args,
                      [&] (StepSequence& steps)
                      {
                          const auto pattern = requireIndex (args, "pattern", steps.patterns.size());
                          const auto row = requireIndex (args, "row", steps.rows.size());
                          const auto step = requireIndex (args, "step", static_cast<size_t> (StepSequence::maxSteps));
                          auto& cell = steps.patterns[static_cast<size_t> (pattern)]
                                           .rows[static_cast<size_t> (row)]
                                           .cells[static_cast<size_t> (step)];

                          if (args.hasProperty ("on"))
                              cell.on = static_cast<bool> (args["on"]);
                          if (args.hasProperty ("velocity"))
                              cell.velocity = static_cast<float> (requireNumber (args, "velocity"));
                          if (args.hasProperty ("length"))
                              cell.length = static_cast<float> (requireNumber (args, "length"));
                          if (args.hasProperty ("chance"))
                              cell.chance = static_cast<float> (requireNumber (args, "chance"));
                          if (args.hasProperty ("ratchet"))
                              cell.ratchet = static_cast<int> (std::lround (requireNumber (args, "ratchet")));
                      });
}

// Several steps at once (painting): cells: [{ row, step, on?, velocity? ... }].
[[nodiscard]] Project setStepCells (const Project& project, const juce::var& args)
{
    return editSteps (project, args,
                      [&] (StepSequence& steps)
                      {
                          const auto pattern = requireIndex (args, "pattern", steps.patterns.size());

                          for (const auto& fields : requireArray (args, "cells"))
                          {
                              const auto row = requireIndex (fields, "row", steps.rows.size());
                              const auto step =
                                  requireIndex (fields, "step", static_cast<size_t> (StepSequence::maxSteps));
                              auto& cell = steps.patterns[static_cast<size_t> (pattern)]
                                               .rows[static_cast<size_t> (row)]
                                               .cells[static_cast<size_t> (step)];

                              if (fields.hasProperty ("on"))
                                  cell.on = static_cast<bool> (fields["on"]);
                              if (fields.hasProperty ("velocity"))
                                  cell.velocity = static_cast<float> (requireNumber (fields, "velocity"));
                          }
                      });
}

// A row's pitch and name (all patterns), or its length in one pattern.
[[nodiscard]] Project setStepRow (const Project& project, const juce::var& args)
{
    return editSteps (project, args,
                      [&] (StepSequence& steps)
                      {
                          const auto row = static_cast<size_t> (requireIndex (args, "row", steps.rows.size()));

                          if (args.hasProperty ("pitch"))
                              steps.rows[row].pitch = static_cast<int> (std::lround (requireNumber (args, "pitch")));
                          if (args.hasProperty ("name"))
                              steps.rows[row].name = args["name"].toString();
                          if (args.hasProperty ("length"))
                          {
                              const auto pattern = requireIndex (args, "pattern", steps.patterns.size());
                              steps.patterns[static_cast<size_t> (pattern)].rows[row].length =
                                  static_cast<int> (std::lround (requireNumber (args, "length")));
                          }
                      });
}

[[nodiscard]] Project addStepRow (const Project& project, const juce::var& args)
{
    return editSteps (
        project, args,
        [&] (StepSequence& steps)
        {
            if (steps.rows.size() >= static_cast<size_t> (StepSequence::maxRows))
                throw CommandError ("A step sequencer has at most " + juce::String (StepSequence::maxRows) + " rows");

            const auto index =
                std::clamp (static_cast<int> (numberOr (args, "index", static_cast<double> (steps.rows.size()))), 0,
                            static_cast<int> (steps.rows.size()));
            steps.rows.insert (
                steps.rows.begin() + index,
                StepRow { static_cast<int> (std::lround (requireNumber (args, "pitch"))), args["name"].toString() });

            for (auto& pattern : steps.patterns)
            {
                StepPattern::Row row;
                // A new row takes the length most rows of the pattern have.
                row.length = pattern.rows.empty() ? 16 : pattern.rows.front().length;
                pattern.rows.insert (pattern.rows.begin() + index, std::move (row));
            }
        });
}

[[nodiscard]] Project removeStepRow (const Project& project, const juce::var& args)
{
    return editSteps (project, args,
                      [&] (StepSequence& steps)
                      {
                          const auto row = requireIndex (args, "row", steps.rows.size());
                          steps.rows.erase (steps.rows.begin() + row);

                          for (auto& pattern : steps.patterns)
                              if (std::cmp_less (row, pattern.rows.size()))
                                  pattern.rows.erase (pattern.rows.begin() + row);
                      });
}

// A pattern's step size and swing; a pattern index one past the last adds an empty pattern.
[[nodiscard]] Project setStepPattern (const Project& project, const juce::var& args)
{
    return editSteps (project, args,
                      [&] (StepSequence& steps)
                      {
                          const auto index = requireIndex (
                              args, "pattern",
                              std::min (steps.patterns.size() + 1, static_cast<size_t> (StepSequence::maxPatterns)));

                          if (std::cmp_equal (index, steps.patterns.size()))
                              steps.patterns.emplace_back();

                          auto& pattern = steps.patterns[static_cast<size_t> (index)];

                          if (args.hasProperty ("rate"))
                              pattern.rate = static_cast<int> (std::lround (requireNumber (args, "rate")));
                          if (args.hasProperty ("swing"))
                              pattern.swing = static_cast<float> (requireNumber (args, "swing"));
                      });
}

// Copies a pattern over another; `to` one past the last appends the copy.
[[nodiscard]] Project copyStepPattern (const Project& project, const juce::var& args)
{
    return editSteps (project, args,
                      [&] (StepSequence& steps)
                      {
                          const auto from = requireIndex (args, "from", steps.patterns.size());
                          const auto to = requireIndex (
                              args, "to",
                              std::min (steps.patterns.size() + 1, static_cast<size_t> (StepSequence::maxPatterns)));
                          const auto copy = steps.patterns[static_cast<size_t> (from)];

                          if (std::cmp_equal (to, steps.patterns.size()))
                              steps.patterns.push_back (copy);
                          else
                              steps.patterns[static_cast<size_t> (to)] = copy;
                      });
}

// Turns every step of a pattern off, keeping its step size, swing and row lengths.
[[nodiscard]] Project clearStepPattern (const Project& project, const juce::var& args)
{
    return editSteps (project, args,
                      [&] (StepSequence& steps)
                      {
                          const auto index = requireIndex (args, "pattern", steps.patterns.size());

                          for (auto& row : steps.patterns[static_cast<size_t> (index)].rows)
                              std::ranges::fill (row.cells, StepCell {});
                      });
}

// A sequencer clip's pattern and start step (SS3, SS4).
[[nodiscard]] Project setClipSequencer (const Project& project, const juce::var& args)
{
    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project&)
                       {
                           if (! clip.isSequencer())
                               throw CommandError ("This isn't a sequencer clip");

                           if (args.hasProperty ("pattern"))
                               clip.sequencerPattern =
                                   std::clamp (requireInt (args, "pattern"), 0, StepSequence::maxPatterns - 1);
                           if (args.hasProperty ("startStep"))
                               clip.sequencerStartStep =
                                   std::clamp (requireInt (args, "startStep"), 0, StepSequence::maxSteps - 1);
                       });
}

// The track's step sequencer, or nullptr.
[[nodiscard]] const GraphNode* stepSequencerOf (const Track& track)
{
    const auto it = std::ranges::find (track.graph.nodes, juce::String (nodetype::stepSequencer), &GraphNode::type);
    return it != track.graph.nodes.end() ? &*it : nullptr;
}

// Replaces a sequencer clip with a clip of the notes it plays (SS10).
[[nodiscard]] Project renderStepClip (const Project& project, const juce::var& args)
{
    const auto clipId = requireId (args, "clipId");
    const auto location = requireClip (project, clipId);
    const auto& track = *project.tracks[location.track];
    const auto* sequencer = stepSequencerOf (track);
    const auto& source = *track.clips[location.clip];

    if (! source.isSequencer())
        throw CommandError ("This isn't a sequencer clip");

    if (sequencer == nullptr || sequencer->steps == nullptr)
        throw CommandError ("This track has no step sequencer");

    std::vector<stepseq::Hit> hits;
    const stepseq::Span span { source.start,
                               source.start + source.length,
                               source.start - source.offset,
                               source.sequencerPattern,
                               source.sequencerStartStep,
                               source.id };
    stepseq::forEachStepHit (*sequencer->steps, span, static_cast<double> (span.start), static_cast<double> (span.end),
                             [&] (const stepseq::Hit& hit) { hits.push_back (hit); });

    return updateClip (project, clipId,
                       [&] (NoteClip& clip, Project& next)
                       {
                           clip.sequencerPattern = -1;
                           clip.sequencerStartStep = 0;
                           clip.offset = 0;
                           clip.loopStart = 0;
                           clip.loopLength = 0;
                           clip.notes.clear();

                           for (const auto& hit : hits)
                           {
                               Note note;
                               note.id = next.nextId++;
                               note.start = std::llround (hit.start) - clip.start;
                               note.length = std::max<Ticks> (1, std::llround (hit.end) - std::llround (hit.start));
                               note.pitch = hit.pitch;
                               note.velocity = hit.velocity;
                               clip.notes.push_back (std::make_shared<const Note> (std::move (note)));
                           }

                           sortNotes (clip.notes);
                       });
}

[[nodiscard]] Project insertNode (const Project& project, const juce::var& args)
{
    requireTrackScope (args);
    const auto type = args["type"].toString();
    const auto& descriptor = requireDescriptor (type);

    // A split has several outputs, so it has no place in a chain: only in a graph or a component (split-nodes.md, SP9).
    // A Gain is an effect like any other; the chain's own Gain is the one feeding the output.
    if (! descriptor.userEditable || type == nodetype::sequencer || nodetype::isModulationSource (type)
        || type == nodetype::bandSplit || type == nodetype::stereoSplit || type == nodetype::stereoMerge)
        throw CommandError (juce::String ("Nodes of type '") + descriptor.type + "' cannot be inserted");

    if (type == nodetype::sampleVoice)
        throw CommandError ("A Sample Voice only runs in a voice graph");

    std::optional<PluginRef> plugin;

    if (nodetype::isPlugin (type))
        plugin = parsePluginRef (args["plugin"]);

    const auto instrument = isInstrument (descriptor);
    const auto midiEffect = nodetype::isMidiEffect (type);

    return updateTrackOrMaster (
        project, requireId (args, "trackId"),
        [&] (Track& track, Project& result)
        {
            if (midiEffect && track.kind != TrackKind::instrument)
                throw CommandError ("Only instrument tracks take MIDI effects");

            if (instrument && track.kind != TrackKind::instrument)
                throw CommandError (track.kind == TrackKind::master ? "The master track has no instrument"
                                                                    : "Audio tracks have no instrument");

            auto chain = requireChain (track.graph);
            auto node = makeNode (result, descriptor.type, 0.0f, 0.0f);
            node.plugin = plugin;

            if (type == nodetype::faust)
                node.code = args["code"].isString() ? args["code"].toString() : juce::String (defaultFaustCode);

            if (type == nodetype::voiceInstrument)
                node.voiceGraph = makeDefaultVoiceGraph (result);

            if (type == nodetype::component)
                fillComponent (node, result, args);

            if (midiEffect)
            {
                // A step sequencer comes first, so the other MIDI effects process what it plays (SS1); one per track.
                const auto sequencer = type == nodetype::stepSequencer;

                if (sequencer && stepSequencerOf (track) != nullptr)
                    throw CommandError ("This track already has a step sequencer");

                if (sequencer)
                    node.steps = defaultSteps (track, result);

                const auto count = static_cast<int> (chain.midiEffects.size());
                const auto index =
                    std::clamp (static_cast<int> (numberOr (args, "index", sequencer ? 0 : count)), 0, count);
                chain.midiEffects.insert (chain.midiEffects.begin() + index, node.id);
            }
            else if (instrument)
            {
                // A track has one instrument: the new one replaces it.
                if (chain.instrument != 0)
                    removeNodeAndConnections (track.graph, chain.instrument);

                chain.instrument = node.id;
            }
            else
            {
                // A track with output tracks is only their header: an effect in its chain would play nowhere but in
                // Main, so effects go on the output tracks (docs/design/multi-output-instruments.md, MO12).
                if (hasOutputTracks (result, track.id))
                    throw CommandError ("This track only feeds its output tracks: add effects to those");

                const auto count = static_cast<int> (chain.effects.size());
                const auto index = std::clamp (static_cast<int> (numberOr (args, "index", count)), 0, count);
                chain.effects.insert (chain.effects.begin() + index, node.id);
            }

            track.graph.nodes.push_back (std::move (node));
            rewire (track.graph, chain);
        });
}

[[nodiscard]] Project reorderNode (const Project& project, const juce::var& args)
{
    requireTrackScope (args);
    const auto nodeId = requireId (args, "nodeId");
    const auto index = requireInt (args, "index");

    return updateTrackOrMaster (project, requireId (args, "trackId"),
                                [&] (Track& track, Project&)
                                {
                                    auto chain = requireChain (track.graph);

                                    // MIDI effects move among themselves, audio effects among theirs.
                                    auto& list =
                                        std::ranges::find (chain.midiEffects, nodeId) != chain.midiEffects.end()
                                            ? chain.midiEffects
                                            : chain.effects;
                                    const auto it = std::ranges::find (list, nodeId);

                                    if (it == list.end())
                                        throw CommandError ("Only effects can be reordered");

                                    list.erase (it);
                                    const auto count = static_cast<int> (list.size());
                                    list.insert (list.begin() + std::clamp (index, 0, count), nodeId);
                                    rewire (track.graph, chain);
                                });
}

[[nodiscard]] Project setNodeBypass (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto bypassed = static_cast<bool> (args["bypassed"]);

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&) { requireNode (scope.graph, nodeId).bypassed = bypassed; });
}

// Replaces a clip's notes with what the track's MIDI effects make of them, and bypasses the effects
// (docs/design/midi-effects.md, ME17). One undo step.
[[nodiscard]] Project renderMidiEffects (const Project& project, const juce::var& args)
{
    const auto clipId = requireId (args, "clipId");
    const auto location = requireClip (project, clipId);
    const auto& track = *project.tracks[location.track];
    const auto chain = findChain (track.graph);

    if (! chain.has_value())
        throw CommandError ("The track's devices aren't in a row: render from a simple chain");

    std::vector<const GraphNode*> effects;

    // A step sequencer only plays in its own clips: rendering a clip of notes leaves it alone.
    for (const auto id : chain->midiEffects)
        if (const auto& node = requireNode (track.graph, id); ! node.bypassed && node.type != nodetype::stepSequencer)
            effects.push_back (&node);

    if (effects.empty())
        throw CommandError ("This track has no MIDI effects playing");

    const auto rendered = midifx::renderMidiEffects (effects, *track.clips[location.clip], project.tempoMap);
    auto result = updateClip (project, clipId,
                              [&] (NoteClip& clip, Project& next)
                              {
                                  std::vector<Note> notes = rendered.notes;

                                  for (auto& note : notes)
                                      note.id = next.nextId++;

                                  for (const auto& link : rendered.links)
                                      notes[link.from].link = NoteLink { notes[link.to].id, link.glideMs };

                                  clip.notes.clear();

                                  for (auto& note : notes)
                                      clip.notes.push_back (std::make_shared<const Note> (std::move (note)));

                                  sortNotes (clip.notes);
                                  clip.offset = 0;
                                  clip.loopStart = 0;
                                  clip.loopLength = 0;
                              });

    for (const auto* effect : effects)
        result = setNodeBypass (
            result, makeObject ({ { "trackId", track.id }, { "nodeId", effect->id }, { "bypassed", true } }));

    return result;
}

[[nodiscard]] Project setPluginMidi (const Project& project, const juce::var& args)
{
    requireTrackScope (args);
    const auto nodeId = requireId (args, "nodeId");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::pluginInstrument)
                                throw CommandError ("Only instrument plugins have MIDI settings");

                            // Switching the mode moves the bend range to the new mode's default,
                            // unless the command sets one.
                            if (args.hasProperty ("mpe") && static_cast<bool> (args["mpe"]) != node.midi.mpe)
                            {
                                node.midi.mpe = static_cast<bool> (args["mpe"]);
                                node.midi.bendRange = MidiOutputSettings::defaultBendRange (node.midi.mpe);
                            }

                            if (args.hasProperty ("bendRange"))
                                node.midi.bendRange = std::clamp (requireInt (args, "bendRange"), limits::minBendRange,
                                                                  limits::maxBendRange);
                        });
}

void validateConnection (const TrackGraph& graph, const Connection& connection, const ComponentGraph* owner)
{
    const auto from = portsOf (requireNode (graph, connection.fromNode), owner);
    const auto to = portsOf (requireNode (graph, connection.toNode), owner);

    if (connection.fromPort < 0 || static_cast<size_t> (connection.fromPort) >= from.outputs.size()
        || connection.toPort < 0 || static_cast<size_t> (connection.toPort) >= to.inputs.size())
        throw CommandError ("No such port");

    const auto type = from.outputs[static_cast<size_t> (connection.fromPort)];

    if (type != to.inputs[static_cast<size_t> (connection.toPort)])
        throw CommandError ("Ports carry different kinds of data");

    if (std::ranges::find (graph.connections, connection) != graph.connections.end())
        throw CommandError ("These ports are already connected");

    const auto inputTaken =
        std::ranges::any_of (graph.connections, [&] (const Connection& c)
                             { return c.toNode == connection.toNode && c.toPort == connection.toPort; });

    if (type == PortType::events && inputTaken)
        throw CommandError ("An event input accepts only one connection");

    // With a component about, what counts is the flat graph, which requireValidComponents() checks after the edit: a way through a box
    // may have a Feedback node in it.
    const auto hasComponent =
        owner != nullptr
        || std::ranges::any_of (graph.nodes, [] (const GraphNode& n) { return n.component != nullptr; });

    if (! hasComponent && createsCycle (graph, connection.fromNode, connection.toNode))
        throw CommandError ("This connection would create a feedback loop");
}

// A cable that would close a loop, with a Feedback node put in it (docs/design/feedback-routing.md, R1): one undo step.
[[nodiscard]] Project closeLoop (const Project& project, const juce::var& args)
{
    const auto cable = parseConnection (args);
    const juce::var nodeArgs (args.getDynamicObject()->clone().release());
    nodeArgs.getDynamicObject()->setProperty ("type", nodetype::feedback);

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project& result)
                        {
                            scope.graph.nodes.push_back (makeUserNode (result, scope, nodeArgs));
                            const auto id = scope.graph.nodes.back().id;

                            for (const auto& connection : { Connection { cable.fromNode, cable.fromPort, id, 0 },
                                                            Connection { id, 0, cable.toNode, cable.toPort } })
                            {
                                validateConnection (scope.graph, connection, scope.component);
                                scope.graph.connections.push_back (connection);
                            }
                        });
}

constexpr float componentEdgeGap = 220.0f;

[[nodiscard]] Project connect (const Project& project, const juce::var& args)
{
    const auto connection = parseConnection (args);

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            validateConnection (scope.graph, connection, scope.component);
                            scope.graph.connections.push_back (connection);
                        });
}

// What a cable dragged in the graph view does: a cable if it can be, or with a Feedback node put in it if it would close a loop, which may
// run through components, so only here can it be told (docs/design/feedback-routing.md, R1).
[[nodiscard]] Project connectOrCloseLoop (const Project& project, const juce::var& args)
{
    try
    {
        return connect (project, args);
    }
    catch (const CommandError& error)
    {
        if (! juce::String (error.what()).contains ("feedback loop"))
            throw;
    }

    return closeLoop (project, args);
}

// Runs `edit` on what's in a component, on whichever graph it's in.
template <typename EditFn>
[[nodiscard]] Project editComponent (const Project& project, const juce::var& args, EditFn&& edit)
{
    const auto nodeId = requireId (args, "nodeId");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.component == nullptr)
                                throw CommandError ("Not a component");

                            auto inside = *node.component;
                            edit (inside, scope.graph, nodeId);
                            node.component = std::make_shared<const ComponentGraph> (std::move (inside));
                        });
}

[[nodiscard]] bool isInputSide (const juce::var& args)
{
    const auto side = args["side"].toString();

    if (side != "input" && side != "output")
        throw CommandError ("A port's side is input or output");

    return side == "input";
}

[[nodiscard]] std::vector<ComponentPort>& portsOnSide (ComponentGraph& inside, const juce::var& args)
{
    return isInputSide (args) ? inside.inputs : inside.outputs;
}

[[nodiscard]] size_t requirePortIndex (const std::vector<ComponentPort>& ports, const juce::var& args)
{
    const auto index = requireInt (args, "index");

    if (index < 0 || std::cmp_greater_equal (index, ports.size()))
        throw CommandError ("No such port");

    return static_cast<size_t> (index);
}

// What a port made from a cable is called: what it connected to.
[[nodiscard]] juce::String portNameFrom (const GraphNode& node)
{
    return node.plugin.has_value() ? node.plugin->name : juce::String (requireDescriptor (node.type).displayName);
}

// Puts nodes in a new component, which sounds the same (docs/design/component-nodes.md, N8): the cables crossing the selection's edge become
// ports, one for each output they come from, so that what one output feeds stays one cable to the box.
[[nodiscard]] Project groupNodes (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "nodeIds");

    if (ids.empty())
        throw CommandError ("Nothing to group");

    return updateGraph (
        project, args,
        [&] (GraphScope& scope, Project& result)
        {
            const std::set<ObjectId> selected (ids.begin(), ids.end());
            auto& graph = scope.graph;
            ComponentGraph inside;
            auto left = std::numeric_limits<float>::max();
            auto right = std::numeric_limits<float>::lowest();
            auto sumX = 0.0f;
            auto sumY = 0.0f;

            for (const auto id : selected)
            {
                const auto& node = requireNode (graph, id);
                requireRemovable (node);
                inside.graph.nodes.push_back (node);
                left = std::min (left, node.x);
                right = std::max (right, node.x);
                sumX += node.x;
                sumY += node.y;
            }

            const auto count = static_cast<float> (selected.size());
            const auto box = makeNode (result, nodetype::component, sumX / count, sumY / count);
            const auto input = makeNode (result, nodetype::componentInput, left - componentEdgeGap, sumY / count);
            const auto output = makeNode (result, nodetype::componentOutput, right + componentEdgeGap, sumY / count);
            inside.graph.nodes.insert (inside.graph.nodes.begin(), { input, output });
            std::ranges::sort (inside.graph.nodes, {}, &GraphNode::id); // creation order, as a file has it

            std::map<std::pair<ObjectId, int>, int> inputPorts;
            std::map<std::pair<ObjectId, int>, int> outputPorts;
            std::vector<Connection> outer;

            const auto typeOf = [&] (const Connection& c)
            {
                const auto ports = portsOf (requireNode (graph, c.fromNode), scope.component);
                return std::cmp_less (c.fromPort, ports.outputs.size())
                           ? ports.outputs[static_cast<size_t> (c.fromPort)]
                           : PortType::audio;
            };

            for (const auto& c : graph.connections)
            {
                const auto fromInside = selected.contains (c.fromNode);
                const auto toInside = selected.contains (c.toNode);

                if (fromInside && toInside)
                {
                    inside.graph.connections.push_back (c);
                }
                else if (toInside)
                {
                    const auto [entry, added] =
                        inputPorts.try_emplace ({ c.fromNode, c.fromPort }, static_cast<int> (inside.inputs.size()));

                    if (added)
                    {
                        inside.inputs.push_back ({ typeOf (c), portNameFrom (requireNode (graph, c.fromNode)) });
                        outer.push_back ({ c.fromNode, c.fromPort, box.id, entry->second });
                    }

                    inside.graph.connections.push_back ({ input.id, entry->second, c.toNode, c.toPort });
                }
                else if (fromInside)
                {
                    const auto [entry, added] =
                        outputPorts.try_emplace ({ c.fromNode, c.fromPort }, static_cast<int> (inside.outputs.size()));

                    if (added)
                    {
                        inside.outputs.push_back ({ typeOf (c), portNameFrom (requireNode (graph, c.fromNode)) });
                        inside.graph.connections.push_back ({ c.fromNode, c.fromPort, output.id, entry->second });
                    }

                    outer.push_back ({ box.id, entry->second, c.toNode, c.toPort });
                }
                else
                {
                    outer.push_back (c);
                }
            }

            auto node = box;
            node.component = std::make_shared<const ComponentGraph> (std::move (inside));
            std::erase_if (graph.nodes, [&] (const GraphNode& n) { return selected.contains (n.id); });
            graph.nodes.push_back (std::move (node));
            graph.connections = std::move (outer);
        });
}

// Puts what's in a component back in its place, which sounds the same (N8). A bypassed one wouldn't, so it's refused.
[[nodiscard]] Project ungroupNode (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");

    return updateGraph (
        project, args,
        [&] (GraphScope& scope, Project&)
        {
            auto& graph = scope.graph;
            const auto box = requireNode (graph, nodeId);

            if (box.component == nullptr)
                throw CommandError ("Not a component");

            if (box.bypassed)
                throw CommandError ("Turn the bypass off before ungrouping: what's inside would start to sound");

            const auto& inside = *box.component;
            auto in = nodeId;
            auto out = nodeId;
            auto sumX = 0.0f;
            auto sumY = 0.0f;
            std::vector<GraphNode> moved;

            for (const auto& node : inside.graph.nodes)
            {
                if (node.type == nodetype::componentInput)
                {
                    in = node.id;
                }
                else if (node.type == nodetype::componentOutput)
                {
                    out = node.id;
                }
                else
                {
                    moved.push_back (node);
                    sumX += node.x;
                    sumY += node.y;
                }
            }

            // Centred where the box was.
            if (! moved.empty())
            {
                const auto count = static_cast<float> (moved.size());

                for (auto& node : moved)
                {
                    node.x += box.x - sumX / count;
                    node.y += box.y - sumY / count;
                }
            }

            auto cables = graph.connections;

            for (auto& cable : cables)
            {
                if (cable.toNode == nodeId)
                    cable.toNode = in;

                if (cable.fromNode == nodeId)
                    cable.fromNode = out;
            }

            cables.insert (cables.end(), inside.graph.connections.begin(), inside.graph.connections.end());

            for (const auto junction : { nodeId, in, out })
                dissolveJunction (cables, junction);

            std::erase_if (graph.nodes, [nodeId] (const GraphNode& n) { return n.id == nodeId; });
            graph.nodes.insert (graph.nodes.end(), moved.begin(), moved.end());
            std::ranges::sort (graph.nodes, {}, &GraphNode::id);
            graph.connections = std::move (cables);
            pruneModulationSlots (graph);
        });
}

// docs/design/component-nodes.md, N6.
[[nodiscard]] Project renameComponent (const Project& project, const juce::var& args)
{
    return editComponent (project, args, [&] (ComponentGraph& inside, TrackGraph&, ObjectId)
                          { inside.name = args["name"].toString(); });
}

[[nodiscard]] std::vector<ExposedParam>::iterator findExposed (ComponentGraph& inside, const juce::var& args)
{
    const auto target = requireId (args, "target");
    const auto param = args["param"].toString();
    return std::ranges::find_if (inside.exposed,
                                 [&] (const ExposedParam& e) { return e.nodeId == target && e.param == param; });
}

[[nodiscard]] Project exposeParam (const Project& project, const juce::var& args)
{
    return editComponent (
        project, args,
        [&] (ComponentGraph& inside, TrackGraph&, ObjectId)
        {
            const auto target = requireId (args, "target");
            const auto param = args["param"].toString();
            const GraphNode* found = nullptr;
            forEachNode (inside.graph,
                         [&] (const GraphNode& node, bool)
                         {
                             if (node.id == target)
                                 found = &node;
                         });

            if (found == nullptr)
                throw CommandError ("That node isn't in this component");

            const auto info = paramOf (*found, param);

            if (! info.has_value())
                throw CommandError ("Unknown parameter '" + param + "'");

            if (findExposed (inside, args) != inside.exposed.end())
                throw CommandError ("That parameter is already on the component");

            inside.exposed.push_back (
                { target, param, args["name"].isString() ? args["name"].toString() : juce::String (info->name) });
        });
}

[[nodiscard]] Project renameExposedParam (const Project& project, const juce::var& args)
{
    return editComponent (project, args,
                          [&] (ComponentGraph& inside, TrackGraph&, ObjectId)
                          {
                              const auto it = findExposed (inside, args);

                              if (it == inside.exposed.end())
                                  throw CommandError ("That parameter isn't on the component");

                              it->name = args["name"].toString();
                          });
}

[[nodiscard]] Project unexposeParam (const Project& project, const juce::var& args)
{
    return editComponent (project, args,
                          [&] (ComponentGraph& inside, TrackGraph&, ObjectId)
                          {
                              const auto it = findExposed (inside, args);

                              if (it == inside.exposed.end())
                                  throw CommandError ("That parameter isn't on the component");

                              inside.exposed.erase (it);
                          });
}

// docs/design/component-nodes.md, N2.
[[nodiscard]] Project addComponentPort (const Project& project, const juce::var& args)
{
    return editComponent (project, args,
                          [&] (ComponentGraph& inside, TrackGraph&, ObjectId)
                          {
                              const auto type = args["type"].toString();

                              if (type != "audio" && type != "events" && type != "modulation")
                                  throw CommandError ("Unknown port type '" + type + "'");

                              portsOnSide (inside, args).push_back ({ portTypeNamed (type), args["name"].toString() });
                          });
}

// A component port's width: stereo, mono or adaptive (docs/design/mono-ports.md, MP11).
[[nodiscard]] Project setComponentPortChannels (const Project& project, const juce::var& args)
{
    return editComponent (project, args,
                          [&] (ComponentGraph& inside, TrackGraph&, ObjectId)
                          {
                              auto& ports = portsOnSide (inside, args);
                              auto& port = ports[requirePortIndex (ports, args)];
                              const auto channels = requireInt (args, "channels");

                              if (port.type != PortType::audio)
                                  throw CommandError ("Only audio ports have a width");

                              if (channels < 0 || channels > 2)
                                  throw CommandError ("A port's width is 2 (stereo), 1 (mono) or 0 (adaptive)");

                              port.channels = channels;
                          });
}

[[nodiscard]] Project renameComponentPort (const Project& project, const juce::var& args)
{
    return editComponent (project, args,
                          [&] (ComponentGraph& inside, TrackGraph&, ObjectId)
                          {
                              auto& ports = portsOnSide (inside, args);
                              ports[requirePortIndex (ports, args)].name = args["name"].toString();
                          });
}

// Removes a port and its cables, inside and out. The ports after it move up one, as a port is known by its position.
[[nodiscard]] Project removeComponentPort (const Project& project, const juce::var& args)
{
    return editComponent (
        project, args,
        [&] (ComponentGraph& inside, TrackGraph& outer, ObjectId nodeId)
        {
            const auto input = isInputSide (args);
            auto& ports = portsOnSide (inside, args);
            const auto index = requirePortIndex (ports, args);
            ports.erase (ports.begin() + static_cast<std::ptrdiff_t> (index));
            const auto port = static_cast<int> (index);

            const auto& nodes = inside.graph.nodes;
            const auto fixed = std::ranges::find (
                nodes, juce::String (input ? nodetype::componentInput : nodetype::componentOutput), &GraphNode::type);
            const auto innerId = fixed != nodes.end() ? fixed->id : ObjectId { 0 };

            // Inside, the Input's are outputs and the Output's are inputs; outside, the other way round.
            const auto close = [port] (TrackGraph& graph, ObjectId id, bool outputs)
            {
                std::erase_if (
                    graph.connections, [&] (const Connection& c)
                    { return outputs ? c.fromNode == id && c.fromPort == port : c.toNode == id && c.toPort == port; });

                for (auto& c : graph.connections)
                {
                    if (outputs && c.fromNode == id && c.fromPort > port)
                        --c.fromPort;
                    else if (! outputs && c.toNode == id && c.toPort > port)
                        --c.toPort;
                }

                pruneModulationSlots (graph);
            };

            close (inside.graph, innerId, input);
            close (outer, nodeId, ! input);
        });
}

[[nodiscard]] Project disconnect (const Project& project, const juce::var& args)
{
    const auto connection = parseConnection (args);

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            if (std::erase (scope.graph.connections, connection) == 0)
                                throw CommandError ("These ports are not connected");

                            pruneModulationSlots (scope.graph);
                        });
}

// Sets a cable's bends on the canvas, or clears them with none (docs/design/graph-grid-and-cables.md, G9). They are
// only drawn: the graph plays the same.
[[nodiscard]] Project setConnectionBends (const Project& project, const juce::var& args)
{
    const auto connection = parseConnection (args);
    std::vector<CablePoint> bends;

    if (args.hasProperty ("bends"))
    {
        const auto& points = requireArray (args, "bends");

        if (points.size() > static_cast<int> (limits::maxCableBends))
            throw CommandError ("A cable can have at most " + juce::String (limits::maxCableBends) + " bends");

        for (const auto& p : points)
            bends.push_back (
                { static_cast<float> (requireNumber (p, "x")), static_cast<float> (requireNumber (p, "y")) });
    }

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            const auto found = std::ranges::find (scope.graph.connections, connection);

                            if (found == scope.graph.connections.end())
                                throw CommandError ("These ports are not connected");

                            found->bends = bends;
                        });
}

// Assigns a modulation input to a parameter, or changes its depth/active state (docs/design/modulation.md, M2).
// An empty param removes the slot. A slot's port must be one of the node's modulation inputs.
[[nodiscard]] Project setModulation (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto port = static_cast<int> (requireNumber (args, "port"));
    const auto paramId = args["param"].toString();

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);
                            const auto descriptor = descriptorOf (node);

                            if (descriptor == nullptr)
                                throw CommandError ("Not a modulation input");

                            if (paramId.isEmpty())
                            {
                                requireModulationPort (node, port);
                                node.modulations.erase (port);
                                return;
                            }

                            checkModulationTarget (node, port, paramId);

                            auto& slot = node.modulations[port];
                            slot.param = paramId;

                            if (args.hasProperty ("depth"))
                                slot.depth = std::clamp (requireNumber (args, "depth"), -1.0, 1.0);

                            if (args.hasProperty ("active"))
                                slot.active = static_cast<bool> (args["active"]);
                        });
}

// Maps a parameter of any node on a track, inside components and voice graphs too, to a macro knob
// (docs/design/macros-and-modulation-column.md, MC9): adds the track's Macros device if it has none, then sets the
// node's slot that maps the parameter to that knob, or takes a free modulation input for it.
[[nodiscard]] Project mapMacro (const Project& project, const juce::var& args)
{
    const auto trackId = requireId (args, "trackId");
    const auto nodeId = requireId (args, "nodeId");
    const auto paramId = args["param"].toString();
    const auto macro = requireInt (args, "macro");
    const auto depth = std::clamp (numberOr (args, "depth", 0.5), -1.0, 1.0);

    if (macro < 0 || macro >= maxMacros)
        throw CommandError ("No macro " + juce::String (macro + 1));

    const auto* track = project.findTrackOrMaster (trackId);

    if (track == nullptr)
        throw CommandError ("No track with id " + juce::String (trackId));

    auto result = project;

    if (! std::ranges::any_of (track->graph.nodes, [] (const GraphNode& n) { return n.type == nodetype::macros; }))
    {
        // Left of the track's first node, where modulation sources go in the graph view.
        auto left = 0.0f;
        auto top = 0.0f;

        for (const auto& node : track->graph.nodes)
        {
            left = std::min (left, node.x);
            top = std::min (top, node.y);
        }

        result = addNode (result, makeObject ({ { "trackId", trackId },
                                                { "type", nodetype::macros },
                                                { "x", left - 240.0f },
                                                { "y", top - 160.0f } }));
    }

    return updateGraph (result, makeObject ({ { "trackId", trackId }, { "nodeId", nodeId } }),
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);
                            const auto ports = portsOf (node);

                            for (auto& [port, slot] : node.modulations)
                            {
                                if (slot.param == paramId && slot.macro == macro)
                                {
                                    slot.depth = depth;
                                    slot.active = true;
                                    return;
                                }
                            }

                            for (int port = 0; std::cmp_less (port, ports.inputs.size()); ++port)
                            {
                                const auto used =
                                    node.modulations.contains (port)
                                    || std::ranges::any_of (scope.graph.connections, [&] (const Connection& c)
                                                            { return c.toNode == nodeId && c.toPort == port; });

                                if (used || ! ports.isInput (port, PortType::modulation))
                                    continue;

                                checkModulationTarget (node, port, paramId);
                                node.modulations[port] = { paramId, depth, true, macro };
                                return;
                            }

                            throw CommandError ("Every modulation input of this device is in use");
                        });
}

// Names a macro knob (MC5); an empty name shows as "Macro N".
[[nodiscard]] Project renameMacro (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto index = requireInt (args, "index");
    const auto name = args["name"].toString().trim().substring (0, 64);

    if (index < 0 || index >= maxMacros)
        throw CommandError ("No macro " + juce::String (index + 1));

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::macros)
                                throw CommandError ("Not a Macros device");

                            if (std::cmp_less_equal (node.macroNames.size(), index))
                                node.macroNames.resize (static_cast<size_t> (index) + 1);

                            node.macroNames[static_cast<size_t> (index)] = name;

                            while (! node.macroNames.empty() && node.macroNames.back().isEmpty())
                                node.macroNames.pop_back();
                        });
}

// Replaces an LFO's one-cycle shape (docs/design/modulation.md, M4) or a Waveshaper's drawn curve
// (docs/design/waveshaper.md, S6). Times are 0..modulationShapeTicks; values -1..1. The command anchors the curve at 0
// and at the end: an LFO's loops back to its first value, so it has no hole; a Waveshaper's holds its end values.
[[nodiscard]] Project setModulationShape (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto shape = curveFromVar (args["points"], -1.0, 1.0);

    if (! shape.empty() && (shape.front().time < 0 || shape.back().time > modulationShapeTicks))
        throw CommandError ("Shape points must be inside one cycle");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::lfo && node.type != nodetype::waveshaper)
                                throw CommandError ("Only LFO and Waveshaper nodes have a shape");

                            auto anchored = shape;

                            if (! anchored.empty())
                            {
                                if (anchored.front().time > 0)
                                    anchored.insert (anchored.begin(),
                                                     CurvePoint { 0, anchored.front().value, CurveShape::linear, 0.0 });

                                if (anchored.back().time < modulationShapeTicks)
                                    anchored.push_back (
                                        { modulationShapeTicks,
                                          node.type == nodetype::lfo ? anchored.front().value : anchored.back().value,
                                          CurveShape::linear, 0.0 });
                            }

                            node.modulationShape = std::move (anchored);
                        });
}

// Sets a built-in parameter where it is, without following links (setNodeParam does).
[[nodiscard]] Project setNodeParamHere (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto paramId = args["param"].toString();
    const auto value = requireNumber (args, "value");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);
                            const auto info = paramOf (node, paramId);

                            if (! info.has_value())
                                throw CommandError ("Unknown parameter '" + paramId + "'");

                            node.params[paramId] = std::clamp (value, static_cast<double> (info->minValue),
                                                               static_cast<double> (info->maxValue));
                        });
}

// ---------------------------------------------------------------------------------------------------------
// Parameter links (docs/design/parameter-linking.md)

// A link end's node, or nullptr when its track or node is gone.
[[nodiscard]] const GraphNode* linkNodeOf (const Project& project, const ParamAddress& address)
{
    const auto* track = project.findTrackOrMaster (address.trackId);
    return track != nullptr ? track->findNode (address.nodeId) : nullptr;
}

// A built-in parameter's description, or nothing for a plugin parameter (or an unknown one).
[[nodiscard]] std::optional<ParamInfo> linkInfoOf (const GraphNode& node, const juce::String& param)
{
    return nodetype::isPlugin (node.type) ? std::nullopt : paramOf (node, param);
}

// A built-in parameter's value in the model (its default when unset).
[[nodiscard]] double modelValueOf (const GraphNode& node, const juce::String& param, const ParamInfo& info)
{
    const auto it = node.params.find (param);
    return it != node.params.end() ? it->second : static_cast<double> (info.defaultValue);
}

// Whether a node sits in a voice graph, at any depth.
[[nodiscard]] bool isInVoiceGraph (const Track& track, ObjectId nodeId)
{
    auto found = false;
    forEachNode (track.graph,
                 [&] (const GraphNode& node, bool inVoice) { found = found || (inVoice && node.id == nodeId); });
    return found;
}

[[nodiscard]] ParamAddress addressFromVar (const juce::var& value, bool needsParam)
{
    if (! value.isObject())
        throw CommandError ("Missing parameter address");

    ParamAddress address { requireId (value, "trackId"), requireId (value, "nodeId"), value["param"].toString() };

    if (needsParam && address.param.isEmpty())
        throw CommandError ("Missing parameter");

    return address;
}

// Checks that a link can go in (L1 to L3, L11) and throws if not.
void checkLink (const Project& project, const ParamLink& link)
{
    const auto* source = linkNodeOf (project, link.source);
    const auto* target = linkNodeOf (project, link.target);

    if (source == nullptr || target == nullptr)
        throw CommandError ("No such node");

    if (link.source == link.target)
        throw CommandError ("A parameter can't follow itself");

    const auto sourceInfo = linkInfoOf (*source, link.source.param);
    const auto targetInfo = linkInfoOf (*target, link.target.param);

    if (! nodetype::isPlugin (source->type) && ! sourceInfo.has_value())
        throw CommandError ("Unknown parameter '" + link.source.param + "'");

    if (! nodetype::isPlugin (target->type) && ! targetInfo.has_value())
        throw CommandError ("Unknown parameter '" + link.target.param + "'");

    if (isInVoiceGraph (*project.findTrackOrMaster (link.source.trackId), link.source.nodeId))
        throw CommandError ("A parameter in a voice graph can't drive a link: each voice has its own");

    // Choices and switches are copied (L2).
    if (targetInfo.has_value() && (targetInfo->kind == ParamKind::choice || targetInfo->kind == ParamKind::toggle))
    {
        if (link.mapping.kind != LinkMappingKind::identity)
            throw CommandError ("A choice or a switch can only follow as it is");

        if (sourceInfo.has_value() && sourceInfo->kind == ParamKind::choice && targetInfo->kind == ParamKind::choice
            && sourceInfo->choices.size() != targetInfo->choices.size())
            throw CommandError ("These choices don't match");
    }

    // One driver per parameter (L3).
    if (const auto* driving = project.linkDriving (link.target); driving != nullptr && driving->id != link.id)
        throw CommandError ("This parameter already follows another");

    // No cycles: walk up from the source; reaching the target would close one.
    auto up = link.source;

    for (size_t steps = 0; steps <= project.links.size(); ++steps)
    {
        if (up == link.target)
            throw CommandError ("This link would make the parameters follow each other in a circle");

        const auto* driving = project.linkDriving (up);

        if (driving == nullptr || driving->id == link.id)
            return;

        up = driving->source;
    }
}

// Sets every built-in target in the model to its built-in source's mapped value, along each chain (L13). Plugin
// sources have no value in the model: the engine follows them.
[[nodiscard]] Project mirrorParamLinks (const Project& project)
{
    auto result = project;

    for (size_t pass = 0; pass <= project.links.size(); ++pass)
    {
        auto changed = false;

        for (const auto& link : project.links)
        {
            if (! link.active)
                continue;

            const auto* source = linkNodeOf (result, link.source);
            const auto* target = linkNodeOf (result, link.target);

            if (source == nullptr || target == nullptr)
                continue;

            const auto sourceInfo = linkInfoOf (*source, link.source.param);
            const auto targetInfo = linkInfoOf (*target, link.target.param);

            if (! sourceInfo.has_value() || ! targetInfo.has_value())
                continue;

            const auto value = static_cast<double> (
                mapLinkValue (link.mapping, &*sourceInfo, &*targetInfo,
                              static_cast<float> (modelValueOf (*source, link.source.param, *sourceInfo))));

            if (std::abs (value - modelValueOf (*target, link.target.param, *targetInfo)) <= 1.0e-6)
                continue;

            result = setNodeParamHere (result, makeObject ({ { "trackId", link.target.trackId },
                                                             { "nodeId", link.target.nodeId },
                                                             { "param", link.target.param },
                                                             { "value", value } }));
            changed = true;
        }

        if (! changed)
            break;
    }

    return result;
}

// Sets a parameter; one that follows a link sets the parameter at the root of its chain instead, through the
// mappings' inverses, so either end of a link can be edited (L4).
[[nodiscard]] Project setNodeParam (const Project& project, const juce::var& args)
{
    auto address = ParamAddress { requireId (args, "trackId"), requireId (args, "nodeId"), args["param"].toString() };
    const auto* driving = project.linkDriving (address);

    if (driving == nullptr || ! driving->active)
        return setNodeParamHere (project, args);

    auto value = static_cast<float> (requireNumber (args, "value"));

    for (size_t steps = 0; driving != nullptr && driving->active && steps <= project.links.size(); ++steps)
    {
        const auto* source = linkNodeOf (project, driving->source);
        const auto* target = linkNodeOf (project, driving->target);

        if (source == nullptr || target == nullptr)
            break;

        if (nodetype::isPlugin (source->type))
            throw CommandError ("This parameter follows a plugin's parameter; change that one");

        const auto sourceInfo = linkInfoOf (*source, driving->source.param);
        const auto targetInfo = linkInfoOf (*target, driving->target.param);
        const auto inverse = invertLinkValue (driving->mapping, sourceInfo ? &*sourceInfo : nullptr,
                                              targetInfo ? &*targetInfo : nullptr, value);

        if (! inverse.has_value())
            throw CommandError ("This parameter follows a link that can't be turned back; change its source");

        value = *inverse;
        address = driving->source;
        driving = project.linkDriving (address);
    }

    return setNodeParamHere (project, makeObject ({ { "trackId", address.trackId },
                                                    { "nodeId", address.nodeId },
                                                    { "param", address.param },
                                                    { "value", value } }));
}

// Links a parameter to another (L1, L14): { source, target, mapping?, active? }.
[[nodiscard]] Project addLink (const Project& project, const juce::var& args)
{
    ParamLink link;
    link.source = addressFromVar (args["source"], true);
    link.target = addressFromVar (args["target"], true);
    link.mapping = args["mapping"].isObject() ? linkMappingFromVar (args["mapping"]) : LinkMapping {};
    link.active = ! args.hasProperty ("active") || static_cast<bool> (args["active"]);

    auto result = project;
    link.id = result.nextId++;
    checkLink (result, link);
    result.links.push_back (std::move (link));
    return result;
}

// Changes a link's mapping or switches it: { id, mapping?, active? }.
[[nodiscard]] Project setLink (const Project& project, const juce::var& args)
{
    const auto id = requireId (args, "id");
    auto result = project;
    const auto it = std::ranges::find_if (result.links, [id] (const ParamLink& l) { return l.id == id; });

    if (it == result.links.end())
        throw CommandError ("No link with id " + juce::String (id));

    if (args["mapping"].isObject())
        it->mapping = linkMappingFromVar (args["mapping"]);

    if (args.hasProperty ("active"))
        it->active = static_cast<bool> (args["active"]);

    checkLink (result, *it);
    return result;
}

// Removes links: { id } or { ids }.
[[nodiscard]] Project removeLink (const Project& project, const juce::var& args)
{
    std::set<ObjectId> ids;

    if (const auto* list = args["ids"].getArray())
        for (const auto& id : *list)
            ids.insert (static_cast<ObjectId> (static_cast<juce::int64> (id)));
    else
        ids.insert (requireId (args, "id"));

    auto result = project;

    if (std::erase_if (result.links, [&] (const ParamLink& l) { return ids.contains (l.id); }) == 0)
        throw CommandError ("No such link");

    return result;
}

// Whether a parameter mirrors inverted in "Mirror, gains inverted" (L5; question 8): a level in dB whose range is
// symmetric around 0.
[[nodiscard]] bool isSymmetricLevel (const ParamInfo& info)
{
    return std::string_view (info.unit) == "dB" && info.kind == ParamKind::continuous && info.minValue < 0.0f
           && juce::approximatelyEqual (info.minValue, -info.maxValue);
}

// Links every parameter of a node to the same parameter of another of its type (L5): { source: { trackId, nodeId },
// target: { trackId, nodeId }, preset: "mirror" | "mirrorInverted" }. Links between the two nodes are replaced;
// parameters that already follow something else are left as they are.
[[nodiscard]] Project linkNodes (const Project& project, const juce::var& args)
{
    const auto from = addressFromVar (args["source"], false);
    const auto to = addressFromVar (args["target"], false);
    const auto preset = args["preset"].toString();
    const auto* source = linkNodeOf (project, from);
    const auto* target = linkNodeOf (project, to);

    if (source == nullptr || target == nullptr)
        throw CommandError ("No such node");

    if (source->id == target->id)
        throw CommandError ("Pick another device to link");

    if (source->type != target->type || nodetype::isPlugin (source->type) || source->type == nodetype::faust)
        throw CommandError ("Only two built-in devices of the same kind can be linked as a whole");

    if (preset != "mirror" && preset != "mirrorInverted")
        throw CommandError ("Unknown link preset '" + preset + "'");

    auto result = project;
    std::erase_if (result.links,
                   [&] (const ParamLink& l)
                   {
                       return l.source.nodeId == from.nodeId && l.target.nodeId == to.nodeId
                              && l.source.trackId == from.trackId && l.target.trackId == to.trackId;
                   });

    // A pair that mirrors its gains inverted cancels only when its cuts are the reciprocals of its boosts: the EQ's
    // Cuts goes to "Mirror boosts" on the source, and the link copies it to the target (section 10).
    if (preset == "mirrorInverted" && requireDescriptor (source->type).indexOfParam ("cuts") >= 0)
        result = setNodeParamHere (
            result,
            makeObject (
                { { "trackId", from.trackId }, { "nodeId", from.nodeId }, { "param", "cuts" }, { "value", 1.0 } }));

    for (const auto& info : requireDescriptor (source->type).params)
    {
        ParamLink link;
        link.source = { from.trackId, from.nodeId, info.id };
        link.target = { to.trackId, to.nodeId, info.id };

        if (result.linkDriving (link.target) != nullptr)
            continue;

        if (preset == "mirrorInverted" && isSymmetricLevel (info))
            link.mapping.kind = LinkMappingKind::inverted;

        link.id = result.nextId++;
        checkLink (result, link);
        result.links.push_back (std::move (link));
    }

    return result;
}

// Replaces a Faust node's script (docs/design/phase11-faust-panels.md, F4). Parameter values stay, by path; the
// engine compiles the script in the background and keeps the old one running if it fails.
[[nodiscard]] Project setNodeCode (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");

    if (! args["code"].isString())
        throw CommandError ("Missing code");

    const auto code = args["code"].toString();

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::faust)
                                throw CommandError ("Only Faust nodes have code");

                            node.code = code;
                        });
}

// Sets the pool file a node reads (docs/design/convolution-reverb.md, I10): `file` pools a new one, `fileId` names one
// in the pool, and `fileId` 0 removes it.
[[nodiscard]] Project setNodeFile (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");

    if (! args.hasProperty ("file") && ! args.hasProperty ("fileId"))
        throw CommandError ("Missing file");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project& result)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (! nodetype::usesAudioFile (node.type))
                                throw CommandError ("This node doesn't read a file");

                            if (args.hasProperty ("file"))
                            {
                                node.fileId = poolAudioFile (result, args["file"]);
                                return;
                            }

                            const auto fileId = static_cast<ObjectId> (static_cast<juce::int64> (args["fileId"]));

                            if (fileId != 0 && result.findAudioFile (fileId) == nullptr)
                                throw CommandError ("No audio file with id " + juce::String (fileId));

                            node.fileId = fileId;
                        });
}

// The zones of the Sampler a command names.
[[nodiscard]] std::vector<SampleZone>& requireSampleZones (GraphScope& scope, const juce::var& args)
{
    auto& node = requireNode (scope.graph, requireId (args, "nodeId"));

    if (! nodetype::playsSamples (node.type))
        throw CommandError ("Only a Sampler has sample zones");

    return node.sampleZones;
}

// New zones from imported files (docs/design/sampler-midi-delay-limiter.md, section 4). On one key if `key` says so, with
// further files on the keys above; a single file with no key covers the keyboard; several take a key each from a kick's.
// A zone on a key goes first, since a note plays the first zone holding it. With `replaceZone`, the first file takes that
// zone's place instead, its sample range starting over.
[[nodiscard]] Project addSampleZones (const Project& project, const juce::var& args)
{
    const auto* files = args["files"].getArray();

    if (files == nullptr || files->isEmpty())
        throw CommandError ("Missing files");

    const auto wholeKeyboard = files->size() == 1 && ! args.hasProperty ("key");
    auto key = args.hasProperty ("key") ? requireInt (args, "key") : 36;

    return updateGraph (
        project, args,
        [&] (GraphScope& scope, Project& result)
        {
            auto& zones = requireSampleZones (scope, args);

            // Zones planned from the files' names and pitches (docs/design/sampler-mapping.md, SM9), added after the
            // zones there are; each file goes into the pool once.
            if (const auto* planned = args["zones"].getArray())
            {
                std::map<int, ObjectId> pooled;

                for (const auto& fields : *planned)
                {
                    const auto file = requireInt (fields, "file");

                    if (file < 0 || file >= files->size())
                        throw CommandError ("No file " + juce::String (file) + " for a zone");

                    if (! pooled.contains (file))
                        pooled[file] = poolAudioFile (result, (*files)[file]);

                    // Any of a zone's fields, as setSampleZone takes them (an SFZ region's loop and envelope).
                    SampleZone zone;
                    zone.id = result.nextId++;
                    zone.fileId = pooled[file];
                    projectfile::applySampleZoneFields (fields, zone);
                    zone.highKey = std::max (zone.lowKey, zone.highKey);
                    zone.highVelocity = std::max (zone.lowVelocity, zone.highVelocity);
                    zones.push_back (zone);
                }

                return;
            }
            auto first = true;
            auto inserted = zones.begin();

            for (const auto& fields : *files)
            {
                const auto replaced = first && args.hasProperty ("replaceZone")
                                          ? std::ranges::find (zones, requireId (args, "replaceZone"), &SampleZone::id)
                                          : zones.end();
                first = false;

                if (replaced != zones.end())
                {
                    replaced->fileId = poolAudioFile (result, fields);
                    replaced->start = replaced->end = 0;
                    replaced->loopStart = replaced->loopEnd = replaced->crossfade = 0;
                    ++key;
                    inserted = replaced;
                    continue;
                }

                SampleZone zone;
                zone.id = result.nextId++;
                zone.fileId = poolAudioFile (result, fields);

                if (! wholeKeyboard)
                    zone.lowKey = zone.highKey = zone.rootKey = std::clamp (key++, 0, 127);

                inserted = wholeKeyboard ? zones.insert (zones.end(), zone) : zones.insert (inserted, zone) + 1;
            }
        });
}

// Points an Output to track node at one of its track's output tracks, or at none with 0
// (docs/design/multi-output-instruments.md, MO3). With `newTrack` (a name), at a new output track, in the same step.
[[nodiscard]] Project setNodeTarget (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    auto target = static_cast<ObjectId> (args.getProperty ("target", 0));
    auto base = project;

    if (args.hasProperty ("newTrack"))
    {
        base = addTrack (
            project,
            makeObject ({ { "kind", "output" }, { "source", args["trackId"] }, { "name", args["newTrack"] } }));
        target = project.nextId; // the new track's id: the first one addTrack handed out
    }

    return updateGraph (base, args,
                        [&] (GraphScope& scope, Project& result)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::busOutput)
                                throw CommandError ("Only an Output to track node has a target track");

                            if (target != 0)
                            {
                                const auto index = result.indexOfTrack (target);

                                if (index < 0 || result.tracks[static_cast<size_t> (index)]->kind != TrackKind::output
                                    || result.tracks[static_cast<size_t> (index)]->source != scope.track.id)
                                    throw CommandError ("An Output to track feeds an output track of its own track");
                            }

                            node.target = target;
                        });
}

// An output track of `track` named `name`, fed by a new Output to track node wired from port `port` of node `nodeId`
// (docs/design/multi-output-instruments.md, MO5). The node goes right of that node, in row `row`.
[[nodiscard]] Track wireOutputTrack (Project& result, Track& track, ObjectId nodeId, int port, const juce::String& name,
                                     int row)
{
    const auto& from = requireNode (track.graph, nodeId);
    Track out;
    out.id = result.nextId++;
    out.kind = TrackKind::output;
    out.source = track.id;
    out.parent = track.parent;
    out.colour = track.colour;
    out.name = name.trim().isNotEmpty() ? name.trim() : track.name + " " + juce::String (port + 1);
    makeBusGraph (out, result);

    auto node = makeNode (result, nodetype::busOutput, from.x + 240.0f, from.y + 90.0f * static_cast<float> (row));
    node.target = out.id;
    track.graph.connections.push_back ({ nodeId, port, node.id, 0 });
    track.graph.nodes.push_back (std::move (node));
    return out;
}

// Puts new output tracks right after `index`'s track and its output tracks there.
[[nodiscard]] Project withOutputTracks (Project result, size_t index, Track track, std::vector<Track> added)
{
    result.tracks[index] = std::make_shared<const Track> (std::move (track));
    auto at = endAfter (result.tracks, index);

    for (auto& out : added)
        result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (at++),
                              std::make_shared<const Track> (std::move (out)));

    normaliseGroups (result);
    return result;
}

// Gives groups of a Sampler's zones, or of a Drum Synth's pads, output tracks of their own, in one step
// (docs/design/multi-output-instruments.md, MO5; docs/design/drum-synth.md, DR9): for each of `outputs` ({ name,
// zoneIds } or { name, pads }), the next free output, an output track named after it, and an Output to track node
// wired from that output to the track.
[[nodiscard]] Project splitInstrumentOutputs (const Project& project, const juce::var& args)
{
    const auto trackId = requireId (args, "trackId");
    const auto nodeId = requireId (args, "nodeId");
    const auto* outputs = args["outputs"].getArray();
    const auto index = project.indexOfTrack (trackId);

    if (outputs == nullptr || outputs->isEmpty())
        throw CommandError ("Missing outputs");

    if (index < 0)
        throw CommandError ("No track with id " + juce::String (trackId));

    auto result = project;
    auto track = *project.tracks[static_cast<size_t> (index)];
    auto& instrument = requireNode (track.graph, nodeId);
    const auto drums = instrument.type == nodetype::drumSynth;

    if (instrument.type != nodetype::sampler && ! drums)
        throw CommandError ("Only a Sampler's zones and a Drum Synth's pads have outputs");

    if (drums)
        instrument.drumPads = drumPadsOf (instrument);

    const auto inUse = [&] (int output)
    {
        return drums ? std::ranges::any_of (instrument.drumPads, [&] (const DrumPad& p) { return p.output == output; })
                     : std::ranges::any_of (instrument.sampleZones,
                                            [&] (const SampleZone& z) { return z.output == output; });
    };

    // The outputs nothing plays from and nothing is wired to.
    const auto most = drums ? maxDrumOutputs : maxSampleOutputs;
    std::vector<int> free;

    for (int output = 2; output <= most; ++output)
        if (! inUse (output)
            && std::ranges::none_of (track.graph.connections, [&] (const Connection& c)
                                     { return c.fromNode == nodeId && c.fromPort == output - 1; }))
            free.push_back (output);

    if (static_cast<size_t> (outputs->size()) > free.size())
        throw CommandError ((drums ? "A Drum Synth" : "A Sampler") + juce::String (" has ") + juce::String (most - 1)
                            + " outputs besides Main; " + juce::String (free.size()) + " are free");

    std::vector<Track> added;

    for (int k = 0; k < outputs->size(); ++k)
    {
        const auto& entry = (*outputs)[k];
        const auto output = free[static_cast<size_t> (k)];
        auto& node = requireNode (track.graph, nodeId);

        if (const auto* zoneIds = entry["zoneIds"].getArray())
            for (const auto& id : *zoneIds)
                for (auto& zone : node.sampleZones)
                    if (zone.id == static_cast<ObjectId> (static_cast<juce::int64> (id)))
                        zone.output = output;

        if (const auto* pads = entry["pads"].getArray())
            for (const auto& pad : *pads)
            {
                const auto i = static_cast<int> (pad);

                if (! drums || i < 0 || i >= drumPadCount)
                    throw CommandError ("No pad " + juce::String (i));

                node.drumPads[static_cast<size_t> (i)].output = output;
            }

        added.push_back (wireOutputTrack (result, track, nodeId, output - 1, entry["name"].toString(), k + 1));
    }

    return withOutputTracks (std::move (result), static_cast<size_t> (index), std::move (track), std::move (added));
}

// Turns all of an instrument plugin's output buses on, or leaves them as the plugin starts
// (docs/design/multi-output-instruments.md, MO1).
[[nodiscard]] Project setPluginOutputs (const Project& project, const juce::var& args)
{
    requireTrackScope (args);
    const auto nodeId = requireId (args, "nodeId");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::pluginInstrument)
                                throw CommandError ("Only instrument plugins have output buses");

                            node.allOutputs = static_cast<bool> (args["all"]);
                        });
}

// A Band Split's number of bands (docs/design/split-nodes.md, SP1): cables from the bands that go away are removed in
// the same step, and crossovers left at the old count's defaults move to the new one's. The Dry port stays.
[[nodiscard]] Project setBandCount (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto bands = requireInt (args, "bands");

    if (bands < 2 || bands > 4)
        throw CommandError ("A Band Split has 2 to 4 bands");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::bandSplit)
                                throw CommandError ("Only a Band Split has bands");

                            const auto defaults = [] (int count) -> std::array<double, 3>
                            {
                                if (count == 2)
                                    return { 1000.0, 2500.0, 6000.0 };

                                if (count == 4)
                                    return { 120.0, 1000.0, 6000.0 };

                                return { 200.0, 2500.0, 6000.0 };
                            };
                            const auto before = defaults (node.bands);
                            const auto after = defaults (bands);
                            const std::array<const char*, 3> ids { "crossover1", "crossover2", "crossover3" };

                            std::array<double, 3> values {};

                            for (size_t i = 0; i < ids.size(); ++i)
                            {
                                const auto it = node.params.find (ids[i]);
                                values[i] = it == node.params.end() || std::abs (it->second - before[i]) < 1.0e-6
                                                ? after[i]
                                                : it->second;
                            }

                            // The crossovers in use stay in order, a third of an octave apart (SP2).
                            const auto gap = std::cbrt (2.0);
                            const auto used = static_cast<size_t> (bands - 1);

                            for (size_t i = 1; i < used; ++i)
                                values[i] = std::max (values[i], values[i - 1] * gap);

                            for (size_t i = used; i-- > 0;)
                                values[i] =
                                    std::min (values[i], 20000.0 / std::pow (gap, static_cast<double> (used - 1 - i)));

                            for (size_t i = 0; i < ids.size(); ++i)
                                node.params[ids[i]] = values[i];

                            node.bands = bands;
                            std::erase_if (scope.graph.connections, [&] (const Connection& c)
                                           { return c.fromNode == nodeId && c.fromPort >= bands && c.fromPort < 4; });
                        });
}

// Mid and side, or left and right, for a Stereo Split or Merge (SP4, SP5). Both have two parts, so cables stay.
[[nodiscard]] Project setStereoMode (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto mode = args["mode"].toString();

    if (mode != "midSide" && mode != "leftRight")
        throw CommandError ("The mode is midSide or leftRight");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::stereoSplit && node.type != nodetype::stereoMerge)
                                throw CommandError ("Only a Stereo Split or Merge has a mode");

                            node.stereoMode = mode == "leftRight" ? StereoMode::leftRight : StereoMode::midSide;
                        });
}

// Chooses the main bus layouts an effect plugin may take (docs/design/mono-ports.md, MP4).
[[nodiscard]] Project setPluginChannels (const Project& project, const juce::var& args)
{
    requireTrackScope (args);
    const auto nodeId = requireId (args, "nodeId");
    const auto name = args["channels"].toString();

    if (name != "auto" && name != "monoToStereo" && name != "stereo")
        throw CommandError ("Channels must be auto, monoToStereo or stereo");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (node.type != nodetype::pluginEffect)
                                throw CommandError ("Only effect plugins have a Channels setting");

                            node.pluginChannels = pluginChannelsFromName (name);
                        });
}

// Gives an instrument plugin's outputs tracks of their own, in one step (MO5): turns all its output buses on, and for
// each of `outputs` ({ port, name }; port 1 is Out 2) not yet wired, an output track and an Output to track node wired
// from that port.
[[nodiscard]] Project splitPluginOutputs (const Project& project, const juce::var& args)
{
    const auto trackId = requireId (args, "trackId");
    const auto nodeId = requireId (args, "nodeId");
    const auto* outputs = args["outputs"].getArray();
    const auto index = project.indexOfTrack (trackId);

    if (outputs == nullptr || outputs->isEmpty())
        throw CommandError ("Missing outputs");

    if (index < 0)
        throw CommandError ("No track with id " + juce::String (trackId));

    auto result = project;
    auto track = *project.tracks[static_cast<size_t> (index)];
    auto& plugin = requireNode (track.graph, nodeId);

    if (plugin.type != nodetype::pluginInstrument)
        throw CommandError ("Only instrument plugins have output buses");

    plugin.allOutputs = true;
    std::vector<Track> added;

    for (const auto& entry : *outputs)
    {
        const auto port = static_cast<int> (entry["port"]);

        if (port < 1 || port >= maxPluginOutputs)
            throw CommandError ("An instrument plugin's extra outputs are ports 1 to "
                                + juce::String (maxPluginOutputs - 1));

        if (std::ranges::any_of (track.graph.connections,
                                 [&] (const Connection& c) { return c.fromNode == nodeId && c.fromPort == port; }))
            continue;

        added.push_back (wireOutputTrack (result, track, nodeId, port, entry["name"].toString(),
                                          static_cast<int> (added.size()) + 1));
    }

    return withOutputTracks (std::move (result), static_cast<size_t> (index), std::move (track), std::move (added));
}

[[nodiscard]] Project setSampleZone (const Project& project, const juce::var& args)
{
    const auto zoneId = requireId (args, "zoneId");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& zones = requireSampleZones (scope, args);
                            const auto zone = std::ranges::find (zones, zoneId, &SampleZone::id);

                            if (zone == zones.end())
                                throw CommandError ("No sample zone with id " + juce::String (zoneId));

                            projectfile::applySampleZoneFields (args, *zone);
                        });
}

[[nodiscard]] Project removeSampleZone (const Project& project, const juce::var& args)
{
    const auto zoneId = requireId (args, "zoneId");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& zones = requireSampleZones (scope, args);

                            if (std::erase_if (zones, [zoneId] (const SampleZone& z) { return z.id == zoneId; }) == 0)
                                throw CommandError ("No sample zone with id " + juce::String (zoneId));
                        });
}

// ---------------------------------------------------------------------------------------------------------
// Drum Synth pads (docs/design/drum-synth.md, DR2, DR4, DR14)

// The Drum Synth a command names, with its pads filled in (an empty list is the default kit).
[[nodiscard]] GraphNode& requireDrumSynth (GraphScope& scope, ObjectId nodeId)
{
    auto& node = requireNode (scope.graph, nodeId);

    if (node.type != nodetype::drumSynth)
        throw CommandError ("Only a Drum Synth has pads");

    node.drumPads = drumPadsOf (node);
    return node;
}

[[nodiscard]] int requirePad (const juce::var& args, const char* key)
{
    if (! args.hasProperty (key))
        throw CommandError ("Missing " + juce::String (key));

    const auto pad = static_cast<int> (args[key]);

    if (pad < 0 || pad >= drumPadCount)
        throw CommandError ("No pad " + juce::String (pad));

    return pad;
}

// A pad parameter's value: the node's, else its default.
[[nodiscard]] float drumValue (const GraphNode& node, const juce::String& id)
{
    if (const auto it = node.params.find (id); it != node.params.end())
        return static_cast<float> (it->second);

    const auto& descriptor = drumSynthDescriptor();
    return descriptor.params[static_cast<size_t> (descriptor.indexOfParam (id))].defaultValue;
}

// The ids of a pad's parameters: the common ones, then every model's own, in the descriptor's order.
[[nodiscard]] std::vector<juce::String> padParamIds (int pad)
{
    std::vector<juce::String> ids;

    for (const auto& info : drumCommonParams())
        ids.push_back (drumParamId (pad, DrumModel::off, info.id));

    for (size_t m = 1; m < drumModelCount; ++m)
        for (const auto& info : drumModelParams (static_cast<DrumModel> (m)))
            ids.push_back (drumParamId (pad, static_cast<DrumModel> (m), info.id));

    return ids;
}

// Sets a pad's fields: any of name, model, output, chokeGroup, mono, locked. Its parameters stay as they are, every
// model's (DR3 as built).
[[nodiscard]] Project setDrumPad (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto index = requirePad (args, "pad");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& pad = requireDrumSynth (scope, nodeId).drumPads[static_cast<size_t> (index)];

                            if (args.hasProperty ("name"))
                                pad.name = args["name"].toString().trim();

                            if (args.hasProperty ("model"))
                            {
                                const auto model = drumModelFromId (args["model"].toString());

                                if (! model.has_value())
                                    throw CommandError ("Unknown drum model '" + args["model"].toString() + "'");

                                pad.model = *model;
                            }

                            if (args.hasProperty ("output"))
                            {
                                const auto output = static_cast<int> (args["output"]);

                                if (output < 1 || output > maxDrumOutputs)
                                    throw CommandError ("A pad's output is 1 to " + juce::String (maxDrumOutputs));

                                pad.output = output;
                            }

                            if (args.hasProperty ("chokeGroup"))
                            {
                                const auto group = static_cast<int> (args["chokeGroup"]);

                                if (group < 0 || group > maxDrumChokeGroup)
                                    throw CommandError ("A choke group is 0 to " + juce::String (maxDrumChokeGroup));

                                pad.chokeGroup = group;
                            }

                            if (args.hasProperty ("mono"))
                                pad.mono = static_cast<bool> (args["mono"]);

                            if (args.hasProperty ("locked"))
                                pad.locked = static_cast<bool> (args["locked"]);
                        });
}

// Copies a pad onto another: its name, model, choke group, mono and every parameter. The target keeps its output and
// its lock, so a split kit stays split.
[[nodiscard]] Project copyDrumPad (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto from = requirePad (args, "from");
    const auto to = requirePad (args, "to");

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireDrumSynth (scope, nodeId);
                            const auto source = node.drumPads[static_cast<size_t> (from)];
                            auto& target = node.drumPads[static_cast<size_t> (to)];
                            target = { source.name,       source.model, target.output,
                                       source.chokeGroup, source.mono,  target.locked };

                            const auto sources = padParamIds (from);
                            const auto targets = padParamIds (to);
                            std::vector<float> values;

                            for (const auto& id : sources)
                                values.push_back (drumValue (node, id));

                            for (size_t i = 0; i < targets.size(); ++i)
                                node.params[targets[i]] = values[i];
                        });
}

// Randomises pads (DR14). Mode "randomise" picks each of a pad's model's parameters within its safe range, and Tune
// within 2 semitones; "vary" moves each by up to a tenth of its safe range, Tune by up to half a semitone and Decay by
// up to a tenth of itself. Level, Pan, Velocity and the pad's fields stay. `pads` lists the pads; without it, every
// pad that plays and isn't locked. `seed` makes it repeatable.
[[nodiscard]] Project randomiseDrumPads (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto mode = args.getProperty ("mode", "randomise").toString();

    if (mode != "randomise" && mode != "vary")
        throw CommandError ("Unknown randomise mode '" + mode + "'");

    const auto vary = mode == "vary";
    juce::Random random (args.hasProperty ("seed") ? static_cast<juce::int64> (args["seed"])
                                                   : juce::Time::currentTimeMillis());

    return updateGraph (
        project, args,
        [&] (GraphScope& scope, Project&)
        {
            auto& node = requireDrumSynth (scope, nodeId);
            std::vector<int> chosen;

            if (const auto* list = args["pads"].getArray())
            {
                for (const auto& entry : *list)
                {
                    const auto pad = static_cast<int> (entry);

                    if (pad < 0 || pad >= drumPadCount)
                        throw CommandError ("No pad " + juce::String (pad));

                    chosen.push_back (pad);
                }
            }
            else
            {
                for (int pad = 0; pad < drumPadCount; ++pad)
                    if (const auto& p = node.drumPads[static_cast<size_t> (pad)];
                        ! p.locked && p.model != DrumModel::off)
                        chosen.push_back (pad);
            }

            const auto between = [&] (float low, float high) { return low + (high - low) * random.nextFloat(); };

            for (const auto pad : chosen)
            {
                const auto model = node.drumPads[static_cast<size_t> (pad)].model;
                const auto params = drumModelParams (model);
                const auto ranges = drumModelRandomRanges (model);

                for (size_t i = 0; i < params.size(); ++i)
                {
                    const auto& info = params[i];
                    const auto& range = ranges[i];

                    if (range.kept())
                        continue;

                    const auto id = drumParamId (pad, model, info.id);
                    const auto width = range.high - range.low;
                    auto value =
                        vary ? drumValue (node, id) + between (-0.1f, 0.1f) * width : between (range.low, range.high);
                    value = std::clamp (value, info.minValue, info.maxValue);

                    if (info.kind == ParamKind::integer)
                        value = std::round (value);

                    node.params[id] = value;
                }

                const auto tune = drumParamId (pad, DrumModel::off, "tune");
                node.params[tune] = vary ? std::clamp (drumValue (node, tune) + between (-0.5f, 0.5f), -24.0f, 24.0f)
                                         : between (-2.0f, 2.0f);

                if (vary)
                {
                    const auto decay = drumParamId (pad, DrumModel::off, "decay");
                    node.params[decay] = std::clamp (drumValue (node, decay) * between (0.9f, 1.1f), 10.0f, 400.0f);
                }
            }
        });
}

[[nodiscard]] Project setNodePosition (const Project& project, const juce::var& args)
{
    const auto nodeId = requireId (args, "nodeId");
    const auto x = static_cast<float> (requireNumber (args, "x"));
    const auto y = static_cast<float> (requireNumber (args, "y"));

    return updateGraph (project, args,
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);
                            node.x = x;
                            node.y = y;
                        });
}

// Replaces an MPE Synth with a Voice Instrument with the default voice graph, at the same place in the graph with the
// same id and connections (docs/design/phase12-voice-scope.md, V4). The synth's parameters, their modulation inputs
// and automation go: the Voice Instrument has none.
[[nodiscard]] Project convertToVoiceGraph (const Project& project, const juce::var& args)
{
    requireTrackScope (args);
    const auto nodeId = requireId (args, "nodeId");

    return updateTrackOrMaster (
        project, requireId (args, "trackId"),
        [&] (Track& track, Project& result)
        {
            auto& node = requireNode (track.graph, nodeId);

            if (node.type != nodetype::mpeSynth && node.type != nodetype::sampler)
                throw CommandError ("Only an MPE Synth or a Sampler can be converted to a voice graph");

            forgetNode (node, track, result);
            node.voiceGraph =
                node.type == nodetype::sampler ? makeSamplerVoiceGraph (result, node) : makeDefaultVoiceGraph (result);
            node.type = nodetype::voiceInstrument;
            node.params.clear();
            node.modulations.clear();
            node.sampleZones.clear();

            // Port 0 takes the notes as before; the modulation inputs have nothing to aim at.
            std::erase_if (track.graph.connections,
                           [&] (const Connection& c) { return c.toNode == nodeId && c.toPort > 0; });
        });
}

// ---------------------------------------------------------------------------------------------------------
// Automation (docs/design/phase10-automation.md)

[[nodiscard]] AutomationTarget parseTarget (const juce::var& value, const Track& track)
{
    AutomationTarget target;
    const auto kind = value["kind"].toString();
    target.param = value["param"].toString();
    target.name = value["name"].toString();

    if (kind == "gain" || kind == "pan")
    {
        target.kind = kind == "gain" ? AutomationTargetKind::gain : AutomationTargetKind::pan;
        target.param = kind;
        return target;
    }

    if (kind == "send")
    {
        // A send's level (R8): param names the send.
        target.kind = AutomationTargetKind::send;

        if (std::ranges::none_of (track.sends, [&] (const Send& s) { return juce::String (s.id) == target.param; }))
            throw CommandError ("No send '" + target.param + "' on this track");

        return target;
    }

    target.nodeId = requireId (value, "nodeId");
    const auto* found = track.findNode (target.nodeId); // a voice-graph node too (phase12-voice-scope.md, V11)

    if (found == nullptr)
        throw CommandError ("No node with id " + juce::String (target.nodeId));

    const auto& node = *found;

    if (kind == "node")
    {
        target.kind = AutomationTargetKind::nodeParam;

        const auto info = paramOf (node, target.param);

        if (! info.has_value())
            throw CommandError ("Unknown parameter '" + target.param + "'");

        if (! info->acceptsAutomation())
            throw CommandError ("'" + target.param + "' can't be automated: it changes the node's latency");

        return target;
    }

    if (kind == "plugin")
    {
        target.kind = AutomationTargetKind::pluginParam;

        if (! node.plugin.has_value() || target.param.isEmpty())
            throw CommandError ("Plugin parameters need a plugin node and a parameter id");

        return target;
    }

    throw CommandError ("Unknown automation target '" + kind + "'");
}

[[nodiscard]] AutomationLane& requireLane (Track& track, ObjectId laneId)
{
    const auto it = std::ranges::find (track.automation, laneId, &AutomationLane::id);

    if (it == track.automation.end())
        throw CommandError ("No automation lane with id " + juce::String (laneId));

    return *it;
}

[[nodiscard]] Project addAutomationLane (const Project& project, const juce::var& args)
{
    return updateTrackOrMaster (project, requireId (args, "trackId"),
                                [&] (Track& track, Project& result)
                                {
                                    AutomationLane lane;
                                    lane.target = parseTarget (args["target"], track);

                                    if (std::ranges::any_of (track.automation, [&] (const AutomationLane& l)
                                                             { return l.target.sameAs (lane.target); }))
                                        throw CommandError ("This parameter already has a lane");

                                    lane.id = result.nextId++;

                                    if (args.hasProperty ("points"))
                                        lane.points = curveFromVar (args["points"], 0.0, 1.0);

                                    track.automation.push_back (std::move (lane));
                                    track.automationExpanded = true;
                                });
}

[[nodiscard]] Project removeAutomationLane (const Project& project, const juce::var& args)
{
    const auto laneId = requireId (args, "laneId");

    return updateTrackOrMaster (
        project, requireId (args, "trackId"),
        [&] (Track& track, Project&)
        {
            if (std::erase_if (track.automation, [&] (const AutomationLane& l) { return l.id == laneId; }) == 0)
                throw CommandError ("No automation lane with id " + juce::String (laneId));
        });
}

[[nodiscard]] Project setAutomationLane (const Project& project, const juce::var& args)
{
    return updateTrackOrMaster (project, requireId (args, "trackId"),
                                [&] (Track& track, Project&)
                                {
                                    auto& lane = requireLane (track, requireId (args, "laneId"));

                                    if (args.hasProperty ("active"))
                                        lane.active = static_cast<bool> (args["active"]);

                                    if (args.hasProperty ("height"))
                                        lane.height = std::clamp (requireInt (args, "height"), limits::minTrackHeight,
                                                                  limits::maxTrackHeight);
                                });
}

[[nodiscard]] Project setAutomationPoints (const Project& project, const juce::var& args)
{
    const auto points = curveFromVar (args["points"], 0.0, 1.0);

    return updateTrackOrMaster (project, requireId (args, "trackId"), [&] (Track& track, Project&)
                                { requireLane (track, requireId (args, "laneId")).points = points; });
}

[[nodiscard]] Project writeAutomation (const Project& project, const juce::var& args)
{
    const auto from = requireTicks (args, "from", 0, limits::maxPosition);
    const auto to = requireTicks (args, "to", from, limits::maxPosition);
    const auto points = curveFromVar (args["points"], 0.0, 1.0);

    return updateTrackOrMaster (project, requireId (args, "trackId"),
                                [&] (Track& track, Project&)
                                {
                                    // The written range replaces what was there (A8).
                                    auto& lane = requireLane (track, requireId (args, "laneId"));
                                    std::erase_if (lane.points, [&] (const CurvePoint& p)
                                                   { return p.time >= from && p.time <= to; });

                                    for (const auto& point : points)
                                        if (point.time >= from && point.time <= to)
                                            lane.points.push_back (point);

                                    std::ranges::sort (lane.points, {}, &CurvePoint::time);
                                });
}

[[nodiscard]] Project setTrackAutomation (const Project& project, const juce::var& args)
{
    return updateTrackOrMaster (project, requireId (args, "id"),
                                [&] (Track& track, Project&)
                                {
                                    if (args.hasProperty ("mode"))
                                        track.automationMode =
                                            static_cast<AutomationMode> (std::clamp (requireInt (args, "mode"), 0, 4));

                                    if (args.hasProperty ("expanded"))
                                        track.automationExpanded = static_cast<bool> (args["expanded"]);
                                });
}

[[nodiscard]] Project setClipController (const Project& project, const juce::var& args)
{
    const auto controller = requireInt (args, "controller");

    if (controller < 0 || controller > controllerKeyPressure)
        throw CommandError ("Unknown controller " + juce::String (controller));

    const auto points = curveFromVar (args["points"], 0.0, 1.0);

    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project&)
                       {
                           if (clip.fileId != 0)
                               throw CommandError ("Audio clips have no controllers");

                           if (points.empty())
                               clip.controllers.erase (controller);
                           else
                               clip.controllers[controller] = points;
                       });
}

// ---------------------------------------------------------------------------------------------------------
// Time stretching (docs/design/time-stretching.md)

[[nodiscard]] AudioFile& requireAudioFile (Project& project, ObjectId fileId)
{
    const auto it = std::ranges::find_if (project.audioFiles, [fileId] (const AudioFile& f) { return f.id == fileId; });

    if (it == project.audioFiles.end())
        throw CommandError ("No audio file with id " + juce::String (fileId));

    return *it;
}

// Seconds per content tick of a one-shot clip: the tick length at the tempo where it starts (TM8).
[[nodiscard]] double oneShotSecondsPerTick (const TempoMap& map, Ticks start)
{
    return 60.0 / (map.bpmAt (static_cast<double> (start)) * static_cast<double> (ticksPerBeat));
}

[[nodiscard]] Ticks roundTicks (double ticks)
{
    return static_cast<Ticks> (std::llround (ticks));
}

// Fade lengths through the timeline: a fade keeps the seconds it covers. `toMusical` converts one-shot content ticks
// (at secondsPerTick) to timeline ticks, and back otherwise.
void convertFades (NoteClip& clip, const TempoMap& map, double secondsPerTick, bool toMusical)
{
    const auto start = static_cast<double> (clip.start);
    const auto end = static_cast<double> (clip.start + clip.length);
    const auto fadeIn = static_cast<double> (clip.fadeIn);
    const auto fadeOut = static_cast<double> (clip.fadeOut);

    if (toMusical)
    {
        clip.fadeIn =
            std::max (Ticks { 0 }, roundTicks (map.tickAt (map.secondsAt (start) + fadeIn * secondsPerTick) - start));
        clip.fadeOut =
            std::max (Ticks { 0 }, roundTicks (end - map.tickAt (map.secondsAt (end) - fadeOut * secondsPerTick)));
    }
    else
    {
        clip.fadeIn = roundTicks ((map.secondsAt (start + fadeIn) - map.secondsAt (start)) / secondsPerTick);
        clip.fadeOut = roundTicks ((map.secondsAt (end) - map.secondsAt (end - fadeOut)) / secondsPerTick);
    }
}

// Converts a clip's content positions between one-shot ticks and musical ticks through the file's two timing maps
// (one-shot content seconds and musical content ticks to frames), so the clip keeps playing the same frames from its
// left edge (TS4). Its start and length stay.
void convertClipTiming (NoteClip& clip, const TempoMap& map, const TimingMap& musical, const TimingMap& oneShot,
                        bool toMusical)
{
    const auto spt = oneShotSecondsPerTick (map, clip.start);
    const auto convert = [&] (Ticks content) -> double
    {
        const auto at = static_cast<double> (content);
        return toMusical ? musical.tickAt (oneShot.frameAt (at * spt)) : oneShot.tickAt (musical.frameAt (at)) / spt;
    };

    if (clip.loopLength > 0)
    {
        const auto loopStart = convert (clip.loopStart);
        const auto loopEnd = convert (clip.loopStart + clip.loopLength);
        clip.loopStart = std::max (Ticks { 0 }, roundTicks (loopStart));
        clip.loopLength = std::max (limits::minLoopLength, roundTicks (loopEnd) - clip.loopStart);
    }
    else
    {
        clip.loopStart = std::max (Ticks { 0 }, roundTicks (convert (clip.loopStart)));
    }

    clip.offset = std::max (Ticks { 0 }, roundTicks (convert (clip.offset)));
    convertFades (clip, map, spt, toMusical);
}

// Applies `update` to every clip of a file.
template <typename UpdateFn>
void forEachClipOf (Project& project, ObjectId fileId, UpdateFn&& update)
{
    for (auto& trackPtr : project.tracks)
    {
        if (std::ranges::none_of (trackPtr->clips, [fileId] (const ClipPtr& c) { return c->fileId == fileId; }))
            continue;

        auto track = *trackPtr;

        for (auto& clipPtr : track.clips)
        {
            if (clipPtr->fileId != fileId)
                continue;

            auto clip = *clipPtr;
            update (clip);
            clipPtr = std::make_shared<const NoteClip> (std::move (clip));
        }

        sortClips (track.clips);
        trackPtr = std::make_shared<const Track> (std::move (track));
    }
}

// The file's clips, earliest first.
[[nodiscard]] std::vector<ClipPtr> clipsOfFile (const Project& project, ObjectId fileId)
{
    std::vector<ClipPtr> clips;

    for (const auto& track : project.tracks)
        for (const auto& clip : track->clips)
            if (clip->fileId == fileId)
                clips.push_back (clip);

    std::ranges::sort (clips, [] (const ClipPtr& a, const ClipPtr& b)
                       { return std::tie (a->start, a->id) < std::tie (b->start, b->id); });
    return clips;
}

// Where a file's frame 0 sounds, as the first of its clips plays it now (the timeline tick), for converting its
// markers between the modes (TS4).
[[nodiscard]] double timingAnchor (const Project& project, const AudioFile& file)
{
    const auto clips = clipsOfFile (project, file.id);

    if (clips.empty())
        return 0.0;

    const auto& clip = *clips.front();
    const auto& map = project.tempoMap;
    const auto start = static_cast<double> (clip.start);

    if (file.timing.musical())
        return start - static_cast<double> (clip.offset);

    return map.tickAt (map.secondsAt (start)
                       - static_cast<double> (clip.offset) * oneShotSecondsPerTick (map, clip.start));
}

// Switches a file to the other mode (TS4): the markers convert so that its first clip plays every frame at the same
// time, and each clip keeps its place and length and plays the same frames from its left edge.
void switchTiming (Project& result, AudioFile& file, std::optional<double> sourceBpm)
{
    const auto toMusical = ! file.timing.musical();
    const auto anchor = timingAnchor (result, file);
    const auto& map = result.tempoMap;
    const TimingMap before (file.timing, file.sampleRate);

    if (toMusical)
        file.timing.sourceBpm =
            std::clamp (sourceBpm.value_or (map.bpmAt (anchor)), limits::minSourceBpm, limits::maxSourceBpm);

    file.timing.markers =
        convertTimingMarkers (map, anchor, file.timing, file.frames, file.sampleRate, file.timing.sourceBpm);
    file.timing.mode = toMusical ? TimingMode::musical : TimingMode::oneShot;
    const TimingMap after (file.timing, file.sampleRate);
    const auto& musical = toMusical ? after : before;
    const auto& oneShot = toMusical ? before : after;
    forEachClipOf (result, file.id,
                   [&] (NoteClip& clip) { convertClipTiming (clip, map, musical, oneShot, toMusical); });
}

[[nodiscard]] Project setFileTiming (const Project& project, const juce::var& args)
{
    auto result = project;
    const auto fileId = requireId (args, "fileId");
    std::ignore = requireAudioFile (result, fileId);
    std::optional<double> sourceBpm;

    if (args.hasProperty ("sourceBpm"))
        sourceBpm = std::clamp (requireNumber (args, "sourceBpm"), limits::minSourceBpm, limits::maxSourceBpm);

    // `file` points into result.audioFiles, which forEachClipOf() leaves alone (it only replaces tracks).
    auto& file = requireAudioFile (result, fileId);

    if (args.hasProperty ("musicalStyle"))
    {
        const auto style = args["musicalStyle"].toString();

        if (style != "stretch" && style != "repitch")
            throw CommandError ("Unknown musical style '" + style + "'");

        file.timing.style = style == "repitch" ? MusicalStyle::repitch : MusicalStyle::stretch;
    }

    if (args.hasProperty ("mode"))
    {
        const auto mode = args["mode"].toString();

        if (mode != "musical" && mode != "oneShot")
            throw CommandError ("Unknown timing mode '" + mode + "'");

        if ((mode == "musical") != file.timing.musical())
        {
            switchTiming (result, file, sourceBpm);
            return result;
        }
    }

    if (sourceBpm.has_value())
        file.timing.sourceBpm = *sourceBpm;

    return result;
}

// The musical file a clip plays, or a refusal for note clips and one-shot files (TS14).
[[nodiscard]] const AudioFile& requireMusicalFileOf (const Project& project, const NoteClip& clip)
{
    const auto* file = clip.fileId != 0 ? project.findAudioFile (clip.fileId) : nullptr;

    if (file == nullptr)
        throw CommandError ("Not an audio clip");

    if (! file->timing.musical())
        throw CommandError ("Make the file musical first");

    return *file;
}

[[nodiscard]] Project setClipTranspose (const Project& project, const juce::var& args)
{
    const auto semitones = std::clamp (requireNumber (args, "semitones"), limits::minTranspose, limits::maxTranspose);

    return updateClip (project, requireId (args, "clipId"),
                       [&] (NoteClip& clip, Project& result)
                       {
                           std::ignore = requireMusicalFileOf (result, clip);
                           clip.transpose = semitones;
                       });
}

// The smallest gap between markers, in the file's content units: a tick (musical) or a millisecond (one-shot).
[[nodiscard]] double markerGap (const AudioFile& file) noexcept
{
    return file.timing.musical() ? 1.0 : 0.001;
}

[[nodiscard]] size_t requireMarkerIndex (const AudioFile& file, const juce::var& args)
{
    const auto index = requireInt (args, "index");

    if (index < 0 || static_cast<size_t> (index) >= file.timing.markers.size())
        throw CommandError ("No timing marker " + juce::String (index));

    return static_cast<size_t> (index);
}

// Keeps a marker strictly between its neighbours (and after the implicit point at 0, 0), by a gap in content units
// and a frame.
void clampMarker (std::vector<TimingMarker>& markers, size_t index, double gap)
{
    constexpr double frameGap = 1.0;
    auto& marker = markers[index];
    const auto lowTick = index > 0 ? markers[index - 1].tick : 0.0;
    const auto lowFrame = index > 0 ? markers[index - 1].frame : 0.0;
    const auto highTick = index + 1 < markers.size() ? markers[index + 1].tick : std::numeric_limits<double>::max();
    const auto highFrame = index + 1 < markers.size() ? markers[index + 1].frame : std::numeric_limits<double>::max();

    if (highTick - lowTick <= 2.0 * gap || highFrame - lowFrame <= 2.0 * frameGap)
        throw CommandError ("No room for the marker between its neighbours");

    marker.tick = std::clamp (marker.tick, lowTick + gap, highTick - gap);
    marker.frame = std::clamp (marker.frame, lowFrame + frameGap, highFrame - frameGap);
}

[[nodiscard]] Project addTimingMarker (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));
    const auto tick = requireNumber (args, "tick");
    const auto frame = args.hasProperty ("frame") ? requireNumber (args, "frame")
                                                  : TimingMap (file.timing, file.sampleRate).frameAt (tick);

    if (tick <= 0.0 || frame <= 0.0)
        throw CommandError ("A timing marker must come after the file's start");

    auto& markers = file.timing.markers;

    if (std::ranges::any_of (markers,
                             [&] (const TimingMarker& m) { return std::abs (m.tick - tick) < markerGap (file); }))
        throw CommandError ("There is a marker here already");

    markers.push_back ({ tick, frame });
    const auto count = markers.size();
    normaliseTimingMarkers (markers);

    if (markers.size() != count)
        throw CommandError ("A timing marker must lie between its neighbours");

    return result;
}

[[nodiscard]] Project moveTimingMarker (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));
    const auto index = requireMarkerIndex (file, args);
    auto& marker = file.timing.markers[index];

    if (args.hasProperty ("tick"))
        marker.tick = requireNumber (args, "tick");

    if (args.hasProperty ("frame"))
        marker.frame = requireNumber (args, "frame");

    clampMarker (file.timing.markers, index, markerGap (file));
    return result;
}

[[nodiscard]] Project removeTimingMarker (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));
    const auto index = requireMarkerIndex (file, args);
    file.timing.markers.erase (file.timing.markers.begin() + static_cast<std::ptrdiff_t> (index));
    return result;
}

[[nodiscard]] Project resetTimingMarkers (const Project& project, const juce::var& args)
{
    auto result = project;
    requireAudioFile (result, requireId (args, "fileId")).timing.markers.clear();
    return result;
}

// "Make copy" (TS2): the clip gets a pool entry of its own, with a copy of the timing, so it can be stretched apart
// from the file's other clips. The audio on disk is shared.
// The file's pitch notes by the ids in args[key], or every note without that argument (PC5).
[[nodiscard]] std::vector<PitchNote*> requirePitchNotes (AudioFile& file, const juce::var& args, const char* key)
{
    std::vector<PitchNote*> notes;

    if (! args.hasProperty (key))
    {
        for (auto& note : file.pitch.notes)
            notes.push_back (&note);

        return notes;
    }

    for (const auto& id : requireArray (args, key))
    {
        const auto it = std::ranges::find (file.pitch.notes, std::llround (static_cast<double> (id)), &PitchNote::id);

        if (it == file.pitch.notes.end())
            throw CommandError ("No such pitch note");

        notes.push_back (&*it);
    }

    return notes;
}

// Copies detected notes into the file's pitch edits (docs/design/pitch-correction.md, section 4.2): { fileId,
// analyser, notes: [{ leadStart, start, end, centre, shape }], replace? }, the notes as audio.pitchAnalysis gives
// them. Without replace, a file that has notes already keeps them.
[[nodiscard]] Project seedPitchNotes (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));

    if (! file.pitch.notes.empty() && ! static_cast<bool> (args.getProperty ("replace", false)))
        throw CommandError ("The file's notes are there already");

    FilePitch pitch;
    pitch.analyser = args["analyser"].toString();
    juce::int64 id = 0;

    for (const auto& n : requireArray (args, "notes"))
    {
        PitchNote note;
        note.id = ++id;
        readNoteFrames (n, file.frames, note);
        note.detectedCentre = note.targetCentre = std::clamp (requireNumber (n, "centre"), 0.0, 127.0);
        note.detectedShape = note.shape = noteShapeFromVar (n["shape"], note);
        pitch.notes.push_back (std::move (note));
    }

    std::ranges::sort (pitch.notes, {}, &PitchNote::start);
    checkNoteOrder (pitch.notes);
    file.pitch = std::move (pitch);
    return result;
}

// Sets notes' pitch (section 4.8): { fileId, noteIds, target? | delta?, vibrato?, transitionMs? }. target is the new
// centre in semitones, delta a change of it; the shape moves with the centre.
[[nodiscard]] Project setNotePitch (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));

    for (auto* note : requirePitchNotes (file, args, "noteIds"))
    {
        if (args.hasProperty ("target"))
            note->targetCentre = requireNumber (args, "target");
        else if (args.hasProperty ("delta"))
            note->targetCentre += requireNumber (args, "delta");

        note->targetCentre = std::clamp (note->targetCentre, 0.0, 127.0);
        note->vibrato = std::clamp (numberOr (args, "vibrato", note->vibrato), 0.0, 2.0);
        note->transitionMs = std::clamp (numberOr (args, "transitionMs", note->transitionMs), 0.0, 500.0);
    }

    return result;
}

// Puts notes back as sung (section 4.8): { fileId, noteIds? }, all notes without noteIds.
[[nodiscard]] Project resetPitchNotes (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));

    for (auto* note : requirePitchNotes (file, args, "noteIds"))
    {
        note->targetCentre = note->detectedCentre;
        note->shape = note->detectedShape;
        note->pivots.clear();
        note->vibrato = 1.0;
    }

    return result;
}

// A curve with every value moved by `by` (re-referencing a shape to another centre).
[[nodiscard]] Curve shifted (Curve curve, double by)
{
    for (auto& point : curve)
        point.value = std::clamp (point.value + by, -maxShapeSemitones, maxShapeSemitones);

    return curve;
}

// The first clip that plays a file, which places its content on the timeline (for the key and the grid), or null.
[[nodiscard]] const NoteClip* firstClipOf (const Project& project, ObjectId fileId)
{
    const NoteClip* found = nullptr;

    for (const auto& track : project.tracks)
        for (const auto& clip : track->clips)
            if (clip->fileId == fileId && (found == nullptr || clip->start < found->start))
                found = clip.get();

    return found;
}

// Snaps notes' pitch (docs/design/pitch-correction.md, section 4.7, PC10): { fileId, noteIds?, mode: semitone | key,
// amount? }. Each target centre moves towards the nearest semitone, or the nearest note of the project's key where the
// file's first clip starts, by amount (0..1, default 1); partial correction keeps some of the singer's intonation.
[[nodiscard]] Project snapNotePitch (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));
    const auto mode = args.getProperty ("mode", "semitone").toString();
    const auto amount = std::clamp (numberOr (args, "amount", 1.0), 0.0, 1.0);

    if (mode != "semitone" && mode != "key")
        throw CommandError ("Snap mode must be 'semitone' or 'key'");

    ProjectKey key;

    if (mode == "key")
    {
        const auto* clip = firstClipOf (result, file.id);
        key = result.keyAtBar (clip != nullptr ? result.tempoMap.barAt (static_cast<double> (clip->start)).bar : 0);

        if (! key.enabled)
            throw CommandError ("Set the project's key first");
    }

    for (auto* note : requirePitchNotes (file, args, "noteIds"))
    {
        const auto nearest = static_cast<int> (std::lround (note->targetCentre));
        auto snapped = static_cast<double> (nearest);

        if (mode == "key")
        {
            const auto mask =
                scales[static_cast<size_t> (std::clamp<int> (key.scale, 0, static_cast<int> (scales.size()) - 1))].mask;
            // The nearer of the scale's notes below and above the centre.
            const auto below =
                lockToScale (static_cast<int> (std::floor (note->targetCentre)), key.root, mask, ScaleDirection::down);
            const auto above =
                lockToScale (static_cast<int> (std::ceil (note->targetCentre)), key.root, mask, ScaleDirection::up);
            snapped = note->targetCentre - below <= above - note->targetCentre ? below : above;
        }

        note->targetCentre += (snapped - note->targetCentre) * amount;
    }

    return result;
}

// Sets a note's shape (docs/design/pitch-correction.md, section 4.4): { fileId, noteId, shape, centre?, pivots? }.
// The UI computes the curve (node edits, tilting) and the pivots, frames from the note's start. shape is relative to
// centre, the target centre the UI edited it against (the note's own without it), so every step of a drag means the
// same however earlier steps moved the centre. The note is then recentred (PitchNote::recentre): its bar goes where
// the edit put its perceived pitch.
[[nodiscard]] Project setNoteShape (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));
    const auto it = std::ranges::find (file.pitch.notes, std::llround (requireNumber (args, "noteId")), &PitchNote::id);

    if (it == file.pitch.notes.end())
        throw CommandError ("No such pitch note");

    auto& note = *it;

    if (args.hasProperty ("centre"))
        note.targetCentre = std::clamp (requireNumber (args, "centre"), 0.0, 127.0);

    note.shape = noteShapeFromVar (args["shape"], note);

    if (args.hasProperty ("pivots"))
    {
        note.pivots.clear();

        for (const auto& pivot : requireArray (args, "pivots"))
        {
            const auto frame = std::llround (static_cast<double> (pivot));

            if (frame <= 0 || frame >= note.end - note.start)
                throw CommandError ("A pivot must lie inside its note");

            note.pivots.push_back (frame);
        }

        std::ranges::sort (note.pivots);
        note.pivots.erase (std::ranges::unique (note.pivots).begin(), note.pivots.end());
    }

    note.recentre();

    return result;
}

// Shape presets on notes (section 4.4): { fileId, noteIds?, op }. Each note is recentred afterwards.
//   flattenOnset    the first segment takes its second node's value as it plays: the scoop goes.
//   flattenRelease  the same for the last segment.
[[nodiscard]] Project shapeNotes (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));
    const auto op = args["op"].toString();

    if (op != "flattenOnset" && op != "flattenRelease")
        throw CommandError ("Unknown shape operation '" + op + "'");

    for (auto* note : requirePitchNotes (file, args, "noteIds"))
    {
        auto& shape = note->shape;

        if (op == "flattenOnset" && shape.size() >= 2)
            shape[0].value = shape[1].value;
        else if (op == "flattenRelease" && shape.size() >= 2)
            shape.back().value = shape[shape.size() - 2].value;

        note->recentre();
    }

    return result;
}

// Splits a note in two (docs/design/pitch-correction.md, section 4.2): { fileId, noteId, frame, centres? }. centres
// are the halves' detected centres, measured on the pitch track by the caller; without them both keep the note's.
// The sung and the edited pitch stay where they were: each half's shapes are re-referenced to its new centre.
[[nodiscard]] Project splitPitchNote (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));
    auto& notes = file.pitch.notes;
    const auto id = std::llround (requireNumber (args, "noteId"));
    const auto it = std::ranges::find (notes, id, &PitchNote::id);

    if (it == notes.end())
        throw CommandError ("No such pitch note");

    const auto frame = std::llround (requireNumber (args, "frame"));
    const auto old = *it;

    if (frame <= old.start || frame >= old.end)
        throw CommandError ("A note splits inside its voiced part");

    auto centres = std::pair { old.detectedCentre, old.detectedCentre };

    if (const auto* given = args["centres"].getArray(); given != nullptr && given->size() == 2)
        centres = { std::clamp (static_cast<double> ((*given)[0]), 0.0, 127.0),
                    std::clamp (static_cast<double> ((*given)[1]), 0.0, 127.0) };

    const auto at = frame - old.start;
    auto [detectedLeft, detectedRight] = splitCurve (old.detectedShape, at, 0.0);
    auto [shapeLeft, shapeRight] = splitCurve (old.shape, at, 0.0);
    const auto edit = old.targetCentre - old.detectedCentre;
    const auto maxId = std::ranges::max (notes, {}, &PitchNote::id).id;

    auto left = old;
    left.end = frame;
    left.detectedCentre = centres.first;
    left.targetCentre = centres.first + edit;
    left.detectedShape = shifted (detectedLeft, old.detectedCentre - centres.first);
    left.shape = shifted (shapeLeft, old.detectedCentre - centres.first);
    left.pivots.clear();

    auto right = old;
    right.id = maxId + 1;
    right.leadStart = right.start = frame;
    right.detectedCentre = centres.second;
    right.targetCentre = centres.second + edit;
    right.detectedShape = shifted (detectedRight, old.detectedCentre - centres.second);
    right.shape = shifted (shapeRight, old.detectedCentre - centres.second);
    right.pivots.clear();

    for (auto* half : { &left, &right })
    {
        normalisePitchShape (half->detectedShape, half->end - half->start);
        normalisePitchShape (half->shape, half->end - half->start);
    }

    for (const auto pivot : old.pivots)
        (pivot < at ? left.pivots : right.pivots).push_back (pivot < at ? pivot : pivot - at);

    *it = std::move (left);
    notes.insert (std::next (it), std::move (right));
    return result;
}

// Joins neighbouring notes into one (section 4.2): { fileId, noteIds, centre? }. centre is the merged note's detected
// centre, measured by the caller; without it, the notes' centres averaged by length. The first note's id, vibrato
// and transition are kept, and its edit of the centre; each note's shapes keep their pitch, re-referenced to the
// merged centres. Across a gap the shapes run smoothly from one note's to the next's.
[[nodiscard]] Project mergePitchNotes (const Project& project, const juce::var& args)
{
    auto result = project;
    auto& file = requireAudioFile (result, requireId (args, "fileId"));
    auto& notes = file.pitch.notes;
    std::vector<size_t> indices;

    for (auto* note : requirePitchNotes (file, args, "noteIds"))
        indices.push_back (static_cast<size_t> (note - notes.data()));

    std::ranges::sort (indices);

    if (indices.size() < 2)
        throw CommandError ("Merging needs two notes or more");

    for (size_t i = 1; i < indices.size(); ++i)
        if (indices[i] != indices[i - 1] + 1)
            throw CommandError ("Only neighbouring notes can be merged");

    const auto first = notes.begin() + static_cast<std::ptrdiff_t> (indices.front());
    const auto last = notes.begin() + static_cast<std::ptrdiff_t> (indices.back()) + 1;
    auto merged = *first;
    merged.end = std::prev (last)->end;

    auto weighted = 0.0;
    auto length = 0.0;

    for (auto n = first; n != last; ++n)
    {
        weighted += n->detectedCentre * static_cast<double> (n->end - n->start);
        length += static_cast<double> (n->end - n->start);
    }

    merged.detectedCentre = std::clamp (numberOr (args, "centre", weighted / length), 0.0, 127.0);
    merged.targetCentre = merged.detectedCentre + (first->targetCentre - first->detectedCentre);
    merged.detectedShape.clear();
    merged.shape.clear();
    merged.pivots.clear();

    const auto append = [] (Curve& into, const Curve& part, juce::int64 offset, double by)
    {
        for (auto point : part)
        {
            point.shape = CurveShape::rounded;
            point.time += offset;
            point.value = std::clamp (point.value + by, -maxShapeSemitones, maxShapeSemitones);

            if (into.empty() || point.time > into.back().time)
                into.push_back (point);
        }
    };

    for (auto n = first; n != last; ++n)
    {
        const auto offset = n->start - merged.start;
        append (merged.detectedShape, n->detectedShape, offset, n->detectedCentre - merged.detectedCentre);
        append (merged.shape, n->shape, offset, n->targetCentre - merged.targetCentre);

        for (const auto pivot : n->pivots)
            merged.pivots.push_back (pivot + offset);
    }

    if (merged.detectedShape.size() > limits::maxCurvePoints || merged.shape.size() > limits::maxCurvePoints)
        throw CommandError ("The merged note's shape has too many points");

    *first = std::move (merged);
    notes.erase (std::next (first), last);
    return result;
}

[[nodiscard]] Project copyPoolFile (const Project& project, const juce::var& args)
{
    const auto clipId = requireId (args, "clipId");
    const auto location = requireClip (project, clipId);
    const auto& clip = *project.tracks[location.track]->clips[location.clip];
    const auto* file = clip.fileId != 0 ? project.findAudioFile (clip.fileId) : nullptr;

    if (file == nullptr)
        throw CommandError ("Not an audio clip");

    auto copy = *file;
    auto result = project;

    const auto base = copy.name;
    auto name = base + " (2)";

    for (int n = 3; std::ranges::any_of (result.audioFiles, [&] (const AudioFile& f) { return f.name == name; }); ++n)
        name = base + " (" + juce::String (n) + ")";

    copy.id = result.nextId++;
    copy.name = name;
    result.audioFiles.push_back (copy);
    const auto newId = copy.id;

    return updateClip (result, clipId, [newId] (NoteClip& c, Project&) { c.fileId = newId; });
}

// Reverse (docs/design/audio-features.md, AU1): each clip plays its file backwards, through a reversed copy of the
// file. The app writes the copies first and passes them in `files`, keyed by the original's id; a file that already
// has a reversed copy reuses it, and a clip of a reversed copy goes back to the original.
[[nodiscard]] Project reverseClips (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "clipIds");
    const std::set<ObjectId> clipIds (ids.begin(), ids.end());
    auto result = project;

    for (const auto clipId : clipIds)
    {
        const auto location = requireClip (result, clipId);
        const auto& clip = *result.tracks[location.track]->clips[location.clip];

        if (clip.fileId == 0)
            throw CommandError ("Only audio clips can be reversed");

        const auto* found = result.findAudioFile (clip.fileId);

        if (found == nullptr)
            throw CommandError ("No audio file with id " + juce::String (clip.fileId));

        const auto file = *found;
        const auto contentTicks = fileContentTicks (file, result.tempoMap, clip.start);
        ObjectId target = 0;

        if (file.reverseOf != 0 && result.findAudioFile (file.reverseOf) != nullptr)
        {
            target = file.reverseOf;
        }
        else if (const auto existing = std::ranges::find (result.audioFiles, file.id, &AudioFile::reverseOf);
                 existing != result.audioFiles.end())
        {
            target = existing->id;
        }
        else
        {
            const auto f = args["files"][juce::Identifier (juce::String (file.id))];

            if (! f.isObject())
                throw CommandError ("No reversed copy of " + file.name);

            auto copy = audioFile (0, f["path"].toString(), file.name + " (reversed)", requireNumber (f, "sampleRate"),
                                   requireInt (f, "channels"), static_cast<juce::int64> (requireNumber (f, "frames")));

            if (copy.path.isEmpty() || copy.frames != file.frames || copy.channels != file.channels)
                throw CommandError ("The reversed copy of " + file.name + " doesn't match it");

            copy.timing = reversedTiming (file.timing, file.sampleRate, file.frames);
            copy.suggestedBpm = file.suggestedBpm;
            copy.reverseOf = file.id;
            copy.id = result.nextId++;
            target = copy.id;
            result.audioFiles.push_back (std::move (copy));
        }

        result = updateClip (result, clipId,
                             [&] (NoteClip& c, Project&)
                             {
                                 mirrorClip (c, contentTicks);
                                 c.fileId = target;
                             });
    }

    return result;
}

// Freeze (docs/design/audio-features.md, AU15): the app renders the track first and passes the render, which joins the
// pool; the track then plays it until unfrozen. With `outputs: [{ node, file }]`, the renders of its Output to track
// nodes join too, and their output tracks play them. The key is taken now, so later edits that change what the render
// depends on make it stale.
[[nodiscard]] Project freezeTrack (const Project& project, const juce::var& args)
{
    const auto trackId = requireId (args, "trackId");
    const auto* track = project.findTrack (trackId);

    if (track == nullptr)
        throw CommandError ("No track with id " + juce::String (trackId));

    if (track->kind != TrackKind::instrument && track->kind != TrackKind::audio)
        throw CommandError ("Only instrument and audio tracks can be frozen");

    auto result = project;
    const auto fileId = poolAudioFile (result, args["file"]);
    const auto start = requireTicks (args, "start", 0, limits::maxPosition);
    const auto key = freezeKey (project, *track);
    std::vector<FrozenOutput> outputs;

    if (const auto* list = args["outputs"].getArray())
        for (const auto& output : *list)
        {
            const auto nodeId = requireId (output, "node");
            const auto node = std::ranges::find (track->graph.nodes, nodeId, &GraphNode::id);

            if (node == track->graph.nodes.end() || node->type != nodetype::busOutput)
                throw CommandError ("No Output to track node with id " + juce::String (nodeId));

            if (std::ranges::find (outputs, nodeId, &FrozenOutput::node) != outputs.end())
                throw CommandError ("Output to track node " + juce::String (nodeId) + " is listed twice");

            outputs.push_back ({ nodeId, poolAudioFile (result, output["file"]) });
        }

    return updateTrack (result, trackId,
                        [&] (Track& t, Project&) { t.freeze = { fileId, start, key, std::move (outputs) }; });
}

// A new audio track holding a render (docs/design/audio-features.md, AU16, AU17), right after another track (after
// its block, for a group). With `like`, it takes that track's colour, fader, pan and sends (a bounced instrument).
[[nodiscard]] Project addRenderTrack (const Project& project, const juce::var& args)
{
    const auto afterId = requireId (args, "after");
    const auto index = project.indexOfTrack (afterId);

    if (index < 0)
        throw CommandError ("No track with id " + juce::String (afterId));

    const auto& after = *project.tracks[static_cast<size_t> (index)];

    if (after.kind == TrackKind::returnTrack)
        throw CommandError ("Renders don't go after return tracks");

    auto result = project;
    Track track;
    track.id = result.nextId++;
    track.kind = TrackKind::audio;
    track.parent = after.parent;
    track.name = args["name"].toString().trim().isNotEmpty() ? args["name"].toString().trim() : after.name + " (audio)";
    track.colour = after.colour;
    makeChainGraph (track, result, { nodetype::audioPlayer });

    if (static_cast<bool> (args["like"]))
    {
        track.gainDb = after.gainDb;
        track.pan = after.pan;

        for (auto send : after.sends)
        {
            send.id = result.nextId++;
            track.sends.push_back (send);
        }
    }

    const auto trackId = track.id;
    const auto at = endAfter (result.tracks, static_cast<size_t> (index));
    result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (at),
                          std::make_shared<const Track> (std::move (track)));
    normaliseGroups (result);

    return applyCommand (
        result, "addAudioClip",
        makeObject (
            { { "trackId", trackId }, { "start", args["start"] }, { "file", args["file"] }, { "recorded", true } }));
}

// Slice to Sampler (docs/design/audio-features.md, AU3): the clip's audio cut at the given frames, one Sampler zone per
// slice on the same pool file, on a new instrument track after the clip's track, with a MIDI clip in the clip's place
// that plays the slices where they were.
[[nodiscard]] Project sliceToSampler (const Project& project, const juce::var& args)
{
    const auto clipId = requireId (args, "clipId");
    const auto location = requireClip (project, clipId);
    const auto& source = *project.tracks[location.track];
    const auto& clip = *source.clips[location.clip];

    if (clip.fileId == 0)
        throw CommandError ("Only audio clips can be sliced");

    if (clip.loopLength > 0)
        throw CommandError ("Unloop the clip to slice it");

    const auto* file = project.findAudioFile (clip.fileId);

    if (file == nullptr)
        throw CommandError ("No audio file with id " + juce::String (clip.fileId));

    const ClipFrames frames (clip, *file, project.tempoMap);
    const auto [from, to] = frames.range();

    if (to <= from)
        throw CommandError ("The clip plays nothing of its file");

    std::vector<juce::int64> bounds { from };

    if (const auto* list = args["slices"].getArray())
        for (const auto& value : *list)
            if (const auto frame = static_cast<juce::int64> (static_cast<double> (value));
                frame > bounds.back() && frame < to)
                bounds.push_back (frame);

    bounds.push_back (to);
    const auto count = static_cast<int> (bounds.size()) - 1;

    if (count > maxSlices)
        throw CommandError ("A Sampler takes at most 128 slices");

    // From C1 by default, lower where the slices wouldn't fit below the top key.
    const auto startKey = std::min (
        args.hasProperty ("startKey") ? std::clamp (requireInt (args, "startKey"), 0, 127) : 36, maxSlices - count);
    const auto gate = args["mode"].toString() == "gate";
    auto result = project;

    Track track;
    track.id = result.nextId++;
    track.parent = source.parent;
    track.name = (clip.name.isNotEmpty() ? clip.name : file->name) + " slices";
    track.colour = source.colour;
    makeChainGraph (track, result, { nodetype::sequencer, nodetype::sampler });
    auto& sampler = *std::ranges::find (track.graph.nodes, juce::String (nodetype::sampler), &GraphNode::type);
    sampler.sampleZones.clear();
    juce::Array<juce::var> notes;

    for (int i = 0; i < count; ++i)
    {
        const auto index = static_cast<size_t> (i);
        SampleZone zone;
        zone.id = result.nextId++;
        zone.fileId = file->id;
        zone.lowKey = zone.highKey = zone.rootKey = startKey + i;
        zone.start = bounds[index];
        zone.end = bounds[index + 1];
        zone.mode = gate ? SampleMode::gate : SampleMode::oneShot;
        zone.attackMs = 1.0; // takes the click off a cut edge
        zone.releaseMs = 5.0;
        sampler.sampleZones.push_back (zone);

        const auto noteStart = std::max (Ticks { 0 }, roundTicks (frames.tickAt (static_cast<double> (bounds[index]))
                                                                  - static_cast<double> (clip.start)));
        const auto noteEnd = i + 1 < count ? roundTicks (frames.tickAt (static_cast<double> (bounds[index + 1]))
                                                         - static_cast<double> (clip.start))
                                           : clip.length;
        notes.add (makeObject ({ { "start", noteStart },
                                 { "length", std::max (Ticks { 1 }, noteEnd - noteStart) },
                                 { "pitch", startKey + i },
                                 { "velocity", 100.0 / 127.0 } }));
    }

    const auto trackId = track.id;
    const auto at = endAfter (result.tracks, static_cast<size_t> (location.track));
    result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (at),
                          std::make_shared<const Track> (std::move (track)));
    normaliseGroups (result);
    result = applyCommand (result, "setTrackDrumMode", makeObject ({ { "id", trackId }, { "mode", "drums" } }));

    return applyCommand (result, "addClip",
                         makeObject ({ { "trackId", trackId },
                                       { "start", clip.start },
                                       { "length", clip.length },
                                       { "name", (clip.name.isNotEmpty() ? clip.name : file->name) + " slices" },
                                       { "notes", notes } }));
}

// Audio to MIDI (docs/design/audio-features.md, AU4): a new instrument track after another, with one clip of notes,
// in drum mode for drums.
[[nodiscard]] Project addNoteTrack (const Project& project, const juce::var& args)
{
    const auto afterId = requireId (args, "after");
    const auto index = project.indexOfTrack (afterId);

    if (index < 0)
        throw CommandError ("No track with id " + juce::String (afterId));

    const auto& after = *project.tracks[static_cast<size_t> (index)];
    auto result = project;
    Track track;
    track.id = result.nextId++;
    track.parent = after.kind == TrackKind::returnTrack ? 0 : after.parent;
    track.name = args["name"].toString().trim().isNotEmpty() ? args["name"].toString().trim() : after.name + " (MIDI)";
    track.colour = after.colour;
    makeChainGraph (track, result, { nodetype::sequencer, nodetype::mpeSynth });
    const auto trackId = track.id;
    const auto at = after.kind == TrackKind::returnTrack ? endOfGroup (result, 0)
                                                         : endAfter (result.tracks, static_cast<size_t> (index));
    result.tracks.insert (result.tracks.begin() + static_cast<std::ptrdiff_t> (at),
                          std::make_shared<const Track> (std::move (track)));
    normaliseGroups (result);

    if (static_cast<bool> (args["drums"]))
        result = applyCommand (result, "setTrackDrumMode", makeObject ({ { "id", trackId }, { "mode", "drums" } }));

    return applyCommand (result, "addClip",
                         makeObject ({ { "trackId", trackId },
                                       { "start", args["start"] },
                                       { "length", args["length"] },
                                       { "name", args["name"] },
                                       { "notes", args["notes"] } }));
}

[[nodiscard]] Project unfreezeTrack (const Project& project, const juce::var& args)
{
    const auto trackId = requireId (args, "trackId");

    if (project.findTrack (trackId) == nullptr)
        throw CommandError ("No track with id " + juce::String (trackId));

    return updateTrack (project, trackId, [] (Track& t, Project&) { t.freeze = {}; });
}

// Crossfades on every edge of the clips that lies over audio of a clip below (AU8): the length (in milliseconds,
// else the automatic length, else 10 ms) where there is no fade yet or a length is given, and the shape if given.
[[nodiscard]] Project crossfadeClips (const Project& project, const juce::var& args)
{
    const auto ids = requireIds (args, "clipIds");
    const auto ms = args.hasProperty ("ms")
                        ? std::optional<double> (std::clamp (requireNumber (args, "ms"), 1.0, limits::maxCrossfadeMs))
                        : std::nullopt;
    const auto shape =
        args.hasProperty ("shape")
            ? std::optional<FadeShape> (static_cast<FadeShape> (std::clamp (requireInt (args, "shape"), 0, 3)))
            : std::nullopt;
    const auto seconds = ms.value_or (project.autoCrossfadeMs > 0.0 ? project.autoCrossfadeMs : 10.0) / 1000.0;
    auto result = project;

    for (const auto clipId : ids)
    {
        const auto location = requireClip (result, clipId);
        const auto& track = *result.tracks[location.track];
        const auto edges = coveredEdges (track.clips, *track.clips[location.clip]);
        const auto* file = result.findAudioFile (track.clips[location.clip]->fileId);
        const auto musical = file != nullptr && file->timing.musical();
        const auto& map = result.tempoMap;

        if (! edges.start && ! edges.end)
            continue;

        result = updateClip (result, clipId,
                             [&] (NoteClip& c, Project&)
                             {
                                 if (edges.start && (ms.has_value() || c.fadeIn == 0))
                                     c.fadeIn = fadeTicksFor (c, true, seconds, musical, map);

                                 if (edges.end && (ms.has_value() || c.fadeOut == 0))
                                     c.fadeOut = fadeTicksFor (c, false, seconds, musical, map);

                                 if (shape.has_value() && edges.start)
                                     c.fadeInShape = *shape;

                                 if (shape.has_value() && edges.end)
                                     c.fadeOutShape = *shape;
                             });
    }

    return result;
}

// Stretch-resize (TS5): the file's audio fills the clip's new length. The source tempo and every marker's tick scale
// by new length / old length, so every clip of the file stretches: each keeps its start, and its length and content
// positions scale too.
[[nodiscard]] Project stretchClip (const Project& project, const juce::var& args)
{
    const auto clipId = requireId (args, "clipId");
    const auto location = requireClip (project, clipId);
    const auto& clip = *project.tracks[location.track]->clips[location.clip];
    const auto length = requireTicks (args, "length", 1, limits::maxPosition);
    const auto& musical = requireMusicalFileOf (project, clip);
    const auto factor = static_cast<double> (length) / static_cast<double> (clip.length);
    // Fewer frames per content tick: the source tempo rises with the length, as the markers' ticks do.
    const auto sourceBpm = musical.timing.sourceBpm * factor;

    if (sourceBpm < limits::minSourceBpm || sourceBpm > limits::maxSourceBpm)
        throw CommandError ("That stretches the audio too far");

    auto result = project;
    auto& file = requireAudioFile (result, musical.id);
    file.timing.sourceBpm = sourceBpm;

    for (auto& marker : file.timing.markers)
        marker.tick *= factor;

    normaliseTimingMarkers (file.timing.markers);
    const auto scale = [factor] (Ticks t) { return roundTicks (static_cast<double> (t) * factor); };

    forEachClipOf (result, file.id,
                   [&] (NoteClip& c)
                   {
                       c.length = c.id == clipId ? length : std::max (Ticks { 1 }, scale (c.length));
                       c.offset = scale (c.offset);
                       c.loopStart = scale (c.loopStart);
                       c.loopLength = c.loopLength > 0 ? std::max (limits::minLoopLength, scale (c.loopLength)) : 0;
                       c.fadeIn = scale (c.fadeIn);
                       c.fadeOut = scale (c.fadeOut);
                   });

    return result;
}

// ---------------------------------------------------------------------------------------------------------
// Command table

[[nodiscard]] Project batch (const Project& project, const juce::var& args);

struct CommandInfo
{
    const char* name;
    const char* label;
    Project (*apply) (const Project&, const juce::var&);
};

constexpr std::array commands {
    CommandInfo { "setTempo", "Set tempo", &setTempo },
    CommandInfo { "addTempoPoint", "Add tempo change", &addTempoPoint },
    CommandInfo { "setTempoPoint", "Change tempo", &setTempoPoint },
    CommandInfo { "removeTempoPoint", "Remove tempo change", &removeTempoPoint },
    CommandInfo { "setSignature", "Set time signature", &setSignature },
    CommandInfo { "moveSignature", "Move time signature", &moveSignature },
    CommandInfo { "removeSignature", "Remove time signature", &removeSignature },
    CommandInfo { "setMetronome", "Toggle metronome", &setMetronome },
    CommandInfo { "addTrack", "Add track", &addTrack },
    CommandInfo { "duplicateTrack", "Duplicate track", &duplicateTrack },
    CommandInfo { "groupTracks", "Group tracks", &groupTracks },
    CommandInfo { "ungroup", "Ungroup", &ungroup },
    CommandInfo { "moveTracks", "Move tracks", &moveTracks },
    CommandInfo { "setTrackFolded", "Fold group", &setTrackFolded },
    CommandInfo { "addSend", "Add send", &addSend },
    CommandInfo { "setSend", "Change send", &setSend },
    CommandInfo { "removeSend", "Remove send", &removeSend },
    CommandInfo { "removeTrack", "Remove track", &removeTrack },
    CommandInfo { "setTrackPan", "Set pan", &setTrackPan },
    CommandInfo { "addAutomationLane", "Add automation lane", &addAutomationLane },
    CommandInfo { "removeAutomationLane", "Remove automation lane", &removeAutomationLane },
    CommandInfo { "setAutomationLane", "Change automation lane", &setAutomationLane },
    CommandInfo { "setAutomationPoints", "Edit automation", &setAutomationPoints },
    CommandInfo { "writeAutomation", "Write automation", &writeAutomation },
    CommandInfo { "setTrackAutomation", "Automation mode", &setTrackAutomation },
    CommandInfo { "setClipController", "Edit controller", &setClipController },
    CommandInfo { "setMidiMapping", "MIDI learn", &setMidiMapping },
    CommandInfo { "removeMidiMapping", "Forget MIDI mapping", &removeMidiMapping },
    CommandInfo { "renameTrack", "Rename track", &renameTrack },
    CommandInfo { "setTrackGain", "Change track gain", &setTrackGain },
    CommandInfo { "setTrackOffset", "Change track offset", &setTrackOffset },
    CommandInfo { "setTrackDrumMode", "Change drum mode", &setTrackDrumMode },
    CommandInfo { "importMidi", "Import MIDI file", &importMidi },
    CommandInfo { "addClip", "Add clip", &addClip },
    CommandInfo { "removeClip", "Remove clip", &removeClip },
    CommandInfo { "moveClip", "Move clip", &moveClip },
    CommandInfo { "resizeClip", "Resize clip", &resizeClip },
    CommandInfo { "deleteClips", "Delete clips", &deleteClips },
    CommandInfo { "bringToTop", "Choose take", &bringToTop },
    CommandInfo { "flattenTakes", "Flatten takes", &flattenTakes },
    CommandInfo { "removeLane", "Delete takes lane", &removeLane },
    CommandInfo { "setTrackLanesExpanded", "Show takes", &setTrackLanesExpanded },
    CommandInfo { "moveClips", "Move clips", &moveClips },
    CommandInfo { "duplicateClips", "Duplicate clips", &duplicateClips },
    CommandInfo { "splitClips", "Split clips", &splitClips },
    CommandInfo { "setClipLoop", "Loop clip", &setClipLoop },
    CommandInfo { "setClipProps", "Edit clip", &setClipProps },
    CommandInfo { "glueClips", "Glue clips", &glueClips },
    CommandInfo { "setTrackHeight", "Resize track", &setTrackHeight },
    CommandInfo { "makeUnique", "Make unique", &makeUnique },
    CommandInfo { "addAudioClip", "Add audio", &addAudioClip },
    CommandInfo { "relinkAudioFile", "Locate audio file", &relinkAudioFile },
    CommandInfo { "setFileTiming", "Change audio timing", &setFileTiming },
    CommandInfo { "setClipTranspose", "Transpose clip", &setClipTranspose },
    CommandInfo { "addTimingMarker", "Add timing marker", &addTimingMarker },
    CommandInfo { "moveTimingMarker", "Move timing marker", &moveTimingMarker },
    CommandInfo { "removeTimingMarker", "Remove timing marker", &removeTimingMarker },
    CommandInfo { "resetTimingMarkers", "Reset timing markers", &resetTimingMarkers },
    CommandInfo { "copyPoolFile", "Make copy", &copyPoolFile },
    CommandInfo { "seedPitchNotes", "Find notes", &seedPitchNotes },
    CommandInfo { "setNotePitch", "Change note pitch", &setNotePitch },
    CommandInfo { "resetPitchNotes", "Reset notes", &resetPitchNotes },
    CommandInfo { "splitPitchNote", "Split note", &splitPitchNote },
    CommandInfo { "mergePitchNotes", "Merge notes", &mergePitchNotes },
    CommandInfo { "setNoteShape", "Edit note shape", &setNoteShape },
    CommandInfo { "shapeNotes", "Shape notes", &shapeNotes },
    CommandInfo { "snapNotePitch", "Snap notes", &snapNotePitch },
    CommandInfo { "reverseClips", "Reverse", &reverseClips },
    CommandInfo { "crossfadeClips", "Crossfade", &crossfadeClips },
    CommandInfo { "setPunch", "Change punch range", &setPunch },
    CommandInfo { "freezeTrack", "Freeze track", &freezeTrack },
    CommandInfo { "unfreezeTrack", "Unfreeze track", &unfreezeTrack },
    CommandInfo { "addRenderTrack", "Add rendered track", &addRenderTrack },
    CommandInfo { "sliceToSampler", "Slice to Sampler", &sliceToSampler },
    CommandInfo { "addNoteTrack", "Audio to MIDI", &addNoteTrack },
    CommandInfo { "setAutoCrossfade", "Change automatic crossfades", &setAutoCrossfade },
    CommandInfo { "setKey", "Change key", &setKey },
    CommandInfo { "moveKey", "Move key change", &moveKey },
    CommandInfo { "removeKey", "Remove key change", &removeKey },
    CommandInfo { "stretchClip", "Stretch clip", &stretchClip },
    CommandInfo { "setTrackSwitches", "Change track settings", &setTrackSwitches },
    CommandInfo { "setTrackColour", "Change track colour", &setTrackColour },
    CommandInfo { "addMarker", "Add marker", &addMarker },
    CommandInfo { "moveMarker", "Move marker", &moveMarker },
    CommandInfo { "renameMarker", "Rename marker", &renameMarker },
    CommandInfo { "removeMarker", "Remove marker", &removeMarker },
    CommandInfo { "setLoop", "Set loop range", &setLoop },
    CommandInfo { "addNotes", "Add notes", &addNotes },
    CommandInfo { "updateNotes", "Edit notes", &updateNotes },
    CommandInfo { "removeNotes", "Delete notes", &removeNotes },
    CommandInfo { "splitNotes", "Split notes", &splitNotes },
    CommandInfo { "setCurve", "Edit expression", &setCurve },
    CommandInfo { "addNode", "Add node", &addNode },
    CommandInfo { "closeLoop", "Close loop", &closeLoop },
    CommandInfo { "addNodes", "Add nodes", &addNodes },
    CommandInfo { "removeNodes", "Delete nodes", &removeNodes },
    CommandInfo { "removeNode", "Remove node", &removeNode },
    CommandInfo { "connect", "Connect", &connect },
    CommandInfo { "connectOrCloseLoop", "Connect", &connectOrCloseLoop },
    CommandInfo { "renameComponent", "Rename component", &renameComponent },
    CommandInfo { "exposeParam", "Show on component", &exposeParam },
    CommandInfo { "renameExposedParam", "Rename control", &renameExposedParam },
    CommandInfo { "unexposeParam", "Remove from component", &unexposeParam },
    CommandInfo { "groupNodes", "Group", &groupNodes },
    CommandInfo { "ungroupNode", "Ungroup", &ungroupNode },
    CommandInfo { "addComponentPort", "Add port", &addComponentPort },
    CommandInfo { "renameComponentPort", "Rename port", &renameComponentPort },
    CommandInfo { "setComponentPortChannels", "Port width", &setComponentPortChannels },
    CommandInfo { "removeComponentPort", "Remove port", &removeComponentPort },
    CommandInfo { "disconnect", "Disconnect", &disconnect },
    CommandInfo { "setConnectionBends", "Edit cable", &setConnectionBends },
    CommandInfo { "setModulation", "Route modulation", &setModulation },
    CommandInfo { "mapMacro", "Map to macro", &mapMacro },
    CommandInfo { "renameMacro", "Rename macro", &renameMacro },
    CommandInfo { "setModulationShape", "Edit LFO shape", &setModulationShape },
    CommandInfo { "setNodeParam", "Change parameter", &setNodeParam },
    CommandInfo { "addLink", "Link parameters", &addLink },
    CommandInfo { "setLink", "Change link", &setLink },
    CommandInfo { "removeLink", "Remove link", &removeLink },
    CommandInfo { "linkNodes", "Link devices", &linkNodes },
    CommandInfo { "setNodeCode", "Edit script", &setNodeCode },
    CommandInfo { "setNoteLinks", "Link notes", &setNoteLinks },
    CommandInfo { "setNodeFile", "Load impulse response", &setNodeFile },
    CommandInfo { "addSampleZones", "Add samples", &addSampleZones },
    CommandInfo { "setNodeTarget", "Choose output track", &setNodeTarget },
    CommandInfo { "splitInstrumentOutputs", "Outputs to their own tracks", &splitInstrumentOutputs },
    CommandInfo { "setDrumPad", "Edit pad", &setDrumPad },
    CommandInfo { "copyDrumPad", "Copy pad", &copyDrumPad },
    CommandInfo { "randomiseDrumPads", "Randomise pads", &randomiseDrumPads },
    CommandInfo { "setPluginOutputs", "Use all outputs", &setPluginOutputs },
    CommandInfo { "splitPluginOutputs", "Outputs to their own tracks", &splitPluginOutputs },
    CommandInfo { "setPluginChannels", "Plugin channels", &setPluginChannels },
    CommandInfo { "setBandCount", "Number of bands", &setBandCount },
    CommandInfo { "setStereoMode", "Split mode", &setStereoMode },
    CommandInfo { "setSampleZone", "Edit sample", &setSampleZone },
    CommandInfo { "removeSampleZone", "Remove sample", &removeSampleZone },
    CommandInfo { "setNodePosition", "Move node", &setNodePosition },
    CommandInfo { "convertToVoiceGraph", "Convert to voice graph", &convertToVoiceGraph },
    CommandInfo { "insertNode", "Insert plugin", &insertNode },
    CommandInfo { "reorderNode", "Move plugin", &reorderNode },
    CommandInfo { "setNodeBypass", "Bypass", &setNodeBypass },
    CommandInfo { "renderMidiEffects", "Render MIDI effects", &renderMidiEffects },
    CommandInfo { "setStepCell", "Edit step", &setStepCell },
    CommandInfo { "setStepCells", "Paint steps", &setStepCells },
    CommandInfo { "setStepRow", "Edit step row", &setStepRow },
    CommandInfo { "addStepRow", "Add step row", &addStepRow },
    CommandInfo { "removeStepRow", "Remove step row", &removeStepRow },
    CommandInfo { "setStepPattern", "Edit pattern", &setStepPattern },
    CommandInfo { "copyStepPattern", "Copy pattern", &copyStepPattern },
    CommandInfo { "clearStepPattern", "Clear pattern", &clearStepPattern },
    CommandInfo { "setClipSequencer", "Change sequencer clip", &setClipSequencer },
    CommandInfo { "renderStepClip", "Render steps to MIDI", &renderStepClip },
    CommandInfo { "setPluginMidi", "Change MIDI settings", &setPluginMidi },
    CommandInfo { "batch", "Edit", &batch },
};

[[nodiscard]] const CommandInfo* findCommand (const juce::String& name) noexcept
{
    for (const auto& command : commands)
        if (name == command.name)
            return &command;

    return nullptr;
}

Project batch (const Project& project, const juce::var& args)
{
    auto result = project;

    for (const auto& entry : requireArray (args, "commands"))
    {
        const auto name = entry["command"].toString();

        if (name == "batch")
            throw CommandError ("Batches cannot be nested");

        result = applyCommand (result, name, entry["args"]);
    }

    return result;
}
// Every clip of the project by id (the "before" side of an audio refit).
[[nodiscard]] std::map<ObjectId, const NoteClip*> clipsById (const Project& project)
{
    std::map<ObjectId, const NoteClip*> clips;

    for (const auto& track : project.tracks)
        for (const auto& clip : track->clips)
            clips.emplace (clip->id, clip.get());

    return clips;
}

// Scales a clip's content positions (ticks at the tempo where the clip starts) by `factor` (TM8).
void scaleContent (NoteClip& clip, double factor)
{
    const auto scale = [factor] (Ticks t)
    { return static_cast<Ticks> (std::llround (static_cast<double> (t) * factor)); };

    clip.offset = scale (clip.offset);
    clip.loopStart = scale (clip.loopStart);
    clip.loopLength = clip.loopLength > 0 ? std::max (limits::minLoopLength, scale (clip.loopLength)) : 0;
    clip.fadeIn = scale (clip.fadeIn);
    clip.fadeOut = scale (clip.fadeOut);
}

// Keeps a clip covering the same seconds as the old one, unless the command set its length itself (TM8).
void refitLength (NoteClip& clip, const NoteClip& old, const Project& before, const Project& after)
{
    if (clip.length != old.length)
        return;

    const auto seconds = before.tempoMap.secondsAt (static_cast<double> (old.start + old.length))
                         - before.tempoMap.secondsAt (static_cast<double> (old.start));
    clip.length = timelineLength (after.tempoMap, clip.start, seconds);
}

// Refits one audio clip after a tempo change or a move: its content scales with the tempo at its start, and it
// keeps covering the seconds it did before.
void refitAudioClip (ClipPtr& clipPtr, const NoteClip& old, const Project& before, const Project& after)
{
    auto clip = *clipPtr;
    const auto factor = after.tempoMap.bpmAt (static_cast<double> (clip.start))
                        / before.tempoMap.bpmAt (static_cast<double> (old.start));

    if (std::abs (factor - 1.0) > 1.0e-12)
        scaleContent (clip, factor);

    refitLength (clip, old, before, after);
    clipPtr = std::make_shared<const NoteClip> (std::move (clip));
}

// Refits the audio clips of one track that existed before and need it; returns whether any changed.
[[nodiscard]] bool refitTrack (Track& track, const std::map<ObjectId, const NoteClip*>& previous, const Project& before,
                               const Project& after, bool mapChanged)
{
    auto changed = false;

    for (auto& clipPtr : track.clips)
    {
        const auto it = previous.find (clipPtr->id);

        if (it == previous.end() || (! mapChanged && it->second->start == clipPtr->start))
            continue;

        // Clips of musical files are in musical ticks, like note clips (docs/design/time-stretching.md, TS2).
        if (const auto* file = after.findAudioFile (clipPtr->fileId); file != nullptr && file->timing.musical())
            continue;

        refitAudioClip (clipPtr, *it->second, before, after);
        changed = true;
    }

    return changed;
}

// Audio clips keep their real-time length (docs/design/tempo-map.md, TM8). Their content positions (offset, loop,
// fades, takes) are ticks at the tempo where the clip starts, and their length is timeline ticks. When the map
// changes or a clip moves, both are refitted: content positions scale with the tempo at the start, and the length
// (unless the command set it) covers the same seconds as before.
[[nodiscard]] Project fitAudioClips (const Project& before, Project after)
{
    const auto mapChanged = ! (before.tempoMap == after.tempoMap);
    std::map<ObjectId, const NoteClip*> previous;

    const auto unchanged = [&] (const Track& track)
    {
        const auto* old = before.findTrack (track.id);
        return old != nullptr && old->clips == track.clips;
    };

    for (auto& trackPtr : after.tracks)
    {
        if (trackPtr->kind != TrackKind::audio || trackPtr->clips.empty() || (! mapChanged && unchanged (*trackPtr)))
            continue;

        if (previous.empty())
            previous = clipsById (before);

        auto track = *trackPtr;

        if (refitTrack (track, previous, before, after, mapChanged))
            trackPtr = std::make_shared<const Track> (std::move (track));
    }

    return after;
}

// Copies the notes of an edited clip to the clips linked with it.
[[nodiscard]] Project syncLinkedClips (const Project& project, ObjectId clipId)
{
    const auto location = requireClip (project, clipId);
    const auto& edited = project.tracks[location.track]->clips[location.clip];

    if (edited->linkId == 0)
        return project;

    auto result = project;

    for (auto& trackPtr : result.tracks)
    {
        auto track = *trackPtr;
        bool changed = false;

        for (auto& clipPtr : track.clips)
        {
            if (clipPtr->linkId != edited->linkId || clipPtr->id == clipId)
                continue;

            auto clip = *clipPtr;
            clip.notes = edited->notes;
            clip.controllers = edited->controllers;
            clipPtr = std::make_shared<const NoteClip> (std::move (clip));
            changed = true;
        }

        if (changed)
            trackPtr = std::make_shared<const Track> (std::move (track));
    }

    return result;
}

// A frozen track's clips and graph are locked (docs/design/audio-features.md, AU15). Checked on what the command
// itself made, before audio clips are refitted to a new tempo, which a frozen track allows (its render goes stale).
void refuseFrozenEdits (const Project& before, const Project& after)
{
    for (const auto& track : after.tracks)
    {
        if (! track->freeze.active())
            continue;

        const auto* old = before.findTrack (track->id);

        if (old == nullptr || ! old->freeze.active() || old == track.get())
            continue;

        if (frozenContent (*old) != frozenContent (*track))
            throw CommandError ("Unfreeze the track to edit it");
    }
}

// Automatic crossfades (docs/design/audio-features.md, AU7): an edge of an audio clip that a command placed (added,
// moved, resized or restacked) over audio of a clip below, and that has no fade, gets a crossfade of the project's
// automatic length.
[[nodiscard]] Project autoCrossfades (const Project& before, Project after)
{
    if (after.autoCrossfadeMs <= 0.0)
        return after;

    std::map<ObjectId, const NoteClip*> previous;
    const auto seconds = after.autoCrossfadeMs / 1000.0;

    for (auto& trackPtr : after.tracks)
    {
        if (trackPtr->kind != TrackKind::audio || trackPtr->clips.size() < 2)
            continue;

        if (const auto* old = before.findTrack (trackPtr->id); old != nullptr && old->clips == trackPtr->clips)
            continue;

        if (previous.empty())
            previous = clipsById (before);

        auto track = *trackPtr;
        auto changed = false;

        for (auto& clipPtr : track.clips)
        {
            const auto& clip = *clipPtr;

            if (clip.fileId == 0 || (clip.fadeIn > 0 && clip.fadeOut > 0))
                continue;

            const auto it = previous.find (clip.id);
            const auto placed = it == previous.end() || it->second->start != clip.start
                                || it->second->length != clip.length || it->second->order != clip.order;

            if (! placed)
                continue;

            const auto edges = coveredEdges (trackPtr->clips, clip);
            const auto* file = after.findAudioFile (clip.fileId);
            const auto musical = file != nullptr && file->timing.musical();
            auto faded = clip;

            if (edges.start && faded.fadeIn == 0)
                faded.fadeIn = fadeTicksFor (faded, true, seconds, musical, after.tempoMap);

            if (edges.end && faded.fadeOut == 0)
                faded.fadeOut = fadeTicksFor (faded, false, seconds, musical, after.tempoMap);

            if (faded.fadeIn != clip.fadeIn || faded.fadeOut != clip.fadeOut)
            {
                clipPtr = std::make_shared<const NoteClip> (std::move (faded));
                changed = true;
            }
        }

        if (changed)
            trackPtr = std::make_shared<const Track> (std::move (track));
    }

    return after;
}

// Whether anything plays in a track's Main: an audio track's clips, an instrument other than a Sampler or a Drum
// Synth, or a Sampler zone or a Drum Synth pad on Main or on an output nothing is wired to, which plays in Main
// (docs/design/multi-output-instruments.md, MO2).
[[nodiscard]] bool mainHasSound (const Track& track)
{
    if (track.kind == TrackKind::audio)
        return true;

    for (const auto& node : track.graph.nodes)
    {
        if (node.type == nodetype::sampler)
        {
            const auto onMain = [&] (const SampleZone& zone)
            {
                return zone.output <= 1
                       || std::ranges::none_of (track.graph.connections, [&] (const Connection& c)
                                                { return c.fromNode == node.id && c.fromPort == zone.output - 1; });
            };

            if (std::ranges::any_of (node.sampleZones, onMain))
                return true;

            continue;
        }

        // A Drum Synth's pads that play and are on Main or an output nothing is wired to (DR9).
        if (node.type == nodetype::drumSynth)
        {
            const auto onMain = [&] (const DrumPad& pad)
            {
                return pad.model != DrumModel::off
                       && (pad.output <= 1
                           || std::ranges::none_of (track.graph.connections, [&] (const Connection& c)
                                                    { return c.fromNode == node.id && c.fromPort == pad.output - 1; }));
            };

            if (std::ranges::any_of (drumPadsOf (node), onMain))
                return true;

            continue;
        }

        const auto* descriptor = NodeRegistry::builtin().find (node.type);

        if (node.type == nodetype::pluginInstrument || node.type == nodetype::audioPlayer
            || node.type == nodetype::faust || node.component != nullptr
            || (descriptor != nullptr && isInstrument (*descriptor)))
            return true;
    }

    return false;
}

// The audio effects of a track that becomes a header move onto its new Main output track, with what belongs to them:
// modulation sources that only modulate them and the cables between those, their automation lanes, MIDI mappings and
// links (docs/design/multi-output-instruments.md, MO12). They replace the Main track's default effects. Macro slots go,
// as the Macros device stays on the header.
void moveEffectsToMain (Project& project, Track& source, Track& main)
{
    auto chain = findChain (source.graph);
    auto mainChain = findChain (main.graph);

    if (! chain.has_value() || chain->effects.empty() || ! mainChain.has_value())
        return;

    // The header's effects take the place of the default ones a new track starts with.
    for (const auto id : mainChain->effects)
        removeNodeAndConnections (main.graph, id);

    std::set<ObjectId> moved (chain->effects.begin(), chain->effects.end());

    for (const auto& node : source.graph.nodes)
    {
        if (! nodetype::isModulationSource (node.type))
            continue;

        const auto outgoing = [&] (const Connection& c) { return c.fromNode == node.id; };
        const auto toMoved = [&] (const Connection& c) { return ! outgoing (c) || moved.contains (c.toNode); };

        if (std::ranges::any_of (source.graph.connections, outgoing)
            && std::ranges::all_of (source.graph.connections, toMoved)
            && std::ranges::none_of (source.graph.connections,
                                     [&] (const Connection& c) { return c.toNode == node.id; }))
            moved.insert (node.id);
    }

    for (const auto& connection : source.graph.connections)
        if (moved.contains (connection.fromNode) && moved.contains (connection.toNode)
            && isExtraConnection (source.graph, connection))
            main.graph.connections.push_back (connection);

    for (auto it = source.graph.nodes.begin(); it != source.graph.nodes.end();)
    {
        if (! moved.contains (it->id))
        {
            ++it;
            continue;
        }

        auto node = std::move (*it);
        std::erase_if (node.modulations, [] (const auto& slot) { return slot.second.macro >= 0; });
        main.graph.nodes.push_back (std::move (node));
        it = source.graph.nodes.erase (it);
    }

    std::erase_if (source.graph.connections,
                   [&] (const Connection& c) { return moved.contains (c.fromNode) || moved.contains (c.toNode); });
    mainChain->effects = chain->effects;
    chain->effects.clear();
    rewire (source.graph, *chain);
    rewire (main.graph, *mainChain);

    const auto movesLane = [&] (const AutomationLane& lane) { return moved.contains (lane.target.nodeId); };

    for (const auto& lane : source.automation)
        if (movesLane (lane))
            main.automation.push_back (lane);

    std::erase_if (source.automation, movesLane);

    for (auto& mapping : project.midiMappings)
        if (mapping.trackId == source.id && moved.contains (mapping.nodeId))
            mapping.trackId = main.id;

    for (auto& link : project.links)
        for (auto* address : { &link.source, &link.target })
            if (address->trackId == source.id && moved.contains (address->nodeId))
                address->trackId = main.id;
}

// A track with output tracks is only a header, so its Main gets an output track of its own, as its first output track
// (docs/design/multi-output-instruments.md, MO12): when the track gets its first output tracks, or its Main gets
// something to play again while it has them. A Main output track removed while the Main plays stays removed.
[[nodiscard]] Project withMainOutputs (const Project& before, Project after)
{
    std::set<ObjectId> sources;

    for (const auto& track : after.tracks)
        if (track->kind == TrackKind::output)
            sources.insert (track->source);

    for (const auto sourceId : sources)
    {
        const auto index = after.indexOfTrack (sourceId);
        const auto previous = before.indexOfTrack (sourceId);

        if (index < 0 || previous < 0 || mainOutputOf (after, sourceId) != nullptr)
            continue;

        const auto& source = *after.tracks[static_cast<size_t> (index)];

        if (! mainHasSound (source)
            || (hasOutputTracks (before, sourceId) && mainHasSound (*before.tracks[static_cast<size_t> (previous)])))
            continue;

        Track main;
        main.id = after.nextId++;
        main.kind = TrackKind::output;
        main.source = source.id;
        main.mainOutput = true;
        main.parent = source.parent;
        main.colour = source.colour;
        main.name = source.name + " Main";
        makeBusGraph (main, after);
        auto header = source;
        moveEffectsToMain (after, header, main);
        after.tracks[static_cast<size_t> (index)] = std::make_shared<const Track> (std::move (header));
        after.tracks.insert (after.tracks.begin() + index + 1, std::make_shared<const Track> (std::move (main)));
        normaliseGroups (after);
    }

    return after;
}
} // namespace

Project applyCommand (const Project& project, const juce::String& name, const juce::var& args)
{
    if (const auto* command = findCommand (name))
    {
        auto applied = command->apply (project, args);
        refuseFrozenEdits (project, applied);
        auto result = withMainOutputs (project, autoCrossfades (project, fitAudioClips (project, std::move (applied))));

        if (name == "addNotes" || name == "updateNotes" || name == "removeNotes" || name == "splitNotes"
            || name == "setCurve" || name == "setClipController" || name == "renderMidiEffects")
            result = syncLinkedClips (result, requireId (args, "clipId"));

        // Links whose ends went go too (L12), and built-in targets follow their sources in the model (L13).
        if (! result.links.empty())
        {
            pruneParamLinks (result);
            result = mirrorParamLinks (result);
        }

        return result;
    }

    throw CommandError ("Unknown command '" + name + "'");
}

LinkMapping linkMappingFromVar (const juce::var& value)
{
    LinkMapping mapping;
    const auto kind = value["kind"].toString();

    if (kind == "inverted")
        mapping.kind = LinkMappingKind::inverted;
    else if (kind == "offset")
        mapping.kind = LinkMappingKind::offset;
    else if (kind == "scaled")
        mapping.kind = LinkMappingKind::scaled;
    else if (kind == "range")
        mapping.kind = LinkMappingKind::range;
    else if (kind.isNotEmpty() && kind != "identity")
        throw CommandError ("Unknown link mapping '" + kind + "'");

    mapping.amount = numberOr (value, "amount", mapping.kind == LinkMappingKind::scaled ? 1.0 : 0.0);

    if (value.hasProperty ("centre"))
        mapping.centre = requireNumber (value, "centre");

    mapping.inMin = std::clamp (numberOr (value, "inMin", 0.0), 0.0, 1.0);
    mapping.inMax = std::clamp (numberOr (value, "inMax", 1.0), 0.0, 1.0);
    mapping.outMin = std::clamp (numberOr (value, "outMin", 0.0), 0.0, 1.0);
    mapping.outMax = std::clamp (numberOr (value, "outMax", 1.0), 0.0, 1.0);

    if (value["curve"].isArray())
    {
        mapping.curve = curveFromVar (value["curve"], 0.0, 1.0);

        if (! mapping.curve.empty()
            && (mapping.curve.front().time < 0 || mapping.curve.back().time > modulationShapeTicks))
            throw CommandError ("A link's curve must stay inside its range");
    }

    return mapping;
}

ParamLink paramLinkFromVar (const juce::var& value)
{
    ParamLink link;
    link.id = requireId (value, "id");
    link.source = addressFromVar (value["source"], true);
    link.target = addressFromVar (value["target"], true);

    if (value["mapping"].isObject())
        link.mapping = linkMappingFromVar (value["mapping"]);

    link.active = ! value.hasProperty ("active") || static_cast<bool> (value["active"]);
    return link;
}

void pruneParamLinks (Project& project)
{
    std::erase_if (project.links,
                   [&project] (const ParamLink& link)
                   {
                       for (const auto* address : { &link.source, &link.target })
                       {
                           const auto* node = linkNodeOf (project, *address);

                           // A plugin's parameters aren't known here; one that went stays, unused (L12).
                           if (node == nullptr
                               || (! nodetype::isPlugin (node->type) && ! paramOf (*node, address->param)))
                               return true;
                       }

                       return false;
                   });
}

Project withMaster (const Project& project)
{
    if (project.master != nullptr)
        return project;

    auto result = project;
    Track master;
    master.id = result.nextId++;
    master.name = "Master";
    master.colour = "#9aa0a6";
    master.kind = TrackKind::master;

    const auto input = makeNode (result, nodetype::mixInput, 0.0f, 0.0f);
    const auto gain = makeNode (result, nodetype::gain, 220.0f, 0.0f);
    const auto output = makeNode (result, nodetype::trackOutput, 440.0f, 0.0f);
    master.graph.nodes = { input, gain, output };
    master.graph.connections = { { input.id, 0, gain.id, 0 }, { gain.id, 0, output.id, 0 } };
    result.master = std::make_shared<const Track> (std::move (master));
    return result;
}

Note noteFromVar (const juce::var& fields)
{
    Note note;
    note.id = fields.hasProperty ("id") ? requireId (fields, "id") : 0;
    applyNoteFields (fields, note, true);

    if (const auto& link = fields["link"]; link.isObject())
        note.link = NoteLink { requireId (link, "to"), std::clamp (requireNumber (link, "glideMs"), 0.0, 10000.0) };

    // Files from before the note end lost its anchor have a point there that only holds the value; it changes
    // nothing and is in the way of resizing the note, so it goes.
    for (const auto lane : mpeLanes)
    {
        auto& curve = note.expression[static_cast<size_t> (lane)];

        if (curve.size() >= 2 && curve.back().time == note.length
            && juce::exactlyEqual (curve.back().value, curve[curve.size() - 2].value))
            curve.pop_back();
    }

    return note;
}

Project withPluginState (const Project& project, ObjectId trackId, ObjectId nodeId,
                         std::shared_ptr<const juce::MemoryBlock> state)
{
    return updateGraph (project, makeObject ({ { "trackId", trackId }, { "nodeId", nodeId } }),
                        [&] (GraphScope& scope, Project&)
                        {
                            auto& node = requireNode (scope.graph, nodeId);

                            if (! node.plugin.has_value())
                                throw CommandError ("Not a plugin node");

                            node.pluginState = std::move (state);
                        });
}

juce::String describeCommand (const juce::String& name, const juce::var& args)
{
    if (name == "batch" && args["label"].isString())
        return args["label"].toString();

    if (const auto* command = findCommand (name))
        return command->label;

    return name;
}

} // namespace dawg
