#include "dawg/Model.h"

#include "dawg/DrumVoices.h"
#include "dawg/JsonPatch.h"
#include "dawg/NodeRegistry.h"
#include "dawg/VarUtils.h"

#include <functional>
#include <limits>
#include <map>
#include <set>

namespace dawg
{

const char* portTypeName (PortType type) noexcept
{
    switch (type)
    {
        case PortType::events:
            return "events";
        case PortType::modulation:
            return "modulation";
        case PortType::audio:
            break;
    }

    return "audio";
}

const char* pluginChannelsName (PluginChannels channels) noexcept
{
    switch (channels)
    {
        case PluginChannels::monoToStereo:
            return "monoToStereo";
        case PluginChannels::stereo:
            return "stereo";
        case PluginChannels::automatic:
            break;
    }

    return "auto";
}

PluginChannels pluginChannelsFromName (const juce::String& name) noexcept
{
    if (name == "monoToStereo")
        return PluginChannels::monoToStereo;

    return name == "stereo" ? PluginChannels::stereo : PluginChannels::automatic;
}

PortType portTypeNamed (const juce::String& name) noexcept
{
    if (name == "audio")
        return PortType::audio;

    return name == "events" ? PortType::events : PortType::modulation;
}

const char* sampleModeName (SampleMode mode) noexcept
{
    switch (mode)
    {
        case SampleMode::gate:
            return "gate";
        case SampleMode::loop:
            return "loop";
        case SampleMode::oneShot:
            break;
    }

    return "oneShot";
}

namespace
{
template <typename Container, typename Fn>
[[nodiscard]] int indexWhere (const Container& items, Fn&& matches) noexcept
{
    for (size_t i = 0; i < items.size(); ++i)
        if (matches (items[i]))
            return static_cast<int> (i);

    return -1;
}

[[nodiscard]] juce::String key (ObjectId id)
{
    return juce::String (id);
}

[[nodiscard]] juce::var toVar (const Curve& curve)
{
    juce::Array<juce::var> points;

    for (const auto& p : curve)
    {
        auto fields = juce::Array<juce::var> { p.time, p.value, static_cast<int> (p.shape), p.tension };

        if (p.shape == CurveShape::free)
        {
            fields.add (p.handleTime);
            fields.add (p.handleValue);
        }

        points.add (fields);
    }

    return points;
}

[[nodiscard]] juce::var controllersVar (const std::map<int, Curve>& controllers)
{
    const auto result = newObject();

    for (const auto& [controller, curve] : controllers)
        result->setProperty (juce::String (controller), toVar (curve));

    return juce::var (result.get());
}

[[nodiscard]] const char* targetKindName (AutomationTargetKind kind) noexcept
{
    switch (kind)
    {
        case AutomationTargetKind::pan:
            return "pan";
        case AutomationTargetKind::nodeParam:
            return "node";
        case AutomationTargetKind::pluginParam:
            return "plugin";
        case AutomationTargetKind::send:
            return "send";
        case AutomationTargetKind::gain:
            break;
    }

    return "gain";
}

// Lanes keyed by id, so editing one lane is one patch operation.
[[nodiscard]] juce::var automationVar (const std::vector<AutomationLane>& lanes)
{
    const auto result = newObject();

    for (const auto& lane : lanes)
        result->setProperty (key (lane.id), makeObject ({ { "id", lane.id },
                                                          { "kind", targetKindName (lane.target.kind) },
                                                          { "nodeId", lane.target.nodeId },
                                                          { "param", lane.target.param },
                                                          { "name", lane.target.name },
                                                          { "points", toVar (lane.points) },
                                                          { "active", lane.active },
                                                          { "height", lane.height } }));

    return juce::var (result.get());
}

// Sends keyed by id, so changing one is one patch operation; ids grow, so their order is the order they were added.
[[nodiscard]] juce::var sendsVar (const std::vector<Send>& sends)
{
    const auto result = newObject();

    for (const auto& send : sends)
        result->setProperty (key (send.id), makeObject ({ { "id", send.id },
                                                          { "target", send.target },
                                                          { "targetNode", send.targetNode },
                                                          { "levelDb", send.levelDb },
                                                          { "preFader", send.preFader },
                                                          { "active", send.active } }));

    return juce::var (result.get());
}

[[nodiscard]] juce::var automationOrder (const std::vector<AutomationLane>& lanes)
{
    juce::Array<juce::var> order;

    for (const auto& lane : lanes)
        order.add (lane.id);

    return order;
}

[[nodiscard]] juce::var toVar (const TrackGraph& graph);

[[nodiscard]] juce::var toVar (const GraphNode& node)
{
    const auto params = newObject();
    const auto modulations = newObject();

    for (const auto& [id, value] : node.params)
        params->setProperty (id, value);

    for (const auto& [port, slot] : node.modulations)
    {
        auto fields = makeObject ({ { "param", slot.param }, { "depth", slot.depth }, { "active", slot.active } });

        if (slot.macro >= 0)
            fields.getDynamicObject()->setProperty ("macro", slot.macro);

        modulations->setProperty (juce::String (port), fields);
    }

    auto result = makeObject ({
        { "id", node.id },
        { "type", node.type },
        { "params", juce::var (params.get()) },
        { "x", node.x },
        { "y", node.y },
        { "bypassed", node.bypassed },
        { "modulations", juce::var (modulations.get()) },
        { "modulationShape", toVar (node.modulationShape) },
    });

    if (node.code.isNotEmpty())
        result.getDynamicObject()->setProperty ("code", node.code);

    if (! node.macroNames.empty())
    {
        juce::Array<juce::var> names;

        for (const auto& name : node.macroNames)
            names.add (name);

        result.getDynamicObject()->setProperty ("macroNames", names);
    }

    if (! node.drumPads.empty())
    {
        juce::Array<juce::var> pads;

        for (const auto& pad : node.drumPads)
            pads.add (makeObject ({ { "name", pad.name },
                                    { "model", drumModelId (pad.model) },
                                    { "output", pad.output },
                                    { "choke", pad.chokeGroup },
                                    { "mono", pad.mono },
                                    { "locked", pad.locked } }));

        result.getDynamicObject()->setProperty ("pads", pads);
    }

    if (node.voiceGraph != nullptr)
        result.getDynamicObject()->setProperty ("voice", toVar (*node.voiceGraph));

    if (node.component != nullptr)
    {
        const auto ports = [] (const std::vector<ComponentPort>& list)
        {
            juce::Array<juce::var> entries;

            for (const auto& port : list)
            {
                auto entry = makeObject ({ { "type", portTypeName (port.type) }, { "name", port.name } });

                // Saved only when not stereo (docs/design/mono-ports.md, MP12).
                if (port.type == PortType::audio && port.channels != 2)
                    entry.getDynamicObject()->setProperty ("channels", port.channels);

                entries.add (entry);
            }

            return juce::var (entries);
        };

        juce::Array<juce::var> exposed;

        for (const auto& param : node.component->exposed)
            exposed.add (makeObject ({ { "nodeId", param.nodeId }, { "param", param.param }, { "name", param.name } }));

        result.getDynamicObject()->setProperty ("component",
                                                makeObject ({ { "name", node.component->name },
                                                              { "graph", toVar (node.component->graph) },
                                                              { "inputs", ports (node.component->inputs) },
                                                              { "outputs", ports (node.component->outputs) },
                                                              { "exposed", juce::var (exposed) } }));
    }

    if (node.fileId != 0)
        result.getDynamicObject()->setProperty ("fileId", node.fileId);

    if (node.type == nodetype::busOutput)
        result.getDynamicObject()->setProperty ("target", node.target);

    if (node.allOutputs)
        result.getDynamicObject()->setProperty ("allOutputs", true);

    // Saved only when not Auto (docs/design/mono-ports.md, MP12).
    if (node.pluginChannels != PluginChannels::automatic)
        result.getDynamicObject()->setProperty ("channels", pluginChannelsName (node.pluginChannels));

    // Saved only when not the default (docs/design/split-nodes.md, section 6).
    if (node.type == nodetype::bandSplit && node.bands != 3)
        result.getDynamicObject()->setProperty ("bands", node.bands);

    if (node.stereoMode == StereoMode::leftRight)
        result.getDynamicObject()->setProperty ("mode", "leftRight");

    // A step sequencer's rows and patterns (docs/design/step-sequencer.md). Steps are written as [on, velocity, length,
    // chance, ratchet] up to each row's last lit one, so an empty pattern stays small.
    if (node.steps != nullptr)
    {
        juce::Array<juce::var> rows;

        for (const auto& row : node.steps->rows)
            rows.add (makeObject ({ { "pitch", row.pitch }, { "name", row.name } }));

        juce::Array<juce::var> patterns;

        for (const auto& pattern : node.steps->patterns)
        {
            juce::Array<juce::var> patternRows;

            for (const auto& row : pattern.rows)
            {
                auto last = static_cast<int> (row.cells.size());

                while (last > 0 && row.cells[static_cast<size_t> (last - 1)] == StepCell {})
                    --last;

                juce::Array<juce::var> cells;

                for (int i = 0; i < last; ++i)
                {
                    const auto& cell = row.cells[static_cast<size_t> (i)];
                    cells.add (
                        juce::Array<juce::var> { cell.on, cell.velocity, cell.length, cell.chance, cell.ratchet });
                }

                patternRows.add (makeObject ({ { "length", row.length }, { "cells", cells } }));
            }

            patterns.add (
                makeObject ({ { "rate", pattern.rate }, { "swing", pattern.swing }, { "rows", patternRows } }));
        }

        result.getDynamicObject()->setProperty ("steps", makeObject ({ { "rows", rows }, { "patterns", patterns } }));
    }

    if (! node.sampleZones.empty())
    {
        juce::Array<juce::var> zones;

        for (const auto& zone : node.sampleZones)
            zones.add (makeObject ({
                { "id", zone.id },
                { "fileId", zone.fileId },
                { "lowKey", zone.lowKey },
                { "highKey", zone.highKey },
                { "rootKey", zone.rootKey },
                { "lowVelocity", zone.lowVelocity },
                { "highVelocity", zone.highVelocity },
                { "start", zone.start },
                { "end", zone.end },
                { "tune", zone.tune },
                { "gainDb", zone.gainDb },
                { "pan", zone.pan },
                { "mode", sampleModeName (zone.mode) },
                { "loopStart", zone.loopStart },
                { "loopEnd", zone.loopEnd },
                { "crossfade", zone.crossfade },
                { "chokeGroup", zone.chokeGroup },
                { "layer", zone.layer },
                { "output", zone.output },
                { "fadeInLowVelocity", zone.fadeInLowVelocity },
                { "fadeInHighVelocity", zone.fadeInHighVelocity },
                { "fadeOutLowVelocity", zone.fadeOutLowVelocity },
                { "fadeOutHighVelocity", zone.fadeOutHighVelocity },
                { "attackMs", zone.attackMs },
                { "decayMs", zone.decayMs },
                { "sustain", zone.sustain },
                { "releaseMs", zone.releaseMs },
                { "attackHandleTime", zone.attackHandleTime },
                { "attackHandleLevel", zone.attackHandleLevel },
                { "decayHandleTime", zone.decayHandleTime },
                { "decayHandleLevel", zone.decayHandleLevel },
                { "releaseHandleTime", zone.releaseHandleTime },
                { "releaseHandleLevel", zone.releaseHandleLevel },
            }));

        result.getDynamicObject()->setProperty ("zones", zones);
    }

    if (node.plugin.has_value())
    {
        auto* object = result.getDynamicObject();
        object->setProperty ("plugin", makeObject ({
                                           { "identifier", node.plugin->identifier },
                                           { "name", node.plugin->name },
                                           { "manufacturer", node.plugin->manufacturer },
                                           { "format", node.plugin->format },
                                       }));
        object->setProperty ("midi", makeObject ({ { "mpe", node.midi.mpe }, { "bendRange", node.midi.bendRange } }));
    }

    return result;
}

[[nodiscard]] juce::var toVar (const TrackGraph& graph)
{
    const auto nodes = newObject();
    const auto connections = newObject();

    for (const auto& node : graph.nodes)
        nodes->setProperty (key (node.id), toVar (node));

    for (const auto& c : graph.connections)
    {
        const auto connectionKey =
            key (c.fromNode) + ":" + juce::String (c.fromPort) + ">" + key (c.toNode) + ":" + juce::String (c.toPort);

        const auto connection = makeObject ({
            { "fromNode", c.fromNode },
            { "fromPort", c.fromPort },
            { "toNode", c.toNode },
            { "toPort", c.toPort },
        });

        // Only a cable with bends has them (docs/design/graph-grid-and-cables.md, G9).
        if (! c.bends.empty())
        {
            juce::Array<juce::var> bends;

            for (const auto& p : c.bends)
                bends.add (makeObject ({ { "x", p.x }, { "y", p.y } }));

            connection.getDynamicObject()->setProperty ("bends", bends);
        }

        connections->setProperty (connectionKey, connection);
    }

    return makeObject ({ { "nodes", juce::var (nodes.get()) }, { "connections", juce::var (connections.get()) } });
}
[[nodiscard]] juce::var midiMappingsVar (const std::vector<MidiMapping>& mappings)
{
    juce::Array<juce::var> result;

    for (const auto& m : mappings)
        result.add (makeObject ({ { "device", m.device },
                                  { "channel", m.channel },
                                  { "controller", m.controller },
                                  { "trackId", m.trackId },
                                  { "nodeId", m.nodeId },
                                  { "param", m.param } }));

    return result;
}

[[nodiscard]] juce::var addressVar (const ParamAddress& a)
{
    return makeObject ({ { "trackId", a.trackId }, { "nodeId", a.nodeId }, { "param", a.param } });
}

[[nodiscard]] const char* linkKindName (LinkMappingKind kind) noexcept
{
    switch (kind)
    {
        case LinkMappingKind::identity:
            return "identity";
        case LinkMappingKind::inverted:
            return "inverted";
        case LinkMappingKind::offset:
            return "offset";
        case LinkMappingKind::scaled:
            return "scaled";
        case LinkMappingKind::range:
            return "range";
    }

    return "identity";
}

// Parameter links (docs/design/parameter-linking.md, L1). The mapping leaves out what its kind doesn't use.
[[nodiscard]] juce::var linksVar (const std::vector<ParamLink>& links)
{
    juce::Array<juce::var> result;

    for (const auto& link : links)
    {
        const auto& m = link.mapping;
        const auto mapping = makeObject ({ { "kind", linkKindName (m.kind) } });
        auto* object = mapping.getDynamicObject();

        if (m.kind == LinkMappingKind::offset || m.kind == LinkMappingKind::scaled)
            object->setProperty ("amount", m.amount);

        if (m.centre.has_value() && (m.kind == LinkMappingKind::inverted || m.kind == LinkMappingKind::scaled))
            object->setProperty ("centre", *m.centre);

        if (m.kind == LinkMappingKind::range)
        {
            object->setProperty ("inMin", m.inMin);
            object->setProperty ("inMax", m.inMax);
            object->setProperty ("outMin", m.outMin);
            object->setProperty ("outMax", m.outMax);

            if (! m.curve.empty())
                object->setProperty ("curve", toVar (m.curve));
        }

        result.add (makeObject ({ { "id", link.id },
                                  { "source", addressVar (link.source) },
                                  { "target", addressVar (link.target) },
                                  { "mapping", mapping },
                                  { "active", link.active } }));
    }

    return result;
}

[[nodiscard]] const char* kindName (TrackKind kind) noexcept
{
    switch (kind)
    {
        case TrackKind::audio:
            return "audio";
        case TrackKind::master:
            return "master";
        case TrackKind::group:
            return "group";
        case TrackKind::returnTrack:
            return "return";
        case TrackKind::output:
            return "output";
        case TrackKind::instrument:
            break;
    }

    return "instrument";
}
} // namespace

int NoteClip::indexOfNote (ObjectId noteId) const noexcept
{
    return indexWhere (notes, [noteId] (const NotePtr& n) { return n->id == noteId; });
}

int TrackGraph::indexOfNode (ObjectId nodeId) const noexcept
{
    return indexWhere (nodes, [nodeId] (const GraphNode& n) { return n.id == nodeId; });
}

int Track::indexOfClip (ObjectId clipId) const noexcept
{
    return indexWhere (clips, [clipId] (const ClipPtr& c) { return c->id == clipId; });
}

NoteClip clipPart (const NoteClip& clip, Ticks from, Ticks to)
{
    jassert (from >= clip.start && to <= clip.start + clip.length && from < to);
    auto part = clip;
    const auto delta = from - clip.start;
    part.start = from;
    part.length = to - from;
    part.offset = clip.offset + delta;

    // A looped clip's content position wraps back into the loop (as trimStart() in Commands.cpp).
    if (clip.loopLength > 0)
    {
        const auto r = (part.offset - clip.loopStart) % clip.loopLength;
        part.offset = clip.loopStart + (r < 0 ? r + clip.loopLength : r);
    }

    // Fades belong to the clip's own edges: a cut edge has none.
    if (from > clip.start)
        part.fadeIn = 0;

    if (to < clip.start + clip.length)
        part.fadeOut = 0;

    return part;
}

std::vector<ClipPtr> playedClips (const std::vector<ClipPtr>& clips)
{
    // The usual case: nothing overlaps (clips are sorted by start), and every clip plays whole.
    const auto overlapping = [&]
    {
        Ticks end = std::numeric_limits<Ticks>::min();

        for (const auto& clip : clips)
        {
            if (clip->start < end)
                return true;

            end = std::max (end, clip->start + clip->length);
        }

        return false;
    }();

    if (! overlapping)
        return clips;

    // From the top of the stack down, each clip plays where nothing above it has played yet.
    std::vector<ClipPtr> byOrder (clips);
    std::ranges::stable_sort (byOrder, [] (const ClipPtr& a, const ClipPtr& b)
                              { return std::tie (a->order, a->id) > std::tie (b->order, b->id); });

    std::map<Ticks, Ticks> taken; // start -> end, disjoint
    std::vector<ClipPtr> result;

    for (const auto& clip : byOrder)
    {
        const auto start = clip->start;
        const auto end = clip->start + clip->length;
        std::vector<std::pair<Ticks, Ticks>> free;
        auto cursor = start;

        for (auto it = taken.upper_bound (start); it != taken.begin();)
        {
            --it;

            cursor = std::max (cursor, it->second);
            break;
        }

        for (auto it = taken.lower_bound (start); it != taken.end() && it->first < end && cursor < end; ++it)
        {
            if (it->first > cursor)
                free.emplace_back (cursor, it->first);

            cursor = std::max (cursor, it->second);
        }

        if (cursor < end)
            free.emplace_back (cursor, end);

        for (const auto& [from, to] : free)
            result.push_back (
                from == start && to == end ? clip : std::make_shared<const NoteClip> (clipPart (*clip, from, to)));

        // Merge the clip's range into what's taken.
        auto mergedStart = start;
        auto mergedEnd = end;

        for (auto it = taken.upper_bound (end); it != taken.begin();)
        {
            --it;

            if (it->second < mergedStart)
                break;

            mergedStart = std::min (mergedStart, it->first);
            mergedEnd = std::max (mergedEnd, it->second);
            it = taken.erase (it);
        }

        taken[mergedStart] = mergedEnd;
    }

    std::ranges::sort (result, [] (const ClipPtr& a, const ClipPtr& b)
                       { return std::tie (a->start, a->id) < std::tie (b->start, b->id); });
    return result;
}

const GraphNode* Track::findNode (ObjectId nodeId) const noexcept
{
    const GraphNode* found = nullptr;
    forEachNode (graph,
                 [&] (const GraphNode& node, bool)
                 {
                     if (node.id == nodeId)
                         found = &node;
                 });
    return found;
}

const GraphNode* Track::findTrackNode (ObjectId nodeId) const noexcept
{
    const GraphNode* found = nullptr;
    forEachNode (graph,
                 [&] (const GraphNode& node, bool inVoice)
                 {
                     if (node.id == nodeId && ! inVoice)
                         found = &node;
                 });
    return found;
}

ObjectId Track::graphOwnerOf (ObjectId nodeId) const noexcept
{
    ObjectId result = 0;
    forEachNode (graph,
                 [&] (const GraphNode& owner, bool)
                 {
                     if ((owner.voiceGraph != nullptr && owner.voiceGraph->indexOfNode (nodeId) >= 0)
                         || (owner.component != nullptr && owner.component->graph.indexOfNode (nodeId) >= 0))
                         result = owner.id;
                 });
    return result;
}

const Track* Project::findTrack (ObjectId id) const noexcept
{
    const auto index = indexOfTrack (id);
    return index >= 0 ? tracks[static_cast<size_t> (index)].get() : nullptr;
}

const Track* Project::findTrackOrMaster (ObjectId id) const noexcept
{
    if (master != nullptr && master->id == id)
        return master.get();

    return findTrack (id);
}

KeyTimeline Project::keyTimeline() const
{
    KeyTimeline timeline;

    for (const auto& change : keys)
        timeline.changes.emplace_back (static_cast<double> (tempoMap.tickOfBar (change.bar)), change.key);

    return timeline;
}

const AudioFile* Project::findAudioFile (ObjectId id) const noexcept
{
    const auto index = indexWhere (audioFiles, [id] (const AudioFile& f) { return f.id == id; });
    return index >= 0 ? &audioFiles[static_cast<size_t> (index)] : nullptr;
}

int Project::indexOfTrack (ObjectId id) const noexcept
{
    return indexWhere (tracks, [id] (const TrackPtr& t) { return t->id == id; });
}

juce::var componentToVar (const GraphNode& node)
{
    return toVar (node)["component"];
}

juce::var toVar (const Note& note)
{
    const auto expression = newObject();
    const auto& lanes = laneInfos();

    for (size_t i = 0; i < numLanes; ++i)
        expression->setProperty (lanes[i].id, toVar (note.expression[i]));

    auto result = makeObject ({
        { "id", note.id },
        { "start", note.start },
        { "length", note.length },
        { "pitch", note.pitch },
        { "velocity", note.velocity },
        { "releaseVelocity", note.releaseVelocity },
        { "vibrato", makeObject ({
                         { "depth", note.vibrato.depth },
                         { "rateHz", note.vibrato.rateHz },
                         { "syncBeats", note.vibrato.syncBeats },
                         { "shape", static_cast<int> (note.vibrato.shape) },
                         { "delay", note.vibrato.delay },
                         { "fadeIn", note.vibrato.fadeIn },
                     }) },
        { "expression", juce::var (expression.get()) },
    });

    if (note.link.has_value())
        result.getDynamicObject()->setProperty (
            "link", makeObject ({ { "to", note.link->to }, { "glideMs", note.link->glideMs } }));

    return result;
}

namespace
{
// The serialisers below leave out their child collection when asked (it is then void in every snapshot), so the
// patch builder can diff a level's own fields and its children separately.

[[nodiscard]] juce::var clipVar (const NoteClip& clip, bool withNotes)
{
    juce::var notes;

    if (withNotes)
    {
        const auto object = newObject();

        for (const auto& note : clip.notes)
            object->setProperty (key (note->id), toVar (*note));

        notes = juce::var (object.get());
    }

    return makeObject ({
        { "id", clip.id },
        { "start", clip.start },
        { "length", clip.length },
        { "offset", clip.offset },
        { "loopStart", clip.loopStart },
        { "loopLength", clip.loopLength },
        { "name", clip.name },
        { "colour", clip.colour },
        { "linkId", clip.linkId },
        { "sequencerPattern", clip.sequencerPattern },
        { "sequencerStartStep", clip.sequencerStartStep },
        { "lane", clip.lane },
        { "order", static_cast<juce::int64> (clip.order) },
        { "fileId", clip.fileId },
        { "gainDb", clip.gainDb },
        { "fadeIn", clip.fadeIn },
        { "fadeOut", clip.fadeOut },
        { "fadeInShape", static_cast<int> (clip.fadeInShape) },
        { "fadeOutShape", static_cast<int> (clip.fadeOutShape) },
        { "transpose", clip.transpose },
        { "controllers", controllersVar (clip.controllers) },
        { "notes", notes },
    });
}

[[nodiscard]] juce::var freezeVar (const TrackFreeze& freeze)
{
    if (! freeze.active())
        return {};

    auto value = makeObject ({ { "fileId", freeze.fileId }, { "start", freeze.start }, { "key", freeze.key } });

    if (! freeze.outputs.empty())
    {
        juce::Array<juce::var> outputs;

        for (const auto& output : freeze.outputs)
            outputs.add (makeObject ({ { "node", output.node }, { "fileId", output.fileId } }));

        value.getDynamicObject()->setProperty ("outputs", outputs);
    }

    return value;
}

[[nodiscard]] juce::var trackVar (const Track& track, bool withClips)
{
    juce::var clips;

    if (withClips)
    {
        const auto object = newObject();

        for (const auto& clip : track.clips)
            object->setProperty (key (clip->id), clipVar (*clip, true));

        clips = juce::var (object.get());
    }

    return makeObject ({
        { "id", track.id },
        { "name", track.name },
        { "colour", track.colour },
        { "gainDb", track.gainDb },
        { "pan", track.pan },
        { "offsetMs", track.offsetMs },
        { "drumMode", track.drumMode },
        { "height", track.height },
        { "kind", kindName (track.kind) },
        { "parent", track.parent },
        { "source", track.source },
        { "mainOutput", track.mainOutput },
        { "folded", track.folded },
        { "mute", track.mute },
        { "solo", track.solo },
        { "inputChannel", track.inputChannel },
        { "inputStereo", track.inputStereo },
        { "monitor", static_cast<int> (track.monitor) },
        { "automation", automationVar (track.automation) },
        { "automationOrder", automationOrder (track.automation) },
        { "automationMode", static_cast<int> (track.automationMode) },
        { "automationExpanded", track.automationExpanded },
        { "lanes", track.lanes },
        { "lanesExpanded", track.lanesExpanded },
        { "clips", clips },
        { "graph", toVar (track.graph) },
        { "sends", sendsVar (track.sends) },
        { "freeze", freezeVar (track.freeze) },
    });
}

[[nodiscard]] juce::var tempoPointsVar (const std::vector<TempoPoint>& points)
{
    juce::Array<juce::var> result;

    for (const auto& point : points)
        result.add (makeObject ({ { "tick", point.tick }, { "bpm", point.bpm }, { "ramp", point.ramp } }));

    return result;
}

// A pool file's timing (docs/design/time-stretching.md, TS2).
[[nodiscard]] juce::var timingVar (const FileTiming& timing)
{
    juce::Array<juce::var> markers;

    for (const auto& marker : timing.markers)
        markers.add (makeObject ({ { "tick", marker.tick }, { "frame", marker.frame } }));

    return makeObject ({ { "mode", timing.musical() ? "musical" : "oneShot" },
                         { "musicalStyle", timing.style == MusicalStyle::repitch ? "repitch" : "stretch" },
                         { "sourceBpm", timing.sourceBpm },
                         { "markers", markers } });
}

// A file's pitch edits (docs/design/pitch-correction.md, PC5).
[[nodiscard]] juce::var pitchVar (const FilePitch& pitch)
{
    juce::Array<juce::var> notes;

    for (const auto& note : pitch.notes)
    {
        juce::Array<juce::var> pivots;

        for (const auto pivot : note.pivots)
            pivots.add (pivot);

        notes.add (makeObject ({ { "id", note.id },
                                 { "leadStart", note.leadStart },
                                 { "start", note.start },
                                 { "end", note.end },
                                 { "detectedCentre", note.detectedCentre },
                                 { "targetCentre", note.targetCentre },
                                 { "detectedShape", toVar (note.detectedShape) },
                                 { "shape", toVar (note.shape) },
                                 { "pivots", pivots },
                                 { "vibrato", note.vibrato },
                                 { "transitionMs", note.transitionMs } }));
    }

    return makeObject ({ { "analyser", pitch.analyser }, { "notes", notes } });
}

[[nodiscard]] juce::var signaturesVar (const std::vector<TimeSignature>& signatures)
{
    juce::Array<juce::var> result;

    for (const auto& signature : signatures)
        result.add (makeObject ({ { "bar", signature.bar },
                                  { "numerator", signature.numerator },
                                  { "denominator", signature.denominator } }));

    return result;
}

// The project's keys (docs/design/scale-highlighting.md, SH9).
[[nodiscard]] juce::var keysVar (const std::vector<KeyChange>& keys)
{
    juce::Array<juce::var> result;

    for (const auto& change : keys)
        result.add (makeObject ({ { "bar", change.bar },
                                  { "enabled", change.key.enabled },
                                  { "root", change.key.root },
                                  { "scale", change.key.scale } }));

    return result;
}

[[nodiscard]] juce::var projectVar (const Project& project, bool withTracks)
{
    const auto markers = newObject();
    const auto audioFiles = newObject();

    for (const auto& file : project.audioFiles)
        audioFiles->setProperty (key (file.id), makeObject ({
                                                    { "id", file.id },
                                                    { "path", file.path },
                                                    { "name", file.name },
                                                    { "sampleRate", file.sampleRate },
                                                    { "channels", file.channels },
                                                    { "frames", file.frames },
                                                    { "timing", timingVar (file.timing) },
                                                    { "suggestedBpm", file.suggestedBpm },
                                                    { "reverseOf", file.reverseOf },
                                                    { "pitch", pitchVar (file.pitch) },
                                                }));

    for (const auto& marker : project.markers)
        markers->setProperty (key (marker.id), makeObject ({
                                                   { "id", marker.id },
                                                   { "position", marker.position },
                                                   { "name", marker.name },
                                               }));

    juce::var tracks;
    juce::var master;
    juce::Array<juce::var> order;

    for (const auto& track : project.tracks)
        order.add (track->id);

    if (withTracks)
    {
        const auto object = newObject();

        for (const auto& track : project.tracks)
        {
            auto value = trackVar (*track, true);

            // A frozen track whose clips, graph or tempo changed since plays a stale render (AU15).
            if (track->freeze.active())
                value["freeze"].getDynamicObject()->setProperty ("stale",
                                                                 freezeKey (project, *track) != track->freeze.key);

            object->setProperty (key (track->id), value);
        }

        tracks = juce::var (object.get());
        master = project.master != nullptr ? trackVar (*project.master, true) : juce::var();
    }

    return makeObject ({
        { "tempoMap", tempoPointsVar (project.tempoMap.points()) },
        { "signatures", signaturesVar (project.tempoMap.signatures()) },
        { "metronome", project.metronome },
        { "autoCrossfadeMs", project.autoCrossfadeMs },
        { "keys", keysVar (project.keys) },
        { "markers", juce::var (markers.get()) },
        { "midiMappings", midiMappingsVar (project.midiMappings) },
        { "links", linksVar (project.links) },
        { "audioFiles", juce::var (audioFiles.get()) },
        { "loop",
          makeObject (
              { { "enabled", project.loop.enabled }, { "start", project.loop.start }, { "end", project.loop.end } }) },
        { "punch", makeObject ({ { "enabled", project.punch.enabled },
                                 { "start", project.punch.start },
                                 { "end", project.punch.end } }) },
        { "trackOrder", order },
        { "tracks", tracks },
        { "master", master },
    });
}

[[nodiscard]] juce::var makeOp (const char* op, const juce::String& path)
{
    return makeObject ({ { "op", op }, { "path", path } });
}

[[nodiscard]] juce::var makeOp (const char* op, const juce::String& path, const juce::var& value)
{
    return makeObject ({ { "op", op }, { "path", path }, { "value", value } });
}

// Diffs two id-keyed collections of shared immutable items. An item both snapshots share is unchanged, so only
// the items that differ are serialised: an edit costs what it changed, not the size of the project.
template <typename Ptr, typename Serialise, typename DiffItem>
void diffById (juce::Array<juce::var>& ops, const juce::String& path, const std::vector<Ptr>& before,
               const std::vector<Ptr>& after, const Serialise& serialise, const DiffItem& diffItem)
{
    std::map<ObjectId, const Ptr*> previous;

    for (const auto& item : before)
        previous.emplace (item->id, &item);

    std::set<ObjectId> kept;

    for (const auto& item : after)
        kept.insert (item->id);

    for (const auto& item : before)
        if (! kept.contains (item->id))
            ops.add (makeOp ("remove", path + "/" + key (item->id)));

    for (const auto& item : after)
    {
        const auto itemPath = path + "/" + key (item->id);
        const auto it = previous.find (item->id);

        if (it == previous.end())
            ops.add (makeOp ("add", itemPath, serialise (*item)));
        else if (*it->second != item)
            diffItem (itemPath, **it->second, *item);
    }
}

void diffClip (juce::Array<juce::var>& ops, const juce::String& path, const NoteClip& before, const NoteClip& after)
{
    appendDiff (ops, path, clipVar (before, false), clipVar (after, false));

    if (before.notes != after.notes)
        diffById (
            ops, path + "/notes", before.notes, after.notes, [] (const Note& note) { return toVar (note); },
            [&ops] (const juce::String& notePath, const Note& a, const Note& b)
            { appendDiff (ops, notePath, toVar (a), toVar (b)); });
}

void diffTrack (juce::Array<juce::var>& ops, const juce::String& path, const Track& before, const Track& after)
{
    appendDiff (ops, path, trackVar (before, false), trackVar (after, false));

    if (before.clips != after.clips)
        diffById (
            ops, path + "/clips", before.clips, after.clips, [] (const NoteClip& clip) { return clipVar (clip, true); },
            [&ops] (const juce::String& clipPath, const NoteClip& a, const NoteClip& b)
            { diffClip (ops, clipPath, a, b); });
}
} // namespace

juce::var toVar (const NoteClip& clip)
{
    return clipVar (clip, true);
}

namespace
{
// The parent of a track once checked: it must exist, be a group, and a return track stays at the top (R10).
[[nodiscard]] ObjectId checkedParent (const Track& track, const std::vector<TrackPtr>& tracks,
                                      const std::map<ObjectId, size_t>& indices)
{
    const auto found = indices.find (track.parent);
    const auto isGroup = found != indices.end() && tracks[found->second]->kind == TrackKind::group;

    return track.parent != 0 && isGroup && track.kind != TrackKind::returnTrack ? track.parent : ObjectId { 0 };
}

// No track may contain itself: a track whose chain of parents loops goes to the top level.
void breakParentCycles (const std::vector<TrackPtr>& tracks, std::vector<ObjectId>& parents,
                        const std::map<ObjectId, size_t>& indices)
{
    for (size_t i = 0; i < parents.size(); ++i)
    {
        std::set<ObjectId> seen { tracks[i]->id };

        for (auto parent = parents[i]; parent != 0; parent = parents[indices.at (parent)])
        {
            if (! seen.insert (parent).second)
            {
                parents[i] = 0;
                break;
            }
        }
    }
}

// A send must target another, existing track, and either key one of its nodes or feed a return (K2).
[[nodiscard]] bool isValidSend (const Send& send, ObjectId trackId, const std::vector<TrackPtr>& tracks,
                                const std::map<ObjectId, size_t>& indices)
{
    const auto found = indices.find (send.target);

    if (send.target == trackId || found == indices.end())
        return false;

    const auto& target = *tracks[found->second];
    return send.targetNode != 0 ? target.findTrackNode (send.targetNode) != nullptr
                                : target.kind == TrackKind::returnTrack;
}

// The track with the sends that no longer make sense dropped.
[[nodiscard]] TrackPtr withValidSends (TrackPtr track, const std::vector<TrackPtr>& tracks,
                                       const std::map<ObjectId, size_t>& indices)
{
    const auto invalid = [&] (const Send& send) { return ! isValidSend (send, track->id, tracks, indices); };

    if (std::ranges::none_of (track->sends, invalid))
        return track;

    auto fixed = *track;
    std::erase_if (fixed.sends, invalid);
    return std::make_shared<const Track> (std::move (fixed));
}

// Puts a track, re-parented if it has to be, and then its members into `ordered`.
void appendSubtree (size_t index, const std::vector<TrackPtr>& tracks, const std::vector<ObjectId>& parents,
                    const std::map<ObjectId, std::vector<size_t>>& members, const std::map<ObjectId, size_t>& indices,
                    std::vector<TrackPtr>& ordered)
{
    auto track = tracks[index];

    if (track->parent != parents[index])
    {
        auto fixed = *track;
        fixed.parent = parents[index];
        track = std::make_shared<const Track> (std::move (fixed));
    }

    ordered.push_back (withValidSends (std::move (track), tracks, indices));

    if (const auto it = members.find (tracks[index]->id); it != members.end())
        for (const auto member : it->second)
            appendSubtree (member, tracks, parents, members, indices, ordered);
}
} // namespace

void normaliseGroups (Project& project)
{
    auto& tracks = project.tracks;
    std::map<ObjectId, size_t> indices;

    for (size_t i = 0; i < tracks.size(); ++i)
        indices[tracks[i]->id] = i;

    // A parent must be an existing group.
    std::vector<ObjectId> parents (tracks.size());

    for (size_t i = 0; i < tracks.size(); ++i)
        parents[i] = checkedParent (*tracks[i], tracks, indices);

    breakParentCycles (tracks, parents, indices);

    // Lay the tree out again: each track followed by its members, in their current order. Output tracks are placed
    // like any other track (docs/design/multi-output-instruments.md, MO4).
    std::map<ObjectId, std::vector<size_t>> members;
    std::vector<size_t> roots;

    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (tracks[i]->kind == TrackKind::returnTrack)
            continue;

        if (parents[i] == 0)
            roots.push_back (i);
        else
            members[parents[i]].push_back (i);
    }

