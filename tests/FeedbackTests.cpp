#include "TestUtils.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/CompiledEngine.h"
#include "dawg/Export.h"
#include "dawg/GraphCompiler.h"
#include "dawg/Modulation.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <functional>
#include <vector>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr std::array<PortInfo, 1> impulseOutput { { { PortType::audio, "Audio" } } };
const NodeDescriptor impulseNodeDescriptor { "test.impulse", "Impulse", {}, impulseOutput, {} };

// Emits a single 1.0 sample the first time it runs.
class OneImpulse final : public Node
{
public:
    OneImpulse()
        : Node (impulseNodeDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (size_t ch = 0; ch < 2; ++ch)
        {
            std::fill_n (context.audioOutputs[0][ch], context.numSamples, 0.0f);

            if (! m_fired)
                context.audioOutputs[0][ch][0] = 1.0f;
        }

        m_fired = true;
    }

private:
    bool m_fired = false;
};

constexpr ObjectId impulse = 10;
constexpr ObjectId mix = 11;
constexpr ObjectId feedback = 12;
constexpr ObjectId output = 13;

[[nodiscard]] GraphNode graphNode (ObjectId id, const char* type, std::map<juce::String, double> params = {})
{
    GraphNode result;
    result.id = id;
    result.type = type;
    result.params = std::move (params);
    return result;
}

[[nodiscard]] NodeRegistry feedbackRegistry()
{
    NodeRegistry registry;
    registerBuiltinNodes (registry);
    registry.add (impulseNodeDescriptor, [] { return std::make_unique<OneImpulse>(); });
    return registry;
}

[[nodiscard]] Project projectOf (TrackGraph graph)
{
    Track track;
    track.id = 1;
    track.graph = std::move (graph);

    Project project;
    project.tracks.push_back (std::make_shared<const Track> (std::move (track)));
    return project;
}

// Impulse -> Mix -> Feedback -> Output, and Feedback back to the Mix.
[[nodiscard]] TrackGraph echoGraph (double timeMs, double amount)
{
    TrackGraph graph;
    graph.nodes = { graphNode (impulse, "test.impulse"), graphNode (mix, nodetype::mix),
                    graphNode (feedback, nodetype::feedback, { { "time", timeMs }, { "amount", amount } }),
                    graphNode (output, nodetype::trackOutput) };
    graph.connections = {
        { impulse, 0, mix, 0 }, { mix, 0, feedback, 0 }, { feedback, 0, mix, 1 }, { feedback, 0, output, 0 }
    };
    return graph;
}

[[nodiscard]] std::vector<float> renderInBlocks (const TrackGraph& graph, int block, int total)
{
    const auto registry = feedbackRegistry();
    GraphCompiler compiler (registry);
    AudioEngine engine;
    engine.setEngine (compiler.compile (projectOf (graph), { sampleRate, block }));
    std::vector<float> result;

    while (static_cast<int> (result.size()) < total)
    {
        const auto buffer = render (engine, block);
        result.insert (result.end(), buffer.left().begin(), buffer.left().end());
    }

    result.resize (static_cast<size_t> (total));
    return result;
}

[[nodiscard]] size_t regionsIn (const std::vector<CompiledStep>& steps)
{
    return static_cast<size_t> (
        std::ranges::count_if (steps, [] (const CompiledStep& s) { return s.regionSteps > 0; }));
}
} // namespace

TEST_CASE ("a cable can only close a loop through a Feedback node", "[feedback]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;

    const auto add = [&] (const char* type)
    {
        project = apply (project, "addNode", { { "trackId", trackId }, { "type", type } });
        return project.tracks[0]->graph.nodes.back().id;
    };
    const auto connect = [&] (ObjectId from, ObjectId to)
    {
        project = apply (
            project, "connect",
            { { "trackId", trackId }, { "fromNode", from }, { "fromPort", 0 }, { "toNode", to }, { "toPort", 0 } });
    };

    const auto first = add (nodetype::gain);
    const auto second = add (nodetype::gain);
    const auto back = add (nodetype::feedback);

    connect (first, second);
    CHECK_THROWS_AS (connect (second, first), CommandError);
    CHECK_THROWS_AS (connect (first, first), CommandError);

    // Through the Feedback node it closes, whichever cable is the last.
    connect (second, back);
    connect (back, first);

    // And a node's self loop is one too.
    const auto alone = add (nodetype::feedback);
    connect (alone, alone);
}

TEST_CASE ("an echo comes back at the loop's length, scaled each time", "[feedback]")
{
    const auto left = renderInBlocks (echoGraph (10.0, 50.0), 128, 2400);

    // Everything else is silent: 10 ms is 480 samples.
    CHECK_THAT (left[480], WithinAbs (0.5, 2.0e-3));
    CHECK_THAT (left[960], WithinAbs (0.25, 2.0e-3));
    CHECK_THAT (left[1440], WithinAbs (0.125, 2.0e-3));
    CHECK_THAT (left[479], WithinAbs (0.0, 1.0e-6));
    CHECK_THAT (left[481], WithinAbs (0.0, 2.0e-3));
}

TEST_CASE ("a loop can't be shorter than its shortest length", "[feedback]")
{
    const auto left = renderInBlocks (echoGraph (0.1, 50.0), 128, 200);
    constexpr size_t shortest = loopChunkSamples + 2;

    CHECK_THAT (left[shortest], WithinAbs (0.5, 2.0e-3));
    CHECK_THAT (left[shortest * 2], WithinAbs (0.25, 2.0e-3));
}

