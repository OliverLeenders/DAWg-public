#include "dawg/ProjectFile.h"

#include "dawg/Commands.h"
#include "dawg/DrumSynth.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Sampler.h"
#include "dawg/Scales.h"
#include "dawg/VarUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <utility>

namespace dawg::projectfile
{

namespace
{
[[nodiscard]] const juce::DynamicObject& requireObject (const juce::var& value, const char* what)
{
    const auto* object = value.getDynamicObject();

    if (object == nullptr)
        throw CommandError (juce::String ("Invalid project file: ") + what + " must be an object");

    return *object;
}

[[nodiscard]] double requireNumber (const juce::var& value, const char* what)
{
    if (! (value.isDouble() || value.isInt() || value.isInt64()) || ! std::isfinite (static_cast<double> (value)))
        throw CommandError (juce::String ("Invalid project file: ") + what + " must be a number");

    return static_cast<double> (value);
}

[[nodiscard]] ObjectId requireId (const juce::var& value, const char* what)
{
    const auto id = static_cast<ObjectId> (requireNumber (value, what));

    if (id <= 0)
        throw CommandError (juce::String ("Invalid project file: bad ") + what);

    return id;
}

[[nodiscard]] TrackGraph parseGraph (const juce::var& value, const StateSource& states);

// Whatever the depth: a component in a voice graph may only hold what a voice graph may (docs/design/component-nodes.md, N3).
void requireAllowedInVoiceGraph (const TrackGraph& graph)
{
    for (const auto& node : graph.nodes)
    {
        if (! nodetype::allowedInVoiceGraph (node.type))
            throw CommandError ("Invalid project file: '" + node.type + "' in a voice graph");

        if (node.component != nullptr)
            requireAllowedInVoiceGraph (node.component->graph);
    }
}

// A Voice Instrument's voice graph (docs/design/phase12-voice-scope.md, V1, V16): only node types that may run per
// voice, and exactly one Voice Input and one Voice Output.
[[nodiscard]] std::shared_ptr<const TrackGraph> parseVoiceGraph (const juce::var& value, const StateSource& states)
{
    auto graph = parseGraph (value, states);

    requireAllowedInVoiceGraph (graph);

    for (const auto* fixed : { nodetype::voiceInput, nodetype::voiceOutput })
        if (std::ranges::count (graph.nodes, juce::String (fixed), &GraphNode::type) != 1)
            throw CommandError (juce::String ("Invalid project file: a voice graph needs one ") + fixed);

    return std::make_shared<const TrackGraph> (std::move (graph));
}

// A component's ports, by position.
[[nodiscard]] std::vector<ComponentPort> parseComponentPorts (const juce::var& value)
{
    std::vector<ComponentPort> ports;

    if (const auto* list = value.getArray())
    {
        for (const auto& port : *list)
        {
            const auto type = port["type"].toString();

            if (type != "audio" && type != "events" && type != "modulation")
                throw CommandError ("Invalid project file: bad component port '" + type + "'");

            const auto channels = port.hasProperty ("channels") ? static_cast<int> (port["channels"]) : 2;

            if (channels < 0 || channels > 2)
                throw CommandError ("Invalid project file: bad component port width");

            ports.push_back ({ portTypeNamed (type), port["name"].toString(), channels });
        }
    }

    return ports;
}

// How many levels of components a graph goes down.
[[nodiscard]] int componentDepth (const TrackGraph& graph)
{
    int deepest = 0;

    for (const auto& node : graph.nodes)
        if (node.component != nullptr)
            deepest = std::max (deepest, 1 + componentDepth (node.component->graph));

    return deepest;
}

// A component (docs/design/component-nodes.md, N3): one Component Input and one Component Output, and none of what there can only be
// one of.
[[nodiscard]] std::shared_ptr<const ComponentGraph> parseComponent (const juce::var& value, const StateSource& states)
{
    auto component = std::make_shared<ComponentGraph>();
    component->graph = parseGraph (value["graph"], states);
    component->inputs = parseComponentPorts (value["inputs"]);
    component->outputs = parseComponentPorts (value["outputs"]);
    component->name = value["name"].toString();

    if (const auto* exposed = value["exposed"].getArray())
    {
        for (const auto& param : *exposed)
        {
            const auto nodeId = requireId (param["nodeId"], "exposed parameter");
            auto within = false;
            forEachNode (component->graph, [&] (const GraphNode& node, bool) { within = within || node.id == nodeId; });

            if (! within)
                throw CommandError ("Invalid project file: an exposed parameter of a node outside its component");

            component->exposed.push_back ({ nodeId, param["param"].toString(), param["name"].toString() });
        }
    }

    for (const auto& node : component->graph.nodes)
        if (! nodetype::allowedInComponent (node.type))
            throw CommandError ("Invalid project file: '" + node.type + "' in a component");

    for (const auto* fixed : { nodetype::componentInput, nodetype::componentOutput })
        if (std::ranges::count (component->graph.nodes, juce::String (fixed), &GraphNode::type) != 1)
            throw CommandError (juce::String ("Invalid project file: a component needs one ") + fixed);

    if (componentDepth (component->graph) >= limits::maxComponentDepth)
        throw CommandError ("Invalid project file: components nested too deeply");

    return component;
}

[[nodiscard]] GraphNode parseNode (const juce::var& value, const StateSource& states)
{
    GraphNode node;
    node.id = requireId (value["id"], "node id");
    node.type = value["type"].toString();

    if (NodeRegistry::builtin().find (node.type) == nullptr)
        throw CommandError ("Invalid project file: unknown node type '" + node.type + "'");

    if (const auto* params = value["params"].getDynamicObject())
        for (const auto& property : params->getProperties())
            node.params[property.name.toString()] = requireNumber (property.value, "parameter");

    node.x = static_cast<float> (requireNumber (value.getProperty ("x", 0), "node x"));
    node.y = static_cast<float> (requireNumber (value.getProperty ("y", 0), "node y"));
    node.bypassed = static_cast<bool> (value["bypassed"]);
    node.code = value["code"].toString();

    if (const auto* slots = value["modulations"].getDynamicObject())
    {
        for (const auto& property : slots->getProperties())
        {
            // Keys are port indices; anything else is a damaged file, not port 0.
            const auto key = property.name.toString();

            if (key.isEmpty() || ! key.containsOnly ("0123456789") || key.length() > 4)
                throw CommandError ("Invalid project file: bad modulation port '" + key + "'");

            const auto port = key.getIntValue();

            const auto& fields = property.value;
            ModulationSlot slot;
            slot.param = fields["param"].toString();
            slot.depth = std::clamp (requireNumber (fields.getProperty ("depth", 0.0), "modulation depth"), -1.0, 1.0);
            slot.active = static_cast<bool> (fields.getProperty ("active", true));
            slot.macro = macroOf (fields);
            node.modulations[port] = std::move (slot);
        }
    }

    node.macroNames = macroNamesOf (value);

    if (value["modulationShape"].isArray())
        node.modulationShape = curveFromVar (value["modulationShape"], -1.0, 1.0);

    if (node.type == nodetype::voiceInstrument)
        node.voiceGraph = parseVoiceGraph (value["voice"], states);

    if (node.type == nodetype::component)
        node.component = parseComponent (value["component"], states);

    if (nodetype::playsSamples (node.type))
        node.sampleZones = sampleZonesFromVar (value["zones"]);

    if (node.type == nodetype::drumSynth)
        node.drumPads = drumPadsFromVar (value["pads"]);

    if (node.type == nodetype::stepSequencer)
        node.steps = stepsFromVar (value["steps"]);

    if (nodetype::usesAudioFile (node.type) && value.hasProperty ("fileId"))
        node.fileId = requireId (value["fileId"], "node file");

    if (node.type == nodetype::busOutput && value.hasProperty ("target"))
        node.target = static_cast<ObjectId> (value["target"]);

    node.allOutputs = node.type == nodetype::pluginInstrument && static_cast<bool> (value["allOutputs"]);

    if (node.type == nodetype::pluginEffect)
        node.pluginChannels = pluginChannelsFromName (value["channels"].toString());

    if (node.type == nodetype::bandSplit && value.hasProperty ("bands"))
        node.bands = std::clamp (static_cast<int> (value["bands"]), 2, 4);

    if ((node.type == nodetype::stereoSplit || node.type == nodetype::stereoMerge)
        && value["mode"].toString() == "leftRight")
        node.stereoMode = StereoMode::leftRight;

    if (nodetype::isPlugin (node.type))
    {
        const auto& plugin = value["plugin"];
        node.plugin = PluginRef { plugin["identifier"].toString(), plugin["name"].toString(),
                                  plugin["manufacturer"].toString(), plugin["format"].toString() };

        if (node.plugin->identifier.isEmpty())
            throw CommandError ("Invalid project file: plugin without identifier");

        // A node saved without MIDI settings predates them, when every plugin got MPE.
        const auto& midi = value["midi"];
        node.midi.mpe = static_cast<bool> (midi.getProperty ("mpe", true));
        node.midi.bendRange = std::clamp (
            static_cast<int> (midi.getProperty ("bendRange", MidiOutputSettings::defaultBendRange (node.midi.mpe))),
            limits::minBendRange, limits::maxBendRange);

        if (value["stateFile"].isString())
        {
            node.pluginState = states (value["stateFile"].toString());

            if (node.pluginState == nullptr)
                throw CommandError ("Invalid project file: missing plugin state " + value["stateFile"].toString());
        }
        else if (value["state"].isString()) // version 0: inline base64
        {
            auto state = std::make_shared<juce::MemoryBlock>();

            if (! state->fromBase64Encoding (value["state"].toString()))
                throw CommandError ("Invalid project file: bad plugin state");

            node.pluginState = std::move (state);
        }
    }

    return node;
}

[[nodiscard]] TrackGraph parseGraph (const juce::var& value, const StateSource& states)
{
    TrackGraph graph;

    for (const auto& property : requireObject (value["nodes"], "graph nodes").getProperties())
        graph.nodes.push_back (parseNode (property.value, states));

    // Creation order is the id order (ids are handed out increasingly).
    std::ranges::sort (graph.nodes, {}, &GraphNode::id);

    for (const auto& property : requireObject (value["connections"], "graph connections").getProperties())
    {
        const auto& c = property.value;
        Connection connection { requireId (c["fromNode"], "connection"),
                                static_cast<int> (requireNumber (c["fromPort"], "port")),
                                requireId (c["toNode"], "connection"),
                                static_cast<int> (requireNumber (c["toPort"], "port")) };

        // A cable's bends on the canvas (docs/design/graph-grid-and-cables.md, G9); absent for the usual curve.
        if (const auto* bends = c["bends"].getArray())
        {
            if (bends->size() > static_cast<int> (limits::maxCableBends))
                throw CommandError ("Invalid project file: a cable with too many bends");

            for (const auto& p : *bends)
                connection.bends.push_back ({ static_cast<float> (requireNumber (p["x"], "bend x")),
                                              static_cast<float> (requireNumber (p["y"], "bend y")) });
        }

        if (graph.indexOfNode (connection.fromNode) < 0 || graph.indexOfNode (connection.toNode) < 0)
            throw CommandError ("Invalid project file: connection to a missing node");

        graph.connections.push_back (connection);
    }

    return graph;
}

// A pool file's timing (docs/design/time-stretching.md, TS2); absent means one-shot.
[[nodiscard]] FileTiming fileTimingFromVar (const juce::var& value)
{
    FileTiming timing;

    if (! value.isObject())
        return timing;

    timing.mode = value["mode"].toString() == "musical" ? TimingMode::musical : TimingMode::oneShot;
    timing.style = value["musicalStyle"].toString() == "repitch" ? MusicalStyle::repitch : MusicalStyle::stretch;
    timing.sourceBpm = std::clamp (requireNumber (value.getProperty ("sourceBpm", 120.0), "source tempo"),
                                   limits::minSourceBpm, limits::maxSourceBpm);

    if (const auto* markers = value["markers"].getArray())
        for (const auto& m : *markers)
            timing.markers.push_back (
                { requireNumber (m["tick"], "marker tick"), requireNumber (m["frame"], "marker frame") });

    normaliseTimingMarkers (timing.markers);
    return timing;
}

[[nodiscard]] ClipPtr parseClip (const juce::var& value)
{
    NoteClip clip;
    clip.id = requireId (value["id"], "clip id");
    clip.start = std::llround (requireNumber (value["start"], "clip start"));
    clip.length = std::llround (requireNumber (value["length"], "clip length"));
    clip.offset =
        std::max (Ticks { 0 },
                  static_cast<Ticks> (std::llround (requireNumber (value.getProperty ("offset", 0), "clip offset"))));
    clip.loopStart =
        std::max (Ticks { 0 },
                  static_cast<Ticks> (std::llround (requireNumber (value.getProperty ("loopStart", 0), "loop start"))));
    clip.loopLength = std::llround (requireNumber (value.getProperty ("loopLength", 0), "loop length"));

    if (clip.loopLength != 0 && clip.loopLength < limits::minLoopLength)
        throw CommandError ("Invalid project file: clip loop too short");

    clip.name = value["name"].toString();
    clip.colour = value["colour"].toString();
    clip.linkId = static_cast<ObjectId> (requireNumber (value.getProperty ("linkId", 0), "link id"));
    clip.sequencerPattern =
        std::clamp (static_cast<int> (value.getProperty ("sequencerPattern", -1)), -1, StepSequence::maxPatterns - 1);
    clip.sequencerStartStep =
        std::clamp (static_cast<int> (value.getProperty ("sequencerStartStep", 0)), 0, StepSequence::maxSteps - 1);
    clip.fileId = static_cast<ObjectId> (requireNumber (value.getProperty ("fileId", 0), "file id"));
    clip.gainDb =
        std::clamp (requireNumber (value.getProperty ("gainDb", 0), "clip gain"), limits::minGainDb, limits::maxGainDb);
    clip.fadeIn = std::max (
        Ticks { 0 }, static_cast<Ticks> (std::llround (requireNumber (value.getProperty ("fadeIn", 0), "fade in"))));
    clip.fadeOut = std::max (
        Ticks { 0 }, static_cast<Ticks> (std::llround (requireNumber (value.getProperty ("fadeOut", 0), "fade out"))));
    clip.fadeInShape =
        static_cast<FadeShape> (std::clamp (static_cast<int> (value.getProperty ("fadeInShape", 0)), 0, 3));
    clip.fadeOutShape =
        static_cast<FadeShape> (std::clamp (static_cast<int> (value.getProperty ("fadeOutShape", 0)), 0, 3));
    clip.transpose = std::clamp (requireNumber (value.getProperty ("transpose", 0), "transpose"), limits::minTranspose,
                                 limits::maxTranspose);
    clip.lane = std::max (0, static_cast<int> (value.getProperty ("lane", 0)));
    clip.order = static_cast<std::int64_t> (static_cast<double> (value.getProperty ("order", 0)));

    if (clip.start < 0 || clip.length < 1 || clip.start > limits::maxPosition || clip.length > limits::maxPosition)
        throw CommandError ("Invalid project file: clip out of range");

    if (const auto* controllers = value["controllers"].getDynamicObject())
        for (const auto& property : controllers->getProperties())
            clip.controllers[std::clamp (property.name.toString().getIntValue(), 0, controllerKeyPressure)] =
                curveFromVar (property.value, 0.0, 1.0);

    for (const auto& property : requireObject (value["notes"], "clip notes").getProperties())
        clip.notes.push_back (std::make_shared<const Note> (noteFromVar (property.value)));

    std::ranges::sort (clip.notes, [] (const NotePtr& a, const NotePtr& b)
                       { return std::tie (a->start, a->id) < std::tie (b->start, b->id); });
    return std::make_shared<const NoteClip> (std::move (clip));
}

[[nodiscard]] AutomationLane parseLane (const juce::var& value)
{
    AutomationLane lane;
    lane.id = requireId (value["id"], "automation lane id");
    const auto kind = value["kind"].toString();

    if (kind == "pan")
        lane.target.kind = AutomationTargetKind::pan;
    else if (kind == "node")
        lane.target.kind = AutomationTargetKind::nodeParam;
    else if (kind == "plugin")
        lane.target.kind = AutomationTargetKind::pluginParam;
    else if (kind == "send")
        lane.target.kind = AutomationTargetKind::send;

    lane.target.nodeId = static_cast<ObjectId> (value["nodeId"]);
    lane.target.param = value["param"].toString();
    lane.target.name = value["name"].toString();
    lane.points = curveFromVar (value["points"], 0.0, 1.0);
    lane.active = static_cast<bool> (value.getProperty ("active", true));
    lane.height = std::clamp (static_cast<int> (value.getProperty ("height", 48)), limits::minTrackHeight,
                              limits::maxTrackHeight);
    return lane;
}

[[nodiscard]] TrackKind parseKind (const juce::String& kind)
{
    if (kind == "audio")
        return TrackKind::audio;

    if (kind == "master")
        return TrackKind::master;

    if (kind == "group")
        return TrackKind::group;

    if (kind == "return")
        return TrackKind::returnTrack;

    if (kind == "output")
        return TrackKind::output;

    return TrackKind::instrument;
}

[[nodiscard]] TrackPtr parseTrack (const juce::var& value, const StateSource& states)
{
    Track track;
    track.id = requireId (value["id"], "track id");
    track.name = value["name"].toString();
    track.colour = value.getProperty ("colour", track.colour).toString();
    track.gainDb = std::clamp (requireNumber (value.getProperty ("gainDb", 0), "track gain"), limits::minGainDb,
                               limits::maxGainDb);
    track.kind = parseKind (value["kind"].toString());
    track.parent = static_cast<ObjectId> (value.getProperty ("parent", 0));
    track.source = track.kind == TrackKind::output ? static_cast<ObjectId> (value.getProperty ("source", 0)) : 0;
    track.mainOutput = track.kind == TrackKind::output && static_cast<bool> (value["mainOutput"]);
    track.folded = static_cast<bool> (value["folded"]);

    // Sends (docs/design/groups-and-sends.md, R4), in the order they were added: by id.
    if (const auto* sends = value["sends"].getDynamicObject())
    {
        for (const auto& property : sends->getProperties())
        {
            const auto& fields = property.value;
            Send send;
            send.id = requireId (fields["id"], "send id");
            send.target = requireId (fields["target"], "send target");
            send.targetNode = static_cast<ObjectId> (fields.getProperty ("targetNode", 0));
            send.levelDb = std::clamp (static_cast<double> (fields.getProperty ("levelDb", 0.0)), limits::minGainDb,
                                       limits::maxGainDb);
            send.preFader = static_cast<bool> (fields["preFader"]);
            send.active = static_cast<bool> (fields.getProperty ("active", true));
            track.sends.push_back (send);
        }

        std::ranges::sort (track.sends, {}, &Send::id);
    }
    track.pan = std::clamp (static_cast<double> (value.getProperty ("pan", 0.0)), limits::minPan, limits::maxPan);

    // The master mixes the whole output, so it has no offset of its own; a file that carries one is normalised.
    track.offsetMs = track.kind == TrackKind::master
                         ? 0.0
                         : std::clamp (requireNumber (value.getProperty ("offsetMs", 0.0), "track offset"),
                                       limits::minOffsetMs, limits::maxOffsetMs);
    track.drumMode = value.getProperty ("drumMode", track.drumMode).toString();
    track.mute = static_cast<bool> (value["mute"]);
    track.solo = static_cast<bool> (value["solo"]);
    track.inputChannel = static_cast<int> (value.getProperty ("inputChannel", -1));
    track.inputStereo = static_cast<bool> (value["inputStereo"]);
    track.monitor = static_cast<MonitorMode> (std::clamp (static_cast<int> (value.getProperty ("monitor", 1)), 0, 2));
    track.height = std::clamp (static_cast<int> (requireNumber (value.getProperty ("height", 64), "track height")),
                               limits::minTrackHeight, limits::maxTrackHeight);

    track.lanes = std::clamp (static_cast<int> (value.getProperty ("lanes", 1)), 1, limits::maxLanes);
    track.lanesExpanded = static_cast<bool> (value["lanesExpanded"]);

    if (const auto& freeze = value["freeze"]; freeze.isObject())
    {
        track.freeze = { requireId (freeze["fileId"], "frozen render"),
                         std::llround (requireNumber (freeze["start"], "frozen start")),
                         freeze["key"].toString(),
                         {} };

        if (const auto* outputs = freeze["outputs"].getArray())
            for (const auto& output : *outputs)
                track.freeze.outputs.push_back ({ requireId (output["node"], "frozen output node"),
                                                  requireId (output["fileId"], "frozen output") });
    }

    for (const auto& property : requireObject (value["clips"], "track clips").getProperties())
    {
        auto clip = parseClip (property.value);

        // A clip past the track's lanes (a hand-edited file) goes into its last lane.
        if (clip->lane >= track.lanes)
        {
            auto moved = *clip;
            moved.lane = track.lanes - 1;
            clip = std::make_shared<const NoteClip> (std::move (moved));
        }

        track.clips.push_back (std::move (clip));
    }

    std::ranges::sort (track.clips, [] (const ClipPtr& a, const ClipPtr& b)
                       { return std::tie (a->start, a->id) < std::tie (b->start, b->id); });
    track.graph = parseGraph (value["graph"], states);

    for (const auto& node : track.graph.nodes)
    {
        if (node.type == nodetype::voiceInput || node.type == nodetype::voiceOutput)
            throw CommandError ("Invalid project file: '" + node.type + "' outside a voice graph");

        if (node.type == nodetype::componentInput || node.type == nodetype::componentOutput)
            throw CommandError ("Invalid project file: '" + node.type + "' outside a component");
    }

    track.automationMode =
        static_cast<AutomationMode> (std::clamp (static_cast<int> (value.getProperty ("automationMode", 1)), 0, 4));
    track.automationExpanded = static_cast<bool> (value["automationExpanded"]);

    if (const auto* order = value["automationOrder"].getArray())
        for (const auto& id : *order)
            track.automation.push_back (parseLane (value["automation"][juce::Identifier (id.toString())]));
    return std::make_shared<const Track> (std::move (track));
}

// The largest id used anywhere in the project.
[[nodiscard]] ObjectId largestId (const Project& project)
{
    ObjectId largest = 0;
    auto tracks = project.tracks;

    if (project.master != nullptr)
        tracks.push_back (project.master);

    for (const auto& track : tracks)
    {
        largest = std::max (largest, track->id);

        for (const auto& clip : track->clips)
        {
            largest = std::max (largest, clip->id);

            largest = std::max (largest, clip->linkId);

            for (const auto& note : clip->notes)
                largest = std::max (largest, note->id);
        }

        forEachNode (track->graph, [&largest] (const GraphNode& node, bool) { largest = std::max (largest, node.id); });

        for (const auto& lane : track->automation)
            largest = std::max (largest, lane.id);

        for (const auto& send : track->sends)
            largest = std::max (largest, send.id);
    }

    for (const auto& marker : project.markers)
        largest = std::max (largest, marker.id);

    for (const auto& file : project.audioFiles)
        largest = std::max (largest, file.id);

    for (const auto& link : project.links)
        largest = std::max (largest, link.id);

    return largest;
}

constexpr const char* projectEntry = "project.json";

[[nodiscard]] juce::String stateEntry (ObjectId trackId, ObjectId nodeId)
{
    return "plugins/" + juce::String (trackId) + "-" + juce::String (nodeId) + ".bin";
}

[[nodiscard]] juce::var nodeVar (const juce::var& graphJson, ObjectId nodeId)
{
    const auto direct = graphJson["nodes"][juce::Identifier (juce::String (nodeId))];

    if (! direct.isVoid())
        return direct;

    if (const auto* nodes = graphJson["nodes"].getDynamicObject())
    {
        for (const auto& property : nodes->getProperties())
        {
            if (! property.value.hasProperty ("component"))
                continue;

            if (const auto found = nodeVar (property.value["component"]["graph"], nodeId); ! found.isVoid())
                return found;
        }
    }

    return {};
}

// Moves the previous version of a file into Backups/, keeping the newest backupsKept.
void backUp (const juce::File& target)
{
    const auto folder = target.getParentDirectory().getChildFile ("Backups");

    if (! folder.createDirectory())
        return;

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    std::ignore = target.copyFileTo (
        folder.getChildFile (target.getFileNameWithoutExtension() + " " + stamp + target.getFileExtension()));

    auto existing = folder.findChildFiles (juce::File::findFiles, false,
                                           target.getFileNameWithoutExtension() + " *" + target.getFileExtension());
    // The timestamp sorts chronologically.
    std::ranges::sort (existing,
                       [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });

    for (int i = 0; i + backupsKept < existing.size(); ++i)
        std::ignore = existing.getReference (i).deleteFile();
}
} // namespace

juce::var toVar (const Project& project, const Metadata& metadata,
                 std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>>& states)
{
    auto json = dawg::toVar (project);

    // Plugin state is stored as separate entries, which the UI form leaves out anyway.
    const auto storeStates = [&] (const Track& track, const juce::var& trackJson)
    {
        forEachNode (track.graph,
                     [&] (const GraphNode& node, bool)
                     {
                         if (node.pluginState == nullptr)
                             return;

                         const auto entry = stateEntry (track.id, node.id);
                         states[entry] = node.pluginState;

                         const auto nodeJson = nodeVar (trackJson["graph"], node.id);

                         if (auto* object = nodeJson.getDynamicObject())
                             object->setProperty ("stateFile", entry);
                     });
    };

    for (const auto& track : project.tracks)
        storeStates (*track, json["tracks"][juce::Identifier (juce::String (track->id))]);

    if (project.master != nullptr)
        storeStates (*project.master, json["master"]);

    return makeObject ({
        { "format", formatName },
        { "version", version },
        { "id", metadata.id },
        { "appVersion", metadata.appVersion },
        { "nextId", project.nextId },
        { "project", json },
    });
}

namespace
{
// The EQ's bands before version 6, when each gained an "in use" switch (docs/design/channel-strip.md, C1): their
// on switch, gain, and whether they were on by default. Files leave a parameter out unless it was changed.
struct Version5EqBand
{
    const char* on;
    const char* gain;
    const char* used;
    bool onByDefault;
};

constexpr std::array<Version5EqBand, 8> version5EqBands { {
    { "hpOn", nullptr, "hpUsed", false },
    { "lowOn", "lowGain", "lowUsed", true },
    { "mid1On", "mid1Gain", "mid1Used", true },
    { "mid2On", "mid2Gain", "mid2Used", true },
    { "mid3On", "mid3Gain", "mid3Used", true },
    { "mid4On", "mid4Gain", "mid4Used", true },
    { "highOn", "highGain", "highUsed", true },
    { "lpOn", nullptr, "lpUsed", false },
} };

// The EQ's frequencies whose ranges widened to 20 Hz - 20 kHz in version 6, with their version 5 ranges.
constexpr std::array<ParamInfo, 4> version5EqFrequencies { {
    { "hpFreq", "HP freq", 20.0f, 2000.0f, 30.0f, ParamKind::continuous, "Hz", 200.0f },
    { "lowFreq", "Low freq", 20.0f, 1000.0f, 120.0f, ParamKind::continuous, "Hz", 150.0f },
    { "highFreq", "High freq", 1000.0f, 20000.0f, 10000.0f, ParamKind::continuous, "Hz", 5000.0f },
    { "lpFreq", "LP freq", 1000.0f, 20000.0f, 18000.0f, ParamKind::continuous, "Hz", 5000.0f },
} };

// A node's parameter as "<nodeId>:<param>".
[[nodiscard]] juce::String paramKey (const juce::var& nodeId, const juce::var& param)
{
    return juce::String (static_cast<juce::int64> (nodeId)) + ":" + param.toString();
}

// The "<nodeId>:<param>" of every parameter a track's lanes or modulations aim at, or the project's MIDI mappings.
void addTargetedParams (const juce::var& track, std::set<juce::String>& targeted)
{
    if (const auto* lanes = track["automation"].getDynamicObject())
        for (const auto& lane : lanes->getProperties())
            if (lane.value["kind"].toString() == "node")
                targeted.insert (paramKey (lane.value["nodeId"], lane.value["param"]));

    if (const auto* nodes = track["graph"]["nodes"].getDynamicObject())
        for (const auto& node : nodes->getProperties())
            if (const auto* slots = node.value["modulations"].getDynamicObject())
                for (const auto& slot : slots->getProperties())
                    targeted.insert (paramKey (node.value["id"], slot.value["param"]));
}

// The version 6 range of a widened EQ frequency.
[[nodiscard]] const ParamInfo& version6EqFrequency (const char* id)
{
    const auto* eq = NodeRegistry::builtin().find (nodetype::eq);
    return eq->params[static_cast<size_t> (eq->indexOfParam (id))];
}

// Marks the bands of an EQ in use where they could be heard in version 5, so the panel shows them and projects
// sound the same: shelves and bells (on by default) with a gain or whose gain something moves, and HPs and LPs (off
// by default, now on by default) that were switched either way or whose switch something moves. The widened
// frequencies' modulation depths are rescaled to sweep about the same range.
void migrateVersion5Eq (juce::DynamicObject& node, const std::set<juce::String>& targeted)
{
    if (! node.getProperty ("params").isObject())
        node.setProperty ("params", juce::var (newObject().get()));

    auto* params = node.getProperty ("params").getDynamicObject();
    const auto id = node.getProperty ("id");

    for (const auto& band : version5EqBands)
    {
        if (band.onByDefault)
        {
            if (std::abs (static_cast<double> (params->getProperty (band.gain))) >= 0.01
                || targeted.contains (paramKey (id, band.gain)))
                params->setProperty (band.used, 1.0);
        }
        else if (params->hasProperty (band.on) || targeted.contains (paramKey (id, band.on)))
        {
            params->setProperty (band.used, 1.0);

            if (! params->hasProperty (band.on))
                params->setProperty (band.on, 0.0);
        }
    }

    if (const auto* slots = node.getProperty ("modulations").getDynamicObject())
        for (const auto& slot : slots->getProperties())
            for (const auto& old : version5EqFrequencies)
                if (auto* fields = slot.value.getDynamicObject();
                    fields != nullptr && fields->getProperty ("param").toString() == old.id)
                {
                    const auto& wide = version6EqFrequency (old.id);
                    const auto span = paramToNormalised (wide, old.maxValue) - paramToNormalised (wide, old.minValue);
                    fields->setProperty ("depth", span * static_cast<double> (fields->getProperty ("depth")));
                }
}

// Migrates a track's EQs to version 6 (migrateVersion5Eq), and the points of lanes on their widened frequencies to
// the same frequencies on the new ranges. Free handles are relative to their segment, so they stay.
void migrateVersion5Eqs (const juce::var& track, const std::set<juce::String>& targeted)
{
    std::set<juce::int64> ids;

    if (const auto* nodes = track["graph"]["nodes"].getDynamicObject())
        for (const auto& property : nodes->getProperties())
            if (auto* node = property.value.getDynamicObject();
                node != nullptr && node->getProperty ("type").toString() == nodetype::eq)
            {
                ids.insert (static_cast<juce::int64> (node->getProperty ("id")));
                migrateVersion5Eq (*node, targeted);
            }

    const auto* lanes = track["automation"].getDynamicObject();

    if (ids.empty() || lanes == nullptr)
        return;

    for (const auto& lane : lanes->getProperties())
    {
        if (lane.value["kind"].toString() != "node" || ! ids.contains (static_cast<juce::int64> (lane.value["nodeId"])))
            continue;

        const auto param = lane.value["param"].toString();
        const auto old =
            std::ranges::find_if (version5EqFrequencies, [&] (const ParamInfo& p) { return param == p.id; });

        if (old == version5EqFrequencies.end())
            continue;

        if (auto* points = lane.value["points"].getArray())
            for (auto& point : *points)
                if (auto* fields = point.getArray(); fields != nullptr && fields->size() > 1)
                    fields->set (1, paramToNormalised (version6EqFrequency (old->id),
                                                       paramFromNormalised (*old, static_cast<double> ((*fields)[1]))));
    }
}

// Version 5 -> 6: the EQ's bands are hidden until added to the panel, and its shelves' and filters' frequencies
// range from 20 Hz to 20 kHz (migrateVersion5Eqs).
[[nodiscard]] juce::var migrate5To6 (const juce::var& json)
{
    const auto model = json["project"].clone();

    if (const auto* object = model.getDynamicObject())
    {
        std::set<juce::String> targeted;

        if (const auto* mappings = object->getProperty ("midiMappings").getArray())
            for (const auto& mapping : *mappings)
                targeted.insert (paramKey (mapping["nodeId"], mapping["param"]));

        std::vector<juce::var> tracks;

        if (const auto* all = object->getProperty ("tracks").getDynamicObject())
            for (const auto& property : all->getProperties())
                tracks.push_back (property.value);

        tracks.push_back (object->getProperty ("master"));

        for (const auto& track : tracks)
            addTargetedParams (track, targeted);

        for (const auto& track : tracks)
            migrateVersion5Eqs (track, targeted);
    }

    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 6);
    result.getDynamicObject()->setProperty ("project", model);
    return result;
}

// The EQ's slopes before version 7: 12 or 24 dB/octave as 0 or 1, now the indices of 6 to 48 dB/octave.
constexpr std::array<const char*, 2> version6EqSlopes { "hpSlope", "lpSlope" };

[[nodiscard]] double version7EqSlope (double version6) noexcept
{
    return version6 > 0.5 ? 3.0 : 1.0;
}

[[nodiscard]] bool isVersion6EqSlope (const juce::String& param)
{
    return std::ranges::any_of (version6EqSlopes, [&param] (const char* slope) { return param == slope; });
}

// Moves one EQ's slopes to version 7: the values, and the modulation depths aimed at them (scaled to the new
// range). An unset slope stays unset: the new default is the old one, 12 dB/octave.
void migrateVersion6Slope (juce::DynamicObject& node)
{
    if (auto* params = node.getProperty ("params").getDynamicObject())
        for (const auto* slope : version6EqSlopes)
            if (params->hasProperty (slope))
                params->setProperty (slope, version7EqSlope (params->getProperty (slope)));

    if (const auto* slots = node.getProperty ("modulations").getDynamicObject())
        for (const auto& slot : slots->getProperties())
            if (auto* fields = slot.value.getDynamicObject();
                fields != nullptr && isVersion6EqSlope (fields->getProperty ("param").toString()))
                fields->setProperty ("depth", 2.0 / 7.0 * static_cast<double> (fields->getProperty ("depth")));
}

// Moves a track's EQ slopes to version 7 (migrateVersion6Slope), and the points of lanes on them (normalised over
// the old 0-1 range and the new 0-7).
void migrateVersion6Slopes (const juce::var& track)
{
    std::set<juce::int64> ids;

    if (const auto* nodes = track["graph"]["nodes"].getDynamicObject())
        for (const auto& property : nodes->getProperties())
            if (auto* node = property.value.getDynamicObject();
                node != nullptr && node->getProperty ("type").toString() == nodetype::eq)
            {
                ids.insert (static_cast<juce::int64> (node->getProperty ("id")));
                migrateVersion6Slope (*node);
            }

    const auto* lanes = track["automation"].getDynamicObject();

    if (ids.empty() || lanes == nullptr)
        return;

    for (const auto& lane : lanes->getProperties())
        if (lane.value["kind"].toString() == "node" && ids.contains (static_cast<juce::int64> (lane.value["nodeId"]))
            && isVersion6EqSlope (lane.value["param"].toString()))
            if (auto* points = lane.value["points"].getArray())
                for (auto& point : *points)
                    if (auto* fields = point.getArray(); fields != nullptr && fields->size() > 1)
                        fields->set (1, version7EqSlope (static_cast<double> ((*fields)[1])) / 7.0);
}

// Version 6 -> 7: the EQ's HP and LP have 6 to 48 dB/octave in 6 dB steps (migrateVersion6Slopes).
[[nodiscard]] juce::var migrate6To7 (const juce::var& json)
{
    const auto model = json["project"].clone();

    if (const auto* object = model.getDynamicObject())
    {
        if (const auto* tracks = object->getProperty ("tracks").getDynamicObject())
            for (const auto& property : tracks->getProperties())
                migrateVersion6Slopes (property.value);

        migrateVersion6Slopes (object->getProperty ("master"));
    }

    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 7);
    result.getDynamicObject()->setProperty ("project", model);
    return result;
}

