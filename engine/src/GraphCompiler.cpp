#include "dawg/GraphCompiler.h"

#include "dawg/BuiltinNodes.h"
#include "dawg/PluginNode.h"
#include "dawg/Sampler.h"
#include "dawg/StretchRenderer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace dawg
{

namespace
{
// Buffer slots in the compiled audio memory. Each slot holds two channels of maxBlockSize samples.
constexpr int silenceSlot = 0;
constexpr int scratchSlot = 1;
constexpr int firstFreeSlot = 2;

using PortRef = std::pair<ObjectId, int>; // (node id, port index)

[[nodiscard]] float decibelsToGain (double db) noexcept
{
    return static_cast<float> (std::pow (10.0, db / 20.0));
}

// Exact comparison for change detection, without tripping -Wfloat-equal.
[[nodiscard]] bool differs (double a, double b) noexcept
{
    return std::less<> {}(a, b) || std::less<> {}(b, a);
}

// Hands out buffer slots and recycles them once their last reader has run.
class SlotAllocator
{
public:
    [[nodiscard]] int acquire()
    {
        if (m_free.empty())
            return m_next++;

        const auto slot = *m_free.begin();
        m_free.erase (m_free.begin());
        return slot;
    }

    void release (int slot)
    {
        if (slot >= firstFreeSlot)
            m_free.insert (slot);
    }

    [[nodiscard]] int slotCount() const noexcept { return m_next; }

private:
    std::set<int> m_free;
    int m_next = firstFreeSlot;
};

// A step planned with slot indices; turned into a CompiledStep once the memory exists.
struct PlannedStep
{
    size_t regionSteps = 0;
    LoopPart loopPart = LoopPart::whole;
    Node* node = nullptr;
    std::vector<int> inputSlots;
    std::vector<int> outputSlots;
    std::vector<int> mixSlots;
    int eventInput = -1;
    int eventOutput = -1;
    bool takesEvents = false;
    juce::uint64 eventSource = 0;
    std::vector<CompiledModulation> modulations;
    std::vector<CompiledLink> links; // docs/design/parameter-linking.md, L6
    std::shared_ptr<const CompiledVoice> voice;
    std::shared_ptr<const SampleMap> samples;
    juce::uint32 wiredOutputs = 1;
    int channels = 2;              // CompiledStep::channels
    std::vector<bool> monoInputs;  // by input; missing entries are stereo (docs/design/mono-ports.md, MP6)
    std::vector<bool> monoOutputs; // by output
    std::vector<bool> monoSources; // by mix source
};

// Marks entry `index` of a step's mono flags.
void markMono (std::vector<bool>& flags, size_t index)
{
    if (flags.size() <= index)
        flags.resize (index + 1, false);

    flags[index] = true;
}

[[nodiscard]] bool isMono (const std::vector<bool>& flags, size_t index) noexcept
{
    return index < flags.size() && flags[index];
}

[[nodiscard]] bool hasAdaptivePorts (const NodeDescriptor& descriptor) noexcept
{
    const auto adaptive = [] (const PortInfo& port)
    { return port.type == PortType::audio && port.channels == adaptiveChannels; };
    return std::ranges::any_of (descriptor.inputs, adaptive) || std::ranges::any_of (descriptor.outputs, adaptive);
}

// An Output to track node (docs/design/multi-output-instruments.md, MO3): the slot the mixer takes to the output track,
// and the latency at it.
struct PlannedBusOutput
{
    ObjectId node = 0;
    ObjectId target = 0;
    int slot = silenceSlot;
    int latency = 0;
};

// A node whose key input is free for key sends (docs/design/sidechain.md, K4): its step and the latency at its input.
struct KeyInput
{
    size_t step = 0;
    int latency = 0;
    Node* node = nullptr;
};

struct PlannedTrack
{
    ObjectId id = 0;
    std::map<ObjectId, KeyInput> keyInputs;
    std::vector<PlannedBusOutput> busOutputs;
    std::vector<PlannedStep> steps;
    int outputSlot = silenceSlot;
    int latency = 0;
    int offset = 0;
    int slotCount = firstFreeSlot;
    std::shared_ptr<const TrackSequence> sequence;
    std::shared_ptr<const AudioSequence> audio;
    std::shared_ptr<TrackState> state;
    std::vector<CompiledLane> lanes;
    std::map<ObjectId, size_t> nodeSteps;      // graph node -> its step, for modulation restores
    int inputEvents = -1;                      // voice graphs: the event buffer the Voice Input feeds
    std::map<ObjectId, PortWidths> portWidths; // nodes with a mono port or a folding input, for the graph view
    std::vector<CompiledLink> links;        // into nodes without a step of the track's own (parameter-linking.md, L6)
    std::map<ObjectId, int> inputLatencies; // graph node -> the latency of what reaches it, for links (L8)
    std::vector<Connection> connections;    // the flattened graph's cables, for links (L8)
};

// Creates or reuses a node instance: (node id, type, delay, graph node for params) -> instance.
using InstanceFactory = std::function<std::shared_ptr<Node> (ObjectId, const juce::String&, int, const GraphNode*)>;

// Finds or creates a modulation slot's shared state, kept across compilations so its base survives them:
// (node id, port, target parameter, base for a new state) -> state.
using ModulationStateFactory =
    std::function<std::shared_ptr<ModulationState> (ObjectId, int, const juce::String&, float)>;

// Compiles a Voice Instrument's voice graph into one program per voice slot (docs/design/phase12-voice-scope.md, V8).
using VoiceCompiler = std::function<std::shared_ptr<const CompiledVoice> (const GraphNode&)>;

// Compiles a Sampler's zones, once for all of a voice graph's slots.
using SampleCompiler = std::function<std::shared_ptr<const SampleMap> (const GraphNode&)>;

// Opens the pool file a node reads (nodetype::usesAudioFile), or returns null for none or a missing one.
using FileOpener = std::function<std::shared_ptr<const AudioSource> (ObjectId)>;

// Makes an LFO's or ADSR's instances for a plugin to run per note (docs/design/clap-hosting.md, C10).
using NoteSourceFactory = std::function<std::vector<Node*> (const GraphNode&)>;

[[nodiscard]] bool sameCurve (const Curve& a, const Curve& b)
{
    return std::ranges::equal (a, b,
                               [] (const CurvePoint& x, const CurvePoint& y)
                               {
                                   return x.time == y.time && ! differs (x.value, y.value) && x.shape == y.shape
                                          && ! differs (x.tension, y.tension) && ! differs (x.handleTime, y.handleTime)
                                          && ! differs (x.handleValue, y.handleValue);
                               });
}

[[nodiscard]] bool sameAutomation (const Track& a, const Track& b)
{
    return std::ranges::equal (a.automation, b.automation,
                               [] (const AutomationLane& x, const AutomationLane& y)
                               {
                                   return x.id == y.id && x.active == y.active && x.target.sameAs (y.target)
                                          && sameCurve (x.points, y.points);
                               });
}

[[nodiscard]] bool sameStructure (const TrackGraph& a, const TrackGraph& b);

[[nodiscard]] bool sameVoiceGraph (const std::shared_ptr<const TrackGraph>& a,
                                   const std::shared_ptr<const TrackGraph>& b)
{
    return a == b || (a != nullptr && b != nullptr && sameStructure (*a, *b));
}

[[nodiscard]] bool sameComponent (const GraphNode& x, const GraphNode& y)
{
    // Bypassing a component changes the flat graph's cables.
    if (x.type == nodetype::component && x.bypassed != y.bypassed)
        return false;

    const auto& a = x.component;
    const auto& b = y.component;
    return a == b
           || (a != nullptr && b != nullptr && a->inputs == b->inputs && a->outputs == b->outputs
               && sameStructure (a->graph, b->graph));
}

// The same modulation slots, aimed at the same targets; their depths may differ.
[[nodiscard]] bool sameModulationSlots (const std::map<int, ModulationSlot>& a, const std::map<int, ModulationSlot>& b)
{
    return std::ranges::equal (a, b,
                               [] (const auto& x, const auto& y)
                               {
                                   return x.first == y.first && x.second.param == y.second.param
                                          && x.second.active == y.second.active && x.second.macro == y.second.macro;
                               });
}

[[nodiscard]] bool sameStructure (const TrackGraph& a, const TrackGraph& b)
{
    if (a.connections != b.connections || a.nodes.size() != b.nodes.size())
        return false;

    // Which modulation slots there are, and their targets, change the compiled steps, so they need a recompile; a depth
    // is stored in the slot's state (publishModulationDepths()), and a shape change only needs configure(), both of
    // which applyNonStructural() does.
    return std::ranges::equal (a.nodes, b.nodes,
                               [] (const GraphNode& x, const GraphNode& y)
                               {
                                   // A Sampler's zones aren't structure: an edit hands the node its new zones
                                   // (publishSampleMaps()), so dragging an envelope never recompiles.
                                   return x.id == y.id && x.type == y.type && x.code == y.code && x.target == y.target
                                          && x.allOutputs == y.allOutputs && x.pluginChannels == y.pluginChannels
                                          && sameModulationSlots (x.modulations, y.modulations)
                                          && sameVoiceGraph (x.voiceGraph, y.voiceGraph) && sameComponent (x, y);
                               });
}

// A track's input as its width: none (0), mono or stereo. A change sets the Audio Player's width (mono-ports.md, MP15),
// so it recompiles; which channel it is doesn't.
[[nodiscard]] int inputWidth (const Track& track) noexcept
{
    return track.inputChannel < 0 ? 0 : (track.inputStereo ? 2 : 1);
}

[[nodiscard]] bool sameStructure (const Track& a, const Track& b)
{
    const auto sameRouting = [] (const Send& x, const Send& y)
    { return x.id == y.id && x.target == y.target && x.targetNode == y.targetNode; };

    return a.id == b.id && a.parent == b.parent && a.kind == b.kind && a.source == b.source
           && std::ranges::equal (a.sends, b.sends, sameRouting) && a.clips == b.clips && sameAutomation (a, b)
           && ! differs (a.offsetMs, b.offsetMs) && inputWidth (a) == inputWidth (b)
           && sameStructure (a.graph, b.graph);
}

[[nodiscard]] bool sameStructure (const Project& a, const Project& b)
{
    if (! (a.tempoMap == b.tempoMap) || a.metronome != b.metronome || a.loop.enabled != b.loop.enabled
        || a.loop.start != b.loop.start || a.loop.end != b.loop.end || a.tracks.size() != b.tracks.size()
        || a.audioFiles.size() != b.audioFiles.size() || a.links != b.links)
        return false;

    // Relinked audio files open different audio, a file's timing changes how its clips play
    // (docs/design/time-stretching.md, TS2), and its pitch edits what they play (docs/design/pitch-correction.md, 5.2).
    for (size_t i = 0; i < a.audioFiles.size(); ++i)
        if (a.audioFiles[i].id != b.audioFiles[i].id || a.audioFiles[i].path != b.audioFiles[i].path
            || ! (a.audioFiles[i].timing == b.audioFiles[i].timing)
            || ! a.audioFiles[i].pitch.soundsLike (b.audioFiles[i].pitch))
            return false;

    const auto same = [] (const TrackPtr& x, const TrackPtr& y)
    { return x == y || (x != nullptr && y != nullptr && sameStructure (*x, *y)); };

    return same (a.master, b.master) && std::ranges::equal (a.tracks, b.tracks, same);
}

// Whether every parameter that changes a node's latency (ParamInfo::changesLatency) kept its value. One that moved
// needs a compilation, which reads the node's latency again (docs/design/saturator.md, W7). Only called after
// sameStructure(), so both graphs hold the same nodes in the same order.
[[nodiscard]] bool sameLatencyParams (const NodeRegistry& registry, const TrackGraph& a, const TrackGraph& b)
{
    for (size_t i = 0; i < a.nodes.size(); ++i)
    {
        const auto& x = a.nodes[i];
        const auto& y = b.nodes[i];

        if (const auto* descriptor = registry.find (x.type))
        {
            for (const auto& info : descriptor->params)
            {
                const auto value = [&info] (const GraphNode& node)
                {
                    const auto it = node.params.find (info.id);
                    return it != node.params.end() ? it->second : static_cast<double> (info.defaultValue);
                };

                if (info.changesLatency && differs (value (x), value (y)))
                    return false;
            }
        }

        if (x.voiceGraph != nullptr && y.voiceGraph != nullptr && x.voiceGraph != y.voiceGraph
            && ! sameLatencyParams (registry, *x.voiceGraph, *y.voiceGraph))
            return false;

        if (x.component != nullptr && y.component != nullptr && x.component != y.component
            && ! sameLatencyParams (registry, x.component->graph, y.component->graph))
            return false;
    }

    return true;
}

[[nodiscard]] bool sameLatencyParams (const NodeRegistry& registry, const Project& a, const Project& b)
{
    const auto same = [&registry] (const TrackPtr& x, const TrackPtr& y)
    { return x == y || x == nullptr || y == nullptr || sameLatencyParams (registry, x->graph, y->graph); };

    return same (a.master, b.master) && std::ranges::equal (a.tracks, b.tracks, same);
}

void storeState (TrackState& state, const Track& track)
{
    state.gain.store (decibelsToGain (track.gainDb));
    state.pan.store (static_cast<float> (track.pan));
    state.mute.store (track.mute);
    state.solo.store (track.solo);
    state.monitor.store (static_cast<int> (track.monitor));
    state.inputChannel.store (track.inputChannel);
    state.inputStereo.store (track.inputStereo);
    state.automationMode.store (static_cast<int> (track.automationMode));
}

void applyParams (Node& instance, const GraphNode& node)
{
    const auto& infos = instance.descriptor().params;

    for (size_t i = 0; i < infos.size(); ++i)
    {
        const auto it = node.params.find (infos[i].id);
        instance.params().set (i, it != node.params.end() ? static_cast<float> (it->second) : infos[i].defaultValue);
    }

    instance.configure (node);
}

// The nodes that can reach `target`, found by walking connections backwards.
[[nodiscard]] std::set<ObjectId> nodesReaching (const TrackGraph& graph, ObjectId target)
{
    std::set<ObjectId> reached { target };
    std::vector<ObjectId> stack { target };

    while (! stack.empty())
    {
        const auto current = stack.back();
        stack.pop_back();

        for (const auto& c : graph.connections)
            if (c.toNode == current && reached.insert (c.fromNode).second)
                stack.push_back (c.fromNode);
    }

    return reached;
}

// What runs together: one node, or all the nodes of a loop (docs/design/feedback-routing.md, R2).
struct PlanGroup
{
    std::vector<const GraphNode*> nodes;          // in order; a loop's Feedback nodes read here
    std::vector<const GraphNode*> feedbackInputs; // a loop's Feedback nodes, which write after the rest
    bool region = false;
};

// The nodes on a loop. Loops only close through Feedback nodes (R1), so each loop has one to start from. Loops sharing a node
// are one region.
[[nodiscard]] std::vector<std::set<ObjectId>> findRegions (const TrackGraph& graph, const std::set<ObjectId>& include)
{
    const auto walk = [&] (ObjectId start, bool forward)
    {
        std::set<ObjectId> reached;
        std::vector<ObjectId> stack { start };

        while (! stack.empty())
        {
            const auto current = stack.back();
            stack.pop_back();

            for (const auto& c : graph.connections)
            {
                const auto from = forward ? c.fromNode : c.toNode;
                const auto to = forward ? c.toNode : c.fromNode;

                if (from == current && include.contains (to) && reached.insert (to).second)
                    stack.push_back (to);
            }
        }

        return reached;
    };

    std::vector<std::set<ObjectId>> regions;

    for (const auto& node : graph.nodes)
    {
        if (node.type != nodetype::feedback || ! include.contains (node.id))
            continue;

        const auto after = walk (node.id, true);

        if (! after.contains (node.id))
            continue; // nothing leads back to it: a plain delay

        std::set<ObjectId> region;

        for (const auto id : walk (node.id, false))
            if (after.contains (id))
                region.insert (id);

        for (auto it = regions.begin(); it != regions.end();)
        {
            if (std::ranges::any_of (*it, [&] (ObjectId id) { return region.contains (id); }))
            {
                region.insert (it->begin(), it->end());
                it = regions.erase (it);
            }
            else
            {
                ++it;
            }
        }

        regions.push_back (std::move (region));
    }

    return regions;
}

// Kahn's algorithm, ties broken by creation order so compilation is deterministic. `pending` and `follows` say how many inputs
// each thing is waiting for, and which others finishing it frees.
[[nodiscard]] std::vector<const GraphNode*> orderRegion (const TrackGraph& graph, const std::set<ObjectId>& region)
{
    std::map<ObjectId, int> pending;

    for (const auto id : region)
        pending[id] = 0;

    const auto counts = [&] (const Connection& c)
    {
        if (! region.contains (c.fromNode) || ! region.contains (c.toNode))
            return false;

        // Nothing has to run before a Feedback node reads.
        return std::ranges::none_of (graph.nodes, [&] (const GraphNode& n)
                                     { return n.id == c.toNode && n.type == nodetype::feedback; });
    };

    for (const auto& c : graph.connections)
        if (counts (c))
            ++pending[c.toNode];

    std::vector<const GraphNode*> order;

    while (true)
    {
        const auto next = std::ranges::find_if (graph.nodes, [&] (const GraphNode& n)
                                                { return pending.contains (n.id) && pending[n.id] == 0; });

        if (next == graph.nodes.end())
            return order;

        pending[next->id] = -1;
        order.push_back (&*next);

        for (const auto& c : graph.connections)
            if (c.fromNode == next->id && counts (c))
                --pending[c.toNode];
    }
}

// A region's group: its nodes in order, and its Feedback nodes, whose inputs close its loops.
[[nodiscard]] PlanGroup regionGroup (const TrackGraph& graph, const std::set<ObjectId>& region)
{
    PlanGroup group;
    group.region = true;
    group.nodes = orderRegion (graph, region);

    for (const auto* node : group.nodes)
        if (node->type == nodetype::feedback)
            group.feedbackInputs.push_back (node);

    return group;
}

[[nodiscard]] std::vector<PlanGroup> planGroups (const TrackGraph& graph, const std::set<ObjectId>& include)
{
    const auto regions = findRegions (graph, include);
    const auto regionOf = [&] (ObjectId id) -> int
    {
        for (size_t i = 0; i < regions.size(); ++i)
            if (regions[i].contains (id))
                return static_cast<int> (i);

        return -1;
    };

    // A node outside any region is its own group, and a group runs once all its inputs from other groups have.
    const auto groupOf = [&] (ObjectId id)
    {
        const auto region = regionOf (id);
        return region >= 0 ? -1 - region : 0;
    };
    const auto sameGroup = [&] (ObjectId x, ObjectId y)
    {
        const auto group = groupOf (x);
        return x == y || (group != 0 && group == groupOf (y));
    };

    std::map<ObjectId, int> pending;

    for (const auto& node : graph.nodes)
        if (include.contains (node.id))
            pending[node.id] = 0;

    for (const auto& c : graph.connections)
        if (pending.contains (c.fromNode) && pending.contains (c.toNode) && ! sameGroup (c.fromNode, c.toNode))
            ++pending[c.toNode];

    std::vector<PlanGroup> groups;

    while (true)
    {
        const auto next = std::ranges::find_if (graph.nodes,
                                                [&] (const GraphNode& n)
                                                {
                                                    if (! pending.contains (n.id) || pending[n.id] != 0)
                                                        return false;

                                                    const auto region = regionOf (n.id);

                                                    return region < 0
                                                           || std::ranges::all_of (
                                                               regions[static_cast<size_t> (region)], [&] (ObjectId id)
                                                               { return pending.contains (id) && pending[id] == 0; });
                                                });

        if (next == graph.nodes.end())
            return groups; // only short of the node count if there's a loop with no Feedback node, which commands prevent

        const auto region = regionOf (next->id);
        PlanGroup group;

        if (region >= 0)
            group = regionGroup (graph, regions[static_cast<size_t> (region)]);
        else
            group.nodes = { &*next };

        for (const auto* node : group.nodes)
            pending[node->id] = -1;

        for (const auto& c : graph.connections)
            if (pending.contains (c.toNode)
                && std::ranges::any_of (group.nodes, [&] (const GraphNode* n) { return n->id == c.fromNode; })
                && ! sameGroup (c.fromNode, c.toNode))
                --pending[c.toNode];

        groups.push_back (std::move (group));
    }
}

// Plans one track graph: execution order, buffer slots, latency-compensation delays and mix steps. Plans a voice graph
// the same way when it has no voice compiler: its Voice Input is then where the voice's events arrive, and its Voice
// Output is the sink (docs/design/phase12-voice-scope.md, V8).
class TrackPlanner
{
public:
    TrackPlanner (const TrackGraph& graph, ObjectId id, const NodeRegistry& registry, InstanceFactory instance,
                  ModulationStateFactory modulationState, int& eventCount, VoiceCompiler voice,
                  SampleCompiler samples = nullptr, NoteSourceFactory noteSources = nullptr)
        : m_graph (graph),
          m_registry (registry),
          m_instance (std::move (instance)),
          m_modulationState (std::move (modulationState)),
          m_eventCount (eventCount),
          m_voice (std::move (voice)),
          m_samples (std::move (samples)),
          m_noteSources (std::move (noteSources)),
          m_voiceScope (m_voice == nullptr)
    {
        m_plan.id = id;
    }

    // Hands nodes that read a pool file their file (docs/design/convolution-reverb.md, I11).
    TrackPlanner& withFiles (FileOpener files)
    {
        m_files = std::move (files);
        return *this;
    }

    // Asks sources that decide their own width (an Audio Player, from its clips and input; mono-ports.md, MP15). The
    // answer is 1 or 2, or 0 for a node that doesn't decide.
    TrackPlanner& withSourceWidths (std::function<int (const GraphNode&)> widths)
    {
        m_sourceWidths = std::move (widths);
        return *this;
    }

    [[nodiscard]] PlannedTrack run()
    {
        const auto* sink = m_voiceScope ? nodetype::voiceOutput : nodetype::trackOutput;
        const auto output = std::ranges::find_if (m_graph.nodes, [&] (const GraphNode& n) { return n.type == sink; });

        if (output == m_graph.nodes.end())
            return std::move (m_plan);

        m_outputId = output->id;
        auto included = nodesReaching (m_graph, m_outputId);

        // Output to track nodes are sinks too, in track graphs (MO3): what reaches them runs as well.
        if (! m_voiceScope)
            for (const auto& node : m_graph.nodes)
                if (node.type == nodetype::busOutput)
                {
                    m_busOutputIds.insert (node.id);
                    included.merge (nodesReaching (m_graph, node.id));
                }

        m_sinksLeft = 1 + static_cast<int> (m_busOutputIds.size());

        for (const auto& c : m_graph.connections)
            if (included.contains (c.toNode))
                ++m_readers[{ c.fromNode, c.fromPort }];

        planWidths (included);

        for (const auto& group : planGroups (m_graph, included))
        {
            if (group.region)
            {
                planRegion (group);
            }
            else if (! planNode (*group.nodes[0]))
            {
                break;
            }
        }

        // Nodes that don't reach the output never run, but keep an instance, so a plugin placed in the graph
        // loads (and its editor opens) before it is connected (docs/design/phase8-node-graph.md, G5). A voice graph
        // has no plugins, so its unconnected nodes get none.
        if (! m_voiceScope)
            for (const auto& node : m_graph.nodes)
                if (! included.contains (node.id))
                    std::ignore = m_instance (node.id, node.type, 0, &node);

        recordWidths (included);
        m_plan.slotCount = m_slots.slotCount();
        return std::move (m_plan);
    }

private:
    // What a node's inputs left behind to clean up once the node has run.
    struct PendingReleases
    {
        std::vector<PortRef> readers;
        std::vector<int> slots;
    };

    // Works out every adaptive node's width (docs/design/mono-ports.md, MP3): mono until a stereo source reaches one of
    // its adaptive inputs. Widths only ever grow, so this settles, loops included: a loop with a stereo node in it is
    // stereo all round, and an all-mono loop stays mono.
    void planWidths (const std::set<ObjectId>& included)
    {
        for (const auto& c : m_graph.connections)
            if (included.contains (c.toNode) && c.fromPort >= 0 && c.fromPort < 32)
                m_wired[c.fromNode] |= 1u << c.fromPort;

        for (const auto& node : m_graph.nodes)
        {
            if (! included.contains (node.id))
                continue;

            // A plugin's widths come from the layouts it takes, a Faust script's from its channels: their instances
            // answer (MP4, MP5).
            if (nodetype::isPlugin (node.type) || node.type == nodetype::faust)
                if (auto instance = m_instance (node.id, node.type, 0, &node))
                    m_deciding[node.id] = std::move (instance);

            if (const auto* descriptor = m_registry.find (node.type);
                descriptor != nullptr && hasAdaptivePorts (*descriptor))
            {
                // A source has nothing to grow its width from: stereo unless it answers mono itself (MP15).
                const auto answer = m_sourceWidths != nullptr ? m_sourceWidths (node) : 0;
                m_widths[node.id] = answer != 0 ? std::clamp (answer, 1, 2) : (descriptor->inputs.empty() ? 2 : 1);
            }
        }

        for (auto changed = true; changed;)
        {
            changed = false;

            for (const auto& c : m_graph.connections)
            {
                const auto it = m_widths.find (c.toNode);

                if (it == m_widths.end() || it->second == 2 || ! included.contains (c.fromNode))
                    continue;

                const auto& to = m_graph.nodes[static_cast<size_t> (m_graph.indexOfNode (c.toNode))];
                const auto& inputs = m_registry.find (to.type)->inputs;
                const auto port = static_cast<size_t> (c.toPort);

                if (port < inputs.size() && inputs[port].type == PortType::audio
                    && inputs[port].channels == adaptiveChannels && sourceWidth (c.fromNode, c.fromPort) == 2)
                {
                    it->second = 2;
                    changed = true;
                }
            }
        }

        // A plugin takes the layout for its width now, before its latency is read.
        for (const auto& [id, node] : m_deciding)
            node->useWidth (nodeWidth (id));
    }

    // Notes the widths the graph view shows (docs/design/mono-ports.md, MP8, MP9): every audio port's, for each node
    // that has a mono port or an input that folds. The others are all stereo and left out.
    void recordWidths (const std::set<ObjectId>& included)
    {
        for (const auto& node : m_graph.nodes)
        {
            if (! included.contains (node.id) || node.id == m_outputId || m_busOutputIds.contains (node.id))
                continue;

            if (node.id < 0)
            {
                recordPortWidth (node);
                continue;
            }

            const auto deciding = m_deciding.find (node.id);
            const auto* descriptor =
                deciding != m_deciding.end() ? &deciding->second->descriptor() : m_registry.find (node.type);

            if (descriptor == nullptr)
                continue;

            PortWidths widths;
            auto mono = false;

            for (const auto& port : descriptor->inputs)
            {
                widths.inputs.push_back (port.type == PortType::audio ? portWidth (node.id, port) : 0);
                mono = mono || widths.inputs.back() == 1;
            }

            for (size_t port = 0; port < descriptor->outputs.size(); ++port)
            {
                widths.outputs.push_back (
                    descriptor->outputs[port].type == PortType::audio ? outputWidth (node.id, *descriptor, port) : 0);
                mono = mono || widths.outputs.back() == 1;
            }

            if (const auto it = m_folded.find (node.id); it != m_folded.end())
                widths.folded = it->second;

            if (mono || ! widths.folded.empty())
                m_plan.portWidths[node.id] = std::move (widths);
        }
    }

    // A component's mono port (MP11) shows where cables meet it: an input on the component itself, an output inside, on
    // its Output node's input. Both are inputs of the node that shows them; a stereo port shows as declared. The other
    // ports of that node are left 0, unknown here. See portWidthNodeId().
    void recordPortWidth (const GraphNode& node)
    {
        if (node.type != nodetype::monoPort)
            return;

        const auto encoded = -node.id - 1;
        const auto port = static_cast<size_t> (encoded % 256);
        auto& widths = m_plan.portWidths[encoded / 512];

        if (widths.inputs.size() <= port)
            widths.inputs.resize (port + 1, 0);

        widths.inputs[port] = 1;

        if (m_folded.contains (node.id))
            widths.folded.push_back (static_cast<int> (port));
    }

    // The width of a node's output port, before anything is planned: from the node itself where it decides its ports
    // (a plugin, a Faust script), else from its type's descriptor. A port neither knows is stereo. A source that
    // decides its own width (an Audio Player of mono clips or a mono input; MP15) answers through its adaptive port,
    // its width set in planWidths().
    [[nodiscard]] int sourceWidth (ObjectId nodeId, int port) const
    {
        const auto deciding = m_deciding.find (nodeId);
        const auto index = m_graph.indexOfNode (nodeId);
        const auto* descriptor =
            deciding != m_deciding.end()
                ? &deciding->second->descriptor()
                : (index >= 0 ? m_registry.find (m_graph.nodes[static_cast<size_t> (index)].type) : nullptr);

        if (descriptor == nullptr || port < 0)
            return 2;

        return outputWidth (nodeId, *descriptor, static_cast<size_t> (port));
    }

    // The width of an output port of a node, asking the node first (MP4).
    [[nodiscard]] int outputWidth (ObjectId nodeId, const NodeDescriptor& descriptor, size_t port) const
    {
        if (port >= descriptor.outputs.size())
            return 2;

        if (const auto it = m_deciding.find (nodeId); it != m_deciding.end())
        {
            const auto wired = m_wired.find (nodeId);
            const auto width =
                it->second->outputWidth (port, nodeWidth (nodeId), wired != m_wired.end() ? wired->second : 0u);

            if (width != adaptiveChannels)
                return std::clamp (width, 1, 2);
        }

        return portWidth (nodeId, descriptor.outputs[port]);
    }

    [[nodiscard]] int portWidth (ObjectId nodeId, const PortInfo& port) const
    {
        if (port.type != PortType::audio)
            return 2;

        if (port.channels == adaptiveChannels)
            return nodeWidth (nodeId);

        return std::clamp (port.channels, 1, 2);
    }

    [[nodiscard]] int nodeWidth (ObjectId nodeId) const
    {
        const auto it = m_widths.find (nodeId);
        return it != m_widths.end() ? it->second : 2;
    }

    // A loop's steps, marked so that they run together in small pieces (docs/design/feedback-routing.md, R2). A slot can still be
    // shared inside a region: each piece only touches its own stretch of a buffer, in the same order every time.
    void planRegion (const PlanGroup& group)
    {
        const auto first = m_plan.steps.size();

        // The way back round a loop isn't a path to compensate: everything coming in lines up with what comes round.
        int entry = 0;

        for (const auto* node : group.nodes)
        {
            m_regionNodes.insert (node->id);

            for (const auto& c : m_graph.connections)
                if (c.toNode == node->id && m_latency.contains (c.fromNode))
                    entry = std::max (entry, m_latency.at (c.fromNode));
        }

        for (const auto* node : group.nodes)
        {
            if (node->type == nodetype::feedback)
                planFeedbackRead (*node, entry);
            else
                std::ignore = planNode (*node);
        }

        for (const auto* node : group.feedbackInputs)
            planFeedbackWrite (*node, entry);

        m_plan.steps[first].regionSteps = m_plan.steps.size() - first;
        m_regionNodes.clear();
    }

    // A Feedback node in a loop has two steps: one with only its output and one with only its input.
    void planFeedbackRead (const GraphNode& graphNode, int entry)
    {
        const auto node = m_instance (graphNode.id, graphNode.type, 0, &graphNode);

        if (node == nullptr)
            return;

        const auto& descriptor = node->descriptor();
        PlannedStep step;
        PendingReleases releases;
        step.loopPart = LoopPart::read;
        step.channels = nodeWidth (graphNode.id);
        step.inputSlots.push_back (silenceSlot);

        for (size_t port = 1; port < descriptor.inputs.size(); ++port)
            planInput (graphNode, descriptor, port, entry, step, releases);

        planOutputs (graphNode.id, descriptor, step, node.get());
        step.node = node.get();
        planModulations (graphNode, descriptor, node.get(), step);
        m_plan.nodeSteps[graphNode.id] = m_plan.steps.size();
        m_plan.steps.push_back (step);
        m_latency[graphNode.id] = entry;

        for (const auto& from : releases.readers)
            releaseReader (from);

        for (const auto slot : releases.slots)
            m_slots.release (slot);
    }

    void planFeedbackWrite (const GraphNode& graphNode, int entry)
    {
        const auto node = m_instance (graphNode.id, graphNode.type, 0, &graphNode);

        if (node == nullptr)
            return;

        const auto& descriptor = node->descriptor();
        const auto latency = inputLatency (graphNode.id);
        PlannedStep step;
        PendingReleases releases;
        step.loopPart = LoopPart::write;
        step.channels = nodeWidth (graphNode.id);
        planInput (graphNode, descriptor, 0, latency, step, releases);

        for (size_t port = 1; port < descriptor.inputs.size(); ++port)
            step.inputSlots.push_back (silenceSlot);

        step.node = node.get();
        m_plan.steps.push_back (step);
        node->setLoopLatency (latency - entry);

        for (const auto& from : releases.readers)
            releaseReader (from);

        for (const auto slot : releases.slots)
            m_slots.release (slot);
    }

    // Modulation slots (docs/design/modulation.md, M5): resolves each slot's target now that the instance exists.
    void planModulations (const GraphNode& graphNode, const NodeDescriptor& descriptor, Node* node, PlannedStep& step)
    {
        for (const auto& [port, slot] : graphNode.modulations)
        {
            // An inactive slot stays compiled at no depth, so its parameter goes back to its base.
            if (slot.param.isEmpty() || port < 0 || static_cast<size_t> (port) >= descriptor.inputs.size()
                || descriptor.inputs[static_cast<size_t> (port)].type != PortType::modulation)
                continue;

            auto modulation = resolveModulationTarget (graphNode, descriptor, node, slot.param);

            if (! modulation.has_value())
                continue;

            const auto base = modulation->base;
            modulation->compiled.port = port;
            modulation->compiled.depth = slot.active ? static_cast<float> (std::clamp (slot.depth, -1.0, 1.0)) : 0.0f;
            modulation->compiled.state = m_modulationState (graphNode.id, port, slot.param, base);
            modulation->compiled.state->depth.store (modulation->compiled.depth);

            if (auto* plugin = modulation->compiled.plugin;
                plugin != nullptr && plugin->isModulatablePerNote (modulation->compiled.pluginParam))
                planNoteSource (graphNode.id, port, modulation->compiled);

            step.modulations.push_back (std::move (modulation->compiled));
        }
    }

    // A modulation input fed by one note-driven source, with notes going into it, runs per note: Note Expression's values
    // are each note's already, and an LFO or ADSR gets an instance per note.
    void planNoteSource (ObjectId nodeId, int port, CompiledModulation& compiled) const
    {
        const Connection* feed = nullptr;

        for (const auto& c : m_graph.connections)
        {
            if (c.toNode == nodeId && c.toPort == port)
            {
                if (feed != nullptr)
                    return;

                feed = &c;
            }
        }

        if (feed == nullptr)
            return;

        const auto index = m_graph.indexOfNode (feed->fromNode);

        if (index < 0)
            return;

        const auto& source = m_graph.nodes[static_cast<size_t> (index)];

        const auto hasNotes = std::ranges::any_of (m_graph.connections, [feed] (const Connection& c)
                                                   { return c.toNode == feed->fromNode && c.toPort == 0; });
        if (! hasNotes)
            return;

        if (source.type == nodetype::noteExpression)
        {
            compiled.noteSource = feed->fromPort;
        }
        else if ((source.type == nodetype::lfo || source.type == nodetype::adsr) && m_noteSources != nullptr)
        {
            compiled.noteNodes = m_noteSources (source);
            compiled.notePort = feed->fromPort;
        }
    }

    struct ResolvedModulation
    {
        CompiledModulation compiled;
        float base = 0.0f; // the parameter's unmodulated value, for a new slot
    };

    // A slot's target: a plugin parameter, or a built-in one that accepts modulation. Nothing if the parameter is
    // unknown or can't be modulated.
    [[nodiscard]] static std::optional<ResolvedModulation> resolveModulationTarget (const GraphNode& graphNode,
                                                                                    const NodeDescriptor& descriptor,
                                                                                    Node* node,
                                                                                    const juce::String& param)
    {
        ResolvedModulation result;

        if (auto* plugin = dynamic_cast<PluginNode*> (node))
        {
            const auto index = plugin->parameterIndex (param);

            if (index < 0)
                return std::nullopt;

            result.compiled.plugin = plugin;
            result.compiled.pluginParam = index;
            result.base = plugin->parameterValue (index);
            return result;
        }

        const auto index = descriptor.indexOfParam (param);

        if (index < 0)
            return std::nullopt;

        const auto& info = descriptor.params[static_cast<size_t> (index)];

        if (! info.acceptsModulation())
            return std::nullopt;

        result.compiled.node = node;
        result.compiled.param = static_cast<size_t> (index);
        result.compiled.info = info;
        const auto it = graphNode.params.find (param);
        result.base = it != graphNode.params.end() ? static_cast<float> (it->second) : info.defaultValue;
        return result;
    }

    // Returns false once the track output and every Output to track have been planned (nothing after them matters).
    [[nodiscard]] bool planNode (const GraphNode& graphNode)
    {
        const auto* descriptor = m_registry.find (graphNode.type);

        if (descriptor == nullptr)
            return true;

        // The Voice Input runs no step: the Voice Instrument writes the voice's events into its buffer.
        if (m_voiceScope && graphNode.type == nodetype::voiceInput)
        {
            m_eventSlots[{ graphNode.id, 0 }] = m_eventCount;
            m_plan.inputEvents = m_eventCount++;
            m_latency[graphNode.id] = 0;
            return true;
        }

        const auto isOutput = graphNode.id == m_outputId;
        const auto isBusOutput = m_busOutputIds.contains (graphNode.id);
        const auto node = isOutput || isBusOutput ? nullptr : m_instance (graphNode.id, graphNode.type, 0, &graphNode);

        if (! isOutput && ! isBusOutput && node == nullptr)
            return true;

        if (node != nullptr)
            node->setInLoop (m_regionNodes.contains (graphNode.id));

        // A Faust node's ports are its script's (docs/design/phase11-faust-panels.md, F2).
        if (node != nullptr)
            descriptor = &node->descriptor();

        const auto latency = inputLatency (graphNode.id);
        PlannedStep step;
        PendingReleases releases;

        for (size_t port = 0; port < descriptor->inputs.size(); ++port)
            planInput (graphNode, *descriptor, port, latency, step, releases);

        if (isOutput)
        {
            // The track output is a sink: its input buffer is what the mixer reads, so it stays reserved.
            m_plan.outputSlot = step.inputSlots.empty() ? silenceSlot : step.inputSlots[0];
            m_plan.latency = latency;
            return --m_sinksLeft > 0;
        }

        if (isBusOutput)
        {
            // So is an Output to track: the mixer adds its input to the output track's bus (MO7).
            m_plan.busOutputs.push_back ({ graphNode.id, graphNode.target,
                                           step.inputSlots.empty() ? silenceSlot : step.inputSlots[0], latency });
            return --m_sinksLeft > 0;
        }

        planOutputs (graphNode.id, *descriptor, step, node.get());
        step.node = node.get();
        step.channels = hasAdaptivePorts (*descriptor) ? nodeWidth (graphNode.id) : 2;

        planModulations (graphNode, *descriptor, node.get(), step);

        // A key input wired inside the track wins; a free one can take key sends from other tracks (K4). In a voice
        // graph, keys are internal only (V6).
        const auto keyWired = std::ranges::any_of (m_graph.connections, [&] (const Connection& c)
                                                   { return c.toNode == graphNode.id && c.toPort == 1; });

        if (! m_voiceScope && descriptor->inputs.size() > 1 && descriptor->inputs[1].type == PortType::audio
            && ! keyWired)
            m_plan.keyInputs[graphNode.id] = { m_plan.steps.size(), latency, node.get() };

        if (! m_voiceScope && graphNode.type == nodetype::voiceInstrument)
            step.voice = m_voice (graphNode);

        if (m_samples != nullptr && nodetype::playsSamples (graphNode.type))
        {
            step.samples = m_samples (graphNode);
            node->setSampleMap (step.samples);
        }

        if (m_files != nullptr && nodetype::usesAudioFile (graphNode.type))
            node->setAudioFile (m_files (graphNode.fileId));

        if (graphNode.type == nodetype::audioPlayer)
            node->setOnlyFile (graphNode.fileId);

        if (graphNode.type == nodetype::feedback)
            node->setLoopLatency (0);

        const auto voiceLatency = step.voice != nullptr ? step.voice->latencySamples : 0;
        m_plan.inputLatencies[graphNode.id] = latency;
        m_plan.nodeSteps[graphNode.id] = m_plan.steps.size();
        m_plan.steps.push_back (step);

        // A bypassed node passes its input straight through, so it adds no delay: bypassing a latent plugin takes its
        // latency out of the whole project, for monitoring through it.
        m_latency[graphNode.id] = latency + (graphNode.bypassed ? 0 : node->latencySamples()) + voiceLatency;

        // Outputs were acquired before inputs are released, so a node never writes into its own input.
        for (const auto& from : releases.readers)
            releaseReader (from);

        for (const auto slot : releases.slots)
            m_slots.release (slot);

        return true;
    }

    [[nodiscard]] int inputLatency (ObjectId nodeId) const
    {
        int latency = 0;

        for (const auto& c : m_graph.connections)
            if (c.toNode == nodeId && m_latency.contains (c.fromNode))
                latency = std::max (latency, m_latency.at (c.fromNode));

        return latency;
    }

    void planInput (const GraphNode& graphNode, const NodeDescriptor& descriptor, size_t port, int latency,
                    PlannedStep& step, PendingReleases& releases)
    {
        std::vector<std::pair<int, int>> sources; // (slot, width)
        PendingReleases sourceReleases;

        if (descriptor.inputs[port].type == PortType::events)
            step.takesEvents = true;

        for (const auto& c : m_graph.connections)
        {
            if (c.toNode != graphNode.id || c.toPort != static_cast<int> (port) || ! m_latency.contains (c.fromNode))
                continue;

            if (descriptor.inputs[port].type == PortType::events)
            {
                if (const auto it = m_eventSlots.find ({ c.fromNode, c.fromPort }); it != m_eventSlots.end())
                {
                    step.eventInput = it->second;
                    const auto source = m_eventSources.find ({ c.fromNode, c.fromPort });
                    step.eventSource = source != m_eventSources.end() ? source->second : 0;
                }
            }
            else
            {
                sources.push_back (planAudioSource (c, graphNode.id, port, latency, sourceReleases));
            }
        }

        // Widths meet at the cable (docs/design/mono-ports.md, MP2): a mono source is read as the same channel twice,
        // which costs nothing, but a stereo source into a mono input is folded, and a sink (the track output, an
        // Output to track) hands its slot on to the mixer, so it gets both channels written.
        const auto width = portWidth (graphNode.id, descriptor.inputs[port]);
        const auto sink = graphNode.id == m_outputId || m_busOutputIds.contains (graphNode.id);

        if (width == 1 && std::ranges::any_of (sources, [] (const auto& source) { return source.second == 2; }))
            m_folded[graphNode.id].push_back (static_cast<int> (port));

        if (sources.empty())
        {
            step.inputSlots.push_back (silenceSlot);
        }
        else if (sources.size() == 1 && ! (width == 1 && sources[0].second == 2) && ! (sink && sources[0].second == 1))
        {
            step.inputSlots.push_back (sources[0].first);

            if (sources[0].second == 1)
                markMono (step.monoInputs, step.inputSlots.size() - 1);

            releases.readers.insert (releases.readers.end(), sourceReleases.readers.begin(),
                                     sourceReleases.readers.end());
            releases.slots.insert (releases.slots.end(), sourceReleases.slots.begin(), sourceReleases.slots.end());
        }
        else
        {
            // Several sources, or one of another width: sum them into one buffer first; the mix step is their reader.
            PlannedStep mix;

            for (const auto& [slot, sourceWidth] : sources)
            {
                mix.mixSlots.push_back (slot);

                if (sourceWidth == 1)
                    markMono (mix.monoSources, mix.mixSlots.size() - 1);
            }

            mix.outputSlots = { m_slots.acquire() };
            mix.channels = width;

            if (width == 1)
                markMono (mix.monoOutputs, 0);

            m_plan.steps.push_back (mix);

            for (const auto& from : sourceReleases.readers)
                releaseReader (from);

            for (const auto slot : sourceReleases.slots)
                m_slots.release (slot);

            step.inputSlots.push_back (mix.outputSlots[0]);
            releases.slots.push_back (mix.outputSlots[0]);

            if (width == 1)
                markMono (step.monoInputs, step.inputSlots.size() - 1);
        }
    }

    // Returns the slot holding a connection's audio and its width, inserting a compensation delay if its path is
    // faster.
    [[nodiscard]] std::pair<int, int> planAudioSource (const Connection& c, ObjectId nodeId, size_t port, int latency,
                                                       PendingReleases& releases)
    {
        const PortRef from { c.fromNode, c.fromPort };
        const auto sourceSlot = m_audioSlots.contains (from) ? m_audioSlots[from] : silenceSlot;
        const auto width = m_outputWidths.contains (from) ? m_outputWidths[from] : 2;
        const auto delay = latency - m_latency[c.fromNode];

        // A delay inside a loop would only make the loop longer (docs/design/feedback-routing.md, R4).
        if (delay <= 0 || (m_regionNodes.contains (c.fromNode) && m_regionNodes.contains (nodeId)))
        {
            releases.readers.push_back (from);
            return { sourceSlot, width };
        }

        const auto delayKey =
            juce::String (c.fromNode) + ":" + juce::String (c.fromPort) + ">" + juce::String (static_cast<int> (port));
        const auto delayNode = m_instance (nodeId, delayKey, delay, nullptr);

        PlannedStep delayStep;
        delayStep.node = delayNode.get();
        delayStep.inputSlots = { sourceSlot };
        delayStep.outputSlots = { m_slots.acquire() };
        delayStep.channels = width;

        if (width == 1)
        {
            markMono (delayStep.monoInputs, 0);
            markMono (delayStep.monoOutputs, 0);
        }

        m_plan.steps.push_back (delayStep);
        releaseReader (from); // the delay step was its reader

        releases.slots.push_back (delayStep.outputSlots[0]);
        return { delayStep.outputSlots[0], width };
    }

    void planOutputs (ObjectId nodeId, const NodeDescriptor& descriptor, PlannedStep& step, const Node* node)
    {
        for (size_t port = 0; port < descriptor.outputs.size(); ++port)
        {
            const PortRef ref { nodeId, static_cast<int> (port) };

            if (descriptor.outputs[port].type == PortType::events)
            {
                m_eventSlots[ref] = m_eventCount;
                m_eventSources[ref] = node != nullptr ? node->serial() : 0;
                step.eventOutput = m_eventCount++;
                step.outputSlots.push_back (scratchSlot);
            }
            else if (m_readers[ref] > 0)
            {
                m_audioSlots[ref] = m_slots.acquire();
                m_outputWidths[ref] = outputWidth (nodeId, descriptor, port);
                step.outputSlots.push_back (m_audioSlots[ref]);

                if (m_outputWidths[ref] == 1)
                    markMono (step.monoOutputs, port);

                if (port < 32)
                    step.wiredOutputs |= 1u << port;
            }
            else
            {
                step.outputSlots.push_back (scratchSlot);
            }
        }
    }

    void releaseReader (PortRef port)
    {
        if (--m_readers[port] == 0)
            if (const auto it = m_audioSlots.find (port); it != m_audioSlots.end())
                m_slots.release (it->second);
    }

    const TrackGraph& m_graph;
    const NodeRegistry& m_registry;
    InstanceFactory m_instance;
    ModulationStateFactory m_modulationState;
    int& m_eventCount;
    VoiceCompiler m_voice;
    SampleCompiler m_samples;
    NoteSourceFactory m_noteSources;
    FileOpener m_files;
    bool m_voiceScope;

    PlannedTrack m_plan;
    ObjectId m_outputId = 0;
    std::set<ObjectId> m_busOutputIds;
    int m_sinksLeft = 1;
    std::map<PortRef, int> m_readers;
    std::map<PortRef, int> m_audioSlots;
    std::map<PortRef, int> m_outputWidths;                // of every planned audio output that has readers
    std::map<ObjectId, int> m_widths;                     // of every adaptive node (MP3)
    std::map<ObjectId, std::shared_ptr<Node>> m_deciding; // nodes whose ports are their own: plugins, Faust (MP4, MP5)
    std::function<int (const GraphNode&)> m_sourceWidths; // see withSourceWidths()
    std::map<ObjectId, juce::uint32> m_wired;             // each node's wired outputs, a bit per port
    std::map<ObjectId, std::vector<int>> m_folded;        // each node's inputs where stereo is folded to mono (MP9)
    std::map<PortRef, int> m_eventSlots;
    std::map<PortRef, juce::uint64> m_eventSources;
    std::map<ObjectId, int> m_latency; // output latency of every planned node
    std::set<ObjectId> m_regionNodes;  // the loop being planned
    SlotAllocator m_slots;
};

// Turns slot indices into channel pointers into the engine's audio memory.
class SlotViews
{
public:
    SlotViews (float* memory, size_t blockSize)
        : m_memory (memory),
          m_blockSize (blockSize)
    {
    }

    // A mono slot is read as its first channel twice (docs/design/mono-ports.md, MP6).
    [[nodiscard]] AudioInput input (int slot, bool mono = false) const noexcept
    {
        const auto* base = channel (slot);
        return { base, mono ? base : base + m_blockSize };
    }

    // A mono slot is written in its first channel; the second goes to scratch.
    [[nodiscard]] AudioOutput output (int slot, bool mono = false) const noexcept
    {
        auto* base = channel (slot);
        return { base, mono ? channel (scratchSlot) : base + m_blockSize };
    }

    [[nodiscard]] size_t blockSize() const noexcept { return m_blockSize; }

private:
    [[nodiscard]] float* channel (int slot) const noexcept
    {
        return m_memory + static_cast<size_t> (slot) * 2 * m_blockSize;
    }

    float* m_memory;
    size_t m_blockSize;
};

[[nodiscard]] CompiledTrack materialise (const PlannedTrack& plan, const SlotViews& views)
{
    CompiledTrack compiled;
    compiled.id = plan.id;
    compiled.sequence = plan.sequence;
    compiled.audio = plan.audio;
    compiled.state = plan.state;
    compiled.output = views.input (plan.outputSlot);
    compiled.latencySamples = plan.latency;
    compiled.lanes = plan.lanes;
    compiled.links = plan.links;

    for (const auto& step : plan.steps)
    {
        CompiledStep result;
        result.regionSteps = step.regionSteps;
        result.loopPart = step.loopPart;
        result.node = step.node;
        result.eventInput = step.eventInput;
        result.eventOutput = step.eventOutput;
        result.takesEvents = step.takesEvents;
        result.eventSource = step.eventSource;

        for (size_t i = 0; i < step.inputSlots.size(); ++i)
            result.audioInputs.push_back (views.input (step.inputSlots[i], isMono (step.monoInputs, i)));

        for (size_t i = 0; i < step.outputSlots.size(); ++i)
        {
            result.audioOutputs.push_back (views.output (step.outputSlots[i], isMono (step.monoOutputs, i)));

            if (i < 32 && isMono (step.monoOutputs, i))
                result.monoOutputs |= 1u << i;
        }

        for (size_t i = 0; i < step.mixSlots.size(); ++i)
            result.mixSources.push_back (views.input (step.mixSlots[i], isMono (step.monoSources, i)));

        result.channels = step.channels;

        result.modulations = step.modulations;
        result.links = step.links;
        result.voice = step.voice;
        result.samples = step.samples;
        result.wiredOutputs = step.wiredOutputs;

        if (std::ranges::any_of (step.modulations,
                                 [] (const CompiledModulation& m) { return m.node != nullptr && ! m.restore; }))
            result.modulationValues = std::make_unique<float[]> ( // NOLINT(modernize-avoid-c-arrays)
                step.modulations.size() * views.blockSize());

        compiled.steps.push_back (std::move (result));
    }

    return compiled;
}

// ---------------------------------------------------------------------------------------------------------
// Compiling a project, step by step (see GraphCompiler::compile)

// Where each track's audio goes, when it becomes ready and in what order things must run
// (docs/design/groups-and-sends.md, R6, R7; docs/design/sidechain.md, K2, K3).
struct BusLayout
{
    std::map<ObjectId, size_t> indexOf;
    std::vector<int> parentOf;                   // the track's group, or -1
    std::vector<int> arrival;                    // samples at which its input is ready
    std::vector<int> reach;                      // samples at which its own output is ready
    int topArrival = 0;                          // samples at which the master mix is ready
    std::vector<std::vector<int>> sendTargets;   // per send: the return track it feeds, or -1
    std::vector<std::vector<int>> keyTargets;    // per send: the track it keys, or -1
    std::vector<std::vector<int>> outputTargets; // per Output to track: the output track it feeds, or -1 (MO7)
    std::vector<std::vector<size_t>> runsAfter;  // per track: the tracks it must run after
    std::vector<size_t> returnOrder;             // the returns, in the order they must run
};

// The latency-compensation taps: what each send reads, and how late the latest key of a node arrives.
struct SendDelays
{
    std::vector<std::vector<int>> slots;             // per send: the slot the send reads
    std::vector<std::vector<int>> busSlots;          // per Output to track: the slot the mixer reads (MO7)
    std::map<std::pair<size_t, ObjectId>, int> late; // per keyed node: samples its latest key is late
};

// The order the tracks run in, and the buses they mix into.
struct MixOrder
{
    std::vector<size_t> order;
    std::vector<int> buses;                              // per track: its bus, or -1
    std::vector<int> position;                           // per track: its place in the run order
    std::map<std::pair<size_t, ObjectId>, int> keyBuses; // per keyed node: the bus its key arrives on
};

// Everything one compilation works out, handed from step to step.
struct CompileState
{
    std::shared_ptr<CompiledEngine> engine;
    std::vector<PlannedTrack> planned;
    std::optional<PlannedTrack> master;
    int eventCount = 0;
    BusLayout layout;
    SendDelays delays;
    MixOrder mix;
};

// The engine for one compilation: its spec and the project-wide settings the audio thread reads.
[[nodiscard]] std::shared_ptr<CompiledEngine> makeEngine (const Project& project, const ProcessSpec& spec)
{
    auto engine = std::make_shared<CompiledEngine>();
    engine->spec = spec;
    engine->tempoMap = project.tempoMap;
    engine->metronome = project.metronome;
    engine->loop = project.loop;
    return engine;
}

// Each track's group, and the sends (R4) that can play: to another, existing return track.
[[nodiscard]] BusLayout baseLayout (const Project& project, size_t count)
{
    BusLayout layout;
    layout.parentOf.assign (count, -1);
    layout.arrival.assign (count, 0);
    layout.reach.assign (count, 0);
    layout.sendTargets.resize (count);
    layout.keyTargets.resize (count);
    layout.runsAfter.resize (count);

    for (size_t i = 0; i < count; ++i)
        layout.indexOf[project.tracks[i]->id] = i;

    for (size_t i = 0; i < count; ++i)
        if (const auto parent = layout.indexOf.find (project.tracks[i]->parent); parent != layout.indexOf.end())
            layout.parentOf[i] = static_cast<int> (parent->second);

    for (size_t i = 0; i < count; ++i)
    {
        for (const auto& send : project.tracks[i]->sends)
        {
            const auto target = layout.indexOf.find (send.target);
            const auto valid = send.targetNode == 0 && target != layout.indexOf.end() && target->second != i
                               && project.tracks[target->second]->kind == TrackKind::returnTrack;
            layout.sendTargets[i].push_back (valid ? static_cast<int> (target->second) : -1);
        }
    }

    // The tracks each track must run after: the groups that contain it, and the senders into it.
    for (size_t i = 0; i < count; ++i)
    {
        if (layout.parentOf[i] >= 0)
            layout.runsAfter[static_cast<size_t> (layout.parentOf[i])].push_back (i);

        for (const auto target : layout.sendTargets[i])
            if (target >= 0)
                layout.runsAfter[static_cast<size_t> (target)].push_back (i);
    }

    return layout;
}

// Whether `to` must run after `from` already.
[[nodiscard]] bool reachesFrom (const BusLayout& layout, size_t from, size_t to)
{
    std::vector<bool> seen (layout.runsAfter.size(), false);
    std::vector<size_t> pending { to };

    while (! pending.empty())
    {
        const auto j = pending.back();
        pending.pop_back();

        if (j == from)
            return true;

        if (! seen[j])
        {
            seen[j] = true;
            pending.insert (pending.end(), layout.runsAfter[j].begin(), layout.runsAfter[j].end());
        }
    }

    return false;
}

// Output to track nodes (docs/design/multi-output-instruments.md, MO7) that can play: to an output track of their own
// track. The output track runs after it.
void addOutputTargets (const Project& project, const std::vector<PlannedTrack>& planned, BusLayout& layout)
{
    const auto count = planned.size();
    layout.outputTargets.resize (count);

    for (size_t i = 0; i < count; ++i)
    {
        for (const auto& output : planned[i].busOutputs)
        {
            const auto target = layout.indexOf.find (output.target);
            const auto valid = target != layout.indexOf.end() && target->second != i
                               && project.tracks[target->second]->kind == TrackKind::output
                               && project.tracks[target->second]->source == project.tracks[i]->id;

            if (valid)
                layout.runsAfter[target->second].push_back (i);

            layout.outputTargets[i].push_back (valid ? static_cast<int> (target->second) : -1);
        }
    }
}

// Key sends (docs/design/sidechain.md, K2, K3) that can play: to a free key input on another track, without making a
// track run both before and after another (a file could hold such a loop; that key is left out).
void addKeyTargets (const Project& project, const std::vector<PlannedTrack>& planned, BusLayout& layout)
{
    const auto count = planned.size();

    for (size_t i = 0; i < count; ++i)
    {
        for (const auto& send : project.tracks[i]->sends)
        {
            const auto target = send.targetNode != 0 ? layout.indexOf.find (send.target) : layout.indexOf.end();
            const auto valid = target != layout.indexOf.end() && target->second != i
                               && planned[target->second].keyInputs.contains (send.targetNode)
                               && ! reachesFrom (layout, target->second, i);

            if (valid)
                layout.runsAfter[target->second].push_back (i);

            layout.keyTargets[i].push_back (valid ? static_cast<int> (target->second) : -1);
        }
    }
}

// The returns in the order they must run: each after the returns that send to it. A send that would close a loop
// between returns (a file could hold one) is left out.
void orderReturns (const Project& project, BusLayout& layout)
{
    const auto count = project.tracks.size();
    std::vector<int> visited (count, 0); // 1 while on the path, 2 when done

    const std::function<void (size_t)> visit = [&] (size_t i)
    {
        visited[i] = 1;

        for (auto& target : layout.sendTargets[i])
        {
            if (target < 0)
                continue;

            if (visited[static_cast<size_t> (target)] == 1)
                target = -1; // a loop
            else if (visited[static_cast<size_t> (target)] == 0)
                visit (static_cast<size_t> (target));
        }

        visited[i] = 2;
        layout.returnOrder.push_back (i);
    };

    for (size_t i = 0; i < count; ++i)
        if (project.tracks[i]->kind == TrackKind::returnTrack && visited[i] == 0)
            visit (i);

    std::ranges::reverse (layout.returnOrder);
}

// Each track's output reaches its bus with its own latency plus the latency at its input; where tracks meet at a bus,
// each is delayed to the latest one. Tracks go after everything that feeds them: members before their group, senders
// before their returns (with any send loop already left out), and an instrument before its output tracks, which an
// Output to track node reaches with the latency at that node (MO7).
void computeLatency (const std::vector<PlannedTrack>& planned, BusLayout& layout)
{
    const auto count = planned.size();
    std::vector<std::vector<size_t>> feeders (count);

    for (size_t i = 0; i < count; ++i)
    {
        if (layout.parentOf[i] >= 0)
            feeders[static_cast<size_t> (layout.parentOf[i])].push_back (i);

        for (const auto target : layout.sendTargets[i])
            if (target >= 0)
                feeders[static_cast<size_t> (target)].push_back (i);

        for (const auto target : layout.outputTargets[i])
            if (target >= 0)
                feeders[static_cast<size_t> (target)].push_back (i);
    }

    std::vector<bool> placed (count, false);
    std::vector<size_t> order;
    const std::function<void (size_t)> visit = [&] (size_t i)
    {
        if (placed[i])
            return;

        placed[i] = true;

        for (const auto feeder : feeders[i])
            visit (feeder);

        order.push_back (i);
    };

    for (size_t i = 0; i < count; ++i)
        visit (i);

    for (const auto i : order)
    {
        layout.reach[i] = planned[i].latency + layout.arrival[i];
        const auto alignedReach = layout.reach[i] - planned[i].offset;
        auto& bus =
            layout.parentOf[i] >= 0 ? layout.arrival[static_cast<size_t> (layout.parentOf[i])] : layout.topArrival;
        bus = std::max (bus, alignedReach);

        // A send leaves its track at the track's own output, before that is aligned at the track's bus.
        for (const auto target : layout.sendTargets[i])
            if (target >= 0)
                layout.arrival[static_cast<size_t> (target)] =
                    std::max (layout.arrival[static_cast<size_t> (target)], alignedReach);

        for (size_t k = 0; k < layout.outputTargets[i].size(); ++k)
            if (const auto target = layout.outputTargets[i][k]; target >= 0)
                layout.arrival[static_cast<size_t> (target)] =
                    std::max (layout.arrival[static_cast<size_t> (target)],
                              layout.arrival[i] + planned[i].busOutputs[k].latency - planned[i].offset);
    }

    // Tracks see the transport `latencySamples - arrival` ahead, which must not be negative: a group or return whose
    // own offset pulls it earlier can have an arrival beyond its offset-adjusted reach.
    if (const auto latest = std::ranges::max_element (layout.arrival); latest != layout.arrival.end())
        layout.topArrival = std::max (layout.topArrival, *latest);
}

// The bus graph and the latencies, in one go.
[[nodiscard]] BusLayout analyseBuses (const Project& project, const std::vector<PlannedTrack>& planned)
{
    auto layout = baseLayout (project, planned.size());
    addOutputTargets (project, planned, layout);
    addKeyTargets (project, planned, layout);
    orderReturns (project, layout);
    computeLatency (planned, layout);
    return layout;
}

// Each track gets its own arena of buffer slots, so tracks that read no bus can render at the same time without
// overwriting each other's scratch. Each arena starts with its own silence and scratch slots.
void allocateSlots (const std::vector<PlannedTrack>& planned, const std::optional<PlannedTrack>& master,
                    const ProcessSpec& spec, int eventCount, CompiledEngine& engine)
{
    const auto blockSize = static_cast<size_t> (spec.maxBlockSize);
    engine.numAudioSlots = 0;

    for (const auto& plan : planned)
        engine.numAudioSlots += plan.slotCount;

    if (master.has_value())
        engine.numAudioSlots += master->slotCount;

    engine.trackMix.assign (planned.size(), TrackMix {});
    engine.audioMemory = std::make_unique<float[]> ( // NOLINT(modernize-avoid-c-arrays): zero-initialised
        static_cast<size_t> (engine.numAudioSlots) * 2 * blockSize);
    engine.numEventBuffers = static_cast<size_t> (eventCount);
    engine.eventBuffers = std::make_unique<EventBuffer[]> (engine.numEventBuffers); // NOLINT(modernize-avoid-c-arrays)
}

// Mixing order (K3): each group after its members, each return after its senders and each keyed track after its key
// sources, so a bus is complete when it runs; otherwise in track-list order. Also the buses they mix into.
[[nodiscard]] MixOrder mixingOrder (const Project& project, const BusLayout& layout, CompiledEngine& engine)
{
    const auto count = project.tracks.size();
    MixOrder mix;
    mix.buses.assign (count, -1);

    for (size_t i = 0; i < count; ++i)
        if (project.tracks[i]->kind == TrackKind::group || project.tracks[i]->kind == TrackKind::returnTrack
            || project.tracks[i]->kind == TrackKind::output)
            mix.buses[i] = engine.numBuses++;

    std::vector<bool> placed (count, false);
    const std::function<void (size_t)> visit = [&] (size_t i)
    {
        if (placed[i])
            return;

        placed[i] = true;

        for (const auto before : layout.runsAfter[i])
            visit (before);

        mix.order.push_back (i);
    };

    for (size_t i = 0; i < count; ++i)
        visit (i);

    mix.position.assign (count, 0);

    for (size_t k = 0; k < mix.order.size(); ++k)
        mix.position[mix.order[k]] = static_cast<int> (k);

    // A key bus per node that is keyed from other tracks (K4).
    for (size_t i = 0; i < count; ++i)
        for (size_t k = 0; k < layout.keyTargets[i].size(); ++k)
            if (const auto keyed = layout.keyTargets[i][k]; keyed >= 0)
            {
                const auto key = std::pair { static_cast<size_t> (keyed), project.tracks[i]->sends[k].targetNode };

                if (! mix.keyBuses.contains (key))
                    mix.keyBuses[key] = engine.numBuses++;
            }

    return mix;
}

// The tracks inside a group: they follow it in the track list until the first one that isn't inside. An output track
// inside brings its instrument along, so soloing the instrument keeps the group audible
// (docs/design/multi-output-instruments.md, MO6).
[[nodiscard]] std::vector<int> insideOf (const Project& project, const BusLayout& layout, const MixOrder& mix,
                                         size_t index)
{
    std::vector<int> inside;

    for (size_t j = index + 1; j < layout.parentOf.size(); ++j)
    {
        auto parent = layout.parentOf[j];

        while (parent >= 0 && static_cast<size_t> (parent) != index)
            parent = layout.parentOf[static_cast<size_t> (parent)];

        if (parent < 0)
            break;

        inside.push_back (mix.position[j]);

        if (const auto source = layout.indexOf.find (project.tracks[j]->source);
            project.tracks[j]->kind == TrackKind::output && source != layout.indexOf.end())
            inside.push_back (mix.position[source->second]);
    }

    return inside;
}

// Points the keyed node's second input at its key bus, and tells it how late its latest key arrives (K5).
template <typename BusOf>
void applyKeyInputs (CompiledTrack& compiled, const PlannedTrack& plan, size_t index, const MixOrder& mix,
                     const SendDelays& delays, const BusOf& busOf)
{
    for (const auto& [nodeId, input] : plan.keyInputs)
    {
        const auto bus = mix.keyBuses.find ({ index, nodeId });

        if (bus != mix.keyBuses.end())
        {
            compiled.steps[input.step].audioInputs[1] = busOf (bus->second);
            compiled.keyed = true;
        }

        const auto late = delays.late.find ({ index, nodeId });
        input.node->setKeyLateSamples (late != delays.late.end() ? std::max (0, late->second) : 0);
    }
}
} // namespace

GraphCompiler::GraphCompiler (const NodeRegistry& registry)
    : m_registry (registry)
{
}

void dissolveJunction (std::vector<Connection>& connections, ObjectId junction, bool* wireLoop)
{
    // One port at a time: a cable may go from one port of a junction to another, as when a component's output feeds another of its inputs
    // through a straight wire.
    const auto nextPort = [&]
    {
        std::optional<int> found;

        for (const auto& c : connections)
            if (c.toNode == junction)
                found = c.toPort;
            else if (c.fromNode == junction)
                found = c.fromPort;

        return found;
    };

    while (const auto next = nextPort())
    {
        const auto port = *next;
        std::vector<Connection> through;
        const auto isHere = [&] (ObjectId id, int p) { return id == junction && p == port; };

        for (const auto& source : connections)
        {
            if (! isHere (source.toNode, source.toPort))
                continue;

            // A cable from a port to itself: wires going round with nothing on the way.
            if (isHere (source.fromNode, source.fromPort))
            {
                if (wireLoop != nullptr)
                    *wireLoop = true;

                continue;
            }

            for (const auto& target : connections)
                if (isHere (target.fromNode, target.fromPort) && ! isHere (target.toNode, target.toPort))
                    through.push_back ({ source.fromNode, source.fromPort, target.toNode, target.toPort });
        }

        std::erase_if (connections, [&] (const Connection& c)
                       { return isHere (c.fromNode, c.fromPort) || isHere (c.toNode, c.toPort); });
        connections.insert (connections.end(), through.begin(), through.end());
    }
}

namespace
{
// The track's Macros device (docs/design/macros-and-modulation-column.md, MC5), or null.
[[nodiscard]] const GraphNode* macrosOf (const TrackGraph& graph) noexcept
{
    const auto found = std::ranges::find (graph.nodes, juce::String (nodetype::macros), &GraphNode::type);
    return found != graph.nodes.end() ? &*found : nullptr;
}

// Whether a flat graph has a slot fed by a macro knob.
[[nodiscard]] bool hasMacroSlots (const TrackGraph& flat) noexcept
{
    return std::ranges::any_of (
        flat.nodes, [] (const GraphNode& node)
        { return std::ranges::any_of (node.modulations, [] (const auto& entry) { return entry.second.macro >= 0; }); });
}

// Each macro slot becomes a cable from its knob's output on the Macros device `macros` (MC8), so the planner orders and
// buffers it like any source.
void wireMacros (TrackGraph& flat, ObjectId macros)
{
    for (const auto& node : flat.nodes)
        for (const auto& [port, slot] : node.modulations)
            if (slot.macro >= 0 && slot.macro < maxMacros && node.id != macros)
                flat.connections.push_back ({ macros, slot.macro, node.id, port });
}
} // namespace

namespace
{
// A component port's width node's id: negative, so it never meets a real node's, and the same on every compilation, so
// its instance is kept. It names whose port it is: the component's (side 0, an input) or its Output node's (side 1).
[[nodiscard]] ObjectId portWidthNodeId (ObjectId owner, int side, int port) noexcept
{
    return -(((owner * 2 + side) * 256) + port + 1);
}

// Puts a component's mono or stereo audio ports between the cables that meet at a junction (docs/design/mono-ports.md,
// MP11): each becomes a node of that width. An adaptive port stays a plain junction.
void addPortWidthNodes (TrackGraph& flat, const std::vector<ComponentPort>& ports, ObjectId junction, ObjectId owner,
                        int side)
{
    for (size_t i = 0; i < ports.size() && i < 256; ++i)
    {
        const auto& port = ports[i];

        if (port.type != PortType::audio || port.channels == adaptiveChannels)
            continue;

        const auto id = portWidthNodeId (owner, side, static_cast<int> (i));
        const auto index = static_cast<int> (i);
        GraphNode node;
        node.id = id;
        node.type = port.channels == 1 ? nodetype::monoPort : nodetype::stereoPort;
        flat.nodes.push_back (std::move (node));

        for (auto& c : flat.connections)
        {
            if (c.toNode == junction && c.toPort == index)
                c = { c.fromNode, c.fromPort, id, 0 };

            if (c.fromNode == junction && c.fromPort == index)
                c = { id, 0, c.toNode, c.toPort };
        }
    }
}
} // namespace

TrackGraph flattenComponents (const TrackGraph& graph, bool* wireLoop, bool portWidths)
{
    TrackGraph flat;
    flat.connections = graph.connections;

    // Each port becomes a junction: a port of the Component Input or Output, which takes the cables on the component's side too. A
    // component with neither is one that passes nothing.
    std::vector<ObjectId> junctions;

    for (const auto& node : graph.nodes)
    {
        if (node.type != nodetype::component)
        {
            flat.nodes.push_back (node);
            continue;
        }

        ObjectId in = node.id;
        ObjectId out = node.id;
        junctions.push_back (node.id);

        if (node.component != nullptr)
        {
            auto inner = flattenComponents (node.component->graph, wireLoop, portWidths);

            for (auto& innerNode : inner.nodes)
            {
                if (innerNode.type == nodetype::componentInput)
                    in = innerNode.id;
                else if (innerNode.type == nodetype::componentOutput)
                    out = innerNode.id;
                else
                    flat.nodes.push_back (std::move (innerNode));
            }

            for (const auto id : { in, out })
                if (id != node.id)
                    junctions.push_back (id);

            if (node.bypassed)
            {
                std::erase_if (inner.connections, [in, out] (const Connection& c)
                               { return c.fromNode == in || c.fromNode == out || c.toNode == in || c.toNode == out; });

                const auto firstAudio = [] (const std::vector<ComponentPort>& ports)
                { return std::ranges::find (ports, PortType::audio, &ComponentPort::type) - ports.begin(); };
                const auto input = firstAudio (node.component->inputs);
                const auto output = firstAudio (node.component->outputs);

                if (std::cmp_less (input, node.component->inputs.size())
                    && std::cmp_less (output, node.component->outputs.size()))
                    inner.connections.push_back ({ in, static_cast<int> (input), out, static_cast<int> (output) });
            }

            flat.connections.insert (flat.connections.end(), inner.connections.begin(), inner.connections.end());
        }

        for (auto& c : flat.connections)
        {
            if (c.toNode == node.id)
                c.toNode = in;

            if (c.fromNode == node.id)
                c.fromNode = out;
        }

        // Only a port with something inside to cross to has a width: a component without an Input or an Output passes
        // nothing there.
        if (portWidths && node.component != nullptr && in != node.id)
            addPortWidthNodes (flat, node.component->inputs, in, node.id, 0);

        if (portWidths && node.component != nullptr && out != node.id)
            addPortWidthNodes (flat, node.component->outputs, out, out, 1);
    }

    for (const auto junction : junctions)
        dissolveJunction (flat.connections, junction, wireLoop);

    return flat;
}

std::shared_ptr<const CompiledEngine> GraphCompiler::update (const Project& project, const ProcessSpec& spec)
{
    if (m_lastCompiled != nullptr && spec == m_spec && sameStructure (*m_lastCompiled, project)
        && sameLatencyParams (m_registry, *m_lastCompiled, project) && ! bypassMovesLatency (*m_lastCompiled, project))
    {
        applyNonStructural (project);
        m_lastCompiled = std::make_shared<const Project> (project);
        return nullptr;
    }

    return compile (project, spec);
}

bool GraphCompiler::bypassMovesLatency (ObjectId trackId, const TrackGraph& a, const TrackGraph& b, bool voice) const
{
    for (size_t i = 0; i < a.nodes.size(); ++i)
    {
        const auto& x = a.nodes[i];
        const auto& y = b.nodes[i];

        if (x.bypassed != y.bypassed)
        {
            const auto* instance = voice ? findVoiceInstance (trackId, x.id, 0) : findInstance (trackId, x.id);

            if (instance != nullptr && instance->latencySamples() > 0)
                return true;
        }

        if (x.voiceGraph != nullptr && y.voiceGraph != nullptr && x.voiceGraph != y.voiceGraph
            && bypassMovesLatency (trackId, *x.voiceGraph, *y.voiceGraph, true))
            return true;

        if (x.component != nullptr && y.component != nullptr && x.component != y.component
            && bypassMovesLatency (trackId, x.component->graph, y.component->graph, voice))
            return true;
    }

    return false;
}

bool GraphCompiler::bypassMovesLatency (const Project& a, const Project& b) const
{
    const auto moves = [this] (const TrackPtr& x, const TrackPtr& y)
    { return x != y && x != nullptr && y != nullptr && bypassMovesLatency (x->id, x->graph, y->graph, false); };

    if (moves (a.master, b.master))
        return true;

    for (size_t i = 0; i < a.tracks.size(); ++i)
        if (moves (a.tracks[i], b.tracks[i]))
            return true;

    return false;
}

void GraphCompiler::reset()
{
    m_instances.clear();
    m_retired.clear();
    m_sequences.clear();
    m_trackStates.clear();
    m_faustPrograms.clear();
    m_modulationStates.clear();
    m_lastCompiled = nullptr;
}

void GraphCompiler::applyNonStructural (const Project& project)
{
    auto tracks = project.tracks;

    if (project.master != nullptr)
        tracks.push_back (project.master);

    for (const auto& track : tracks)
    {
        storeState (*stateFor (track->id), *track);
        storeSends (*track);

        applyGraphParams (track->id, track->graph);
        publishModulationDepths (track->id, track->graph);

        const Track* old = nullptr;

        if (m_lastCompiled != nullptr)
            old = track == project.master ? m_lastCompiled->master.get() : m_lastCompiled->findTrack (track->id);

        if (old != nullptr)
            publishSampleMaps (project, track->id, track->graph, old->graph, false);
    }

    // Only when the keys changed: the tempo map, which places them, changing recompiles.
    if (m_lastCompiled == nullptr || m_lastCompiled->keys != project.keys)
        publishKey (project);
}

// Every track graph node gets the project's keys (docs/design/scale-highlighting.md, SH6, SH9). Voice graphs hold no
// MIDI effects, so they are left out.
void GraphCompiler::publishKey (const Project& project) const
{
    const auto keys = std::make_shared<const KeyTimeline> (project.keyTimeline());

    for (const auto& track : project.tracks)
        publishKey (track->id, track->graph, keys);

    if (project.master != nullptr)
        publishKey (project.master->id, project.master->graph, keys);
}

void GraphCompiler::publishKey (ObjectId trackId, const TrackGraph& graph,
                                const std::shared_ptr<const KeyTimeline>& keys) const
{
    for (const auto& node : graph.nodes)
    {
        if (auto* instance = findInstance (trackId, node.id))
            instance->setKeys (keys);

        if (node.component != nullptr)
            publishKey (trackId, node.component->graph, keys);
    }
}

// Samplers whose zones changed get them compiled again and handed over, in every voice slot for a voice graph's
// (sameStructure() leaves zones out), and nodes whose pool file changed get it opened. The graphs have the same structure, so their nodes pair up in order.
void GraphCompiler::publishSampleMaps (const Project& project, ObjectId trackId, const TrackGraph& graph,
                                       const TrackGraph& old, bool voice) const
{
    for (size_t i = 0; i < graph.nodes.size() && i < old.nodes.size(); ++i)
    {
        const auto& node = graph.nodes[i];
        const auto& before = old.nodes[i];

        if (nodetype::usesAudioFile (node.type) && node.fileId != before.fileId && ! voice)
            if (auto* instance = findInstance (trackId, node.id))
                instance->setAudioFile (openFile (project, node.fileId));

        // Turning Keep length on or off changes the zones' renders too (PM2, PM4).
        const auto keepsLength = [] (const GraphNode& n)
        {
            const auto mode = n.params.find ("pitchMode");
            return mode != n.params.end() && mode->second >= 0.5;
        };

        if (nodetype::playsSamples (node.type)
            && (node.sampleZones != before.sampleZones || keepsLength (node) != keepsLength (before)))
        {
            const auto map = compileSampleMap (node, project, m_audioResolver, sampleRenderer (node.id));

            if (voice)
            {
                for (int slot = 0; std::cmp_less (slot, VoiceAllocator::numVoices); ++slot)
                    if (auto* instance = findVoiceInstance (trackId, node.id, slot))
                        instance->setSampleMap (map);
            }
            else if (auto* instance = findInstance (trackId, node.id))
            {
                instance->setSampleMap (map);
            }
        }

        if (node.voiceGraph != nullptr && before.voiceGraph != nullptr && node.voiceGraph != before.voiceGraph)
            publishSampleMaps (project, trackId, *node.voiceGraph, *before.voiceGraph, true);

        if (node.component != nullptr && before.component != nullptr && node.component != before.component)
            publishSampleMaps (project, trackId, node.component->graph, before.component->graph, voice);
    }
}

void GraphCompiler::applyGraphParams (ObjectId trackId, const TrackGraph& graph) const
{
    for (const auto& node : graph.nodes)
    {
        if (auto* instance = findInstance (trackId, node.id))
            applyParams (*instance, node);

        if (node.voiceGraph != nullptr)
            applyVoiceParams (trackId, *node.voiceGraph);

        if (node.component != nullptr)
            applyGraphParams (trackId, node.component->graph);

        // The instances a plugin runs per note (docs/design/clap-hosting.md, C10).
        if (node.type == nodetype::lfo || node.type == nodetype::adsr)
            for (int slot = 0; slot < maxNoteSourceVoices; ++slot)
                if (auto* instance = findVoiceInstance (trackId, node.id, slot))
                    applyParams (*instance, node);
    }
}

// Modulation slots' depths go to their shared states, in every voice slot for a voice graph's: a depth drag changes
// nothing else, so it never recompiles. Nodes inside components keep their ids in the flat graph.
void GraphCompiler::publishModulationDepths (ObjectId trackId, const TrackGraph& graph)
{
    for (const auto& node : graph.nodes)
    {
        for (const auto& [port, slot] : node.modulations)
        {
            const auto depth = slot.active ? static_cast<float> (std::clamp (slot.depth, -1.0, 1.0)) : 0.0f;

            for (auto it = m_modulationStates.lower_bound ({ trackId, node.id, port, slot.param, -1 });
                 it != m_modulationStates.end() && it->first.track == trackId && it->first.node == node.id
                 && it->first.port == port && it->first.param == slot.param;
                 ++it)
                it->second->depth.store (depth, std::memory_order_relaxed);
        }

        if (node.voiceGraph != nullptr)
            publishModulationDepths (trackId, *node.voiceGraph);

        if (node.component != nullptr)
            publishModulationDepths (trackId, node.component->graph);
    }
}

// Every voice slot's instance of a voice node gets the model's values (docs/design/phase12-voice-scope.md, V11).
void GraphCompiler::applyVoiceParams (ObjectId trackId, const TrackGraph& graph) const
{
    for (const auto& node : graph.nodes)
    {
        for (int voice = 0; std::cmp_less (voice, VoiceAllocator::numVoices); ++voice)
            if (auto* instance = findVoiceInstance (trackId, node.id, voice))
                applyParams (*instance, node);

        if (node.component != nullptr)
            applyVoiceParams (trackId, node.component->graph);
    }
}

void GraphCompiler::forgetPluginInstance (ObjectId trackId, ObjectId nodeId)
{
    std::erase_if (m_instances,
                   [&] (const auto& entry)
                   {
                       return entry.first.track == trackId && entry.first.node == nodeId
                              && dynamic_cast<PluginNode*> (entry.second.get()) != nullptr;
                   });
}

std::shared_ptr<PluginNode> GraphCompiler::findPluginInstance (ObjectId trackId, ObjectId nodeId) const
{
    for (const auto& [key, node] : m_instances)
        if (key.track == trackId && key.node == nodeId && key.delay == 0 && key.voice < 0)
            return std::dynamic_pointer_cast<PluginNode> (node);

    return nullptr;
}

Node* GraphCompiler::findInstance (ObjectId trackId, ObjectId nodeId) const
{
    for (const auto& [key, node] : m_instances)
        if (key.track == trackId && key.node == nodeId && key.delay == 0 && key.voice < 0)
            return node.get();

    return nullptr;
}

SampleRenderer GraphCompiler::sampleRenderer (ObjectId nodeId) const
{
    if (m_stretch == nullptr)
        return {};

    // The node is told when its renders are ready, as a stretched clip is, and the next compilation plays them.
    return [renderer = m_stretch, nodeId] (const AudioFile& file, juce::int64 start, juce::int64 frames, double scale)
    { return renderer->find (samplerRenderJob (file, start, frames, scale), nodeId); };
}

std::shared_ptr<const AudioSource> GraphCompiler::openFile (const Project& project, ObjectId fileId) const
{
    const auto* file = fileId != 0 ? project.findAudioFile (fileId) : nullptr;
    return file != nullptr && m_audioResolver != nullptr ? m_audioResolver (*file) : nullptr;
}

Node* GraphCompiler::findVoiceInstance (ObjectId trackId, ObjectId nodeId, int voice) const
{
    for (const auto& [key, node] : m_instances)
        if (key.track == trackId && key.node == nodeId && key.delay == 0 && key.voice == voice)
            return node.get();

    return nullptr;
}

std::shared_ptr<Node> GraphCompiler::instance (const InstanceKey& key, const GraphNode* node)
{
    if (const auto used = m_used.find (key); used != m_used.end())
        return used->second;

    std::shared_ptr<Node> result;
    const auto retired = std::ranges::find_if (m_retired, [&] (const auto& entry)
                                               { return ! (entry.first < key) && ! (key < entry.first); });

    if (key.type == nodetype::faust && key.delay == 0 && node != nullptr)
    {
        result = faustInstance (key, *node);
    }
    else if (const auto existing = m_instances.find (key); existing != m_instances.end())
    {
        result = existing->second;
    }
    else if (retired != m_retired.end())
    {
        // A removed plugin coming back (undo): reuse the live instance with all its state. Preparing again
        // resets voices that were cut off when it was removed.
        result = retired->second;
        m_retired.erase (retired);
        result->prepare (m_spec);
    }
    else
    {
        result = key.delay > 0 ? std::shared_ptr<Node> (makeDelayCompensationNode (key.delay))
                               : std::shared_ptr<Node> (m_registry.create (key.type));

        if (result == nullptr)
            return nullptr;

        result->prepare (m_spec);

        if (auto plugin = std::dynamic_pointer_cast<PluginNode> (result);
            plugin != nullptr && node != nullptr && m_pluginLoader != nullptr)
            m_pluginLoader (key.track, *node, plugin);
    }

    if (node != nullptr)
        applyParams (*result, *node);

    m_used[key] = result;
    return result;
}

std::shared_ptr<Node> GraphCompiler::faustInstance (const InstanceKey& key, const GraphNode& node)
{
    // The node runs the newest script that compiled: while a new one compiles or if it fails, the last good one
    // keeps playing (F4). A different program means a new instance that fades from the old one.
    auto& program = m_faustPrograms[{ key.track, key.node }];
    const auto requested = FaustCompiler::shared().request (node.code);

    if (requested.program != nullptr)
        program = requested.program;

    const auto existing = m_instances.find (key);
    auto old = existing != m_instances.end() ? std::dynamic_pointer_cast<FaustNode> (existing->second) : nullptr;

    if (old != nullptr && old->program() == program)
    {
        old->releaseFinishedFade();
        return old;
    }

    auto result = std::make_shared<FaustNode> (program, std::move (old));
    result->prepare (m_spec);
    return result;
}

std::shared_ptr<const TrackSequence> GraphCompiler::sequenceFor (const Track& track)
{
    auto& entry = m_sequences[track.id];

    if (entry.sequence == nullptr || entry.clips != track.clips)
    {
        entry.clips = track.clips;
        // Stacked takes play top first (docs/design/takes-lanes.md, T8).
        entry.sequence = std::make_shared<const TrackSequence> (playedClips (track.clips));
    }

    return entry.sequence;
}

std::shared_ptr<LaneState> GraphCompiler::laneState (ObjectId laneId)
{
    auto& state = m_laneStates[laneId];

    if (state == nullptr)
        state = std::make_shared<LaneState>();

    return state;
}

std::vector<CompiledLane> GraphCompiler::compileLanes (const Track& track, const Project& project)
{
    std::vector<CompiledLane> lanes;

    for (const auto& lane : track.automation)
    {
        if (! lane.active || lane.points.empty())
            continue;

        // A lane on a parameter that follows a link doesn't play: the link's source is what is automated
        // (docs/design/parameter-linking.md, L9).
        if (const auto* link = project.linkDriving ({ track.id, lane.target.nodeId, lane.target.param });
            link != nullptr && link->active
            && (lane.target.kind == AutomationTargetKind::nodeParam
                || lane.target.kind == AutomationTargetKind::pluginParam))
            continue;

        CompiledLane compiled;
        compiled.kind = lane.target.kind;
        compiled.points = lane.points;
        compiled.state = laneState (lane.id);

        if (lane.target.kind == AutomationTargetKind::send)
        {
            // A send's level (R8): the send's place in the track's sends.
            const auto send = std::ranges::find_if (track.sends, [&] (const Send& s)
                                                    { return juce::String (s.id) == lane.target.param; });

            if (send != track.sends.end())
            {
                compiled.send = static_cast<int> (send - track.sends.begin());
                lanes.push_back (std::move (compiled));
            }

            continue;
        }

        if (resolveLaneTarget (track.id, lane.target, compiled))
            lanes.push_back (std::move (compiled));
    }

    return lanes;
}

bool GraphCompiler::resolveLaneTarget (ObjectId trackId, const AutomationTarget& target, CompiledLane& compiled) const
{
    if (target.kind == AutomationTargetKind::gain || target.kind == AutomationTargetKind::pan)
        return true;

    // The node's instance from this compilation (nodes that don't run have one too), or a voice-graph node's
    // instances, one per voice slot (docs/design/phase12-voice-scope.md, V11).
    std::vector<Node*> instances;

    for (const auto& [key, instance] : m_used)
        if (key.track == trackId && key.node == target.nodeId && key.delay == 0)
            instances.push_back (instance.get());

    if (instances.empty())
        return false;

    auto* node = instances.front();

    if (target.kind == AutomationTargetKind::nodeParam)
    {
        const auto index = node->descriptor().indexOfParam (target.param);

        if (index < 0)
            return false;

        compiled.node = node;
        compiled.param = static_cast<size_t> (index);
        compiled.info = node->descriptor().params[compiled.param];
        compiled.voiceNodes.assign (instances.begin() + 1, instances.end());
        return true;
    }

    // Plugins that are still loading get their lanes when they finish (the host recompiles).
    compiled.plugin = dynamic_cast<PluginNode*> (node);
    compiled.pluginParam = compiled.plugin != nullptr ? compiled.plugin->parameterIndex (target.param) : -1;
    return compiled.pluginParam >= 0;
}

void GraphCompiler::forgetRemovedLanes (const Project& project)
{
    std::set<ObjectId> laneIds;
    auto tracks = project.tracks;

    if (project.master != nullptr)
        tracks.push_back (project.master);

    for (const auto& track : tracks)
        for (const auto& lane : track->automation)
            laneIds.insert (lane.id);

    std::erase_if (m_laneStates, [&] (const auto& entry) { return ! laneIds.contains (entry.first); });
}

std::shared_ptr<ModulationState> GraphCompiler::modulationState (const ModulationKey& key, float base)
{
    m_usedModulations.insert (key);
    auto& state = m_modulationStates[key];

    if (state == nullptr)
    {
        state = std::make_shared<ModulationState>();
        state->base.store (base);
    }

    return state;
}

std::shared_ptr<SendState> GraphCompiler::sendState (ObjectId sendId)
{
    auto& state = m_sendStates[sendId];

    if (state == nullptr)
        state = std::make_shared<SendState>();

    return state;
}

void GraphCompiler::storeSends (const Track& track)
{
    for (const auto& send : track.sends)
    {
        auto& state = *sendState (send.id);
        state.gain.store (decibelsToGain (send.levelDb));
        state.active.store (send.active);
        state.preFader.store (send.preFader);
    }
}

std::shared_ptr<TrackState> GraphCompiler::stateFor (ObjectId trackId)
{
    auto& state = m_trackStates[trackId];

    if (state == nullptr)
        state = std::make_shared<TrackState>();

    return state;
}

// The steps of compile(), in order. This is a nested class so that it can reach the compiler's caches, which the
// steps above and below maintain; one is made per compilation and thrown away.
class GraphCompiler::CompileRun
{
public:
    explicit CompileRun (GraphCompiler& compiler)
        : m_compiler (compiler)
    {
    }

    [[nodiscard]] std::shared_ptr<const CompiledEngine> run (const Project& project, const ProcessSpec& spec);

private:
    void prepareSpec (const ProcessSpec& spec);
    [[nodiscard]] PlannedTrack planTrack (const Project& project, const Track& track, CompileState& state);
    [[nodiscard]] std::shared_ptr<const CompiledVoice> compileVoice (const Project& project, ObjectId trackId,
                                                                     const GraphNode& owner);
    void planTracks (const Project& project, CompileState& state);
    void addModulationRestores (CompileState& state);
    void addLinks (const Project& project, CompileState& state);
    void addDelays (const Project& project, CompileState& state);
    void addSends (const Project& project, const CompileState& state, size_t index, CompiledTrack& compiled,
                   const SlotViews& views);
    void buildTracks (const Project& project, CompileState& state);
    static void linkBusReaders (CompiledEngine& engine);
    void finishEngine (const Project& project, CompileState& state);

    GraphCompiler& m_compiler;
};

void GraphCompiler::CompileRun::prepareSpec (const ProcessSpec& spec)
{
    if (spec == m_compiler.m_spec)
        return;

    // Instances are prepared for one spec: a device change starts fresh, except for nodes that are expensive to
    // recreate or carry user state (plugins), which are prepared again. Retired instances are prepared when they
    // come back.
    m_compiler.m_spec = spec;
    std::erase_if (m_compiler.m_instances, [] (const auto& entry) { return ! entry.second->keepAcrossSpecChange(); });

    for (const auto& [key, node] : m_compiler.m_instances)
        node->prepare (spec);
}

PlannedTrack GraphCompiler::CompileRun::planTrack (const Project& project, const Track& track, CompileState& state)
{
    const auto trackId = track.id;
    const InstanceFactory factory =
        [this, trackId] (ObjectId nodeId, const juce::String& type, int delay, const GraphNode* node)
    { return m_compiler.instance ({ trackId, nodeId, type, delay }, node); };
    const ModulationStateFactory modulationState =
        [this, trackId] (ObjectId nodeId, int port, const juce::String& param, float base)
    { return m_compiler.modulationState ({ trackId, nodeId, port, param }, base); };

    const VoiceCompiler voice = [this, trackId, &project] (const GraphNode& owner)
    { return compileVoice (project, trackId, owner); };
    auto flat = flattenComponents (track.graph, nullptr, true);

    if (const auto* macros = macrosOf (track.graph); macros != nullptr)
        wireMacros (flat, macros->id);

    auto result =
        TrackPlanner (
            flat, trackId, m_compiler.m_registry, factory, modulationState, state.eventCount, voice,
            [this, &project] (const GraphNode& node)
            {
                return compileSampleMap (node, project, m_compiler.m_audioResolver,
                                         m_compiler.sampleRenderer (node.id));
            },
            [this, trackId] (const GraphNode& node)
            {
                std::vector<Node*> nodes;
                nodes.reserve (static_cast<size_t> (maxNoteSourceVoices));

                for (int slot = 0; slot < maxNoteSourceVoices; ++slot)
                    nodes.push_back (m_compiler.instance ({ trackId, node.id, node.type, 0, slot }, &node).get());

                return nodes;
            })
            .withFiles ([this, &project] (ObjectId fileId) { return m_compiler.openFile (project, fileId); })
            .withSourceWidths (
                [&project, &track] (const GraphNode& node)
                { return node.type == nodetype::audioPlayer ? playerWidth (track, project, node.fileId) : 0; })
            .run();
    result.sequence = m_compiler.sequenceFor (track);
    result.state = m_compiler.stateFor (trackId);
    result.offset = static_cast<int> (std::llround (
        std::clamp (track.offsetMs, limits::minOffsetMs, limits::maxOffsetMs) * m_compiler.m_spec.sampleRate / 1000.0));
    storeState (*result.state, track);
    m_compiler.storeSends (track);

    if (track.kind == TrackKind::audio)
        result.audio = std::make_shared<const AudioSequence> (
            compileAudio (track, project, m_compiler.m_audioResolver, m_compiler.m_stretch));

    result.lanes = m_compiler.compileLanes (track, project);
    result.connections = flat.connections;

    // A track with output tracks is a header: what reaches its Track Output plays on its Main output track, as one more
    // Output to track, with the latency at the Track Output (docs/design/multi-output-instruments.md, MO12).
    if (const auto* main = mainOutputOf (project, track.id); main != nullptr)
        if (const auto output =
                std::ranges::find (track.graph.nodes, juce::String (nodetype::trackOutput), &GraphNode::type);
            output != track.graph.nodes.end())
            result.busOutputs.push_back ({ output->id, main->id, result.outputSlot, result.latency });

    return result;
}

// A voice graph is planned once per voice slot, each with its own instances, modulation states, buffers and event
// buffers (docs/design/phase12-voice-scope.md, V8, V12). Instances are cached per slot, so they survive recompiles
// (V10).
std::shared_ptr<const CompiledVoice> GraphCompiler::CompileRun::compileVoice (const Project& project, ObjectId trackId,
                                                                              const GraphNode& owner)
{
    auto voice = std::make_shared<CompiledVoice>();

    if (owner.voiceGraph == nullptr)
        return voice;

    const auto blockSize = static_cast<size_t> (m_compiler.m_spec.maxBlockSize);
    std::map<ObjectId, std::shared_ptr<const SampleMap>> sampleMaps;
    const SampleCompiler samples = [this, &project, &sampleMaps] (const GraphNode& node)
    {
        auto& map = sampleMaps[node.id];

        if (map == nullptr)
            map = compileSampleMap (node, project, m_compiler.m_audioResolver, m_compiler.sampleRenderer (node.id));

        return map;
    };

    auto flat = flattenComponents (*owner.voiceGraph, nullptr, true);

    // Macro slots in a voice graph read a follower of the track's Macros device, one per voice (MC8).
    const auto* track = project.findTrackOrMaster (trackId);
    const auto* macros = track != nullptr ? macrosOf (track->graph) : nullptr;
    std::shared_ptr<Node> leader;

    if (macros != nullptr && hasMacroSlots (flat))
    {
        leader = m_compiler.instance ({ trackId, macros->id, macros->type, 0 }, macros);
        flat.nodes.push_back (*macros);
        wireMacros (flat, macros->id);
    }

    for (size_t slot = 0; slot < voice->slots.size(); ++slot)
    {
        const auto index = static_cast<int> (slot);
        const InstanceFactory factory =
            [this, trackId, index] (ObjectId nodeId, const juce::String& type, int delay, const GraphNode* node)
        { return m_compiler.instance ({ trackId, nodeId, type, delay, index }, node); };
        const ModulationStateFactory modulationState =
            [this, trackId, index] (ObjectId nodeId, int port, const juce::String& param, float base)
        { return m_compiler.modulationState ({ trackId, nodeId, port, param, index }, base); };

        if (leader != nullptr)
            if (const auto follower = m_compiler.instance ({ trackId, macros->id, macros->type, 0, index }, macros))
                followMacros (*follower, leader);

        int eventCount = 0;
        const auto plan =
            TrackPlanner (flat, owner.id, m_compiler.m_registry, factory, modulationState, eventCount, nullptr, samples)
                .run();

        auto& compiled = voice->slots[slot];
        compiled.audioMemory = std::make_unique<float[]> ( // NOLINT(modernize-avoid-c-arrays): zero-initialised
            static_cast<size_t> (plan.slotCount) * 2 * blockSize);
        compiled.eventBuffers = std::make_unique<EventBuffer[]> ( // NOLINT(modernize-avoid-c-arrays)
            static_cast<size_t> (eventCount));

        auto program = materialise (plan, SlotViews (compiled.audioMemory.get(), blockSize));
        compiled.steps = std::move (program.steps);
        compiled.output = program.output;
        compiled.inputEvents = plan.inputEvents;

        for (const auto& step : compiled.steps)
            if (step.node != nullptr)
                compiled.nodes.push_back (step.node);

        voice->latencySamples = plan.latency;
    }

    return voice;
}

void GraphCompiler::CompileRun::planTracks (const Project& project, CompileState& state)
{
    state.planned.reserve (project.tracks.size());

    // A frozen track plays its render instead of its clips and graph (docs/design/audio-features.md, AU15).
    for (const auto& track : project.tracks)
        state.planned.push_back (track->freeze.active() ? planTrack (project, frozenView (*track, project), state)
                                                        : planTrack (project, *track, state));

    if (project.master != nullptr)
        state.master = planTrack (project, *project.master, state);

    m_compiler.m_portWidths.clear();

    for (const auto& track : state.planned)
        for (const auto& [node, widths] : track.portWidths)
            m_compiler.m_portWidths[{ track.id, node }] = widths;

    if (state.master.has_value())
        for (const auto& [node, widths] : state.master->portWidths)
            m_compiler.m_portWidths[{ state.master->id, node }] = widths;
}

// A slot that is gone (removed, or pointed at another parameter) leaves its plugin parameter at a modulated value.
// The new engine gets a one-off entry that hands the parameter back at its base (docs/design/modulation.md, M5).
// Built-in parameters need none: applyParams() sets them from the model on every compilation.
void GraphCompiler::CompileRun::addModulationRestores (CompileState& state)
{
    const auto planOf = [&] (ObjectId trackId) -> PlannedTrack*
    {
        for (auto& plan : state.planned)
            if (plan.id == trackId)
                return &plan;

        return state.master.has_value() && state.master->id == trackId ? &*state.master : nullptr;
    };

    for (auto it = m_compiler.m_modulationStates.begin(); it != m_compiler.m_modulationStates.end();)
    {
        const auto& [key, modulationState] = *it;

        if (m_compiler.m_usedModulations.contains (key))
        {
            ++it;
            continue;
        }

        auto* plan = planOf (key.track);

        if (const auto step =
                plan != nullptr ? plan->nodeSteps.find (key.node) : std::map<ObjectId, size_t>::iterator {};
            plan != nullptr && step != plan->nodeSteps.end())
        {
            auto& planned = plan->steps[step->second];

            if (auto* plugin = dynamic_cast<PluginNode*> (planned.node))
            {
                CompiledModulation restore;
                restore.restore = true;
                restore.depth = 0.0f;
                restore.plugin = plugin;
                restore.pluginParam = plugin->parameterIndex (key.param);
                restore.state = modulationState;
                planned.modulations.push_back (std::move (restore));
            }
        }

        it = m_compiler.m_modulationStates.erase (it);
    }
}

// Parameter links (docs/design/parameter-linking.md, L6 to L12): each active link resolved to its ends' instances and
// put on its target's step, or on the target's track when the target has no step of the track's own (a voice graph's
// nodes). Links whose ends aren't there (a plugin still loading, a parameter gone after a rescan) do nothing until a
// later compilation finds them.
void GraphCompiler::CompileRun::addLinks (const Project& project, CompileState& state)
{
    const auto planOf = [&] (ObjectId trackId) -> PlannedTrack*
    {
        for (auto& plan : state.planned)
            if (plan.id == trackId)
                return &plan;

        return state.master.has_value() && state.master->id == trackId ? &*state.master : nullptr;
    };

    // A node's instances: the track's own first, then a voice graph's slots.
    const auto instancesOf = [&] (const ParamAddress& address)
    {
        std::vector<Node*> found;

        for (const auto& [key, instance] : m_compiler.m_used)
            if (key.track == address.trackId && key.node == address.nodeId && key.delay == 0)
                found.insert (key.voice < 0 ? found.begin() : found.end(), instance.get());

        return found;
    };

    // Whether audio goes from one node to another along the track's cables.
    const auto feeds = [] (const PlannedTrack& plan, ObjectId from, ObjectId to)
    {
        std::set<ObjectId> seen { from };
        std::vector<ObjectId> open { from };

        while (! open.empty())
        {
            const auto node = open.back();
            open.pop_back();

            for (const auto& c : plan.connections)
            {
                if (c.fromNode != node)
                    continue;

                if (c.toNode == to)
                    return true;

                if (seen.insert (c.toNode).second)
                    open.push_back (c.toNode);
            }
        }

        return false;
    };

    std::set<ObjectId> used;

    for (const auto& link : project.links)
    {
        if (! link.active)
            continue;

        auto* targetPlan = planOf (link.target.trackId);
        const auto sources = instancesOf (link.source);
        const auto targets = instancesOf (link.target);

        if (targetPlan == nullptr || planOf (link.source.trackId) == nullptr || sources.empty() || targets.empty())
            continue;

        CompiledLink compiled;
        compiled.mapping = link.mapping;

        if (auto* plugin = dynamic_cast<PluginNode*> (sources.front()))
        {
            compiled.sourcePlugin = plugin;
            compiled.sourcePluginParam = plugin->parameterIndex (link.source.param);

            if (compiled.sourcePluginParam < 0)
                continue;
        }
        else
        {
            const auto index = sources.front()->descriptor().indexOfParam (link.source.param);

            if (index < 0)
                continue;

            compiled.sourceNode = sources.front();
            compiled.sourceParam = static_cast<size_t> (index);
            compiled.sourceInfo = sources.front()->descriptor().params[compiled.sourceParam];
        }

        if (auto* plugin = dynamic_cast<PluginNode*> (targets.front()))
        {
            compiled.targetPlugin = plugin;
            compiled.targetPluginParam = plugin->parameterIndex (link.target.param);

            if (compiled.targetPluginParam < 0)
                continue;
        }
        else
        {
            const auto index = targets.front()->descriptor().indexOfParam (link.target.param);

            if (index < 0)
                continue;

            compiled.targetNode = targets.front();
            compiled.targetParam = static_cast<size_t> (index);
            compiled.targetInfo = targets.front()->descriptor().params[compiled.targetParam];
            compiled.targetVoices.assign (targets.begin() + 1, targets.end());
        }

        auto& linkState = m_compiler.m_linkStates[link.id];

        if (linkState == nullptr)
            linkState = std::make_shared<LinkState>();

        used.insert (link.id);
        compiled.state = linkState;
        compiled.crossTrack = link.source.trackId != link.target.trackId;

        if (compiled.crossTrack)
        {
            state.engine->linkSources.push_back ({ compiled.sourceNode, compiled.sourceParam, compiled.sourcePlugin,
                                                   compiled.sourcePluginParam, linkState });
        }
        else
        {
            const auto from = targetPlan->inputLatencies.find (link.source.nodeId);
            const auto to = targetPlan->inputLatencies.find (link.target.nodeId);

            // What reaches the target went through the source this long ago (L8).
            if (from != targetPlan->inputLatencies.end() && to != targetPlan->inputLatencies.end()
                && feeds (*targetPlan, link.source.nodeId, link.target.nodeId))
                compiled.delaySamples = std::max (0, to->second - from->second);
        }

        // On the target's own step when it has one in the track, else on the track.
        const auto step = targetPlan->nodeSteps.find (link.target.nodeId);

        if (step != targetPlan->nodeSteps.end() && compiled.targetVoices.empty()
            && targetPlan->steps[step->second].node == targets.front())
            targetPlan->steps[step->second].links.push_back (std::move (compiled));
        else
            targetPlan->links.push_back (std::move (compiled));
    }

    std::erase_if (m_compiler.m_linkStates, [&] (const auto& entry) { return ! used.contains (entry.first); });
}

// Delays: a track's output to its bus, and each send from the track's own output to its return or key input.
void GraphCompiler::CompileRun::addDelays (const Project& project, CompileState& state)
{
    const auto count = state.planned.size();
    state.delays.slots.resize (count);
    state.delays.busSlots.resize (count);

    const auto addDelay = [&] (PlannedTrack& plan, ObjectId key, int input, int delay)
    {
        const auto delayNode = m_compiler.instance ({ plan.id, key, "master", delay }, nullptr);
        PlannedStep step;
        step.node = delayNode.get();
        step.inputSlots = { input };
        step.outputSlots = { plan.slotCount };
        plan.steps.push_back (step);
        return plan.slotCount++;
    };

    for (size_t i = 0; i < count; ++i)
    {
        auto& plan = state.planned[i];
        const auto tap = plan.outputSlot;
        const auto alignedReach = state.layout.reach[i] - plan.offset;
        const auto delay =
            (state.layout.parentOf[i] >= 0 ? state.layout.arrival[static_cast<size_t> (state.layout.parentOf[i])]
                                           : state.layout.topArrival)
            - alignedReach;

        if (delay > 0 && tap != silenceSlot)
            plan.outputSlot = addDelay (plan, 0, tap, delay);

        const auto& sends = project.tracks[i]->sends;

        for (size_t k = 0; k < sends.size(); ++k)
        {
            // A key meets its node's input: the keyed track's input plus the latency before the node (K5). A source
            // slower than that path leaves the key late; the node is told by how much.
            const auto target = state.layout.sendTargets[i][k];
            const auto keyed = state.layout.keyTargets[i][k];
            auto sendDelay = target >= 0 ? state.layout.arrival[static_cast<size_t> (target)] - alignedReach : 0;

            if (keyed >= 0)
            {
                const auto& input = state.planned[static_cast<size_t> (keyed)].keyInputs.at (sends[k].targetNode);
                sendDelay = state.layout.arrival[static_cast<size_t> (keyed)] + input.latency - alignedReach;
                auto& late = state.delays.late[{ static_cast<size_t> (keyed), sends[k].targetNode }];
                late = std::max (late, -sendDelay);
            }

            state.delays.slots[i].push_back (
                sendDelay > 0 && tap != silenceSlot ? addDelay (plan, sends[k].id, tap, sendDelay) : tap);
        }

        // An Output to track reaches its output track in step with what else arrives there (MO7).
        for (size_t k = 0; k < plan.busOutputs.size(); ++k)
        {
            const auto output = plan.busOutputs[k];
            const auto target = state.layout.outputTargets[i][k];
            const auto outputDelay = target >= 0 ? state.layout.arrival[static_cast<size_t> (target)]
                                                       - (state.layout.arrival[i] + output.latency - plan.offset)
                                                 : 0;
            state.delays.busSlots[i].push_back (outputDelay > 0 && output.slot != silenceSlot
                                                    ? addDelay (plan, output.node, output.slot, outputDelay)
                                                    : output.slot);
        }
    }
}

void GraphCompiler::CompileRun::addSends (const Project& project, const CompileState& state, size_t index,
                                          CompiledTrack& compiled, const SlotViews& views)
{
    const auto& sends = project.tracks[index]->sends;

    for (size_t k = 0; k < sends.size(); ++k)
    {
        const auto target = state.layout.sendTargets[index][k];
        const auto keyed = state.layout.keyTargets[index][k];
        CompiledSend send;
        send.bus = target >= 0 ? state.mix.buses[static_cast<size_t> (target)] : -1;
        send.target = target >= 0 ? state.mix.position[static_cast<size_t> (target)] : -1;

        if (keyed >= 0)
        {
            send.key = true;
            send.bus = state.mix.keyBuses.at ({ static_cast<size_t> (keyed), sends[k].targetNode });
            send.target = state.mix.position[static_cast<size_t> (keyed)];
        }

        send.source = views.input (state.delays.slots[index][k]);
        send.state = m_compiler.sendState (sends[k].id);
        compiled.sends.push_back (std::move (send));
    }

    for (size_t k = 0; k < state.planned[index].busOutputs.size(); ++k)
    {
        const auto target = state.layout.outputTargets[index][k];

        if (target >= 0)
            compiled.busOutputs.push_back ({ state.mix.buses[static_cast<size_t> (target)],
                                             views.input (state.delays.busSlots[index][k]),
                                             state.planned[index].busOutputs[k].node });
    }
}

void GraphCompiler::CompileRun::buildTracks (const Project& project, CompileState& state)
{
    auto& engine = *state.engine;
    const auto blockSize = static_cast<size_t> (engine.spec.maxBlockSize);

    engine.busMemory = std::make_unique<float[]> ( // NOLINT(modernize-avoid-c-arrays): zero-initialised
        static_cast<size_t> (engine.numBuses) * 2 * blockSize);

    const auto busInput = [&] (int bus) -> AudioInput
    {
        const auto* first = engine.busMemory.get() + static_cast<size_t> (bus) * 2 * blockSize;
        return { first, first + blockSize };
    };

    int slotBase = 0;

    for (const auto i : state.mix.order)
    {
        const auto& plan = state.planned[i];

        // Each track's slots are its own arena: no other track reads or writes them.
        const SlotViews views (engine.audioMemory.get() + static_cast<size_t> (slotBase) * 2 * blockSize, blockSize);
        auto compiled = materialise (plan, views);
        applyKeyInputs (compiled, plan, i, state.mix, state.delays, busInput);

        compiled.bus = state.mix.buses[i];
        compiled.outputBus =
            state.layout.parentOf[i] >= 0 ? state.mix.buses[static_cast<size_t> (state.layout.parentOf[i])] : -1;
        compiled.aheadSamples = engine.latencySamples - state.layout.arrival[i];

        for (auto parent = state.layout.parentOf[i]; parent >= 0;
             parent = state.layout.parentOf[static_cast<size_t> (parent)])
            compiled.enclosing.push_back (state.mix.position[static_cast<size_t> (parent)]);

        compiled.inside = insideOf (project, state.layout, state.mix, i);
        compiled.isReturn = project.tracks[i]->kind == TrackKind::returnTrack;
        compiled.header = hasOutputTracks (project, project.tracks[i]->id);

        // An output track is heard when its instrument is soloed (MO6).
        if (const auto source = state.layout.indexOf.find (project.tracks[i]->source);
            project.tracks[i]->kind == TrackKind::output && source != state.layout.indexOf.end())
            compiled.source = state.mix.position[source->second];

        addSends (project, state, i, compiled, views);
        engine.tracks.push_back (std::move (compiled));
        slotBase += plan.slotCount;
    }

    linkBusReaders (engine);
}

// Which track reads each bus, and so which tracks each track waits for (docs/design/phase2-engine.md, 5.6).
void GraphCompiler::CompileRun::linkBusReaders (CompiledEngine& engine)
{
    auto& tracks = engine.tracks;
    engine.busReaders.assign (static_cast<size_t> (engine.numBuses), -1);

    for (size_t j = 0; j < tracks.size(); ++j)
    {
        if (tracks[j].bus >= 0)
            engine.busReaders[static_cast<size_t> (tracks[j].bus)] = static_cast<int> (j);

        for (const auto& send : tracks[j].sends)
            if (send.key && send.bus >= 0 && send.target >= 0)
                engine.busReaders[static_cast<size_t> (send.bus)] = send.target;
    }

    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto feed = [&] (int bus)
        {
            if (bus < 0 || bus >= engine.numBuses)
                return;

            const auto reader = engine.busReaders[static_cast<size_t> (bus)];

            if (reader < 0 || reader == static_cast<int> (i))
                return;

            tracks[i].feeds.push_back (reader);
            tracks[static_cast<size_t> (reader)].fedBy.push_back (static_cast<int> (i));
        };

        feed (tracks[i].outputBus);

        for (const auto& output : tracks[i].busOutputs)
            feed (output.bus);

        for (const auto& send : tracks[i].sends)
            feed (send.bus);
    }

    for (auto& track : tracks)
        for (auto* list : { &track.fedBy, &track.feeds })
        {
            std::ranges::sort (*list);
            list->erase (std::ranges::unique (*list).begin(), list->end());
        }

    engine.waiting = std::make_unique<std::atomic<int>[]> (tracks.size());    // NOLINT(modernize-avoid-c-arrays)
    engine.readyOrder = std::make_unique<std::atomic<int>[]> (tracks.size()); // NOLINT(modernize-avoid-c-arrays)
}

