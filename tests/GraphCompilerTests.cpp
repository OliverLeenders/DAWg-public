#include "TestUtils.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/PluginNode.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr std::array<PortInfo, 1> stereo { { { PortType::audio, "Audio" } } };

// Emits a single 1.0 sample the first time it runs.
const NodeDescriptor impulseDescriptor { "test.impulse", "Impulse", {}, stereo, {} };

class ImpulseNode final : public Node
{
public:
    ImpulseNode()
        : Node (impulseDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (auto* channel : context.audioOutputs[0])
            std::fill_n (channel, context.numSamples, 0.0f);

        if (! m_fired)
        {
            context.audioOutputs[0][0][0] = 1.0f;
            context.audioOutputs[0][1][0] = 1.0f;
            m_fired = true;
        }
    }

private:
    bool m_fired = false;
};

// Delays audio by a fixed amount and reports it as latency.
constexpr int testLatency = 100;
const NodeDescriptor latencyDescriptor { "test.latency", "Latency", stereo, stereo, {} };

class LatencyNode final : public Node
{
public:
    LatencyNode()
        : Node (latencyDescriptor),
          m_delay (makeDelayCompensationNode (testLatency))
    {
    }

    void prepare (const ProcessSpec& processSpec) override { m_delay->prepare (processSpec); }
    void process (const ProcessContext& context) noexcept override { m_delay->process (context); }
    [[nodiscard]] int latencySamples() const noexcept override { return testLatency; }

private:
    std::unique_ptr<Node> m_delay;
};

[[nodiscard]] NodeRegistry testRegistry()
{
    NodeRegistry registry;
    registerBuiltinNodes (registry);
    registry.add (impulseDescriptor, [] { return std::make_unique<ImpulseNode>(); });
    registry.add (latencyDescriptor, [] { return std::make_unique<LatencyNode>(); });
    return registry;
}

[[nodiscard]] GraphNode node (ObjectId id, const char* type)
{
    GraphNode result;
    result.id = id;
    result.type = type;
    return result;
}

[[nodiscard]] Project projectWithGraph (TrackGraph graph)
{
    Track track;
    track.id = 1;
    track.graph = std::move (graph);

    Project project;
    project.tracks.push_back (std::make_shared<const Track> (std::move (track)));
    project.nextId = 1000;
    return project;
}
} // namespace

TEST_CASE ("the default track compiles to sequencer, synth, EQ, compressor and gain", "[compiler]")
{
    GraphCompiler compiler;
    const auto engine = compiler.compile (apply ({}, "addTrack", {}), spec);

    REQUIRE (engine->tracks.size() == 1);
    CHECK (engine->tracks[0].steps.size() == 5);
    CHECK (engine->numEventBuffers == 1);
}

TEST_CASE ("a long chain reuses two buffers", "[compiler]")
{
    TrackGraph graph;
    graph.nodes.push_back (node (1, "test.impulse"));

    for (ObjectId id = 2; id < 12; ++id)
    {
        graph.nodes.push_back (node (id, nodetype::gain));
        graph.connections.push_back ({ id - 1, 0, id, 0 });
    }

    graph.nodes.push_back (node (99, nodetype::trackOutput));
    graph.connections.push_back ({ 11, 0, 99, 0 });

    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    const auto engine = compiler.compile (projectWithGraph (graph), spec);

    CHECK (engine->tracks[0].steps.size() == 11);
    CHECK (engine->numAudioSlots == 4); // silence + scratch + two ping-pong buffers
}

TEST_CASE ("nodes that do not reach the output are skipped", "[compiler]")
{
    TrackGraph graph;
    graph.nodes = { node (1, "test.impulse"), node (2, nodetype::gain), node (3, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 3, 0 } };

    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    CHECK (compiler.compile (projectWithGraph (graph), spec)->tracks[0].steps.size() == 1);
}

TEST_CASE ("parallel paths with different latency are aligned", "[compiler][latency]")
{
    // impulse -> latency -> output, and impulse -> output directly: both arrive at sample 100.
    TrackGraph graph;
    graph.nodes = { node (1, "test.impulse"), node (2, "test.latency"), node (3, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 1, 0, 3, 0 } };

    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    const auto compiled = compiler.compile (projectWithGraph (graph), spec);
    CHECK (compiled->tracks[0].latencySamples == testLatency);

    AudioEngine engine;
    engine.setEngine (compiled);
    const auto buffer = render (engine, blockSize);

    for (int i = 0; i < blockSize; ++i)
        CHECK_THAT (buffer.left()[static_cast<size_t> (i)], WithinAbs (i == testLatency ? 2.0 : 0.0, 1.0e-6));
}

TEST_CASE ("an Output to track reaches its output track in step with the track", "[compiler][latency][outputs]")
{
    // impulse -> track output, and impulse -> latency -> Output to track: both arrive at sample 100
    // (docs/design/multi-output-instruments.md, MO7). The track is a header, so its Track Output plays through its Main
    // output track (MO12).
    TrackGraph graph;
    graph.nodes = { node (1, "test.impulse"), node (2, "test.latency"), node (3, nodetype::trackOutput),
                    node (4, nodetype::busOutput) };
    graph.nodes[3].target = 2;
    graph.connections = { { 1, 0, 3, 0 }, { 1, 0, 2, 0 }, { 2, 0, 4, 0 } };
    auto project = projectWithGraph (graph);

    Track output;
    output.id = 2;
    output.kind = TrackKind::output;
    output.source = 1;
    output.graph.nodes = { node (10, nodetype::mixInput), node (11, nodetype::trackOutput) };
    output.graph.connections = { { 10, 0, 11, 0 } };
    project.tracks.push_back (std::make_shared<const Track> (std::move (output)));

    Track main;
    main.id = 5;
    main.kind = TrackKind::output;
    main.source = 1;
    main.mainOutput = true;
    main.graph.nodes = { node (12, nodetype::mixInput), node (13, nodetype::trackOutput) };
    main.graph.connections = { { 12, 0, 13, 0 } };
    project.tracks.push_back (std::make_shared<const Track> (std::move (main)));

    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    const auto buffer = render (engine, blockSize);

    for (int i = 0; i < blockSize; ++i)
        CHECK_THAT (buffer.left()[static_cast<size_t> (i)], WithinAbs (i == testLatency ? 2.0 : 0.0, 1.0e-6));
}