// Before the LFO's tempo-synced cycle became a note length it was "syncBeats", a number of beats. Version 7 still
// read such files and kept the old value without using it, so the cycle fell back to a quarter note and saving
// wrote both.
constexpr const char* legacyLfoSyncBeats = "syncBeats";

// The note length nearest an old cycle in beats, as an index into lfoCycleBeats (nearest on a log scale, so 0.25
// beats is 1/16 and 1.0 is 1/4).
[[nodiscard]] double lfoCycleForBeats (double beats) noexcept
{
    const auto target = std::log2 (std::max (beats, lfoCycleBeats.front()));
    const auto nearest = std::ranges::min_element (lfoCycleBeats, {}, [target] (double length)
                                                   { return std::abs (std::log2 (length) - target); });
    return static_cast<double> (std::distance (lfoCycleBeats.begin(), nearest));
}

// Moves a track's LFOs to version 8: an old "syncBeats" becomes the nearest note length, unless the file already
// has a cycle (one set since the change), and is dropped either way.
void migrateVersion7Lfos (const juce::var& track)
{
    if (const auto* nodes = track["graph"]["nodes"].getDynamicObject())
        for (const auto& property : nodes->getProperties())
            if (const auto* node = property.value.getDynamicObject();
                node != nullptr && node->getProperty ("type").toString() == nodetype::lfo)
                if (auto* params = node->getProperty ("params").getDynamicObject();
                    params != nullptr && params->hasProperty (legacyLfoSyncBeats))
                {
                    if (! params->hasProperty ("cycle"))
                        params->setProperty ("cycle", lfoCycleForBeats (params->getProperty (legacyLfoSyncBeats)));

                    params->removeProperty (legacyLfoSyncBeats);
                }
}