void GraphCompiler::CompileRun::finishEngine (const Project& project, CompileState& state)
{
    auto& engine = *state.engine;
    const auto blockSize = static_cast<size_t> (engine.spec.maxBlockSize);

    if (state.master.has_value())
    {
        // The master's arena comes after every track's.
        int slotBase = 0;

        for (const auto& plan : state.planned)
            slotBase += plan.slotCount;

        engine.master = materialise (
            *state.master,
            SlotViews (engine.audioMemory.get() + static_cast<size_t> (slotBase) * 2 * blockSize, blockSize));
        engine.mixMemory = std::make_unique<float[]> (2 * blockSize); // NOLINT(modernize-avoid-c-arrays)
    }

    for (const auto& [key, node] : m_compiler.m_used)
        engine.nodes.push_back (node);

    // Removed plugins stay alive for a while, so undoing the removal brings back the same instance.
    for (auto& [key, node] : m_compiler.m_instances)
        if (! m_compiler.m_used.contains (key) && node->keepAcrossSpecChange())
            m_compiler.m_retired.emplace_back (key, std::move (node));

    while (m_compiler.m_retired.size() > maxRetiredPlugins)
        m_compiler.m_retired.erase (m_compiler.m_retired.begin());

    m_compiler.m_instances = std::exchange (m_compiler.m_used, {});
    m_compiler.m_lastCompiled = std::make_shared<const Project> (project);

    // Forget caches of tracks that no longer exist.
    std::erase_if (m_compiler.m_sequences, [&] (const auto& entry) { return project.indexOfTrack (entry.first) < 0; });
    std::erase_if (m_compiler.m_trackStates,
                   [&] (const auto& entry) { return project.findTrackOrMaster (entry.first) == nullptr; });

    m_compiler.forgetRemovedLanes (project);

    std::set<ObjectId> sendIds;

    for (const auto& track : project.tracks)
        for (const auto& send : track->sends)
            sendIds.insert (send.id);

    std::erase_if (m_compiler.m_sendStates, [&] (const auto& entry) { return ! sendIds.contains (entry.first); });
    std::erase_if (m_compiler.m_faustPrograms,
                   [&] (const auto& entry)
                   {
                       return std::ranges::none_of (m_compiler.m_instances,
                                                    [&] (const auto& instance)
                                                    {
                                                        return instance.first.track == entry.first.first
                                                               && instance.first.node == entry.first.second
                                                               && instance.first.type == nodetype::faust;
                                                    });
                   });
}