TEST_CASE ("a loop sounds the same at any buffer size", "[feedback]")
{
    // With real nodes in the loop, and something taking its output after the region.
    auto graph = echoGraph (3.0, 90.0);
    constexpr ObjectId filter = 20;
    constexpr ObjectId saturator = 21;
    constexpr ObjectId after = 22;
    graph.nodes.push_back (graphNode (filter, nodetype::filter, { { "cutoff", 3000.0 } }));
    graph.nodes.push_back (graphNode (saturator, nodetype::saturator));
    graph.nodes.push_back (graphNode (after, nodetype::gain));
    graph.connections = { { impulse, 0, mix, 0 },        { mix, 0, filter, 0 },   { filter, 0, saturator, 0 },
                          { saturator, 0, feedback, 0 }, { feedback, 0, mix, 1 }, { saturator, 0, after, 0 },
                          { after, 0, output, 0 } };

    constexpr int total = 9600;
    const auto reference = renderInBlocks (graph, 1024, total);
    CHECK (*std::ranges::max_element (reference) > 0.01f);

    for (const auto block : { 32, 128, 480 })
    {
        const auto left = renderInBlocks (graph, block, total);
        float largest = 0.0f;

        for (size_t i = 0; i < left.size(); ++i)
            largest = std::max (largest, std::abs (left[i] - reference[i]));

        INFO ("block " << block);
        CHECK (largest < 1.0e-5f);
    }
}

TEST_CASE ("loops compile into regions", "[feedback]")
{
    const auto registry = feedbackRegistry();
    const auto compile = [&] (const TrackGraph& graph)
    {
        GraphCompiler compiler (registry);
        return compiler.compile (projectOf (graph), spec);
    };

    SECTION ("a loop is one region, with the Feedback node's two steps")
    {
        const auto engine = compile (echoGraph (10.0, 50.0));
        const auto& steps = engine->tracks[0].steps;
        CHECK (regionsIn (steps) == 1);
        CHECK (steps[1].regionSteps == 3); // read, mix, write; the impulse runs first
        CHECK (steps[1].loopPart == LoopPart::read);
        CHECK (steps[3].loopPart == LoopPart::write);
    }

    SECTION ("two loops sharing a node are one region")
    {
        auto graph = echoGraph (10.0, 50.0);
        constexpr ObjectId another = 30;
        graph.nodes.push_back (graphNode (another, nodetype::feedback));
        graph.connections.push_back ({ mix, 0, another, 0 });
        graph.connections.push_back ({ another, 0, mix, 2 });
        CHECK (regionsIn (compile (graph)->tracks[0].steps) == 1);
    }

    SECTION ("two separate loops are two")
    {
        auto graph = echoGraph (10.0, 50.0);
        constexpr ObjectId mixB = 30;
        constexpr ObjectId feedbackB = 31;
        graph.nodes.push_back (graphNode (mixB, nodetype::mix));
        graph.nodes.push_back (graphNode (feedbackB, nodetype::feedback));
        graph.connections = { { impulse, 0, mix, 0 },     { mix, 0, feedback, 0 },   { feedback, 0, mix, 1 },
                              { feedback, 0, mixB, 0 },   { mixB, 0, feedbackB, 0 }, { feedbackB, 0, mixB, 1 },
                              { feedbackB, 0, output, 0 } };
        CHECK (regionsIn (compile (graph)->tracks[0].steps) == 2);
    }

    SECTION ("a Feedback node with no way back is a plain delay")
    {
        TrackGraph graph;
        graph.nodes = { graphNode (impulse, "test.impulse"),
                        graphNode (feedback, nodetype::feedback, { { "time", 1.0 }, { "amount", 100.0 } }),
                        graphNode (output, nodetype::trackOutput) };
        graph.connections = { { impulse, 0, feedback, 0 }, { feedback, 0, output, 0 } };
        CHECK (regionsIn (compile (graph)->tracks[0].steps) == 0);

        const auto left = renderInBlocks (graph, 128, 200);
        CHECK_THAT (left[48], WithinAbs (1.0, 2.0e-3));
    }

    SECTION ("a loop in a voice graph compiles per voice")
    {
        TrackGraph voice;
        constexpr ObjectId voiceIn = 40;
        constexpr ObjectId oscillator = 41;
        constexpr ObjectId voiceOut = 42;
        voice.nodes = { graphNode (voiceIn, nodetype::voiceInput), graphNode (oscillator, nodetype::oscillator),
                        graphNode (mix, nodetype::mix), graphNode (feedback, nodetype::feedback),
                        graphNode (voiceOut, nodetype::voiceOutput) };
        voice.connections = {
            { oscillator, 0, mix, 0 }, { mix, 0, feedback, 0 }, { feedback, 0, mix, 1 }, { feedback, 0, voiceOut, 0 }
        };

        TrackGraph graph;
        graph.nodes = { graphNode (1, nodetype::sequencer), graphNode (2, nodetype::voiceInstrument),
                        graphNode (3, nodetype::trackOutput) };
        graph.nodes[1].voiceGraph = std::make_shared<const TrackGraph> (voice);
        graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 } };

        const auto engine = compile (graph);
        const auto step =
            std::ranges::find_if (engine->tracks[0].steps, [] (const CompiledStep& s) { return s.voice != nullptr; });
        REQUIRE (step != engine->tracks[0].steps.end());
        CHECK (regionsIn (step->voice->slots[0].steps) == 1);
        CHECK (regionsIn (step->voice->slots[15].steps) == 1);
    }
}

namespace
{
constexpr std::array<ParamInfo, 0> noParams {};
const NodeDescriptor boostDescriptor { "test.boost", "Boost", impulseOutput, impulseOutput, noParams };
const NodeDescriptor scriptedDescriptor { "test.scripted", "Scripted", {}, impulseOutput, noParams };

// +24 dB, or so.
class Boost final : public Node
{
public:
    Boost()
        : Node (boostDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (size_t ch = 0; ch < 2; ++ch)
            for (int i = 0; i < context.numSamples; ++i)
                context.audioOutputs[0][ch][i] = context.audioInputs[0][ch][i] * 16.0f;
    }
};

// One sample of 1.0 at the first call, of NaN at sample 4800, and of 1.0 again at sample 24000.
class Scripted final : public Node
{
public:
    Scripted()
        : Node (scriptedDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (size_t ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < context.numSamples; ++i)
            {
                const auto at = m_position + i;
                auto value = at == 0 || at == 24000 ? 1.0f : 0.0f;

                if (at == 4800)
                    value = std::numeric_limits<float>::quiet_NaN();

                context.audioOutputs[0][ch][i] = value;
            }
        }

        m_position += context.numSamples;
    }

private:
    int m_position = 0;
};