TEST_CASE ("node instances survive recompilation", "[compiler]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    const auto synthId = project.tracks[0]->graph.nodes[1].id;

    GraphCompiler compiler;
    std::ignore = compiler.compile (project, spec);
    auto* synth = compiler.findInstance (trackId, synthId);
    REQUIRE (synth != nullptr);

    project = apply (project, "addClip", { { "trackId", trackId }, { "start", 0 }, { "length", ticksPerBeat } });
    CHECK (compiler.update (project, spec) != nullptr); // structural change: recompiled
    CHECK (compiler.findInstance (trackId, synthId) == synth);

    std::ignore = compiler.compile (project, { 44100.0, blockSize }); // a new device spec starts fresh
    CHECK (compiler.findInstance (trackId, synthId) != nullptr);
}

TEST_CASE ("parameter changes are applied without recompiling", "[compiler]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    const auto& nodes = project.tracks[0]->graph.nodes;
    const auto gainId = std::ranges::find_if (nodes, [] (const GraphNode& n) { return n.type == nodetype::gain; })->id;

    GraphCompiler compiler;
    REQUIRE (compiler.update (project, spec) != nullptr);

    project = apply (project, "setNodeParam",
                     { { "trackId", trackId }, { "nodeId", gainId }, { "param", "gainDb" }, { "value", -6.0 } });
    project = apply (project, "setTrackGain", { { "id", trackId }, { "gainDb", -12.0 } });

    CHECK (compiler.update (project, spec) == nullptr);
    CHECK_THAT (compiler.findInstance (trackId, gainId)->params().get (0), WithinAbs (-6.0, 1.0e-6));

    // A timing offset changes the delays, so it cannot be applied in place like a gain.
    project = apply (project, "setTrackOffset", { { "id", trackId }, { "offsetMs", 5.0 } });
    CHECK (compiler.update (project, spec) != nullptr);
}

TEST_CASE ("moving a free curve handle recompiles the automation lane", "[compiler][automation]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    const auto target = makeObject ({ { "kind", "gain" } });
    project = apply (project, "addAutomationLane", { { "trackId", trackId }, { "target", target } });
    const auto laneId = project.tracks[0]->automation[0].id;

    const auto setPoints = [&] (double handleValue)
    {
        using List = juce::Array<juce::var>;
        const auto points = List { List { 0, 0.0, 3, 0, 0.5, handleValue }, List { 1000, 1.0 } };
        project = apply (project, "setAutomationPoints",
                         { { "trackId", trackId }, { "laneId", laneId }, { "points", points } });
    };

    setPoints (0.2);
    GraphCompiler compiler;
    std::ignore = compiler.compile (project, spec);

    setPoints (0.8); // only the handle moves
    CHECK (compiler.update (project, spec) != nullptr);
}

TEST_CASE ("a voice graph compiles once per voice slot, and its latency is the instrument's", "[compiler][voice]")
{
    // Sequencer -> Voice Instrument -> output; inside: Oscillator -> latency -> Voice Output.
    TrackGraph voice;
    voice.nodes = { node (10, nodetype::voiceInput), node (11, nodetype::oscillator), node (12, "test.latency"),
                    node (13, nodetype::voiceOutput) };
    voice.connections = { { 11, 0, 12, 0 }, { 12, 0, 13, 0 } };

    TrackGraph graph;
    graph.nodes = { node (1, nodetype::sequencer), node (2, nodetype::voiceInstrument),
                    node (3, nodetype::trackOutput) };
    graph.nodes[1].voiceGraph = std::make_shared<const TrackGraph> (voice);
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 } };

    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    const auto compiled = compiler.compile (projectWithGraph (graph), spec);

    REQUIRE (compiled->tracks.size() == 1);
    CHECK (compiled->tracks[0].latencySamples == testLatency);

    const auto step =
        std::ranges::find_if (compiled->tracks[0].steps, [] (const CompiledStep& s) { return s.voice != nullptr; });
    REQUIRE (step != compiled->tracks[0].steps.end());
    CHECK (step->voice->latencySamples == testLatency);

    // Each slot has its own instances and memory. Nothing reads the Voice Input, so it gets no event buffer.
    std::set<const Node*> oscillators;

    for (int slot = 0; std::cmp_less (slot, VoiceAllocator::numVoices); ++slot)
    {
        const auto& program = step->voice->slots[static_cast<size_t> (slot)];
        CHECK (program.steps.size() == 2);
        CHECK (program.inputEvents == -1);
        oscillators.insert (compiler.findVoiceInstance (1, 11, slot));
    }

    CHECK (oscillators.size() == VoiceAllocator::numVoices);
    CHECK (step->voice->slots[0].audioMemory != step->voice->slots[1].audioMemory);
    CHECK (compiler.findInstance (1, 11) == nullptr); // voice nodes have no track-scope instance
}

// ---------------------------------------------------------------------------------------------------------
// Mono ports (docs/design/mono-ports.md)