std::shared_ptr<const CompiledEngine> GraphCompiler::CompileRun::run (const Project& project, const ProcessSpec& spec)
{
    prepareSpec (spec);
    m_compiler.m_used.clear();

    // Renders queued for an earlier version that this one no longer needs are dropped (TS9).
    if (m_compiler.m_stretch != nullptr)
        m_compiler.m_stretch->beginRequests();

    const juce::ScopeGuard endRequests { [this]
                                         {
                                             if (m_compiler.m_stretch != nullptr)
                                                 m_compiler.m_stretch->endRequests();
                                         } };
    m_compiler.m_usedModulations.clear();

    CompileState state;
    state.engine = makeEngine (project, spec);

    planTracks (project, state);
    addModulationRestores (state);
    addLinks (project, state);
    state.layout = analyseBuses (project, state.planned);
    state.engine->masterLatencySamples = state.master.has_value() ? state.master->latency : 0;
    state.engine->latencySamples = state.layout.topArrival + state.engine->masterLatencySamples;

    addDelays (project, state);
    allocateSlots (state.planned, state.master, spec, state.eventCount, *state.engine);
    state.mix = mixingOrder (project, state.layout, *state.engine);
    buildTracks (project, state);
    finishEngine (project, state);

    return state.engine;
}

std::shared_ptr<const CompiledEngine> GraphCompiler::compile (const Project& project, const ProcessSpec& spec)
{
    auto compiled = CompileRun (*this).run (project, spec);
    publishKey (project);
    return compiled;
}

} // namespace dawg