constexpr ObjectId boost = 50;

[[nodiscard]] NodeRegistry safetyRegistry()
{
    auto registry = feedbackRegistry();
    registry.add (boostDescriptor, [] { return std::make_unique<Boost>(); });
    registry.add (scriptedDescriptor, [] { return std::make_unique<Scripted>(); });
    return registry;
}

[[nodiscard]] TrackGraph runawayGraph()
{
    auto graph = echoGraph (1.0, 150.0);
    graph.nodes[2].params["above"] = 1.0;
    graph.nodes.push_back (graphNode (boost, "test.boost"));
    graph.connections = { { impulse, 0, mix, 0 },
                          { mix, 0, boost, 0 },
                          { boost, 0, feedback, 0 },
                          { feedback, 0, mix, 1 },
                          { feedback, 0, output, 0 } };
    return graph;
}

// An engine that keeps its compiler, for changing a graph as it plays.
struct Rig
{
    explicit Rig (const TrackGraph& graph, int block = 128)
        : registry (safetyRegistry()),
          compiler (registry),
          blockLength (block)
    {
        play (graph);
    }

    void play (const TrackGraph& graph)
    {
        compiled = compiler.compile (projectOf (graph), { sampleRate, blockLength });
        engine.setEngine (compiled);
    }

    [[nodiscard]] std::vector<float> next (int total)
    {
        std::vector<float> result;

        while (static_cast<int> (result.size()) < total)
        {
            const auto buffer = render (engine, blockLength);
            result.insert (result.end(), buffer.left().begin(), buffer.left().end());
        }

        return result;
    }

    NodeRegistry registry;
    GraphCompiler compiler;
    AudioEngine engine;
    std::shared_ptr<const CompiledEngine> compiled;
    int blockLength;
};

[[nodiscard]] float loudest (const std::vector<float>& samples)
{
    float result = 0.0f;

    for (const auto sample : samples)
        result = std::max (result, std::abs (sample));

    return result;
}
} // namespace

TEST_CASE ("a runaway loop stays under the limit, for as long as it runs", "[feedback][safety]")
{
    Rig rig (runawayGraph());
    float top = 0.0f;

    // Three minutes: Amount 150 % and +24 dB in a 1 ms loop.
    for (int second = 0; second < 180; ++second)
    {
        const auto left = rig.next (static_cast<int> (sampleRate));
        REQUIRE (std::ranges::all_of (left, [] (float x) { return std::isfinite (x); }));
        top = std::max (top, loudest (left));
    }

    CHECK (top < 2.0f);
    CHECK (top > 1.0f); // it did run away

    const auto* node = rig.compiler.findInstance (1, feedback);
    REQUIRE (node != nullptr);
    CHECK (static_cast<bool> (node->meters()["limiting"]));
}

TEST_CASE ("the limit changes nothing below full scale", "[feedback][safety]")
{
    Rig rig (echoGraph (1.0, 80.0));
    const auto left = rig.next (2400);
    CHECK (loudest (left) < 1.0f);

    const auto* node = rig.compiler.findInstance (1, feedback);
    REQUIRE (node != nullptr);
    CHECK_FALSE (static_cast<bool> (node->meters()["limiting"]));
}

TEST_CASE ("Amount stops at 100 % unless it's allowed further", "[feedback][safety]")
{
    const auto firstEcho = [] (bool above)
    {
        auto graph = echoGraph (10.0, 150.0);
        graph.nodes[2].params["above"] = above ? 1.0 : 0.0;
        Rig rig (graph);
        return rig.next (480 + 128)[480];
    };

    CHECK_THAT (firstEcho (false), WithinAbs (1.0, 0.01));

    // 150 % of the impulse is above full scale, so the limit has bent it a little: 1 + tanh (0.5).
    CHECK_THAT (firstEcho (true), WithinAbs (1.462, 0.01));
}

TEST_CASE ("nothing invalid stays in a loop", "[feedback][safety]")
{
    auto graph = echoGraph (10.0, 90.0);
    graph.nodes[0].type = "test.scripted";
    Rig rig (graph);

    const auto before = rig.next (4800);
    CHECK (loudest (before) > 0.5f);

    // The NaN goes round. The track reports it and goes quiet, with no NaN reaching the output.
    const auto during = rig.next (19200);
    CHECK (std::ranges::all_of (during, [] (float x) { return std::isfinite (x); }));
    CHECK (rig.compiled->tracks[0].state->invalidBlocks.load() > 0);
    CHECK (loudest ({ during.end() - 1000, during.end() }) < 1.0e-3f);

    // The next impulse rings as before.
    const auto after = rig.next (4800);
    CHECK (std::ranges::all_of (after, [] (float x) { return std::isfinite (x); }));
    CHECK (loudest (after) > 0.5f);
}

TEST_CASE ("bypassing a Feedback node breaks the loop", "[feedback][safety]")
{
    auto graph = echoGraph (10.0, 100.0);
    Rig rig (graph);
    CHECK (loudest (rig.next (4800)) > 0.5f);

    auto bypassed = graph;
    bypassed.nodes[2].bypassed = true;
    rig.play (bypassed);
    CHECK (loudest (rig.next (4800)) == 0.0f);

    // Back on, from silence: what was in the line is gone.
    rig.play (graph);
    CHECK (loudest (rig.next (4800)) < 1.0e-3f);
}

TEST_CASE ("Clear empties a loop", "[feedback][safety]")
{
    Rig rig (echoGraph (10.0, 100.0));
    CHECK (loudest (rig.next (4800)) > 0.5f);

    auto* node = rig.compiler.findInstance (1, feedback);
    REQUIRE (node != nullptr);
    node->requestClear();
    CHECK (loudest (rig.next (4800)) == 0.0f);
}

TEST_CASE ("a loop doesn't collect DC", "[feedback][safety]")
{
    // The impulse's own offset dies away, rather than building up at 100 %.
    Rig rig (echoGraph (1.0, 100.0));
    std::ignore = rig.next (static_cast<int> (sampleRate) * 5);
    const auto left = rig.next (4800);
    double sum = 0.0;

    for (const auto sample : left)
        sum += sample;

    CHECK (std::abs (sum / static_cast<double> (left.size())) < 1.0e-3);
}