// Version 7 -> 8: LFOs saved with a cycle in beats keep it as a note length (migrateVersion7Lfos).
[[nodiscard]] juce::var migrate7To8 (const juce::var& json)
{
    const auto model = json["project"].clone();

    if (const auto* object = model.getDynamicObject())
    {
        if (const auto* tracks = object->getProperty ("tracks").getDynamicObject())
            for (const auto& property : tracks->getProperties())
                migrateVersion7Lfos (property.value);

        migrateVersion7Lfos (object->getProperty ("master"));
    }

    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 8);
    result.getDynamicObject()->setProperty ("project", model);
    return result;
}

// Moves a track's plugin instruments to version 9: plain MIDI used a fixed ±2 semitone bend range and ignored the
// stored one (48 by default), which it now follows, so a plain MIDI node gets 2.
void migrateVersion8Midi (const juce::var& track)
{
    if (const auto* nodes = track["graph"]["nodes"].getDynamicObject())
        for (const auto& property : nodes->getProperties())
            if (auto* midi = property.value["midi"].getDynamicObject();
                midi != nullptr && midi->hasProperty ("mpe") && ! static_cast<bool> (midi->getProperty ("mpe")))
                midi->setProperty ("bendRange", MidiOutputSettings::plainBendRange);
}

// Version 8 -> 9: plain MIDI plugin instruments keep their ±2 semitone bend range (migrateVersion8Midi).
[[nodiscard]] juce::var migrate8To9 (const juce::var& json)
{
    const auto model = json["project"].clone();

    if (const auto* object = model.getDynamicObject())
    {
        if (const auto* tracks = object->getProperty ("tracks").getDynamicObject())
            for (const auto& property : tracks->getProperties())
                migrateVersion8Midi (property.value);

        migrateVersion8Midi (object->getProperty ("master"));
    }

    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 9);
    result.getDynamicObject()->setProperty ("project", model);
    return result;
}