namespace
{
constexpr std::array<PortInfo, 1> mono { { { PortType::audio, "Audio", 1 } } };

// A mono source: 1.0 at the first sample, written in its one channel. It writes 99 into the second channel, which is
// scratch for a mono output, to show that doing so is harmless.
const NodeDescriptor monoSourceDescriptor { "test.monoSource", "Mono source", {}, mono, {} };

class MonoSourceNode final : public Node
{
public:
    MonoSourceNode()
        : Node (monoSourceDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        std::fill_n (context.audioOutputs[0][0], context.numSamples, 0.0f);
        std::fill_n (context.audioOutputs[0][1], context.numSamples, 99.0f);

        if (! m_fired)
            context.audioOutputs[0][0][0] = 1.0f;

        m_fired = true;
    }

private:
    bool m_fired = false;
};

// A stereo source: 1.0 left and 0.5 right at the first sample.
const NodeDescriptor stereoSourceDescriptor { "test.stereoSource", "Stereo source", {}, stereo, {} };

class StereoSourceNode final : public Node
{
public:
    StereoSourceNode()
        : Node (stereoSourceDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (auto* channel : context.audioOutputs[0])
            std::fill_n (channel, context.numSamples, 0.0f);

        if (! m_fired)
        {
            context.audioOutputs[0][0][0] = 1.0f;
            context.audioOutputs[0][1][0] = 0.5f;
        }

        m_fired = true;
    }

private:
    bool m_fired = false;
};

// A mono input and a stereo output: what it hears, on both sides.
const NodeDescriptor monoInputDescriptor { "test.monoInput", "Mono input", mono, stereo, {} };

class MonoInputNode final : public Node
{
public:
    MonoInputNode()
        : Node (monoInputDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (auto* channel : context.audioOutputs[0])
            std::copy_n (context.audioInputs[0][0], context.numSamples, channel);
    }
};

[[nodiscard]] NodeRegistry monoRegistry()
{
    auto registry = testRegistry();
    registry.add (monoSourceDescriptor, [] { return std::make_unique<MonoSourceNode>(); });
    registry.add (stereoSourceDescriptor, [] { return std::make_unique<StereoSourceNode>(); });
    registry.add (monoInputDescriptor, [] { return std::make_unique<MonoInputNode>(); });
    return registry;
}

// The widths of the compiled track's steps that run nodes of a type.
[[nodiscard]] std::vector<int> widthsOf (const CompiledEngine& engine, const char* type)
{
    std::vector<int> widths;

    for (const auto& step : engine.tracks[0].steps)
        if (step.node != nullptr && juce::String (step.node->descriptor().type) == type)
            widths.push_back (step.channels);

    return widths;
}

// One sample of a graph's output, left and right.
[[nodiscard]] std::pair<float, float> sampleOf (const NodeRegistry& registry, const TrackGraph& graph, int at = 0)
{
    GraphCompiler compiler (registry);
    AudioEngine engine;
    engine.setEngine (compiler.compile (projectWithGraph (graph), spec));
    const auto buffer = render (engine, blockSize);
    return { buffer.left()[static_cast<size_t> (at)], buffer.right()[static_cast<size_t> (at)] };
}
} // namespace