TEST_CASE ("resetting a Feedback node forgets the line without clearing it", "[feedback][safety]")
{
    NodeRegistry registry;
    registerBuiltinNodes (registry);
    auto node = registry.create (nodetype::feedback);
    REQUIRE (node != nullptr);
    node->prepare ({ sampleRate, 64 });
    node->params().set (1, 1.0f); // 48 samples

    std::array<float, 64> in {};
    std::array<float, 64> left {};
    std::array<float, 64> right {};
    in.fill (0.5f);
    const std::array<AudioInput, 1> inputs { AudioInput { in.data(), in.data() } };
    const std::array<AudioOutput, 1> outputs { AudioOutput { left.data(), right.data() } };
    Transport transport;
    ProcessContext context;
    context.numSamples = 64;
    context.transport = &transport;
    context.audioInputs = inputs;
    context.audioOutputs = outputs;
    node->process (context);
    CHECK (std::abs (left[60]) > 0.1f);

    node->reset();
    std::array<float, 64> silence {};
    const std::array<AudioInput, 1> quiet { AudioInput { silence.data(), silence.data() } };
    context.audioInputs = quiet;
    node->process (context);
    CHECK (loudest ({ left.begin(), left.end() }) == 0.0f);
}

namespace
{
constexpr int latentSamples = 64;
const NodeDescriptor latentDescriptor { "test.latent", "Latent", impulseOutput, impulseOutput, noParams };

class Latent final : public Node
{
public:
    Latent()
        : Node (latentDescriptor),
          m_delay (makeDelayCompensationNode (latentSamples))
    {
    }

    void prepare (const ProcessSpec& processSpec) override { m_delay->prepare (processSpec); }
    void process (const ProcessContext& context) noexcept override { m_delay->process (context); }
    [[nodiscard]] int latencySamples() const noexcept override { return latentSamples; }

private:
    std::unique_ptr<Node> m_delay;
};

[[nodiscard]] std::vector<float> renderEcho (double timeMs, bool withLatency)
{
    auto graph = echoGraph (timeMs, 50.0);
    constexpr ObjectId latent = 60;

    if (withLatency)
    {
        graph.nodes.push_back (graphNode (latent, "test.latent"));
        graph.connections = { { impulse, 0, mix, 0 },
                              { mix, 0, latent, 0 },
                              { latent, 0, feedback, 0 },
                              { feedback, 0, mix, 1 },
                              { latent, 0, output, 0 } };
    }

    auto registry = feedbackRegistry();
    registry.add (latentDescriptor, [] { return std::make_unique<Latent>(); });
    GraphCompiler compiler (registry);
    AudioEngine engine;
    engine.setEngine (compiler.compile (projectOf (graph), spec));
    return render (engine, 2400).left();
}

[[nodiscard]] size_t loudestAt (const std::vector<float>& samples, size_t from, size_t to)
{
    size_t best = from;

    for (auto i = from; i < to; ++i)
        if (std::abs (samples[i]) > std::abs (samples[best]))
            best = i;

    return best;
}

// A string's frequency, from where its waveform best repeats.
[[nodiscard]] double frequencyOfString (const std::vector<float>& samples, size_t from, size_t window)
{
    const auto correlation = [&] (size_t lag)
    {
        double sum = 0.0;

        for (size_t i = 0; i < window; ++i)
            sum += static_cast<double> (samples[from + i]) * static_cast<double> (samples[from + i + lag]);

        return sum;
    };

    // The first strong peak, not the many later ones at whole periods.
    constexpr size_t shortestLag = 10;
    constexpr size_t longestLag = 1200;
    std::vector<double> values;

    for (size_t lag = 0; lag <= longestLag; ++lag)
        values.push_back (correlation (lag));

    const auto top = *std::max_element (values.begin() + shortestLag, values.end());

    for (auto lag = shortestLag; lag < longestLag; ++lag)
    {
        if (values[lag] > 0.9 * top && values[lag] >= values[lag - 1] && values[lag] >= values[lag + 1])
        {
            const auto offset =
                0.5 * (values[lag - 1] - values[lag + 1]) / (values[lag - 1] - 2.0 * values[lag] + values[lag + 1]);
            return sampleRate / (static_cast<double> (lag) + offset);
        }
    }

    return 0.0;
}

// A plucked string: a burst of noise into a loop that follows the note.
struct StringRig
{
    explicit StringRig (double pitch, bool withFilter = false)
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        project = setup.project;
        project = apply (project, "addNotes",
                         { { "clipId", setup.clipId },
                           { "notes", juce::var (juce::Array<juce::var> { noteArgs (0, 8 * ticksPerBeat, pitch) }) } });

