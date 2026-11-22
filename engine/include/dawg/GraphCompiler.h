#pragma once

#include "dawg/CompiledEngine.h"
#include "dawg/FaustNode.h"
#include "dawg/NodeRegistry.h"
#include "dawg/PluginNode.h"
#include "dawg/Sampler.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace dawg
{

/**
    Takes one of a component's ports' ends out of a set of cables: at each port, everything into it goes on to everything out of it.

    @param connections  The cables.
    @param junction     The node whose ports are to go: a Component Input or Output, or a component in cables rewritten to them.
    @param wireLoop     If given, set when wires alone go round in a loop.
*/
void dissolveJunction (std::vector<Connection>& connections, ObjectId junction, bool* wireLoop = nullptr);

/**
    A graph with its components replaced by what's in them, and each cable to a port by cables straight to what's on the other side
    (docs/design/component-nodes.md, N4). So a component costs nothing when it runs, and can't change the sound. A bypassed component's
    first audio input goes straight to its first audio output; what's in it stays in the graph, connected to nothing outside, so that its
    plugins stay loaded.

    With portWidths, each mono or stereo audio port becomes an internal node of that width (nodetype::monoPort,
    nodetype::stereoPort) instead of a junction, so what crosses it takes the port's width (docs/design/mono-ports.md,
    MP11); adaptive ports stay junctions. The compiler asks for this; the structure alone (a cable check) doesn't.

    @param graph       A track's or a voice's graph.
    @param wireLoop    If given, set when wires alone went round in a loop, with no node on the way, which leaves no trace in the result.
    @param portWidths  Whether ports become width nodes.
    @return            The same graph with no components in it.
*/
[[nodiscard]] TrackGraph flattenComponents (const TrackGraph& graph, bool* wireLoop = nullptr, bool portWidths = false);

/**
    The widths of a node's ports after a compilation (docs/design/mono-ports.md, MP8, MP9).
*/
struct PortWidths
{
    std::vector<int> inputs;  ///< Per input port: 1 or 2, 0 for a port that isn't audio.
    std::vector<int> outputs; ///< Per output port: 1 or 2, 0 for a port that isn't audio.
    std::vector<int> folded;  ///< The input ports where a stereo source is folded to mono.

    /**
        Compares all fields.
    */
    [[nodiscard]] bool operator== (const PortWidths&) const noexcept = default;
};

/**
    Turns a Project into a CompiledEngine. See docs/design/phase2-engine.md, section 5.4.

    Node instances are cached by (track, node, type) and reused across compilations, so synth voices and
    effect state survive edits. Parameter-only changes don't need a compilation at all (D8): update() detects
    them and only writes the new values.

    @note  Message thread only.
*/
class GraphCompiler final
{
public:
    /**
        Creates a compiler.

        @param registry  The node types to instantiate from. Must outlive the compiler.
    */
    explicit GraphCompiler (const NodeRegistry& registry = NodeRegistry::builtin());

    /**
        Brings the engine up to date with a project.

        @param project  The current project.
        @param spec     The audio device's sample rate and block size.
        @return         A new engine to publish, or nullptr if only parameters (or other non-structural
                        values such as track gain or names) changed and were applied in place.
    */
    [[nodiscard]] std::shared_ptr<const CompiledEngine> update (const Project& project, const ProcessSpec& spec);

    /**
        Compiles a project unconditionally.

        @param project  The project.
        @param spec     The sample rate and block size to prepare nodes for.
        @return         The compiled engine.
    */
    [[nodiscard]] std::shared_ptr<const CompiledEngine> compile (const Project& project, const ProcessSpec& spec);

    /**
        Starts loading a plugin into a newly created plugin node. The loader instantiates the plugin
        (usually asynchronously), restores node.pluginState, then calls PluginNode::attach() or
        PluginNode::fail() on the message thread, and recompiles if the latency is not zero.

        @param trackId  The track holding the node.
        @param node     The node's model data (plugin reference, state).
        @param target   The node to attach to. Hold it weakly: it may be dropped before loading finishes.
    */
    using PluginLoader =
        std::function<void (ObjectId trackId, const GraphNode& node, const std::shared_ptr<PluginNode>& target)>;

    /**
        Forgets a node's plugin instance, so that the next compile() makes and loads a new one.

        @param trackId  The track.
        @param nodeId   The node.
    */
    void forgetPluginInstance (ObjectId trackId, ObjectId nodeId);

    /**
        Finds a track node's plugin node, sharing ownership.

        @param trackId  The track.
        @param nodeId   The node.
        @return         The node, or nullptr if there is no such plugin node.
    */
    [[nodiscard]] std::shared_ptr<PluginNode> findPluginInstance (ObjectId trackId, ObjectId nodeId) const;

    /**
        Sets the function that loads plugins into new plugin nodes. Without one, plugin nodes stay in the
        loading state (tests attach processors themselves through findInstance()).

        @param loader  The loader, or nullptr.
    */
    void setPluginLoader (PluginLoader loader) { m_pluginLoader = std::move (loader); }

    /**
        Sets how audio clips' files are opened. Without a resolver, audio clips play silence.

        @param resolver  Opens pool files (and caches them; it is called on every compilation).
    */
    void setAudioSourceResolver (AudioSourceResolver resolver) { m_audioResolver = std::move (resolver); }

    /**
        Sets what makes the renders of stretched musical files (docs/design/time-stretching.md, TS7). Without one,
        they play the repitch preview.

        @param renderer  The renderer, or nullptr. Must outlive the compiler's use of it.
    */
    void setStretchRenderer (StretchRenderer* renderer) { m_stretch = renderer; }

    /**
        Most removed plugin instances kept alive for undo (docs/design/phase4-plugin-hosting.md, P3).
    */
    static constexpr size_t maxRetiredPlugins = 16;

    /**
        Forgets all cached instances (including retired plugins) and sequences, so the next update() compiles
        from scratch. Use when a different project is loaded: ids may repeat with different meaning.
    */
    void reset();

    /**
        Makes the next update() compile even if the project's structure is unchanged, e.g. because a Faust script
        finished compiling in the background.
    */
    void invalidate() { m_lastCompiled = nullptr; }

    /**
        Returns every node instance the last compilation uses (for plugin non-realtime mode and re-preparing
        between export passes; docs/design/phase9-export-midi-input.md, X1, X3).
    */
    [[nodiscard]] std::vector<std::shared_ptr<Node>> instances() const
    {
        std::vector<std::shared_ptr<Node>> result;
        result.reserve (m_instances.size());

        for (const auto& [key, node] : m_instances)
            result.push_back (node);

        return result;
    }

    /**
        Returns the shared state of a track (created if needed): gain, mute, solo and the recording settings the
        app writes directly (arm).

        @param trackId  The track.
        @return         Its state.
    */
    [[nodiscard]] std::shared_ptr<TrackState> trackState (ObjectId trackId) { return stateFor (trackId); }

    /**
        Returns the shared state of an automation lane (created if needed): its last played value and whether it
        is being written (docs/design/phase10-automation.md, A4, A6).

        @param laneId  The lane.
        @return        Its state.
    */
    [[nodiscard]] std::shared_ptr<LaneState> laneState (ObjectId laneId);

    /**
        Returns the node instance for a graph node, if the last compilation created one.

        @param trackId  The track id.
        @param nodeId   The node id.
        @return         The instance, or nullptr.
    */
    [[nodiscard]] Node* findInstance (ObjectId trackId, ObjectId nodeId) const;

    /**
        Returns a voice-graph node's instance in one voice slot, if the last compilation created one
        (docs/design/phase12-voice-scope.md, V8).

        @param trackId  The track id.
        @param nodeId   The voice-graph node's id.
        @param voice    The voice slot, 0..VoiceAllocator::numVoices - 1.
        @return         The instance, or nullptr.
    */
    [[nodiscard]] Node* findVoiceInstance (ObjectId trackId, ObjectId nodeId, int voice) const;

    /**
        The widths a track graph's nodes' ports got in the last compilation (docs/design/mono-ports.md, MP8, MP9), by
        (track id, node id): only nodes with a mono port or an input that folds stereo to mono; any other is all stereo.
    */
    [[nodiscard]] const std::map<std::pair<ObjectId, ObjectId>, PortWidths>& portWidths() const noexcept
    {
        return m_portWidths;
    }

private:
    // Whether a node with latency was bypassed or un-bypassed, which changes the delays the compiler worked out. Only called
    // after the projects were found to have the same structure.
    [[nodiscard]] bool bypassMovesLatency (const Project& a, const Project& b) const;
    [[nodiscard]] bool bypassMovesLatency (ObjectId trackId, const TrackGraph& a, const TrackGraph& b,
                                           bool voice) const;

    struct InstanceKey
    {
        ObjectId track;
        ObjectId node;
        juce::String type;
        int delay;      // > 0 for latency-compensation delays (node is then the destination node, port in type)
        int voice = -1; // the voice slot of a voice-graph node, or -1 (docs/design/phase12-voice-scope.md, V8)

        [[nodiscard]] bool operator< (const InstanceKey& other) const
        {
            return std::tie (track, node, type, delay, voice)
                   < std::tie (other.track, other.node, other.type, other.delay, other.voice);
        }
    };

    // A modulation slot: its node, port and target parameter (docs/design/modulation.md, M5).
    struct ModulationKey
    {
        ObjectId track;
        ObjectId node;
        int port;
        juce::String param;
        int voice = -1; // the voice slot of a voice-graph node, or -1

        [[nodiscard]] bool operator< (const ModulationKey& other) const
        {
            return std::tie (track, node, port, param, voice)
                   < std::tie (other.track, other.node, other.port, other.param, other.voice);
        }
    };

    struct SequenceCacheEntry
    {
        std::vector<ClipPtr> clips;
        std::shared_ptr<const TrackSequence> sequence;
    };

    [[nodiscard]] std::shared_ptr<Node> instance (const InstanceKey& key, const GraphNode* node);
    [[nodiscard]] std::shared_ptr<Node> faustInstance (const InstanceKey& key, const GraphNode& node);
    [[nodiscard]] std::shared_ptr<const TrackSequence> sequenceFor (const Track& track);
    [[nodiscard]] std::shared_ptr<TrackState> stateFor (ObjectId trackId);
    [[nodiscard]] std::shared_ptr<SendState> sendState (ObjectId sendId);
    [[nodiscard]] std::shared_ptr<ModulationState> modulationState (const ModulationKey& key, float base);
    void storeSends (const Track& track);
    void applyNonStructural (const Project& project);
    void applyVoiceParams (ObjectId trackId, const TrackGraph& graph) const;
    void applyGraphParams (ObjectId trackId, const TrackGraph& graph) const;
    void publishModulationDepths (ObjectId trackId, const TrackGraph& graph);
    void publishKey (const Project& project) const;
    void publishKey (ObjectId trackId, const TrackGraph& graph, const std::shared_ptr<const KeyTimeline>& keys) const;
    void publishSampleMaps (const Project& project, ObjectId trackId, const TrackGraph& graph, const TrackGraph& old,
                            bool voice) const;
    [[nodiscard]] std::vector<CompiledLane> compileLanes (const Track& track, const Project& project);
    [[nodiscard]] bool resolveLaneTarget (ObjectId trackId, const AutomationTarget& target,
                                          CompiledLane& compiled) const;
    void forgetRemovedLanes (const Project& project);

    class CompileRun; // the steps of compile(), in GraphCompiler.cpp

    const NodeRegistry& m_registry;
    ProcessSpec m_spec;
    std::map<InstanceKey, std::shared_ptr<Node>> m_instances;
    std::map<InstanceKey, std::shared_ptr<Node>> m_used;                  // filled during a compilation
    std::map<std::pair<ObjectId, ObjectId>, PortWidths> m_portWidths;     // see portWidths()
    std::vector<std::pair<InstanceKey, std::shared_ptr<Node>>> m_retired; // removed plugins, oldest first
    PluginLoader m_pluginLoader;
    AudioSourceResolver m_audioResolver;

    // Opens a pool file by id for a node that reads one, or returns null for 0 or a missing file.
    [[nodiscard]] std::shared_ptr<const AudioSource> openFile (const Project& project, ObjectId fileId) const;

    // Renders a Sampler's zones at other pitches through the stretch renderer, for Keep length
    // (docs/design/sampler-mapping.md, PM2); none without a renderer.
    [[nodiscard]] SampleRenderer sampleRenderer (ObjectId nodeId) const;
    StretchRenderer* m_stretch = nullptr;
    std::map<ObjectId, SequenceCacheEntry> m_sequences;
    std::map<ObjectId, std::shared_ptr<TrackState>> m_trackStates;
    std::map<ObjectId, std::shared_ptr<LaneState>> m_laneStates;
    std::map<ObjectId, std::shared_ptr<SendState>> m_sendStates;
    std::map<ObjectId, std::shared_ptr<LinkState>> m_linkStates; // by link id, kept across compilations
    std::map<ModulationKey, std::shared_ptr<ModulationState>> m_modulationStates; // kept across compilations
    std::set<ModulationKey> m_usedModulations;                                    // by the compilation running
    std::map<std::pair<ObjectId, ObjectId>, std::shared_ptr<const FaustProgram>> m_faustPrograms; // last good, by node
    std::shared_ptr<const Project> m_lastCompiled;
};

} // namespace dawg