TEST_CASE ("a mono source is heard on both sides at full level", "[compiler][mono]")
{
    const auto registry = monoRegistry();
    TrackGraph graph;
    graph.nodes = { node (1, "test.monoSource"), node (2, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 } };

    const auto [left, right] = sampleOf (registry, graph);
    CHECK_THAT (left, WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (right, WithinAbs (1.0, 1.0e-6));
}

TEST_CASE ("adaptive nodes after a mono source run mono", "[compiler][mono]")
{
    const auto registry = monoRegistry();
    TrackGraph graph;
    graph.nodes = { node (1, "test.monoSource"), node (2, nodetype::gain), node (3, nodetype::gain),
                    node (4, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 3, 0, 4, 0 } };

    GraphCompiler compiler (registry);
    CHECK (widthsOf (*compiler.compile (projectWithGraph (graph), spec), nodetype::gain) == std::vector { 1, 1 });

    const auto [left, right] = sampleOf (registry, graph);
    CHECK_THAT (left, WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (right, WithinAbs (1.0, 1.0e-6));

    SECTION ("a stereo source into a node makes it stereo from there on")
    {
        graph.nodes.push_back (node (5, "test.stereoSource"));
        graph.connections.push_back ({ 5, 0, 3, 0 });
        CHECK (widthsOf (*compiler.compile (projectWithGraph (graph), spec), nodetype::gain) == std::vector { 1, 2 });

        const auto [l, r] = sampleOf (registry, graph);
        CHECK_THAT (l, WithinAbs (2.0, 1.0e-6));
        CHECK_THAT (r, WithinAbs (1.5, 1.0e-6));
    }
}

TEST_CASE ("a stereo source into a mono input is folded to its mid", "[compiler][mono]")
{
    const auto registry = monoRegistry();
    TrackGraph graph;
    graph.nodes = { node (1, "test.stereoSource"), node (2, "test.monoInput"), node (3, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 } };

    auto [left, right] = sampleOf (registry, graph);
    CHECK_THAT (left, WithinAbs (0.75, 1.0e-6));
    CHECK_THAT (right, WithinAbs (0.75, 1.0e-6));

    SECTION ("several sources of both widths into a mono input")
    {
        graph.nodes.push_back (node (4, "test.monoSource"));
        graph.connections.push_back ({ 4, 0, 2, 0 });
        std::tie (left, right) = sampleOf (registry, graph);
        CHECK_THAT (left, WithinAbs (1.75, 1.0e-6));
        CHECK_THAT (right, WithinAbs (1.75, 1.0e-6));
    }

    SECTION ("several sources of both widths into a stereo input")
    {
        graph.nodes.push_back (node (4, "test.monoSource"));
        graph.connections = { { 1, 0, 3, 0 }, { 4, 0, 3, 0 } };
        std::tie (left, right) = sampleOf (registry, graph);
        CHECK_THAT (left, WithinAbs (2.0, 1.0e-6));
        CHECK_THAT (right, WithinAbs (1.5, 1.0e-6));
    }
}

TEST_CASE ("the Mix node is mono only when all its inputs are", "[compiler][mono]")
{
    const auto registry = monoRegistry();
    TrackGraph graph;
    graph.nodes = { node (1, "test.monoSource"), node (2, "test.monoSource"), node (3, nodetype::mix),
                    node (4, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 3, 0 }, { 2, 0, 3, 1 }, { 3, 0, 4, 0 } };

    GraphCompiler compiler (registry);
    CHECK (widthsOf (*compiler.compile (projectWithGraph (graph), spec), nodetype::mix) == std::vector { 1 });
    auto [left, right] = sampleOf (registry, graph);
    CHECK_THAT (left, WithinAbs (2.0, 1.0e-6));
    CHECK_THAT (right, WithinAbs (2.0, 1.0e-6));

    graph.nodes.push_back (node (5, "test.stereoSource"));
    graph.connections.push_back ({ 5, 0, 3, 2 });
    CHECK (widthsOf (*compiler.compile (projectWithGraph (graph), spec), nodetype::mix) == std::vector { 2 });
    std::tie (left, right) = sampleOf (registry, graph);
    CHECK_THAT (left, WithinAbs (3.0, 1.0e-6));
    CHECK_THAT (right, WithinAbs (2.5, 1.0e-6));
}

TEST_CASE ("a mono path is delay-compensated in mono", "[compiler][mono][latency]")
{
    // mono source -> latency -> output, and mono source -> gain -> output: both arrive at sample 100.
    const auto registry = monoRegistry();
    TrackGraph graph;
    graph.nodes = { node (1, "test.monoSource"), node (2, "test.latency"), node (3, nodetype::gain),
                    node (4, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 4, 0 }, { 1, 0, 3, 0 }, { 3, 0, 4, 0 } };

    GraphCompiler compiler (registry);
    CHECK (widthsOf (*compiler.compile (projectWithGraph (graph), spec), "dawg.internal.delay") == std::vector { 1 });

    const auto [left, right] = sampleOf (registry, graph, testLatency);
    CHECK_THAT (left, WithinAbs (2.0, 1.0e-6));
    CHECK_THAT (right, WithinAbs (2.0, 1.0e-6));
}

TEST_CASE ("a loop is mono unless something in it is stereo", "[compiler][mono][feedback]")
{
    // mono source -> Mix -> Gain -> output, and Gain -> Feedback -> Mix.
    const auto registry = monoRegistry();
    TrackGraph graph;
    graph.nodes = { node (1, "test.monoSource"), node (2, nodetype::mix), node (3, nodetype::gain),
                    node (4, nodetype::feedback), node (5, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 3, 0, 4, 0 }, { 4, 0, 2, 1 }, { 3, 0, 5, 0 } };

    GraphCompiler compiler (registry);
    auto compiled = compiler.compile (projectWithGraph (graph), spec);
    CHECK (widthsOf (*compiled, nodetype::mix) == std::vector { 1 });
    CHECK (widthsOf (*compiled, nodetype::gain) == std::vector { 1 });
    CHECK (widthsOf (*compiled, nodetype::feedback) == std::vector { 1, 1 });

    graph.nodes.push_back (node (6, "test.stereoSource"));
    graph.connections.push_back ({ 6, 0, 4, 0 });
    compiled = compiler.compile (projectWithGraph (graph), spec);
    CHECK (widthsOf (*compiled, nodetype::mix) == std::vector { 2 });
    CHECK (widthsOf (*compiled, nodetype::gain) == std::vector { 2 });
    CHECK (widthsOf (*compiled, nodetype::feedback) == std::vector { 2, 2 });
}

TEST_CASE ("a stereo graph compiles as it did before mono ports", "[compiler][mono]")
{
    GraphCompiler compiler;
    const auto engine = compiler.compile (apply ({}, "addTrack", {}), spec);

    for (const auto& step : engine->tracks[0].steps)
    {
        CHECK (step.channels == 2);
        CHECK (step.monoOutputs == 0u);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Mono built-in effects (docs/design/mono-ports.md, step 2)

namespace
{
// A mono sine, loud enough for the dynamics and the clipper to act on.
const NodeDescriptor monoSineDescriptor { "test.monoSine", "Mono sine", {}, mono, {} };

class MonoSineNode final : public Node
{
public:
    MonoSineNode()
        : Node (monoSineDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override { m_phase = 0.0; }

    void process (const ProcessContext& context) noexcept override
    {
        for (int s = 0; s < context.numSamples; ++s)
        {
            context.audioOutputs[0][0][s] = static_cast<float> (0.9 * std::sin (m_phase));
            m_phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / sampleRate;
        }
    }

private:
    double m_phase = 0.0;
};

// Stereo in and out: copies, so what follows it hears a mono signal as two equal channels.
const NodeDescriptor widenDescriptor { "test.widen", "Widen", stereo, stereo, {} };

class WidenNode final : public Node
{
public:
    WidenNode()
        : Node (widenDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (size_t ch = 0; ch < 2; ++ch)
            std::copy_n (context.audioInputs[0][ch], context.numSamples, context.audioOutputs[0][ch]);
    }
};

[[nodiscard]] NodeRegistry effectRegistry()
{
    auto registry = monoRegistry();
    registry.add (monoSineDescriptor, [] { return std::make_unique<MonoSineNode>(); });
    registry.add (widenDescriptor, [] { return std::make_unique<WidenNode>(); });
    return registry;
}

// Renders a mono sine through an effect and a Gain, straight or first made stereo, and gives the steps' widths.
struct EffectRun
{
    StereoBuffer buffer { 0 };
    std::vector<int> effectWidths;
    std::vector<int> gainWidths;
};

[[nodiscard]] EffectRun runEffect (const NodeRegistry& registry, const char* type, bool widened)
{
    TrackGraph graph;
    graph.nodes = { node (1, "test.monoSine"), node (2, "test.widen"), node (3, type), node (4, nodetype::gain),
                    node (5, nodetype::trackOutput) };
    graph.connections = { { widened ? ObjectId { 2 } : ObjectId { 1 }, 0, 3, 0 }, { 3, 0, 4, 0 }, { 4, 0, 5, 0 } };

    if (widened)
        graph.connections.push_back ({ 1, 0, 2, 0 });

    GraphCompiler compiler (registry);
    auto compiled = compiler.compile (projectWithGraph (graph), spec);
    EffectRun run { StereoBuffer { 0 }, widthsOf (*compiled, type), widthsOf (*compiled, nodetype::gain) };

    AudioEngine engine;
    engine.setEngine (std::move (compiled));
    run.buffer = render (engine, 8 * blockSize);
    return run;
}

void checkSame (const StereoBuffer& monoRun, const StereoBuffer& stereoRun)
{
    auto worst = 0.0f;
    auto loudest = 0.0f;

    for (size_t i = 0; i < stereoRun.left().size(); ++i)
    {
        worst = std::max ({ worst, std::abs (monoRun.left()[i] - stereoRun.left()[i]),
                            std::abs (monoRun.right()[i] - stereoRun.right()[i]) });
        loudest = std::max (loudest, std::abs (stereoRun.left()[i]));
    }

    CHECK (worst < 1.0e-5f);
    CHECK (loudest > 0.01f); // the effect let something through to compare
}
} // namespace

TEST_CASE ("per-channel effects run mono after a mono source and sound as in stereo", "[compiler][mono]")
{
    const auto registry = effectRegistry();

    for (const auto* type :
         { nodetype::eq, nodetype::filter, nodetype::limiter, nodetype::saturator, nodetype::clipper,
           nodetype::waveshaper, nodetype::bitcrusher, nodetype::compressor, nodetype::gate, nodetype::expander })
    {
        INFO (type);
        const auto monoRun = runEffect (registry, type, false);
        const auto stereoRun = runEffect (registry, type, true);
        CHECK (monoRun.effectWidths == std::vector { 1 });
        CHECK (monoRun.gainWidths == std::vector { 1 });
        CHECK (stereoRun.effectWidths == std::vector { 2 });
        checkSame (monoRun.buffer, stereoRun.buffer);
    }
}

TEST_CASE ("mono-in, stereo-out effects read a mono source and give stereo", "[compiler][mono]")
{
    const auto registry = effectRegistry();

    for (const auto* type : { nodetype::delay, nodetype::utility, nodetype::chorus, nodetype::flanger, nodetype::phaser,
                              nodetype::loopReverb, nodetype::convolutionReverb })
    {
        INFO (type);
        const auto monoRun = runEffect (registry, type, false);
        const auto stereoRun = runEffect (registry, type, true);
        CHECK (monoRun.effectWidths == std::vector { 1 });
        CHECK (monoRun.gainWidths == std::vector { 2 }); // the output is stereo, so what follows is too
        checkSame (monoRun.buffer, stereoRun.buffer);
    }
}

TEST_CASE ("a bypassed mono-in, stereo-out effect passes a mono source to both sides", "[compiler][mono]")
{
    const auto registry = monoRegistry();
    TrackGraph graph;
    auto chorus = node (2, nodetype::chorus);
    chorus.bypassed = true;
    graph.nodes = { node (1, "test.monoSource"), chorus, node (3, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 } };

    const auto [left, right] = sampleOf (registry, graph);
    CHECK_THAT (left, WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (right, WithinAbs (1.0, 1.0e-6));
}

// ---------------------------------------------------------------------------------------------------------
// Mono ports for plugins (docs/design/mono-ports.md, MP4)

namespace
{
// An effect with a main input and output that accepts the main layouts in `accepted` (a bit per mono → mono,
// mono → stereo and stereo → stereo). Each output channel plays the input channel of the same index, or the last one,
// times its gain, which stands for its state.
class LayoutEffect final : public juce::AudioProcessor
{
public:
    explicit LayoutEffect (int accepted)
        : juce::AudioProcessor (BusesProperties()
                                    .withInput ("Input", juce::AudioChannelSet::stereo())
                                    .withOutput ("Output", juce::AudioChannelSet::stereo())),
          m_accepted (accepted)
    {
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto in = layouts.getMainInputChannels();
        const auto out = layouts.getMainOutputChannels();
        const auto bit = in == 1 && out == 1 ? 1 : in == 1 && out == 2 ? 2 : in == 2 && out == 2 ? 4 : 0;
        return (m_accepted & bit) != 0;
    }

    const juce::String getName() const override { return "Layouts"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override { ++prepared; }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        const auto ins = getMainBusNumInputChannels();
        const auto outs = getMainBusNumOutputChannels();

        for (int ch = outs - 1; ch >= 0; --ch)
        {
            buffer.copyFrom (ch, 0, buffer, std::min (ch, ins - 1), 0, buffer.getNumSamples());
            buffer.applyGain (ch, 0, buffer.getNumSamples(), gain);
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int /*index*/) override {}
    const juce::String getProgramName (int /*index*/) override { return {}; }
    void changeProgramName (int /*index*/, const juce::String& /*newName*/) override {}
    void getStateInformation (juce::MemoryBlock& /*destData*/) override {}
    void setStateInformation (const void* /*data*/, int /*sizeInBytes*/) override {}

    float gain = 1.0f;
    int prepared = 0;

private:
    int m_accepted;
};

constexpr int monoToMono = 1;
constexpr int monoToStereo = 2;
constexpr int stereoToStereo = 4;

[[nodiscard]] GraphNode effectSettings (PluginChannels channels)
{
    GraphNode node;
    node.type = nodetype::pluginEffect;
    node.pluginChannels = channels;
    return node;
}

[[nodiscard]] std::unique_ptr<PluginNode> effectWith (int accepted, PluginChannels channels)
{
    auto node = std::make_unique<PluginNode> (false);
    node->configure (effectSettings (channels));
    node->prepare (spec);
    node->attach (std::make_unique<LayoutEffect> (accepted));
    return node;
}

using Layout = PluginNode::MainLayout;

// A track: a source, an effect plugin accepting `accepted`, a Gain, the output.
struct PluginRig
{
    PluginRig (const char* source, int accepted, PluginChannels channels = PluginChannels::automatic)
        : compiler (registry)
    {
        graph.nodes = { node (1, source), node (2, nodetype::pluginEffect), node (3, nodetype::gain),
                        node (4, nodetype::trackOutput) };
        graph.nodes[1].pluginChannels = channels;
        graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 3, 0, 4, 0 } };
        compiler.setPluginLoader ([accepted] (ObjectId, const GraphNode&, const std::shared_ptr<PluginNode>& target)
                                  { target->attach (std::make_unique<LayoutEffect> (accepted)); });
    }

    [[nodiscard]] std::pair<float, float> sample()
    {
        compiled = compiler.compile (projectWithGraph (graph), spec);
        widths = widthsOf (*compiled, nodetype::gain);
        AudioEngine engine;
        engine.setEngine (compiled);
        const auto buffer = render (engine, blockSize);
        return { buffer.left()[0], buffer.right()[0] };
    }

    [[nodiscard]] PluginNode& plugin() const
    {
        return dynamic_cast<PluginNode&> (*compiler.findInstance (projectWithGraph (graph).tracks[0]->id, 2));
    }

    [[nodiscard]] Layout layout() const
    {
        const auto* processor = plugin().processor();
        return { processor->getMainBusNumInputChannels(), processor->getMainBusNumOutputChannels() };
    }

    NodeRegistry registry = monoRegistry();
    TrackGraph graph;
    GraphCompiler compiler;
    std::shared_ptr<const CompiledEngine> compiled;
    std::vector<int> widths;
};
} // namespace

TEST_CASE ("a plugin's main buses take the first layout it accepts for its input", "[plugins][mono]")
{
    const auto all = monoToMono | monoToStereo | stereoToStereo;

    SECTION ("Auto: fed mono, mono → mono first; fed stereo, stereo first")
    {
        const auto node = effectWith (all, PluginChannels::automatic);
        CHECK (node->layoutFor (1) == Layout { 1, 1 });
        CHECK (node->layoutFor (2) == Layout { 2, 2 });
        CHECK (effectWith (monoToStereo | stereoToStereo, PluginChannels::automatic)->layoutFor (1) == Layout { 1, 2 });
        CHECK (effectWith (stereoToStereo, PluginChannels::automatic)->layoutFor (1) == Layout { 2, 2 });
        CHECK (effectWith (monoToMono, PluginChannels::automatic)->layoutFor (2) == Layout { 1, 1 });
        CHECK (effectWith (monoToStereo, PluginChannels::automatic)->layoutFor (2) == Layout { 1, 2 });
    }

    SECTION ("Mono → stereo, else stereo")
    {
        CHECK (effectWith (all, PluginChannels::monoToStereo)->layoutFor (1) == Layout { 1, 2 });
        CHECK (effectWith (all, PluginChannels::monoToStereo)->layoutFor (2) == Layout { 1, 2 });
        CHECK (effectWith (monoToMono | stereoToStereo, PluginChannels::monoToStereo)->layoutFor (1)
               == Layout { 2, 2 });
    }

    SECTION ("Stereo only")
    {
        CHECK (effectWith (all, PluginChannels::stereo)->layoutFor (1) == Layout { 2, 2 });
    }

    SECTION ("nothing accepted keeps the layout it was attached with")
    {
        CHECK (effectWith (0, PluginChannels::automatic)->layoutFor (1) == Layout { 2, 2 });
    }

    SECTION ("while loading, the width it is fed")
    {
        PluginNode loading (false);
        CHECK (loading.layoutFor (1) == Layout { 1, 1 });
        CHECK (loading.outputWidth (0, 1, 1u) == 1);
    }
}

TEST_CASE ("a mono-fed plugin runs mono to mono, and what follows it runs mono", "[plugins][mono][compiler]")
{
    PluginRig rig ("test.monoSource", monoToMono | stereoToStereo);
    const auto [left, right] = rig.sample();
    CHECK (rig.layout() == Layout { 1, 1 });
    CHECK (rig.widths == std::vector { 1 });
    CHECK_THAT (left, WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (right, WithinAbs (1.0, 1.0e-6));

    SECTION ("a stereo source instead takes it to stereo, keeping its state")
    {
        auto& processor = dynamic_cast<LayoutEffect&> (*rig.plugin().processor());
        processor.gain = 0.5f;
        const auto prepared = processor.prepared;
        rig.graph.nodes[0] = node (1, "test.stereoSource");
        const auto [l, r] = rig.sample();
        CHECK (rig.layout() == Layout { 2, 2 });
        CHECK (rig.widths == std::vector { 2 });
        CHECK (processor.prepared == prepared + 1);
        CHECK_THAT (l, WithinAbs (0.5, 1.0e-6));
        CHECK_THAT (r, WithinAbs (0.25, 1.0e-6));
    }
}

TEST_CASE ("a mono-fed plugin that gives stereo makes what follows stereo", "[plugins][mono][compiler]")
{
    PluginRig rig ("test.monoSource", monoToStereo | stereoToStereo);
    std::ignore = rig.sample();
    CHECK (rig.layout() == Layout { 1, 2 });
    CHECK (rig.widths == std::vector { 2 });
}

TEST_CASE ("a mono-only plugin fed stereo hears both channels folded", "[plugins][mono][compiler]")
{
    PluginRig rig ("test.stereoSource", monoToMono);
    const auto [left, right] = rig.sample();
    CHECK (rig.layout() == Layout { 1, 1 });
    CHECK_THAT (left, WithinAbs (0.75, 1.0e-6));
    CHECK_THAT (right, WithinAbs (0.75, 1.0e-6));
}

TEST_CASE ("the Channels setting chooses a plugin's layouts", "[plugins][mono][compiler]")
{
    PluginRig rig ("test.monoSource", monoToMono | monoToStereo | stereoToStereo, PluginChannels::stereo);
    std::ignore = rig.sample();
    CHECK (rig.layout() == Layout { 2, 2 });
    CHECK (rig.widths == std::vector { 2 });

    rig.graph.nodes[1].pluginChannels = PluginChannels::monoToStereo;
    std::ignore = rig.sample();
    CHECK (rig.layout() == Layout { 1, 2 });

    const auto project =
        apply (projectWithGraph (rig.graph), "setPluginChannels",
               { { "trackId", projectWithGraph (rig.graph).tracks[0]->id }, { "nodeId", 2 }, { "channels", "auto" } });
    CHECK (project.tracks[0]->graph.nodes[1].pluginChannels == PluginChannels::automatic);

    // Saved when not Auto (MP12). The file knows only built-in nodes.
    auto saved = rig.graph;
    saved.nodes[0] = node (1, nodetype::gain);
    saved.nodes[1].plugin = PluginRef { "test.layouts", "Layouts", "DAWg", "VST3" };
    const juce::TemporaryFile temporary (juce::String ("PluginChannels") + projectfile::extension);
    REQUIRE (projectfile::write (projectWithGraph (saved), { "id", "v" }, temporary.getFile(), false).isEmpty());
    CHECK (projectfile::read (temporary.getFile()).project.tracks[0]->graph.nodes[1].pluginChannels
           == PluginChannels::monoToStereo);
}

TEST_CASE ("an instrument's mono output buses are mono ports", "[plugins][mono]")
{
    // MultiOutputTests' instrument has a stereo Main, a stereo Kick and a mono Hat; here only the widths matter.
    class MonoBuses final : public juce::AudioProcessor
    {
    public:
        MonoBuses()
            : juce::AudioProcessor (BusesProperties()
                                        .withOutput ("Main", juce::AudioChannelSet::mono())
                                        .withOutput ("Kick", juce::AudioChannelSet::stereo())
                                        .withOutput ("Hat", juce::AudioChannelSet::mono()))
        {
        }

        bool isBusesLayoutSupported (const BusesLayout& /*layouts*/) const override { return true; }
        const juce::String getName() const override { return "Buses"; }
        void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override {}
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>& /*buffer*/, juce::MidiBuffer& /*midi*/) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return true; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int /*index*/) override {}
        const juce::String getProgramName (int /*index*/) override { return {}; }
        void changeProgramName (int /*index*/, const juce::String& /*newName*/) override {}
        void getStateInformation (juce::MemoryBlock& /*destData*/) override {}
        void setStateInformation (const void* /*data*/, int /*sizeInBytes*/) override {}
    };

    PluginNode node (true);
    node.prepare (spec);
    node.attach (std::make_unique<MonoBuses>());

    CHECK (node.outputWidth (2, 2, 1u | 4u) == 1); // Hat
    CHECK (node.outputWidth (1, 2, 1u | 2u) == 2); // Kick
    CHECK (node.outputWidth (0, 2, 1u | 2u) == 1); // Main, with Kick on its own port
    CHECK (node.outputWidth (0, 2, 1u) == 2);      // Main, with Kick playing in it
}

TEST_CASE ("the compiler reports the ports that are mono and the inputs that fold", "[compiler][mono]")
{
    // docs/design/mono-ports.md, MP8, MP9: stereo source -> mono input (folds) -> Gain (mono) -> output.
    const auto registry = monoRegistry();
    TrackGraph graph;
    graph.nodes = { node (1, "test.stereoSource"), node (2, "test.monoInput"), node (3, nodetype::gain),
                    node (4, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 }, { 3, 0, 4, 0 } };

    GraphCompiler compiler (registry);
    const auto project = projectWithGraph (graph);
    std::ignore = compiler.compile (project, spec);
    const auto trackId = project.tracks[0]->id;
    const auto& widths = compiler.portWidths();

    // test.monoInput gives stereo, so the Gain after it is stereo too and isn't listed; neither is the source.
    REQUIRE (widths.contains ({ trackId, 2 }));
    CHECK (widths.at ({ trackId, 2 }).inputs == std::vector { 1 });
    CHECK (widths.at ({ trackId, 2 }).outputs == std::vector { 2 });
    CHECK (widths.at ({ trackId, 2 }).folded == std::vector { 0 });
    CHECK (! widths.contains ({ trackId, 1 }));
    CHECK (! widths.contains ({ trackId, 3 }));

    // A mono source into the Gain lists it, with its modulation inputs as 0.
    graph.connections = { { 5, 0, 3, 0 }, { 3, 0, 4, 0 } };
    graph.nodes.push_back (node (5, "test.monoSource"));
    std::ignore = compiler.compile (projectWithGraph (graph), spec);
    REQUIRE (compiler.portWidths().contains ({ trackId, 3 }));
    const auto& gain = compiler.portWidths().at ({ trackId, 3 });
    CHECK (gain.inputs.front() == 1);
    CHECK (std::ranges::all_of (gain.inputs | std::views::drop (1), [] (int w) { return w == 0; }));
    CHECK (gain.outputs == std::vector { 1 });
    CHECK (gain.folded.empty());
}

// ---------------------------------------------------------------------------------------------------------
// Component ports with a width (docs/design/mono-ports.md, MP11)

namespace
{
// A component holding In -> Gain -> Out, with one audio port each way of the given widths, fed by `source`.
[[nodiscard]] TrackGraph componentGraph (const char* source, int inputWidth, int outputWidth)
{
    auto inside = std::make_shared<ComponentGraph>();
    inside->graph.nodes = { node (10, nodetype::componentInput), node (11, nodetype::gain),
                            node (12, nodetype::componentOutput) };
    inside->graph.connections = { { 10, 0, 11, 0 }, { 11, 0, 12, 0 } };
    inside->inputs = { { PortType::audio, "In", inputWidth } };
    inside->outputs = { { PortType::audio, "Out", outputWidth } };

    auto box = node (2, nodetype::component);
    box.component = inside;
    TrackGraph graph;
    graph.nodes = { node (1, source), box, node (3, nodetype::trackOutput) };
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 } };
    return graph;
}
} // namespace

TEST_CASE ("a component's mono, stereo and adaptive ports", "[compiler][mono][component]")
{
    const auto registry = monoRegistry();
    GraphCompiler compiler (registry);

    SECTION ("a mono input folds a stereo source, and what is inside runs mono")
    {
        const auto graph = componentGraph ("test.stereoSource", 1, 0);
        const auto project = projectWithGraph (graph);
        CHECK (widthsOf (*compiler.compile (project, spec), nodetype::gain) == std::vector { 1 });

        const auto [left, right] = sampleOf (registry, graph);
        CHECK_THAT (left, WithinAbs (0.75, 1.0e-6));
        CHECK_THAT (right, WithinAbs (0.75, 1.0e-6));

        // The graph view shows the fold on the component's input.
        const auto& widths = compiler.portWidths().at ({ project.tracks[0]->id, 2 });
        CHECK (widths.inputs == std::vector { 1 });
        CHECK (widths.folded == std::vector { 0 });
    }

    SECTION ("an adaptive input takes its source's width")
    {
        CHECK (widthsOf (*compiler.compile (projectWithGraph (componentGraph ("test.monoSource", 0, 0)), spec),
                         nodetype::gain)
               == std::vector { 1 });
        CHECK (widthsOf (*compiler.compile (projectWithGraph (componentGraph ("test.stereoSource", 0, 0)), spec),
                         nodetype::gain)
               == std::vector { 2 });
    }

    SECTION ("a stereo input makes a mono source stereo inside, at full level")
    {
        const auto graph = componentGraph ("test.monoSource", 2, 0);
        CHECK (widthsOf (*compiler.compile (projectWithGraph (graph), spec), nodetype::gain) == std::vector { 2 });
        const auto [left, right] = sampleOf (registry, graph);
        CHECK_THAT (left, WithinAbs (1.0, 1.0e-6));
        CHECK_THAT (right, WithinAbs (1.0, 1.0e-6));
    }

    SECTION ("a mono output folds what is inside, shown on the Output node")
    {
        const auto graph = componentGraph ("test.stereoSource", 0, 1);
        const auto project = projectWithGraph (graph);
        std::ignore = compiler.compile (project, spec);
        const auto& widths = compiler.portWidths().at ({ project.tracks[0]->id, 12 });
        CHECK (widths.inputs == std::vector { 1 });
        CHECK (widths.folded == std::vector { 0 });

        const auto [left, right] = sampleOf (registry, graph);
        CHECK_THAT (left, WithinAbs (0.75, 1.0e-6));
        CHECK_THAT (right, WithinAbs (0.75, 1.0e-6));
    }
}

TEST_CASE ("a component port's width is set by a command and saved when not stereo", "[mono][component][commands]")
{
    auto graph = componentGraph ("test.monoSource", 2, 2);
    graph.nodes[0] = node (1, nodetype::gain); // the file knows only built-in nodes
    auto project = projectWithGraph (graph);
    const auto trackId = project.tracks[0]->id;
    const auto set = [&] (const char* side, int channels)
    {
        project = apply (
            project, "setComponentPortChannels",
            { { "trackId", trackId }, { "nodeId", 2 }, { "side", side }, { "index", 0 }, { "channels", channels } });
    };

    set ("input", 1);
    set ("output", 0);
    CHECK_THROWS (set ("input", 3));
    const auto& component = *project.tracks[0]->graph.nodes[1].component;
    CHECK (component.inputs[0].channels == 1);
    CHECK (component.outputs[0].channels == 0);

    const juce::TemporaryFile temporary (juce::String ("PortWidths") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "v" }, temporary.getFile(), false).isEmpty());
    const auto loaded = projectfile::read (temporary.getFile()).project;
    const auto& reopened = *loaded.tracks[0]->graph.nodes[1].component;
    CHECK (reopened.inputs[0].channels == 1);
    CHECK (reopened.outputs[0].channels == 0);
}