        const auto of = [] (const TrackGraph& graph, const char* type)
        {
            const auto it = std::ranges::find_if (graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
            REQUIRE (it != graph.nodes.end());
            return it->id;
        };

        voiceId = of (project.tracks[0]->graph, nodetype::mpeSynth);
        project = apply (project, "convertToVoiceGraph", { { "trackId", trackId }, { "nodeId", voiceId } });

        const auto oscillator = of (voice(), nodetype::oscillator);
        const auto envelope = of (voice(), nodetype::adsr);
        const auto expression = of (voice(), nodetype::noteExpression);
        const auto gain = of (voice(), nodetype::gain);
        const auto out = of (voice(), nodetype::voiceOutput);

        edit ("setNodeParam", { { "nodeId", oscillator }, { "param", "waveform" }, { "value", 4.0 } });
        edit ("setNodeParam", { { "nodeId", envelope }, { "param", "attackMs" }, { "value", 0.0 } });
        edit ("setNodeParam", { { "nodeId", envelope }, { "param", "decayMs" }, { "value", 5.0 } });
        edit ("setNodeParam", { { "nodeId", envelope }, { "param", "sustain" }, { "value", 0.0 } });

        edit ("addNode", { { "type", nodetype::mix } });
        const auto mixId = voice().nodes.back().id;
        edit ("addNode", { { "type", nodetype::feedback } });
        loop = voice().nodes.back().id;

        edit ("setNodeParam", { { "nodeId", loop }, { "param", "mode" }, { "value", 2.0 } });
        edit ("setNodeParam", { { "nodeId", loop }, { "param", "amount" }, { "value", 99.0 } });

        const auto connect = [&] (ObjectId from, int fromPort, ObjectId to, int toPort)
        {
            edit ("connect",
                  { { "fromNode", from }, { "fromPort", fromPort }, { "toNode", to }, { "toPort", toPort } });
        };

        edit ("disconnect", { { "fromNode", gain }, { "fromPort", 0 }, { "toNode", out }, { "toPort", 0 } });
        connect (gain, 0, mixId, 0);
        connect (mixId, 0, loop, 0);

        if (withFilter)
        {
            edit ("addNode", { { "type", nodetype::filter } });
            const auto filter = voice().nodes.back().id;
            edit ("setNodeParam", { { "nodeId", filter }, { "param", "cutoff" }, { "value", 7000.0 } });
            edit ("setNodeParam", { { "nodeId", filter }, { "param", "resonance" }, { "value", 0.0 } });
            edit ("setNodeParam", { { "nodeId", loop }, { "param", "trim" }, { "value", 1.6 } });
            connect (loop, 0, filter, 0);
            connect (filter, 0, mixId, 1);
            connect (filter, 0, out, 0);
        }
        else
        {
            connect (loop, 0, mixId, 1);
            connect (loop, 0, out, 0);
        }

        connect (expression, 0, loop, 1);
        edit ("setModulation", { { "nodeId", loop }, { "port", 1 }, { "param", "pitch" }, { "depth", 1.0 } });
    }

    [[nodiscard]] const TrackGraph& voice() const
    {
        const auto& graph = project.tracks[0]->graph;
        return *graph.nodes[static_cast<size_t> (graph.indexOfNode (voiceId))].voiceGraph;
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("trackId", trackId);
        object.getDynamicObject()->setProperty ("voiceNodeId", voiceId);
        project = applyCommand (project, command, object);
    }

    [[nodiscard]] std::vector<float> play (int numSamples)
    {
        engine.setEngine (compiler.compile (project, spec));
        engine.play();
        return render (engine, numSamples).left();
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId voiceId = 0;
    ObjectId loop = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};

[[nodiscard]] float peakOf (const std::vector<float>& samples, size_t from, size_t to)
{
    float result = 0.0f;

    for (auto i = from; i < to; ++i)
        result = std::max (result, std::abs (samples[i]));

    return result;
}

[[nodiscard]] double centsBetween (double frequency, double target)
{
    return 1200.0 * std::log2 (frequency / target);
}

[[nodiscard]] double frequencyOfNote (double pitch)
{
    return 440.0 * std::pow (2.0, (pitch - 69.0) / 12.0);
}
} // namespace

TEST_CASE ("a length between samples is read between samples", "[feedback][length]")
{
    // 480.5 samples: the echo is shared by the two samples either side.
    const auto left = renderEcho (480.5 / 48.0, false);
    CHECK_THAT (left[480], WithinAbs (left[481], 2.0e-3));
    CHECK (std::abs (left[480]) > 0.2f);
    CHECK (std::abs (left[480]) < 0.4f);
}

TEST_CASE ("Sync takes its length from the tempo", "[feedback][length]")
{
    auto graph = echoGraph (10.0, 50.0);
    const auto quarter =
        std::ranges::find_if (lfoCycleChoices, [] (const char* name) { return juce::String (name) == "1/4"; });
    REQUIRE (quarter != lfoCycleChoices.end());
    graph.nodes[2].params["mode"] = 1.0;
    graph.nodes[2].params["note"] = static_cast<double> (std::distance (lfoCycleChoices.begin(), quarter));

    const auto left = renderInBlocks (graph, 128, 30000);
    CHECK (loudestAt (left, 100, 30000) == 24000); // 120 bpm
}

TEST_CASE ("latency on a loop is part of its length", "[feedback][length]")
{
    // The whole trip stays 10 ms with 64 samples of latency on it, and the first echo lands where it would have.
    const auto left = renderEcho (10.0, true);
    const auto first = loudestAt (left, 0, 300);
    const auto echo = loudestAt (left, 300, 800);
    CHECK (first == latentSamples);
    CHECK (echo - first == 480);
}

TEST_CASE ("a loop too short for its latency stops at its shortest", "[feedback][length]")
{
    const auto left = renderEcho (1.0, true); // 48 samples asked for, with 64 of latency
    const auto first = loudestAt (left, 0, 70);
    const auto echo = loudestAt (left, first + 1, first + 200);
    CHECK (echo - first == latentSamples + loopChunkSamples + 2);
}

TEST_CASE ("a plucked string plays in a voice, in tune", "[feedback][length]")
{
    constexpr int total = 48000;

    for (const auto pitch : { 45.0, 57.0, 69.0, 81.0, 88.0 })
    {
        StringRig rig (pitch);
        const auto left = rig.play (total);
        const auto frequency = frequencyOfString (left, 24000, 4096);
        INFO ("note " << pitch << " at " << frequency << " Hz");
        CHECK (std::abs (centsBetween (frequency, frequencyOfNote (pitch))) < 5.0);
        CHECK (peakOf (left, 1000, 4000) > 0.01f); // at 99 % a trip, high strings die sooner
    }
}

TEST_CASE ("the string's top note is where the design says", "[feedback][length]")
{
    // E7 (MIDI 100) is about 18 samples, and higher notes stay there.
    StringRig rig (108.0);
    const auto frequency = frequencyOfString (rig.play (24000), 12000, 4096);
    INFO (frequency << " Hz");
    CHECK_THAT (frequency, WithinAbs (48000.0 / 18.0, 40.0));
}

TEST_CASE ("Trim keeps a string with a filter in its loop in tune across the keyboard", "[feedback][length]")
{
    for (const auto pitch : { 45.0, 57.0, 69.0, 81.0 })
    {
        StringRig rig (pitch, true);
        const auto frequency = frequencyOfString (rig.play (48000), 1200, 4096);
        INFO ("note " << pitch << " at " << frequency << " Hz");
        CHECK (std::abs (centsBetween (frequency, frequencyOfNote (pitch)))
               < 5.0); // while it's still audible: a high note soon dies away
    }
}

TEST_CASE ("a cable closing a loop gets a Feedback node", "[feedback]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::gain } });
    const auto first = project.tracks[0]->graph.nodes.back().id;
    project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::gain } });
    const auto second = project.tracks[0]->graph.nodes.back().id;
    project = apply (
        project, "connect",
        { { "trackId", trackId }, { "fromNode", first }, { "fromPort", 0 }, { "toNode", second }, { "toPort", 0 } });

    project = apply (project, "closeLoop",
                     { { "trackId", trackId },
                       { "fromNode", second },
                       { "fromPort", 0 },
                       { "toNode", first },
                       { "toPort", 0 },
                       { "x", 100 },
                       { "y", 200 } });

    const auto& node = project.tracks[0]->graph.nodes.back();
    CHECK (node.type == nodetype::feedback);
    CHECK_THAT (node.x, WithinAbs (100.0, 0.5));
    const auto& cables = project.tracks[0]->graph.connections;
    CHECK (std::ranges::find (cables, Connection { second, 0, node.id, 0 }) != cables.end());
    CHECK (std::ranges::find (cables, Connection { node.id, 0, first, 0 }) != cables.end());
}