// Moves a track's gains to version 10: the gain in dB can no longer be modulated, so its slots aim at the amplitude.
void migrateVersion9Gains (const juce::var& track)
{
    if (const auto* nodes = track["graph"]["nodes"].getDynamicObject())
        for (const auto& property : nodes->getProperties())
            if (property.value["type"].toString() == nodetype::gain)
                if (const auto* slots = property.value["modulations"].getDynamicObject())
                    for (const auto& slot : slots->getProperties())
                        if (auto* object = slot.value.getDynamicObject();
                            object != nullptr && object->getProperty ("param").toString() == "gainDb")
                            object->setProperty ("param", "amplitude");
}

// Version 9 -> 10: modulation of a gain's dB moves to its amplitude (migrateVersion9Gains).
[[nodiscard]] juce::var migrate9To10 (const juce::var& json)
{
    const auto model = json["project"].clone();

    if (const auto* object = model.getDynamicObject())
    {
        if (const auto* tracks = object->getProperty ("tracks").getDynamicObject())
            for (const auto& property : tracks->getProperties())
                migrateVersion9Gains (property.value);

        migrateVersion9Gains (object->getProperty ("master"));
    }

    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 10);
    result.getDynamicObject()->setProperty ("project", model);
    return result;
}

// Version 10 -> 11: Voice Instruments with their voice graphs (docs/design/phase12-voice-scope.md, V16). Older files
// have none, so nothing changes.
[[nodiscard]] juce::var migrate10To11 (const juce::var& json)
{
    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 11);
    return result;
}