    // Return tracks come last, in their order (R10).
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i]->kind == TrackKind::returnTrack)
            roots.push_back (i);

    std::vector<TrackPtr> ordered;
    ordered.reserve (tracks.size());

    for (const auto root : roots)
        appendSubtree (root, tracks, parents, members, indices, ordered);

    tracks = std::move (ordered);
}

size_t subtreeEnd (const std::vector<TrackPtr>& tracks, size_t index)
{
    // Everything after the track that lies inside it: its members, and theirs.
    std::set<ObjectId> inside { tracks[index]->id };
    auto end = index + 1;

    while (end < tracks.size() && inside.contains (tracks[end]->parent))
        inside.insert (tracks[end++]->id);

    return end;
}

juce::var toVar (const Track& track)
{
    return trackVar (track, true);
}

juce::var toVar (const Project& project)
{
    return projectVar (project, true);
}

juce::String frozenContent (const Track& track)
{
    auto clips = newObject();

    for (const auto& clip : track.clips)
        clips->setProperty (key (clip->id), clipVar (*clip, true));

    return juce::JSON::toString (
        makeObject ({ { "clips", juce::var (clips.get()) }, { "graph", toVar (track.graph) } }), true);
}

juce::String freezeKey (const Project& project, const Track& track)
{
    // The fader, pan and send lanes stay live; every other lane drives a node the render played.
    juce::Array<juce::var> lanes;

    for (const auto& lane : track.automation)
        if (lane.target.kind != AutomationTargetKind::gain && lane.target.kind != AutomationTargetKind::pan
            && lane.target.kind != AutomationTargetKind::send)
            lanes.add (automationVar ({ lane }));

    const auto text = frozenContent (track) + juce::JSON::toString (juce::var (lanes), true)
                      + juce::String (track.offsetMs) + kindName (track.kind)
                      + juce::JSON::toString (tempoPointsVar (project.tempoMap.points()), true)
                      + juce::JSON::toString (signaturesVar (project.tempoMap.signatures()), true);
    return juce::String::toHexString (text.hashCode64());
}