TEST_CASE ("a default track's chain can be looped while it plays", "[feedback]")
{
    auto setup = projectWithClip();
    auto project = setup.project;

    const auto of = [&] (const char* type)
    {
        const auto& nodes = project.tracks[0]->graph.nodes;
        const auto it = std::ranges::find_if (nodes, [type] (const GraphNode& n) { return n.type == type; });
        REQUIRE (it != nodes.end());
        return it->id;
    };

    const auto registry = feedbackRegistry();
    GraphCompiler compiler (registry);
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    std::ignore = render (engine, 1024);

    project = apply (project, "closeLoop",
                     { { "trackId", setup.trackId },
                       { "fromNode", of (nodetype::gain) },
                       { "fromPort", 0 },
                       { "toNode", of (nodetype::eq) },
                       { "toPort", 0 },
                       { "x", 100 },
                       { "y", 300 } });

    if (auto compiled = compiler.update (project, spec))
        engine.setEngine (std::move (compiled));

    const auto buffer = render (engine, 4800);
    CHECK (std::ranges::all_of (buffer.left(), [] (float x) { return std::isfinite (x); }));
}

namespace
{
constexpr std::array<PortInfo, 1> constantOutput { { { PortType::modulation, "Mod" } } };
const NodeDescriptor constantDescriptor { "test.constant", "Constant", {}, constantOutput, noParams };

// A modulation source that stays at full scale.
class Constant final : public Node
{
public:
    Constant()
        : Node (constantDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        std::fill_n (context.audioOutputs[0][0], context.numSamples, 1.0f);
        std::fill_n (context.audioOutputs[0][1], context.numSamples, 1.0f);
    }
};

constexpr std::array<PortInfo, 2> clickerInputs { { { PortType::audio, "Audio" }, { PortType::events, "Notes" } } };
const NodeDescriptor clickerDescriptor { "test.clicker", "Clicker", clickerInputs, impulseOutput, noParams };

// Passes audio on, and adds one sample of 1.0 where each note starts.
class Clicker final : public Node
{
public:
    Clicker()
        : Node (clickerDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (size_t ch = 0; ch < 2; ++ch)
            for (int i = 0; i < context.numSamples; ++i)
                context.audioOutputs[0][ch][i] = context.audioInputs[0][ch][i];

        if (context.eventsIn == nullptr)
            return;

        for (const auto& event : context.eventsIn->events())
        {
            if (event.type != NoteEventType::noteOn)
                continue;

            context.audioOutputs[0][0][event.sampleOffset] += 1.0f;
            context.audioOutputs[0][1][event.sampleOffset] += 1.0f;
        }
    }
};

constexpr ObjectId source = 70;

[[nodiscard]] TrackGraph modulatedEcho (const char* sourceType, const char* param, double depth, double timeMs = 10.0)
{
    auto graph = echoGraph (timeMs, 50.0);
    graph.nodes.push_back (graphNode (source, sourceType));
    graph.connections.push_back ({ source, 0, feedback, 1 });
    graph.nodes[2].modulations[1] = { param, depth, true };
    return graph;
}

[[nodiscard]] std::vector<float> renderModulated (const TrackGraph& graph, int block, int total)
{
    auto registry = feedbackRegistry();
    registry.add (constantDescriptor, [] { return std::make_unique<Constant>(); });
    GraphCompiler compiler (registry);
    AudioEngine engine;
    engine.setEngine (compiler.compile (projectOf (graph), { sampleRate, block }));
    std::vector<float> result;

    while (static_cast<int> (result.size()) < total)
    {
        const auto buffer = render (engine, block);
        result.insert (result.end(), buffer.left().begin(), buffer.left().end());
    }

    result.resize (static_cast<size_t> (total));
    return result;
}

// How strong one frequency is in a stretch of samples.
[[nodiscard]] double strengthAt (const std::vector<float>& samples, size_t from, size_t window, double frequency)
{
    double re = 0.0;
    double im = 0.0;

    for (size_t i = 0; i < window; ++i)
    {
        const auto position = static_cast<double> (i);
        const auto hann =
            0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * position / static_cast<double> (window));
        const auto phase = juce::MathConstants<double>::twoPi * frequency * position / sampleRate;
        re += hann * static_cast<double> (samples[from + i]) * std::cos (phase);
        im += hann * static_cast<double> (samples[from + i]) * std::sin (phase);
    }

    return std::hypot (re, im);
}