// Version 11 -> 12: a track's drum mode (docs/design/humanise-and-presets.md). Older tracks go by their names.
[[nodiscard]] juce::var migrate11To12 (const juce::var& json)
{
    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 12);
    return result;
}

// Version 13 -> 14: components (docs/design/component-nodes.md, N10). No older project has one.
[[nodiscard]] juce::var migrate13To14 (const juce::var& json)
{
    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 14);
    return result;
}

// Version 12 -> 13: linked notes (docs/design/linked-notes.md). Older notes have no links.
[[nodiscard]] juce::var migrate12To13 (const juce::var& json)
{
    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 13);
    return result;
}

// Version 15 -> 16: time stretching (docs/design/time-stretching.md, TS6). Pool files are one-shot and clips have no
// transpose by default, so older projects sound the same; the parser fills in both.
[[nodiscard]] juce::var migrate15To16 (const juce::var& json)
{
    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 16);
    return result;
}

// Version 16 -> 17: audio clip operations (docs/design/audio-features.md, AU9). No older pool file is a reversed copy,
// and the parser fills in reverseOf.
[[nodiscard]] juce::var migrate16To17 (const juce::var& json)
{
    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 17);
    return result;
}

// Version 14 -> 15: takes lanes (docs/design/takes-lanes.md, T9). A clip's stack order is its place on the track, and
// a loop-recorded audio clip with several takes becomes one clip per take, each in a lane of its own, the last pass
// on top where it played before; the takes fields go.
// Format 15: a clip's takes become clips of their own in the takes lanes below it (docs/design/takes-lanes.md).
void takesToLanes (juce::DynamicObject& track, juce::DynamicObject& clips, juce::int64& nextId)
{
    std::vector<juce::var> added;
    int lanes = 1;
    juce::int64 order = 0;

    for (const auto& entry : clips.getProperties())
    {
        auto* clip = entry.value.getDynamicObject();
        const auto takes = static_cast<int> (clip->getProperty ("takes"));
        const auto takeStart = static_cast<double> (clip->getProperty ("takeStart"));
        const auto takeLength = static_cast<double> (clip->getProperty ("takeLength"));
        // Orders spaced out, so the takes below a clip fit between it and the clip before.
        order += limits::maxLanes + 1;
        clip->setProperty ("order", order);
        clip->setProperty ("lane", 0);

        if (takes > 1 && takeLength > 0)
        {
            // The clip keeps the take it plays, on top in lane 0; each other take goes below it, the
            // newest first, so the passes read downwards from the last.
            const auto offset = static_cast<double> (clip->getProperty ("offset"));
            const auto within = std::fmod (offset - takeStart, takeLength);
            const auto playing = static_cast<int> ((offset - takeStart) / takeLength);
            int lane = 1;

            for (int take = takes - 1; take >= 0; --take)
            {
                if (take == playing)
                    continue;

                auto copy = entry.value.clone();
                auto* fields = copy.getDynamicObject();
                fields->setProperty ("id", ++nextId);
                fields->setProperty ("offset", takeStart + take * takeLength + within);
                fields->setProperty ("order", order - lane); // below the clip that plays, newest first
                fields->setProperty ("lane", lane++);
                fields->removeProperty ("takes");
                fields->removeProperty ("takeStart");
                fields->removeProperty ("takeLength");
                added.push_back (copy);
            }

            lanes = std::max (lanes, lane);
        }

        clip->removeProperty ("takes");
        clip->removeProperty ("takeStart");
        clip->removeProperty ("takeLength");
    }

    for (const auto& copy : added)
        clips.setProperty (juce::String (static_cast<juce::int64> (static_cast<double> (copy["id"]))), copy);

    track.setProperty ("lanes", lanes);
}