bool hasOutputTracks (const Project& project, ObjectId trackId)
{
    return std::ranges::any_of (project.tracks, [trackId] (const TrackPtr& t)
                                { return t->kind == TrackKind::output && t->source == trackId; });
}

const Track* mainOutputOf (const Project& project, ObjectId trackId)
{
    const auto main =
        std::ranges::find_if (project.tracks, [trackId] (const TrackPtr& t)
                              { return t->kind == TrackKind::output && t->source == trackId && t->mainOutput; });
    return main != project.tracks.end() ? main->get() : nullptr;
}

Track frozenView (const Track& track, const Project& project)
{
    const auto* file = track.freeze.active() ? project.findAudioFile (track.freeze.fileId) : nullptr;

    if (file == nullptr)
        return track;

    auto view = track;
    const auto& map = project.tempoMap;
    const auto start = static_cast<double> (track.freeze.start);

    // Each render as a clip; synthetic ids are negative, so they never meet the project's (ids are unique across it).
    const auto clipOf = [&] (ObjectId id, const AudioFile& render)
    {
        NoteClip clip;
        clip.id = id;
        clip.fileId = render.id;
        clip.start = track.freeze.start;
        const auto seconds = static_cast<double> (render.frames) / render.sampleRate;
        clip.length = std::max (Ticks { 1 },
                                static_cast<Ticks> (std::ceil (map.tickAt (map.secondsAt (start) + seconds) - start)));
        return std::make_shared<const NoteClip> (std::move (clip));
    };

    view.clips = { clipOf (-track.id, *file) };
    view.kind = TrackKind::audio;
    view.inputChannel = -1;
    view.offsetMs = 0.0; // the render has the offset in it

    // An Audio Player of the render into the Track Output, which nothing else feeds now.
    const auto output = std::ranges::find (view.graph.nodes, juce::String (nodetype::trackOutput), &GraphNode::type);

    if (output == view.graph.nodes.end())
        return track;

    const auto feed = [&view] (ObjectId playerId, ObjectId fileId, ObjectId nodeId)
    {
        std::erase_if (view.graph.connections, [nodeId] (const Connection& c) { return c.toNode == nodeId; });
        GraphNode player;
        player.id = playerId;
        player.type = nodetype::audioPlayer;
        player.fileId = fileId;
        view.graph.nodes.push_back (player);
        view.graph.connections.push_back ({ playerId, 0, nodeId, 0 });
    };

    feed (-track.id, file->id, output->id);

    // And one of each Output to track's render into it, so its output track plays on; one without a render is silent.
    std::vector<ObjectId> outputs;

    for (const auto& node : track.graph.nodes)
        if (node.type == nodetype::busOutput)
            outputs.push_back (node.id);

    for (const auto nodeId : outputs)
    {
        const auto frozen = std::ranges::find (track.freeze.outputs, nodeId, &FrozenOutput::node);
        const auto* render = frozen != track.freeze.outputs.end() ? project.findAudioFile (frozen->fileId) : nullptr;

        if (render == nullptr)
        {
            std::erase_if (view.graph.connections, [nodeId] (const Connection& c) { return c.toNode == nodeId; });
            continue;
        }

        view.clips.push_back (clipOf (-nodeId, *render));
        feed (-nodeId, render->id, nodeId);
    }

    return view;
}

juce::Array<juce::var> diff (const Project& before, const Project& after)
{
    juce::Array<juce::var> ops;
    appendDiff (ops, {}, projectVar (before, false), projectVar (after, false));

    const auto trackDiff = [&ops] (const juce::String& path, const Track& a, const Track& b)
    { diffTrack (ops, path, a, b); };

    if (before.tracks != after.tracks)
        diffById (
            ops, "/tracks", before.tracks, after.tracks, [] (const Track& track) { return trackVar (track, true); },
            trackDiff);

    if (before.master != after.master)
    {
        if (before.master != nullptr && after.master != nullptr)
            trackDiff ("/master", *before.master, *after.master);
        else
            ops.add (
                makeOp ("replace", "/master", after.master != nullptr ? trackVar (*after.master, true) : juce::var()));
    }

    return ops;
}

} // namespace dawg