// The default track's chain, with a loop round its EQ and Gain.
struct LoopedSong
{
    explicit LoopedSong (double amount, int block)
        : blockLength (block)
    {
        auto setup = projectWithClip();
        project = apply (setup.project, "addNotes",
                         { { "clipId", setup.clipId },
                           { "notes", juce::var (juce::Array<juce::var> { noteArgs (0, ticksPerBeat, 69.0) }) } });

        const auto of = [this] (const char* type)
        {
            const auto& nodes = project.tracks[0]->graph.nodes;
            const auto it = std::ranges::find_if (nodes, [type] (const GraphNode& n) { return n.type == type; });
            REQUIRE (it != nodes.end());
            return it->id;
        };

        project = apply (project, "closeLoop",
                         { { "trackId", setup.trackId },
                           { "fromNode", of (nodetype::gain) },
                           { "fromPort", 0 },
                           { "toNode", of (nodetype::eq) },
                           { "toPort", 0 },
                           { "x", 100 },
                           { "y", 300 } });
        project = apply (project, "setNodeParam",
                         { { "trackId", setup.trackId },
                           { "nodeId", project.tracks[0]->graph.nodes.back().id },
                           { "param", "amount" },
                           { "value", amount } });
        engine.setEngine (compiler.compile (project, { sampleRate, block }));
    }

    [[nodiscard]] std::vector<float> exported (const ExportRange& range)
    {
        std::vector<float> result;
        const auto frames = renderRange (engine, range, sampleRate, blockLength,
                                         [&] (std::array<const float*, 2> channels, int count)
                                         {
                                             result.insert (result.end(), channels[0], channels[0] + count);
                                             return true;
                                         });
        REQUIRE (frames == static_cast<juce::int64> (result.size()));
        return result;
    }

    Project project;
    GraphCompiler compiler;
    AudioEngine engine;
    int blockLength;
};
} // namespace

TEST_CASE ("a depth of 1 moves a loop's Time by 50 ms", "[feedback][modulation]")
{
    // +10 ms at full scale: 20 ms in all, which is 960 samples.
    const auto left = renderModulated (modulatedEcho ("test.constant", "time", 0.2), 128, 2400);
    CHECK_THAT (left[960], WithinAbs (0.5, 2.0e-3));
    CHECK_THAT (left[480], WithinAbs (0.0, 2.0e-3));
}

TEST_CASE ("Amount can be modulated", "[feedback][modulation]")
{
    // From 50 % to 75 %, of a range of 300 %.
    const auto left = renderModulated (modulatedEcho ("test.constant", "amount", 0.25 / 3.0), 128, 1200);
    CHECK_THAT (left[480], WithinAbs (0.75, 2.0e-3));
    CHECK_THAT (left[960], WithinAbs (0.5625, 2.0e-3));
}

TEST_CASE ("a loop with an LFO on its Time sounds the same at any buffer size", "[feedback][modulation]")
{
    auto graph = modulatedEcho (nodetype::lfo, "time", 0.05, 4.0);
    graph.nodes[2].params["amount"] = 90.0;
    graph.nodes.back().params["rateMs"] = 100.0;

    constexpr int total = 9600;
    const auto reference = renderModulated (graph, 1024, total);

    for (const auto block : { 64, 256 })
    {
        const auto left = renderModulated (graph, block, total);
        float largest = 0.0f;

        for (size_t i = 0; i < left.size(); ++i)
            largest = std::max (largest, std::abs (left[i] - reference[i]));

        INFO ("block " << block);
        CHECK (largest < 1.0e-4f);
    }
}

TEST_CASE ("a node on a loop hears its notes on time, at any buffer size", "[feedback][length]")
{
    constexpr ObjectId sequencer = 80;
    constexpr ObjectId clicker = 81;

    for (const auto block : { 64, 128, 480 })
    {
        auto setup = projectWithClip();
        auto project =
            apply (setup.project, "addNotes",
                   { { "clipId", setup.clipId },
                     { "notes", juce::var (juce::Array<juce::var> { noteArgs (ticksPerBeat, ticksPerBeat, 60.0) }) } });

        Track track = *project.tracks[0];
        track.graph = {};
        track.graph.nodes = { graphNode (sequencer, nodetype::sequencer), graphNode (clicker, "test.clicker"),
                              graphNode (feedback, nodetype::feedback, { { "time", 10.0 }, { "amount", 50.0 } }),
                              graphNode (output, nodetype::trackOutput) };
        track.graph.connections = { { sequencer, 0, clicker, 1 },
                                    { clicker, 0, feedback, 0 },
                                    { feedback, 0, clicker, 0 },
                                    { clicker, 0, output, 0 } };
        project.tracks[0] = std::make_shared<const Track> (std::move (track));

        auto registry = feedbackRegistry();
        registry.add (clickerDescriptor, [] { return std::make_unique<Clicker>(); });
        GraphCompiler compiler (registry);
        AudioEngine engine;
        engine.setEngine (compiler.compile (project, { sampleRate, block }));
        engine.play();

        std::vector<float> left;

        while (left.size() < 26000)
        {
            const auto buffer = render (engine, block);
            left.insert (left.end(), buffer.left().begin(), buffer.left().end());
        }

        INFO ("block " << block);
        CHECK (loudestAt (left, 0, 26000) == 24000); // a beat at 120 bpm
        CHECK_THAT (left[24480], WithinAbs (0.5, 2.0e-3));
        CHECK_THAT (left[24960], WithinAbs (0.25, 2.0e-3));
    }
}

TEST_CASE ("two strings in a voice graph each ring at their own note", "[feedback][length]")
{
    constexpr double low = 57.0;
    constexpr double high = 64.0;
    StringRig rig (low);
    rig.project = apply (rig.project, "addNotes",
                         { { "clipId", rig.project.tracks[0]->clips[0]->id },
                           { "notes", juce::var (juce::Array<juce::var> { noteArgs (0, 8 * ticksPerBeat, high) }) } });
    const auto left = rig.play (24000);

    constexpr size_t from = 4800;
    constexpr size_t window = 8192;
    const auto between = strengthAt (left, from, window, frequencyOfNote (60.5));
    CHECK (strengthAt (left, from, window, frequencyOfNote (low)) > 10.0 * between);
    CHECK (strengthAt (left, from, window, frequencyOfNote (high)) > 10.0 * between);
}