[[nodiscard]] juce::var migrate14To15 (const juce::var& json)
{
    const auto model = json["project"].clone();
    auto nextId = static_cast<juce::int64> (static_cast<double> (json["nextId"]));

    if (const auto* object = model.getDynamicObject())
    {
        if (const auto* tracks = object->getProperty ("tracks").getDynamicObject())
        {
            for (const auto& property : tracks->getProperties())
            {
                auto* track = property.value.getDynamicObject();
                auto* clips = track != nullptr ? track->getProperty ("clips").getDynamicObject() : nullptr;

                if (clips == nullptr)
                    continue;

                takesToLanes (*track, *clips, nextId);
            }
        }
    }

    auto result = json.clone();
    result.getDynamicObject()->setProperty ("version", 15);
    result.getDynamicObject()->setProperty ("nextId", nextId + 1);
    result.getDynamicObject()->setProperty ("project", model);
    return result;
}

[[nodiscard]] TempoMap tempoMapFromVar (const juce::var& points, const juce::var& signatures)
{
    if (points.getArray() == nullptr || points.getArray()->isEmpty())
        throw CommandError ("Project has no tempo");

    std::vector<TempoPoint> tempo;
    std::vector<TimeSignature> bars;

    for (const auto& point : *points.getArray())
        tempo.push_back ({ static_cast<Ticks> (requireNumber (point["tick"], "tempo point")),
                           requireNumber (point["bpm"], "tempo"), static_cast<bool> (point["ramp"]) });

    if (const auto* list = signatures.getArray())
        for (const auto& signature : *list)
            bars.push_back ({ static_cast<int> (signature["bar"]),
                              static_cast<int> (requireNumber (signature["numerator"], "time signature")),
                              static_cast<int> (requireNumber (signature["denominator"], "time signature")) });

    return { std::move (tempo), std::move (bars) };
}
} // namespace

juce::var migrate (const juce::var& json)
{
    auto result = json;

    if (result["format"].toString() != formatName)
        throw CommandError ("Not a DAWg project file");

    if (static_cast<int> (result["version"]) > version)
        throw CommandError ("This project was saved by a newer version of DAWg");

    // Files before version 5 predate the current Loop Reverb and aren't read.
    if (static_cast<int> (result["version"]) < oldestVersion)
        throw CommandError ("This project was saved by a version of DAWg too old to open");

    if (static_cast<int> (result["version"]) == 5)
        result = migrate5To6 (result);

    if (static_cast<int> (result["version"]) == 6)
        result = migrate6To7 (result);

    if (static_cast<int> (result["version"]) == 7)
        result = migrate7To8 (result);

    if (static_cast<int> (result["version"]) == 8)
        result = migrate8To9 (result);

    if (static_cast<int> (result["version"]) == 9)
        result = migrate9To10 (result);

    if (static_cast<int> (result["version"]) == 10)
        result = migrate10To11 (result);

    if (static_cast<int> (result["version"]) == 11)
        result = migrate11To12 (result);

    if (static_cast<int> (result["version"]) == 12)
        result = migrate12To13 (result);

    if (static_cast<int> (result["version"]) == 13)
        result = migrate13To14 (result);

    if (static_cast<int> (result["version"]) == 14)
        result = migrate14To15 (result);

    if (static_cast<int> (result["version"]) == 15)
        result = migrate15To16 (result);

    if (static_cast<int> (result["version"]) == 16)
        result = migrate16To17 (result);

    return result;
}

void applySampleZoneFields (const juce::var& fields, SampleZone& zone)
{
    const auto integer = [&fields] (const char* name, int& field, int low, int high)
    {
        if (fields.hasProperty (name))
            field = std::clamp (static_cast<int> (std::lround (requireNumber (fields[name], name))), low, high);
    };
    const auto frames = [&fields] (const char* name, juce::int64& field)
    {
        if (fields.hasProperty (name))
            field = std::max<juce::int64> (0, static_cast<juce::int64> (requireNumber (fields[name], name)));
    };
    const auto number = [&fields] (const char* name, double& field, double low, double high)
    {
        if (fields.hasProperty (name))
            field = std::clamp (requireNumber (fields[name], name), low, high);
    };

    integer ("lowKey", zone.lowKey, 0, 127);
    integer ("highKey", zone.highKey, 0, 127);
    integer ("rootKey", zone.rootKey, 0, 127);
    integer ("lowVelocity", zone.lowVelocity, 0, 127);
    integer ("highVelocity", zone.highVelocity, 0, 127);
    frames ("start", zone.start);
    frames ("end", zone.end);
    number ("tune", zone.tune, -48.0, 48.0);
    number ("gainDb", zone.gainDb, -60.0, 24.0);
    number ("pan", zone.pan, -1.0, 1.0);
    frames ("loopStart", zone.loopStart);
    frames ("loopEnd", zone.loopEnd);
    frames ("crossfade", zone.crossfade);
    integer ("chokeGroup", zone.chokeGroup, 0, maxChokeGroup);
    integer ("layer", zone.layer, 1, maxSampleLayers);
    integer ("output", zone.output, 1, maxSampleOutputs);
    integer ("fadeInLowVelocity", zone.fadeInLowVelocity, 0, 127);
    integer ("fadeInHighVelocity", zone.fadeInHighVelocity, 0, 127);
    integer ("fadeOutLowVelocity", zone.fadeOutLowVelocity, 0, 127);
    integer ("fadeOutHighVelocity", zone.fadeOutHighVelocity, 0, 127);

    number ("attackMs", zone.attackMs, 0.0, 10000.0);
    number ("decayMs", zone.decayMs, 0.0, 10000.0);
    number ("sustain", zone.sustain, 0.0, 1.0);
    number ("releaseMs", zone.releaseMs, 0.0, 10000.0);
    number ("attackHandleTime", zone.attackHandleTime, 0.0, 1.0);
    number ("attackHandleLevel", zone.attackHandleLevel, 0.0, 1.0);
    number ("decayHandleTime", zone.decayHandleTime, 0.0, 1.0);
    number ("decayHandleLevel", zone.decayHandleLevel, 0.0, 1.0);
    number ("releaseHandleTime", zone.releaseHandleTime, 0.0, 1.0);
    number ("releaseHandleLevel", zone.releaseHandleLevel, 0.0, 1.0);

    if (fields.hasProperty ("mode"))
    {
        const auto mode = fields["mode"].toString();
        zone.mode = SampleMode::oneShot;

        if (mode == "loop")
            zone.mode = SampleMode::loop;
        else if (mode == "gate")
            zone.mode = SampleMode::gate;
    }

    zone.highKey = std::max (zone.highKey, zone.lowKey);
    zone.highVelocity = std::max (zone.highVelocity, zone.lowVelocity);
}

StepSequence normalisedSteps (StepSequence steps)
{
    if (steps.rows.size() > static_cast<size_t> (StepSequence::maxRows))
        steps.rows.resize (static_cast<size_t> (StepSequence::maxRows));

    for (auto& row : steps.rows)
        row.pitch = std::clamp (row.pitch, 0, 127);

    if (steps.patterns.empty())
        steps.patterns.emplace_back();

    if (steps.patterns.size() > static_cast<size_t> (StepSequence::maxPatterns))
        steps.patterns.resize (static_cast<size_t> (StepSequence::maxPatterns));

    for (auto& pattern : steps.patterns)
    {
        pattern.rate = std::clamp (pattern.rate, 0, static_cast<int> (stepRates.size()) - 1);
        pattern.swing = std::clamp (pattern.swing, 0.5f, 0.75f);
        pattern.rows.resize (steps.rows.size());

        for (auto& row : pattern.rows)
        {
            row.length = std::clamp (row.length, 1, StepSequence::maxSteps);
            row.cells.resize (static_cast<size_t> (StepSequence::maxSteps));

            for (auto& cell : row.cells)
            {
                cell.velocity = std::clamp (cell.velocity, 0.0f, 1.0f);
                cell.length = std::clamp (cell.length, 0.1f, 16.0f);
                cell.chance = std::clamp (cell.chance, 0.0f, 1.0f);
                cell.ratchet = std::clamp (cell.ratchet, 1, 4);
            }
        }
    }

    return steps;
}

std::shared_ptr<const StepSequence> stepsFromVar (const juce::var& value)
{
    StepSequence steps;

    if (const auto* rows = value["rows"].getArray())
        for (const auto& row : *rows)
            steps.rows.push_back ({ static_cast<int> (row.getProperty ("pitch", 36)), row["name"].toString() });

    if (const auto* patterns = value["patterns"].getArray())
    {
        for (const auto& item : *patterns)
        {
            StepPattern pattern;
            pattern.rate = static_cast<int> (item.getProperty ("rate", 2));
            pattern.swing = static_cast<float> (static_cast<double> (item.getProperty ("swing", 0.5)));

            if (const auto* rows = item["rows"].getArray())
            {
                for (const auto& r : *rows)
                {
                    StepPattern::Row row;
                    row.length = static_cast<int> (r.getProperty ("length", 16));

                    if (const auto* cells = r["cells"].getArray())
                    {
                        for (const auto& c : *cells)
                        {
                            StepCell cell;

                            if (const auto* fields = c.getArray(); fields != nullptr && fields->size() >= 5)
                            {
                                cell.on = static_cast<bool> ((*fields)[0]);
                                cell.velocity = static_cast<float> (static_cast<double> ((*fields)[1]));
                                cell.length = static_cast<float> (static_cast<double> ((*fields)[2]));
                                cell.chance = static_cast<float> (static_cast<double> ((*fields)[3]));
                                cell.ratchet = static_cast<int> ((*fields)[4]);
                            }

                            row.cells.push_back (cell);

                            if (row.cells.size() >= static_cast<size_t> (StepSequence::maxSteps))
                                break;
                        }
                    }

                    pattern.rows.push_back (std::move (row));
                }
            }

            steps.patterns.push_back (std::move (pattern));
        }
    }

    return std::make_shared<const StepSequence> (normalisedSteps (std::move (steps)));
}

std::vector<SampleZone> sampleZonesFromVar (const juce::var& value)
{
    std::vector<SampleZone> zones;

    if (const auto* list = value.getArray())
    {
        for (const auto& fields : *list)
        {
            SampleZone zone;
            zone.id = requireId (fields["id"], "sample zone id");
            zone.fileId = requireId (fields["fileId"], "sample zone file");
            applySampleZoneFields (fields, zone);
            zones.push_back (zone);
        }
    }

    return zones;
}

std::vector<DrumPad> drumPadsFromVar (const juce::var& value)
{
    std::vector<DrumPad> pads;
    const auto* list = value.getArray();

    if (list == nullptr)
        return pads;

    if (list->size() != drumPadCount)
        throw CommandError ("A Drum Synth has " + juce::String (drumPadCount) + " pads, not "
                            + juce::String (list->size()));

    for (const auto& fields : *list)
    {
        DrumPad pad;
        pad.name = fields["name"].toString();
        const auto model = drumModelFromId (fields.getProperty ("model", "off").toString());

        if (! model.has_value())
            throw CommandError ("Unknown drum model '" + fields["model"].toString() + "'");

        pad.model = *model;
        pad.output = std::clamp (static_cast<int> (fields.getProperty ("output", 1)), 1, maxDrumOutputs);
        pad.chokeGroup = std::clamp (static_cast<int> (fields.getProperty ("choke", 0)), 0, maxDrumChokeGroup);
        pad.mono = static_cast<bool> (fields["mono"]);
        pad.locked = static_cast<bool> (fields["locked"]);
        pads.push_back (std::move (pad));
    }

    return pads;
}

juce::var savedComponentVar (const GraphNode& node)
{
    auto json = componentToVar (node);

    if (node.component != nullptr)
        forEachNode (node.component->graph,
                     [&] (const GraphNode& inner, bool)
                     {
                         if (inner.pluginState == nullptr)
                             return;

                         if (auto* object = nodeVar (json["graph"], inner.id).getDynamicObject())
                             object->setProperty ("state", inner.pluginState->toBase64Encoding());
                     });

    return json;
}

int macroOf (const juce::var& fields)
{
    if (! fields.hasProperty ("macro"))
        return -1;

    const auto macro = requireNumber (fields["macro"], "modulation macro");

    if (macro < -1.0 || macro >= maxMacros || ! juce::exactlyEqual (macro, std::floor (macro)))
        throw CommandError ("Invalid modulation macro " + juce::String (macro));

    return static_cast<int> (macro);
}

std::vector<juce::String> macroNamesOf (const juce::var& node)
{
    std::vector<juce::String> names;

    if (const auto* list = node["macroNames"].getArray())
        for (const auto& name : *list)
            if (std::cmp_less (names.size(), maxMacros))
                names.push_back (name.toString().substring (0, 64));

    return names;
}

std::shared_ptr<const ComponentGraph> componentFromVar (const juce::var& json)
{
    // A pasted plugin's state stays behind, as the UI's form of a model has none. A saved component has it inline, which parsing reads.
    return parseComponent (json, [] (const juce::String&) { return std::shared_ptr<const juce::MemoryBlock> {}; });
}

std::shared_ptr<const TrackGraph> voiceGraphFromVar (const juce::var& json)
{
    // A voice graph holds no plugins, so no plugin state is read.
    return parseVoiceGraph (json, [] (const juce::String&) { return std::shared_ptr<const juce::MemoryBlock> {}; });
}

namespace
{
// The project's keys (docs/design/scale-highlighting.md, SH9), sorted by bar with one on bar 0; no key when the file
// has none.
[[nodiscard]] std::vector<KeyChange> keysFromVar (const juce::var& value)
{
    std::vector<KeyChange> keys;

    if (const auto* list = value.getArray())
    {
        for (const auto& item : *list)
        {
            KeyChange change;
            change.bar = std::max (0, static_cast<int> (item.getProperty ("bar", 0)));
            change.key.enabled = static_cast<bool> (item["enabled"]);
            change.key.root = std::clamp (static_cast<int> (item.getProperty ("root", 0)), 0, 11);
            change.key.scale =
                std::clamp (static_cast<int> (item.getProperty ("scale", 0)), 0, static_cast<int> (scales.size()) - 1);
            std::erase_if (keys, [&] (const KeyChange& k) { return k.bar == change.bar; });
            keys.push_back (change);
        }
    }

    std::ranges::sort (keys, {}, &KeyChange::bar);

    if (keys.empty() || keys.front().bar != 0)
        keys.insert (keys.begin(), KeyChange {});

    return keys;
}
} // namespace