TEST_CASE ("Clear empties a loop in every voice", "[feedback][safety]")
{
    StringRig rig (57.0);
    CHECK (peakOf (rig.play (4800), 2400, 4800) > 0.01f);

    for (int voice = 0; std::cmp_less (voice, VoiceAllocator::numVoices); ++voice)
        if (auto* node = rig.compiler.findVoiceInstance (rig.trackId, rig.loop, voice))
            node->requestClear();

    const auto after = render (rig.engine, 4800).left();
    CHECK (peakOf (after, 2400, 4800) < 1.0e-4f);
}

TEST_CASE ("loops survive a save and open, and play as before", "[feedback][file]")
{
    constexpr double pitch = 57.0;
    StringRig rig (pitch);

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("dawg-test-" + juce::Uuid().toString());
    REQUIRE (folder.createDirectory());
    const auto file = folder.getChildFile (juce::String ("Loop") + projectfile::extension);
    REQUIRE (projectfile::write (rig.project, { "id", "0.1.0" }, file).isEmpty());
    const auto loaded = projectfile::read (file);
    std::ignore = folder.deleteRecursively();

    CHECK (juce::JSON::toString (toVar (loaded.project)) == juce::JSON::toString (toVar (rig.project)));

    rig.project = loaded.project;
    const auto frequency = frequencyOfString (rig.play (48000), 24000, 4096);
    CHECK (std::abs (centsBetween (frequency, frequencyOfNote (pitch))) < 5.0);
}

TEST_CASE ("an export with a loop is the same at any buffer size, and every time", "[feedback][export]")
{
    const ExportRange range { 0.0, 2.0, false, 0.5, {}, false, {} };
    constexpr size_t frames = 72000;

    LoopedSong song (70.0, 512);
    const auto reference = song.exported (range);
    REQUIRE (reference.size() >= frames);
    CHECK (loudest (reference) > 0.01f);

    // The second time starts as the app starts each pass, with nothing left in the loop.
    for (const auto& node : song.compiler.instances())
        node->prepare ({ sampleRate, 512 });

    const auto again = song.exported (range);
    REQUIRE (again.size() >= frames);

    LoopedSong other (70.0, 128);
    const auto smaller = other.exported (range);
    REQUIRE (smaller.size() >= frames);

    float largest = 0.0f;
    float largestAgain = 0.0f;

    for (size_t i = 0; i < frames; ++i)
    {
        largest = std::max (largest, std::abs (smaller[i] - reference[i]));
        largestAgain = std::max (largestAgain, std::abs (again[i] - reference[i]));
    }

    CHECK (largest < 1.0e-4f);
    CHECK (largestAgain < 1.0e-4f);
}

TEST_CASE ("an export's tail ends at its limit with a loop that never dies", "[feedback][export]")
{
    LoopedSong song (100.0, 512);
    const auto left = song.exported ({ 0.0, 1.0, true, 2.0, {}, false, {} });
    CHECK (left.size() < 24000 + 96000 + 1024);
    CHECK (std::ranges::all_of (left, [] (float x) { return std::isfinite (x); }));
}

namespace
{
// A string played live, for how the sustain pedal reaches it.
struct LiveString
{
    LiveString() { rig.engine.setEngine (rig.compiler.compile (rig.project, spec)); }

    void note()
    {
        LiveInput on;
        on.trackId = rig.trackId;
        on.noteId = noteId = rig.engine.nextLiveNoteId();
        on.pitch = 57.0;
        rig.engine.sendLiveInput (on);
    }

    void lift()
    {
        LiveInput off;
        off.trackId = rig.trackId;
        off.noteId = noteId;
        off.type = NoteEventType::noteOff;
        rig.engine.sendLiveInput (off);
    }

    void pedal (bool down)
    {
        LiveInput input;
        input.trackId = rig.trackId;
        input.type = NoteEventType::sustain;
        input.value = down ? 1.0f : 0.0f;
        rig.engine.sendLiveInput (input);
    }

    // How loud it is over the next stretch.
    [[nodiscard]] float next (int samples)
    {
        const auto left = render (rig.engine, samples).left();
        return peakOf (left, 0, left.size());
    }

    // Whether what comes next is louder than what came before: a new pluck, as a string only dies away.
    [[nodiscard]] bool plucksAfter (const std::function<void()>& action)
    {
        const auto before = next (2048);
        action();
        const auto after = next (2048);
        INFO ("from " << before << " to " << after);
        return after > before * 1.05f;
    }

    StringRig rig { 57.0 };
    ObjectId noteId = 0;
};
} // namespace

TEST_CASE ("the pedal never plucks a string again", "[feedback][length]")
{
    SECTION ("lifting it after holding a note")
    {
        LiveString string;
        string.note();
        std::ignore = string.next (4096);
        string.pedal (true);
        string.lift();
        std::ignore = string.next (4096);
        CHECK_FALSE (string.plucksAfter ([&] { string.pedal (false); }));
    }

    SECTION ("lifting it after pressing it before the note")
    {
        LiveString string;
        string.pedal (true);
        string.note();
        std::ignore = string.next (4096);
        string.lift();
        std::ignore = string.next (4096);
        CHECK_FALSE (string.plucksAfter ([&] { string.pedal (false); }));
    }

    SECTION ("pressing it on a note already let go")
    {
        LiveString string;
        string.note();
        std::ignore = string.next (4096);
        string.lift();
        std::ignore = string.next (4096);
        CHECK_FALSE (string.plucksAfter ([&] { string.pedal (true); }));
        CHECK_FALSE (string.plucksAfter ([&] { string.pedal (false); }));
    }

    SECTION ("pressing it again, as when changing pedal between chords")
    {
        LiveString string;
        string.pedal (true);
        string.note();
        std::ignore = string.next (4096);
        string.lift();
        std::ignore = string.next (4096);
        string.pedal (false);
        std::ignore = string.next (1024);
        CHECK_FALSE (string.plucksAfter ([&] { string.pedal (true); }));
    }

    SECTION ("a note played with it down still plucks")
    {
        LiveString string;
        string.pedal (true);
        std::ignore = string.next (1024);
        string.note();
        CHECK (string.next (2048) > 0.01f);
    }
}