Loaded fromVar (const juce::var& json, const StateSource& states)
{
    const auto& model = json["project"];
    Loaded loaded;
    loaded.metadata.id = json["id"].toString();
    loaded.metadata.appVersion = json["appVersion"].toString();

    auto& project = loaded.project;
    project.tempoMap = tempoMapFromVar (model["tempoMap"], model["signatures"]);
    project.metronome = static_cast<bool> (model["metronome"]);
    project.autoCrossfadeMs =
        std::clamp (static_cast<double> (model.getProperty ("autoCrossfadeMs", 10.0)), 0.0, limits::maxCrossfadeMs);
    project.keys = keysFromVar (model["keys"]);

    if (const auto* mappings = model["midiMappings"].getArray())
    {
        for (const auto& m : *mappings)
            project.midiMappings.push_back (
                { m["device"].toString(), std::clamp (static_cast<int> (m.getProperty ("channel", 1)), 1, 16),
                  std::clamp (static_cast<int> (m["controller"]), 0, 127), static_cast<ObjectId> (m["trackId"]),
                  static_cast<ObjectId> (m["nodeId"]), m["param"].toString() });
    }

    // Parameter links (docs/design/parameter-linking.md, L1); one whose ends are gone is dropped once the tracks are
    // read (pruneParamLinks).
    if (const auto* links = model["links"].getArray())
        for (const auto& link : *links)
            project.links.push_back (paramLinkFromVar (link));

    if (const auto* markers = model["markers"].getDynamicObject())
    {
        for (const auto& property : markers->getProperties())
        {
            const auto& m = property.value;
            project.markers.push_back ({ requireId (m["id"], "marker id"),
                                         std::llround (requireNumber (m["position"], "marker position")),
                                         m["name"].toString() });
        }

        std::ranges::sort (project.markers, [] (const Marker& a, const Marker& b)
                           { return std::tie (a.position, a.id) < std::tie (b.position, b.id); });
    }

    if (const auto* files = model["audioFiles"].getDynamicObject())
    {
        for (const auto& property : files->getProperties())
        {
            const auto& f = property.value;
            auto file = audioFile (requireId (f["id"], "audio file id"), f["path"].toString(), f["name"].toString(),
                                   requireNumber (f["sampleRate"], "sample rate"),
                                   static_cast<int> (requireNumber (f["channels"], "channels")),
                                   static_cast<juce::int64> (requireNumber (f["frames"], "frames")));
            file.timing = fileTimingFromVar (f["timing"]);
            file.pitch = filePitchFromVar (f["pitch"], file.frames);
            file.suggestedBpm = std::max (0.0, requireNumber (f.getProperty ("suggestedBpm", 0), "suggested tempo"));
            file.reverseOf =
                static_cast<ObjectId> (std::max (0.0, requireNumber (f.getProperty ("reverseOf", 0), "reversed file")));
            project.audioFiles.push_back (std::move (file));
        }

        std::ranges::sort (project.audioFiles, {}, &AudioFile::id);
    }

    if (const auto& loop = model["loop"]; loop.isObject())
    {
        project.loop.enabled = static_cast<bool> (loop["enabled"]);
        project.loop.start = std::llround (requireNumber (loop["start"], "loop start"));
        project.loop.end = std::llround (requireNumber (loop["end"], "loop end"));

        if (project.loop.start < 0 || project.loop.end <= project.loop.start)
            throw CommandError ("Invalid project file: bad loop range");
    }

    if (const auto& punch = model["punch"]; punch.isObject())
    {
        project.punch.enabled = static_cast<bool> (punch["enabled"]);
        project.punch.start = std::llround (requireNumber (punch["start"], "punch-in"));
        project.punch.end = std::llround (requireNumber (punch["end"], "punch-out"));

        if (project.punch.start < 0 || project.punch.end <= project.punch.start)
            throw CommandError ("Invalid project file: bad punch range");
    }

    const auto* order = model["trackOrder"].getArray();

    if (order == nullptr)
        throw CommandError ("Invalid project file: missing track order");

    std::set<ObjectId> seen;

    for (const auto& id : *order)
    {
        const auto& track = model["tracks"][juce::Identifier (id.toString())];

        if (track.getDynamicObject() == nullptr || ! seen.insert (static_cast<ObjectId> (id)).second)
            throw CommandError ("Invalid project file: bad track order");

        project.tracks.push_back (parseTrack (track, states));
    }

    // Groups (docs/design/groups-and-sends.md, R2): a file whose groups don't hold together is repaired.
    normaliseGroups (project);

    // The master track (docs/design/mixing-basics.md, M4). Files from before it have none; the app adds the
    // default one when it opens them (withMaster()).
    if (const auto& master = model["master"]; master.isObject())
    {
        project.master = parseTrack (master, states);

        if (project.master->kind != TrackKind::master || seen.contains (project.master->id))
            throw CommandError ("Invalid project file: bad master track");
    }

    pruneParamLinks (project);

    project.nextId =
        std::max (static_cast<ObjectId> (requireNumber (json["nextId"], "nextId")), largestId (project) + 1);
    return loaded;
}

juce::String write (const Project& project, const Metadata& metadata, const juce::File& target, bool backups)
{
    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> states;
    const auto json = juce::JSON::toString (toVar (project, metadata, states));
    const auto now = juce::Time::getCurrentTime();

    juce::ZipFile::Builder zip;
    constexpr int jsonCompression = 6;
    zip.addEntry (std::make_unique<juce::MemoryInputStream> (json.toRawUTF8(), json.getNumBytesAsUTF8(), true),
                  jsonCompression, projectEntry, now);

    for (const auto& [path, state] : states)
        zip.addEntry (std::make_unique<juce::MemoryInputStream> (*state, true), 0, path, now);

    // Write next to the target, so the final rename stays on one volume (F2).
    const juce::TemporaryFile temporary (target);

    {
        auto stream = temporary.getFile().createOutputStream();

        if (stream == nullptr)
            return "Cannot write to " + target.getParentDirectory().getFullPathName();

        if (! zip.writeToStream (*stream, nullptr))
            return "Writing " + target.getFileName() + " failed";

        stream->flush();

        if (stream->getStatus().failed())
            return stream->getStatus().getErrorMessage();
    }

    if (backups && target.existsAsFile())
        backUp (target);

    if (! temporary.overwriteTargetFileWithTemporary())
        return "Could not replace " + target.getFullPathName();

    return {};
}

Loaded read (const juce::File& file)
{
    if (! file.existsAsFile())
        throw CommandError ("File not found: " + file.getFullPathName());

    juce::MemoryBlock data;

    if (! file.loadFileAsData (data))
        throw CommandError ("Cannot read " + file.getFullPathName());

    Loaded loaded;
    const auto isZip = data.getSize() > 2 && data[0] == 'P' && data[1] == 'K';

    if (isZip)
    {
        juce::MemoryInputStream input (data, false);
        juce::ZipFile zip (input);

        const auto entryData = [&zip] (const juce::String& path) -> std::shared_ptr<const juce::MemoryBlock>
        {
            const auto index = zip.getIndexOfFileName (path);
            const std::unique_ptr<juce::InputStream> stream (index >= 0 ? zip.createStreamForEntry (index) : nullptr);

            if (stream == nullptr)
                return nullptr;

            auto block = std::make_shared<juce::MemoryBlock>();
            stream->readIntoMemoryBlock (*block);
            return block;
        };

        const auto json = entryData (projectEntry);

        if (json == nullptr)
            throw CommandError ("Not a DAWg project file");

        loaded = fromVar (migrate (juce::JSON::parse (json->toString())), entryData);
    }
    else
    {
        loaded = fromVar (migrate (juce::JSON::parse (data.toString())), [] (const juce::String&) { return nullptr; });
    }

    if (loaded.metadata.id.isEmpty())
        loaded.metadata.id = juce::Uuid().toString();

    return loaded;
}

Summary summarise (const Project& project)
{
    Ticks end = 0;

    for (const auto& track : project.tracks)
        for (const auto& clip : track->clips)
            end = std::max (end, clip->start + clip->length);

    return { project.tempoMap.bpmAt (0.0), static_cast<int> (project.tracks.size()),
             project.tempoMap.secondsAt (static_cast<double> (end)) };
}

} // namespace dawg::projectfile
