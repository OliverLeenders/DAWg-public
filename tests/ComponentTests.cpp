#include "TestUtils.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/CompiledEngine.h"
#include "dawg/GraphCompiler.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <vector>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Components (docs/design/component-nodes.md): grouping nodes can't change the sound.

namespace
{
constexpr std::array<PortInfo, 1> audioPort { { { PortType::audio, "Audio" } } };
const NodeDescriptor impulseDescriptor { "test.impulse", "Impulse", {}, audioPort, {} };
const NodeDescriptor latentDescriptor { "test.latent", "Latent", audioPort, audioPort, {} };

constexpr int latentSamples = 64;

// One sample of 1.0, and then one of 0.5 every 1000 samples, so that things downstream always have something to work on.
class Pulses final : public Node
{
public:
    Pulses()
        : Node (impulseDescriptor)
    {
    }

    void prepare (const ProcessSpec& /*spec*/) override {}

    void process (const ProcessContext& context) noexcept override
    {
        for (int i = 0; i < context.numSamples; ++i)
        {
            const auto at = m_position + i;
            auto value = at % 1000 == 0 ? 0.5f : 0.0f;

            if (at == 0)
                value = 1.0f;
            context.audioOutputs[0][0][i] = value;
            context.audioOutputs[0][1][i] = value;
        }

        m_position += context.numSamples;
    }

private:
    int m_position = 0;
};

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

[[nodiscard]] NodeRegistry testRegistry()
{
    NodeRegistry registry;
    registerBuiltinNodes (registry);
    registry.add (impulseDescriptor, [] { return std::make_unique<Pulses>(); });
    registry.add (latentDescriptor, [] { return std::make_unique<Latent>(); });
    return registry;
}

[[nodiscard]] GraphNode graphNode (ObjectId id, const char* type, std::map<juce::String, double> params = {})
{
    GraphNode result;
    result.id = id;
    result.type = type;
    result.params = std::move (params);
    return result;
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

[[nodiscard]] std::vector<float> rendered (const TrackGraph& graph, int total = 9600)
{
    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    AudioEngine engine;
    engine.setEngine (compiler.compile (projectOf (graph), spec));
    return render (engine, total).left();
}

[[nodiscard]] float largestDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    float largest = 0.0f;

    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        largest = std::max (largest, std::abs (a[i] - b[i]));

    return largest;
}

[[nodiscard]] float loudest (const std::vector<float>& samples)
{
    float result = 0.0f;

    for (const auto sample : samples)
        result = std::max (result, std::abs (sample));

    return result;
}

constexpr ObjectId firstId = 1000;

// Puts some of a graph's nodes in a component with a new id, as grouping will: each cable crossing the edge gets a port of its own.
[[nodiscard]] TrackGraph grouped (const TrackGraph& graph, const std::set<ObjectId>& ids, ObjectId id = firstId,
                                  bool bypassed = false)
{
    const auto in = id + 1;
    const auto out = id + 2;
    const auto kindOf = [] (const Connection& c, const TrackGraph& g)
    {
        const auto to = g.nodes[static_cast<size_t> (g.indexOfNode (c.toNode))];
        return c.toPort > 0 && to.modulations.contains (c.toPort) ? PortType::modulation : PortType::audio;
    };

    ComponentGraph inside;
    inside.graph.nodes = { graphNode (in, nodetype::componentInput), graphNode (out, nodetype::componentOutput) };

    TrackGraph result;

    for (const auto& node : graph.nodes)
        (ids.contains (node.id) ? inside.graph.nodes : result.nodes).push_back (node);

    auto component = graphNode (id, nodetype::component);
    component.bypassed = bypassed;

    for (const auto& c : graph.connections)
    {
        const auto fromInside = ids.contains (c.fromNode);
        const auto toInside = ids.contains (c.toNode);

        if (fromInside && toInside)
        {
            inside.graph.connections.push_back (c);
        }
        else if (toInside)
        {
            const auto port = static_cast<int> (inside.inputs.size());
            inside.inputs.push_back ({ kindOf (c, graph), "In" });
            inside.graph.connections.push_back ({ in, port, c.toNode, c.toPort });
            result.connections.push_back ({ c.fromNode, c.fromPort, id, port });
        }
        else if (fromInside)
        {
            const auto port = static_cast<int> (inside.outputs.size());
            inside.outputs.push_back ({ PortType::audio, "Out" });
            inside.graph.connections.push_back ({ c.fromNode, c.fromPort, out, port });
            result.connections.push_back ({ id, port, c.toNode, c.toPort });
        }
        else
        {
            result.connections.push_back (c);
        }
    }

    component.component = std::make_shared<const ComponentGraph> (std::move (inside));
    result.nodes.push_back (std::move (component));
    return result;
}

constexpr ObjectId pulses = 10;
constexpr ObjectId mix = 11;
constexpr ObjectId loop = 12;
constexpr ObjectId filter = 13;
constexpr ObjectId latent = 14;
constexpr ObjectId lfo = 15;
constexpr ObjectId outputMix = 16;
constexpr ObjectId output = 17;

// Pulses -> Mix -> Feedback -> Filter -> Mix, and on to the output, with a dry path through Latent, and an LFO on the filter's cutoff.
[[nodiscard]] TrackGraph busyGraph()
{
    TrackGraph graph;
    graph.nodes = { graphNode (pulses, "test.impulse"),
                    graphNode (mix, nodetype::mix),
                    graphNode (loop, nodetype::feedback, { { "time", 3.0 }, { "amount", 80.0 } }),
                    graphNode (filter, nodetype::filter, { { "cutoff", 2000.0 }, { "resonance", 10.0 } }),
                    graphNode (latent, "test.latent"),
                    graphNode (lfo, nodetype::lfo, { { "rateMs", 50.0 } }),
                    graphNode (outputMix, nodetype::mix),
                    graphNode (output, nodetype::trackOutput) };
    graph.nodes[3].modulations[1] = { "cutoff", 0.05, true };
    graph.connections = { { pulses, 0, mix, 0 },       { mix, 0, loop, 0 },         { loop, 0, filter, 0 },
                          { filter, 0, mix, 1 },       { filter, 0, outputMix, 0 }, { pulses, 0, latent, 0 },
                          { latent, 0, outputMix, 1 }, { lfo, 0, filter, 1 },       { outputMix, 0, output, 0 } };
    return graph;
}
} // namespace

TEST_CASE ("grouping nodes doesn't change the sound", "[component]")
{
    const auto plain = rendered (busyGraph());
    REQUIRE (loudest (plain) > 0.1f);

    // A test that could tell: the loop and the LFO are both heard.
    auto noLfo = busyGraph();
    noLfo.nodes[3].modulations.clear();
    REQUIRE (largestDifference (rendered (noLfo), plain) > 1.0e-3f);

    SECTION ("a whole loop")
    {
        CHECK (largestDifference (rendered (grouped (busyGraph(), { mix, loop, filter })), plain) < 1.0e-5f);
    }

    SECTION ("a loop crossing the edge")
    {
        CHECK (largestDifference (rendered (grouped (busyGraph(), { loop })), plain) < 1.0e-5f);
        CHECK (largestDifference (rendered (grouped (busyGraph(), { filter, outputMix })), plain) < 1.0e-5f);
    }

    SECTION ("latency, which the dry path still lines up with")
    {
        CHECK (largestDifference (rendered (grouped (busyGraph(), { latent })), plain) < 1.0e-5f);
    }

    SECTION ("modulation across the edge")
    {
        // The source outside and its target inside, and the other way round.
        CHECK (largestDifference (rendered (grouped (busyGraph(), { filter })), plain) < 1.0e-5f);
        CHECK (largestDifference (rendered (grouped (busyGraph(), { lfo })), plain) < 1.0e-5f);
    }

    SECTION ("everything but the two ends")
    {
        CHECK (
            largestDifference (rendered (grouped (busyGraph(), { mix, loop, filter, latent, lfo, outputMix })), plain)
            < 1.0e-5f);
    }

    SECTION ("several levels down")
    {
        auto graph = grouped (busyGraph(), { filter }, 1000);
        graph = grouped (graph, { loop, 1000 }, 1100);
        graph = grouped (graph, { mix, 1100 }, 1200);
        CHECK (largestDifference (rendered (graph), plain) < 1.0e-5f);
    }

    SECTION ("two components side by side")
    {
        auto graph = grouped (busyGraph(), { mix, loop }, 1000);
        graph = grouped (graph, { filter }, 1100);
        CHECK (largestDifference (rendered (graph), plain) < 1.0e-5f);
    }
}

TEST_CASE ("a key input works across a component's edge", "[component]")
{
    constexpr ObjectId compressor = 20;

    const auto keyed = [] (bool withKey)
    {
        TrackGraph graph;
        graph.nodes = { graphNode (pulses, "test.impulse"), graphNode (latent, "test.latent"),
                        graphNode (compressor, nodetype::compressor, { { "threshold", -40.0 }, { "ratio", 20.0 } }),
                        graphNode (output, nodetype::trackOutput) };
        graph.connections = { { pulses, 0, compressor, 0 }, { compressor, 0, output, 0 } };

        if (withKey)
        {
            graph.connections.push_back ({ pulses, 0, latent, 0 });
            graph.connections.push_back ({ latent, 0, compressor, 1 });
        }

        return graph;
    };

    const auto plain = rendered (keyed (true));
    CHECK (largestDifference (plain, rendered (keyed (false))) > 1.0e-3f); // the key matters
    CHECK (largestDifference (rendered (grouped (keyed (true), { compressor })), plain) < 1.0e-5f);
    CHECK (largestDifference (rendered (grouped (keyed (true), { latent })), plain) < 1.0e-5f);
}

TEST_CASE ("a component's cables flatten to direct ones", "[component]")
{
    constexpr ObjectId a = 1;
    constexpr ObjectId b = 2;
    constexpr ObjectId c = 3;
    constexpr ObjectId d = 4;
    constexpr ObjectId box = 5;
    constexpr ObjectId in = 6;
    constexpr ObjectId out = 7;

    const auto boxWith = [&] (std::vector<Connection> cables, bool bypassed = false)
    {
        ComponentGraph inside;
        inside.graph.nodes = { graphNode (in, nodetype::componentInput), graphNode (out, nodetype::componentOutput),
                               graphNode (d, nodetype::gain) };
        inside.graph.connections = std::move (cables);
        inside.inputs = { { PortType::audio, "In" } };
        inside.outputs = { { PortType::audio, "Out" } };

        auto node = graphNode (box, nodetype::component);
        node.component = std::make_shared<const ComponentGraph> (std::move (inside));
        node.bypassed = bypassed;
        return node;
    };

    const auto flat = [&] (GraphNode component)
    {
        TrackGraph graph;
        graph.nodes = { graphNode (a, nodetype::gain), graphNode (b, nodetype::gain), graphNode (c, nodetype::gain),
                        std::move (component) };
        graph.connections = { { a, 0, box, 0 }, { b, 0, box, 0 }, { box, 0, c, 0 }, { box, 0, c, 1 } };
        return flattenComponents (graph);
    };

    const auto has = [] (const TrackGraph& graph, Connection cable)
    { return std::ranges::find (graph.connections, cable) != graph.connections.end(); };

    SECTION ("a wire straight through is a plain cable, from each source to each target")
    {
        const auto graph = flat (boxWith ({ { in, 0, out, 0 } }));
        CHECK (graph.connections.size() == 4);
        CHECK (has (graph, { a, 0, c, 0 }));
        CHECK (has (graph, { a, 0, c, 1 }));
        CHECK (has (graph, { b, 0, c, 0 }));
        CHECK (has (graph, { b, 0, c, 1 }));
        CHECK (graph.indexOfNode (box) < 0);
        CHECK (graph.indexOfNode (in) < 0);
        CHECK (graph.indexOfNode (d) >= 0);
    }

    SECTION ("an output feeding another of the same component's inputs, through wires, gets there")
    {
        ComponentGraph inside;
        inside.graph.nodes = { graphNode (in, nodetype::componentInput), graphNode (out, nodetype::componentOutput) };
        inside.graph.connections = { { in, 0, out, 0 }, { in, 1, out, 1 } };
        inside.inputs = { { PortType::audio, "A" }, { PortType::audio, "B" } };
        inside.outputs = { { PortType::audio, "A" }, { PortType::audio, "B" } };

        auto component = graphNode (box, nodetype::component);
        component.component = std::make_shared<const ComponentGraph> (std::move (inside));

        TrackGraph graph;
        graph.nodes = { graphNode (a, nodetype::gain), graphNode (c, nodetype::gain), std::move (component) };
        graph.connections = { { a, 0, box, 0 }, { box, 0, box, 1 }, { box, 1, c, 0 } };

        auto wireLoop = false;
        const auto result = flattenComponents (graph, &wireLoop);
        CHECK_FALSE (wireLoop);
        REQUIRE (result.connections.size() == 1);
        CHECK (has (result, { a, 0, c, 0 }));
    }

    SECTION ("a port with nothing behind it is silent, not an error")
    {
        CHECK (flat (boxWith ({})).connections.empty());
    }

    SECTION ("bypassed: the first audio input goes to the first audio output, and what's inside stays, unconnected")
    {
        const auto graph = flat (boxWith ({ { in, 0, d, 0 }, { d, 0, out, 0 } }, true));
        CHECK (graph.connections.size() == 4);
        CHECK (has (graph, { a, 0, c, 0 }));
        CHECK (graph.indexOfNode (d) >= 0);
    }

    SECTION ("a graph with no component is unchanged")
    {
        TrackGraph graph;
        graph.nodes = { graphNode (a, nodetype::gain), graphNode (b, nodetype::gain) };
        graph.connections = { { a, 0, b, 0 } };
        const auto result = flattenComponents (graph);
        CHECK (result.connections == graph.connections);
        CHECK (result.nodes.size() == 2);
    }
}

TEST_CASE ("bypassing a component passes its audio past what's inside", "[component]")
{
    auto through = grouped (busyGraph(), { mix, loop, filter }, firstId, true);
    const auto bypassed = rendered (through);

    // The pulses and the dry path, with the loop gone.
    TrackGraph plain;
    plain.nodes = { graphNode (pulses, "test.impulse"), graphNode (latent, "test.latent"),
                    graphNode (outputMix, nodetype::mix), graphNode (output, nodetype::trackOutput) };
    plain.connections = {
        { pulses, 0, outputMix, 0 }, { pulses, 0, latent, 0 }, { latent, 0, outputMix, 1 }, { outputMix, 0, output, 0 }
    };
    CHECK (largestDifference (bypassed, rendered (plain)) < 1.0e-5f);
    CHECK (largestDifference (bypassed, rendered (busyGraph())) > 0.01f); // and the loop made a difference

    // What's inside keeps its instance, for a plugin to stay loaded.
    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    std::ignore = compiler.compile (projectOf (through), spec);
    CHECK (compiler.findInstance (1, loop) != nullptr);
}

TEST_CASE ("a loop in a component in a voice graph compiles per voice", "[component]")
{
    constexpr ObjectId voiceIn = 40;
    constexpr ObjectId oscillator = 41;
    constexpr ObjectId voiceOut = 42;

    TrackGraph voice;
    voice.nodes = { graphNode (voiceIn, nodetype::voiceInput), graphNode (oscillator, nodetype::oscillator),
                    graphNode (mix, nodetype::mix), graphNode (loop, nodetype::feedback),
                    graphNode (voiceOut, nodetype::voiceOutput) };
    voice.connections = {
        { oscillator, 0, mix, 0 }, { mix, 0, loop, 0 }, { loop, 0, mix, 1 }, { loop, 0, voiceOut, 0 }
    };

    TrackGraph graph;
    graph.nodes = { graphNode (1, nodetype::sequencer), graphNode (2, nodetype::voiceInstrument),
                    graphNode (3, nodetype::trackOutput) };
    graph.nodes[1].voiceGraph = std::make_shared<const TrackGraph> (grouped (voice, { mix, loop }));
    graph.connections = { { 1, 0, 2, 0 }, { 2, 0, 3, 0 } };

    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    const auto engine = compiler.compile (projectOf (graph), spec);
    const auto step =
        std::ranges::find_if (engine->tracks[0].steps, [] (const CompiledStep& s) { return s.voice != nullptr; });
    REQUIRE (step != engine->tracks[0].steps.end());
    CHECK (std::ranges::any_of (step->voice->slots[3].steps, [] (const CompiledStep& s) { return s.regionSteps > 0; }));
    CHECK (compiler.findVoiceInstance (1, loop, 3) != nullptr);
}

TEST_CASE ("editing inside a component reaches the running engine", "[component]")
{
    const auto registry = testRegistry();
    GraphCompiler compiler (registry);
    AudioEngine engine;

    const auto withAmount = [] (double amount, double cutoff = 2000.0)
    {
        auto graph = busyGraph();
        graph.nodes[2].params["amount"] = amount;
        graph.nodes[3].params["cutoff"] = cutoff;
        return grouped (graph, { mix, loop, filter });
    };

    engine.setEngine (compiler.compile (projectOf (withAmount (80.0)), spec));
    std::ignore = render (engine, 1024);

    SECTION ("a parameter needs no compilation, and gets there")
    {
        CHECK (compiler.update (projectOf (withAmount (20.0)), spec) == nullptr);
        const auto* node = compiler.findInstance (1, loop);
        REQUIRE (node != nullptr);
        CHECK_THAT (node->params().get (5), WithinAbs (20.0, 1.0e-3));
    }

    SECTION ("a cable inside does")
    {
        auto graph = withAmount (80.0);
        auto inside = *graph.nodes.back().component;
        inside.graph.connections.pop_back();
        graph.nodes.back().component = std::make_shared<const ComponentGraph> (std::move (inside));
        CHECK (compiler.update (projectOf (graph), spec) != nullptr);
    }

    SECTION ("bypassing it does")
    {
        auto graph = withAmount (80.0);
        graph.nodes.back().bypassed = true;
        CHECK (compiler.update (projectOf (graph), spec) != nullptr);
    }
}

namespace
{
// A real project's track with a component holding a Gain, in a file's form.
[[nodiscard]] Project projectWithComponent (const std::function<void (ComponentGraph&)>& change = nullptr)
{
    auto project = apply ({}, "addTrack", {});
    Track track = *project.tracks[0];

    ComponentGraph inside;
    inside.graph.nodes = { graphNode (firstId + 1, nodetype::componentInput),
                           graphNode (firstId + 2, nodetype::componentOutput),
                           graphNode (firstId + 3, nodetype::gain) };
    inside.graph.connections = { { firstId + 1, 0, firstId + 3, 0 }, { firstId + 3, 0, firstId + 2, 0 } };
    inside.inputs = { { PortType::audio, "In" }, { PortType::events, "Notes" }, { PortType::modulation, "Dark" } };
    inside.outputs = { { PortType::audio, "Out" } };

    if (change)
        change (inside);

    auto component = graphNode (firstId, nodetype::component);
    component.component = std::make_shared<const ComponentGraph> (std::move (inside));
    track.graph.nodes.push_back (std::move (component));
    project.tracks[0] = std::make_shared<const Track> (std::move (track));
    project.nextId = firstId + 100;
    return project;
}

struct TempFile
{
    TempFile()
        : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("dawg-test-" + juce::Uuid().toString()))
    {
        REQUIRE (folder.createDirectory());
    }

    ~TempFile() { std::ignore = folder.deleteRecursively(); }

    TempFile (const TempFile&) = delete;
    TempFile& operator= (const TempFile&) = delete;
    TempFile (TempFile&&) = delete;
    TempFile& operator= (TempFile&&) = delete;

    [[nodiscard]] juce::File file() const
    {
        return folder.getChildFile (juce::String ("Song") + projectfile::extension);
    }

    juce::File folder;
};

[[nodiscard]] Project roundTrip (const Project& project)
{
    const TempFile temp;
    REQUIRE (projectfile::write (project, { "id", "0.1.0" }, temp.file()).isEmpty());
    return projectfile::read (temp.file()).project;
}
} // namespace

TEST_CASE ("components survive a save and open, and damaged ones are refused", "[component][file]")
{
    SECTION ("the same model comes back, ports and all")
    {
        const auto project = projectWithComponent();
        const auto loaded = roundTrip (project);
        CHECK (juce::JSON::toString (toVar (loaded)) == juce::JSON::toString (toVar (project)));

        const auto& node = loaded.tracks[0]->graph.nodes.back();
        REQUIRE (node.component != nullptr);
        CHECK (node.component->inputs[1] == ComponentPort { PortType::events, "Notes" });
        CHECK (node.component->inputs[2].type == PortType::modulation);
    }

    SECTION ("what there can only be one of doesn't go in one")
    {
        for (const auto* type : { nodetype::trackOutput, nodetype::sequencer, nodetype::voiceOutput })
        {
            const auto project = projectWithComponent (
                [type] (ComponentGraph& inside) { inside.graph.nodes.push_back (graphNode (firstId + 4, type)); });
            CHECK_THROWS_AS (roundTrip (project), CommandError);
        }
    }

    SECTION ("it needs its one Input and one Output")
    {
        const auto project = projectWithComponent (
            [] (ComponentGraph& inside)
            {
                inside.graph.connections.clear();
                inside.graph.nodes.erase (inside.graph.nodes.begin());
            });
        CHECK_THROWS_AS (roundTrip (project), CommandError);
    }

    SECTION ("nesting stops at its limit")
    {
        const auto nested = [] (int levels)
        {
            ComponentGraph inner;
            ObjectId id = firstId + 10;
            const auto fixed = [&id] (ComponentGraph& graph)
            {
                graph.graph.nodes.push_back (graphNode (id++, nodetype::componentInput));
                graph.graph.nodes.push_back (graphNode (id++, nodetype::componentOutput));
            };
            fixed (inner);

            for (int level = 1; level < levels; ++level)
            {
                ComponentGraph outer;
                fixed (outer);
                auto box = graphNode (id++, nodetype::component);
                box.component = std::make_shared<const ComponentGraph> (std::move (inner));
                outer.graph.nodes.push_back (std::move (box));
                inner = std::move (outer);
            }

            return projectWithComponent ([&inner] (ComponentGraph& inside) { inside = inner; });
        };

        CHECK_NOTHROW (roundTrip (nested (limits::maxComponentDepth)));
        CHECK_THROWS_AS (roundTrip (nested (limits::maxComponentDepth + 1)), CommandError);
    }

    SECTION ("a plugin in a component in a voice graph, at any depth, is refused")
    {
        auto plugin = graphNode (firstId + 20, nodetype::pluginEffect);
        plugin.plugin = PluginRef { "id", "Name", "Maker", "VST3" };

        ComponentGraph inside;
        inside.graph.nodes = { graphNode (firstId + 21, nodetype::componentInput),
                               graphNode (firstId + 22, nodetype::componentOutput), std::move (plugin) };
        auto box = graphNode (firstId + 23, nodetype::component);
        box.component = std::make_shared<const ComponentGraph> (std::move (inside));

        auto project = apply ({}, "addTrack", {});
        project =
            apply (project, "addNode", { { "trackId", project.tracks[0]->id }, { "type", nodetype::voiceInstrument } });
        Track track = *project.tracks[0];
        auto& owner = track.graph.nodes.back();
        auto voice = *owner.voiceGraph;
        voice.nodes.push_back (std::move (box));
        owner.voiceGraph = std::make_shared<const TrackGraph> (std::move (voice));
        project.tracks[0] = std::make_shared<const Track> (std::move (track));
        project.nextId = firstId + 100;
        CHECK_THROWS_AS (roundTrip (project), CommandError);
    }
}

namespace
{
// A track with a real chain and a component added by command, through which the tests edit.
struct Edited
{
    Edited()
    {
        auto setup = projectWithClip();
        project = setup.project;
        trackId = setup.trackId;
        box = add (nodetype::component);
    }

    ObjectId add (const char* type, ObjectId into = 0)
    {
        auto args = makeObject ({ { "trackId", trackId }, { "type", type } });

        if (into != 0)
            args.getDynamicObject()->setProperty ("voiceNodeId", into);

        const auto next = project.nextId;
        project = applyCommand (project, "addNode", args);
        return next;
    }

    void connect (ObjectId from, int fromPort, ObjectId to, int toPort, ObjectId into = 0)
    {
        auto args = makeObject ({ { "trackId", trackId },
                                  { "fromNode", from },
                                  { "fromPort", fromPort },
                                  { "toNode", to },
                                  { "toPort", toPort } });

        if (into != 0)
            args.getDynamicObject()->setProperty ("voiceNodeId", into);

        project = applyCommand (project, "connect", args);
    }

    [[nodiscard]] const Track& track() const { return *project.tracks[0]; }

    [[nodiscard]] const ComponentGraph& inside (ObjectId id) const
    {
        const auto* node = track().findNode (id);
        REQUIRE (node != nullptr);
        REQUIRE (node->component != nullptr);
        return *node->component;
    }

    [[nodiscard]] ObjectId fixed (ObjectId id, const char* type) const
    {
        const auto& nodes = inside (id).graph.nodes;
        const auto it = std::ranges::find (nodes, juce::String (type), &GraphNode::type);
        REQUIRE (it != nodes.end());
        return it->id;
    }

    [[nodiscard]] ObjectId chainNode (const char* type) const
    {
        const auto& nodes = track().graph.nodes;
        const auto it = std::ranges::find (nodes, juce::String (type), &GraphNode::type);
        REQUIRE (it != nodes.end());
        return it->id;
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId box = 0;
};
} // namespace

TEST_CASE ("commands reach inside a component", "[component][commands]")
{
    Edited edited;

    SECTION ("a new one has an audio input and output, wired straight through")
    {
        const auto& inside = edited.inside (edited.box);
        CHECK (inside.inputs.size() == 1);
        CHECK (inside.outputs.size() == 1);
        CHECK (inside.graph.connections.size() == 1);
    }

    SECTION ("nodes are added and wired inside, by its ports")
    {
        const auto gain = edited.add (nodetype::gain, edited.box);
        const auto in = edited.fixed (edited.box, nodetype::componentInput);
        const auto out = edited.fixed (edited.box, nodetype::componentOutput);
        edited.connect (in, 0, gain, 0, edited.box);
        edited.connect (gain, 0, out, 0);

        CHECK (edited.inside (edited.box).graph.connections.size() == 3);
        CHECK (edited.track().graph.indexOfNode (gain) < 0);

        // Ports that aren't there.
        CHECK_THROWS_AS (edited.connect (in, 1, gain, 0, edited.box), CommandError);
        CHECK_THROWS_AS (edited.connect (gain, 0, out, 1, edited.box), CommandError);
    }

    SECTION ("the node a command names finds its graph, at any depth")
    {
        const auto inner = edited.add (nodetype::component, edited.box);
        const auto gain = edited.add (nodetype::gain, inner);
        edited.project =
            apply (edited.project, "setNodeParam",
                   { { "trackId", edited.trackId }, { "nodeId", gain }, { "param", "gainDb" }, { "value", -6.0 } });
        edited.project = apply (edited.project, "setNodeBypass",
                                { { "trackId", edited.trackId }, { "nodeId", gain }, { "bypassed", true } });

        const auto* node = edited.track().findNode (gain);
        REQUIRE (node != nullptr);
        CHECK_THAT (node->params.at ("gainDb"), WithinAbs (-6.0, 1.0e-9));
        CHECK (node->bypassed);
        CHECK (edited.track().graphOwnerOf (gain) == inner);
        CHECK (edited.track().graphOwnerOf (inner) == edited.box);
        CHECK (edited.track().graphOwnerOf (edited.box) == 0);
    }

    SECTION ("the Input and Output can't be removed, or added")
    {
        const auto in = edited.fixed (edited.box, nodetype::componentInput);
        CHECK_THROWS_AS (apply (edited.project, "removeNode", { { "trackId", edited.trackId }, { "nodeId", in } }),
                         CommandError);
        CHECK_THROWS_AS (edited.add (nodetype::componentInput, edited.box), CommandError);
    }

    SECTION ("removing a node inside forgets its lanes, and removing a component forgets those of all it holds")
    {
        const auto gain = edited.add (nodetype::gain, edited.box);
        const auto lane = [&] (const Project& project)
        {
            return apply (
                project, "addAutomationLane",
                { { "trackId", edited.trackId },
                  { "target", makeObject ({ { "kind", "node" }, { "nodeId", gain }, { "param", "gainDb" } }) } });
        };

        const auto withLane = lane (edited.project);
        REQUIRE (withLane.tracks[0]->automation.size() == 1);
        CHECK (apply (withLane, "removeNode", { { "trackId", edited.trackId }, { "nodeId", gain } })
                   .tracks[0]
                   ->automation.empty());
        CHECK (apply (withLane, "removeNode", { { "trackId", edited.trackId }, { "nodeId", edited.box } })
                   .tracks[0]
                   ->automation.empty());
    }

    SECTION ("a MIDI mapping can aim at a node inside")
    {
        const auto gain = edited.add (nodetype::gain, edited.box);
        CHECK_NOTHROW (apply (edited.project, "setMidiMapping",
                              { { "trackId", edited.trackId },
                                { "nodeId", gain },
                                { "param", "gainDb" },
                                { "channel", 1 },
                                { "controller", 7 } }));
    }
}

TEST_CASE ("a loop can't close through a component without a Feedback node", "[component][commands]")
{
    Edited edited;
    const auto gain = edited.chainNode (nodetype::gain);
    const auto eq = edited.chainNode (nodetype::eq);

    // EQ -> box (a straight wire), and on to the Gain, which comes after the EQ.
    edited.connect (eq, 0, edited.box, 0);
    edited.connect (edited.box, 0, gain, 0);

    SECTION ("back around the box's outside")
    {
        CHECK_THROWS_AS (edited.connect (gain, 0, edited.box, 0), CommandError);
    }

    SECTION ("a cable that only closes it once the box is gone")
    {
        const auto other = edited.add (nodetype::component);
        edited.connect (edited.box, 0, other, 0);
        CHECK_THROWS_AS (edited.connect (other, 0, edited.box, 0), CommandError);
    }

    SECTION ("through a Feedback node inside, it may")
    {
        const auto way = edited.add (nodetype::feedback, edited.box);
        const auto in = edited.fixed (edited.box, nodetype::componentInput);
        const auto out = edited.fixed (edited.box, nodetype::componentOutput);
        const auto args = makeObject ({ { "trackId", edited.trackId },
                                        { "fromNode", in },
                                        { "fromPort", 0 },
                                        { "toNode", out },
                                        { "toPort", 0 } });
        edited.project = applyCommand (edited.project, "disconnect", args);
        edited.connect (in, 0, way, 0, edited.box);
        edited.connect (way, 0, out, 0, edited.box);
        CHECK_NOTHROW (edited.connect (gain, 0, edited.box, 0));
    }
}

TEST_CASE ("what a component holds follows where it is", "[component][commands]")
{
    SECTION ("nesting stops at the limit")
    {
        Edited edited;
        auto level = edited.box;

        for (int depth = 1; depth < limits::maxComponentDepth; ++depth)
            level = edited.add (nodetype::component, level);

        CHECK_THROWS_AS (edited.add (nodetype::component, level), CommandError);
    }

    SECTION ("in a voice graph, no plugin at any depth")
    {
        Edited edited;
        const auto voice = edited.add (nodetype::voiceInstrument);
        const auto box = edited.add (nodetype::component, voice);
        const auto inner = edited.add (nodetype::component, box);

        auto args = makeObject (
            { { "trackId", edited.trackId }, { "voiceNodeId", inner }, { "type", nodetype::pluginEffect } });
        args.getDynamicObject()->setProperty ("plugin", makeObject ({ { "identifier", "x" }, { "name", "X" } }));
        CHECK_THROWS_AS (applyCommand (edited.project, "addNode", args), CommandError);

        CHECK_NOTHROW (edited.add (nodetype::feedback, inner));
    }

    SECTION ("a component holding a Voice Instrument can't be pasted into a voice graph")
    {
        Edited edited;
        std::ignore = edited.add (nodetype::voiceInstrument, edited.box);
        const auto voice = edited.add (nodetype::voiceInstrument);

        const auto form = toVar (edited.project)["tracks"][juce::Identifier (juce::String (edited.trackId))]["graph"]
                                                ["nodes"][juce::Identifier (juce::String (edited.box))];
        const auto paste = [&] (const juce::var& args)
        {
            juce::Array<juce::var> nodes;
            nodes.add (makeObject ({ { "type", nodetype::component }, { "component", form["component"] } }));
            args.getDynamicObject()->setProperty ("nodes", nodes);
            return applyCommand (edited.project, "addNodes", args);
        };

        CHECK_THROWS_AS (paste (makeObject ({ { "trackId", edited.trackId }, { "voiceNodeId", voice } })),
                         CommandError);

        // It's fine on the track, with new ids all the way down.
        const auto pasted = paste (makeObject ({ { "trackId", edited.trackId } }));
        std::set<ObjectId> ids;
        auto repeated = false;
        forEachNode (pasted.tracks[0]->graph,
                     [&] (const GraphNode& node, bool) { repeated = ! ids.insert (node.id).second || repeated; });
        CHECK_FALSE (repeated);
    }
}

TEST_CASE ("everything that walks a track's nodes goes into components", "[component][walks]")
{
    Edited edited;
    const auto inner = edited.add (nodetype::component, edited.box);

    auto args =
        makeObject ({ { "trackId", edited.trackId }, { "voiceNodeId", inner }, { "type", nodetype::pluginEffect } });
    args.getDynamicObject()->setProperty ("plugin", makeObject ({ { "identifier", "x" }, { "name", "X" } }));
    const auto plugin = edited.project.nextId;
    edited.project = applyCommand (edited.project, "addNode", args);

    SECTION ("the walk itself, with whether a node runs per voice")
    {
        const auto voice = edited.add (nodetype::voiceInstrument, inner);
        const auto inVoice = edited.add (nodetype::component, voice);
        const auto deepest = edited.add (nodetype::gain, inVoice);

        std::map<ObjectId, bool> seen;
        forEachNode (edited.track().graph, [&] (const GraphNode& node, bool flag) { seen[node.id] = flag; });
        CHECK (seen.at (plugin) == false);
        CHECK (seen.at (voice) == false);
        CHECK (seen.at (inVoice) == true);
        CHECK (seen.at (deepest) == true);
        CHECK (edited.track().findNode (deepest) != nullptr);
        CHECK (edited.track().findTrackNode (plugin) != nullptr);
        CHECK (edited.track().findTrackNode (deepest) == nullptr);
    }

    SECTION ("plugin state is stamped and saved from inside, and comes back")
    {
        const auto state = std::make_shared<const juce::MemoryBlock> ("\x01\x02\x03", 3);
        edited.project = withPluginState (edited.project, edited.trackId, plugin, state);
        REQUIRE (edited.track().findNode (plugin)->pluginState != nullptr);

        const auto loaded = roundTrip (edited.project);
        const auto* node = loaded.tracks[0]->findNode (plugin);
        REQUIRE (node != nullptr);
        REQUIRE (node->pluginState != nullptr);
        CHECK (*node->pluginState == *state);
    }

    SECTION ("the file's next id is past everything inside")
    {
        const TempFile temp;
        REQUIRE (projectfile::write (edited.project, { "id", "0.1.0" }, temp.file()).isEmpty());
        CHECK (projectfile::read (temp.file()).project.nextId > plugin);
    }

    SECTION ("a copied track gets new ids at every depth, and its lanes follow")
    {
        edited.project =
            apply (edited.project, "addAutomationLane",
                   { { "trackId", edited.trackId },
                     { "target", makeObject ({ { "kind", "plugin" }, { "nodeId", plugin }, { "param", "0" } }) } });
        edited.project = apply (edited.project, "duplicateTrack", { { "id", edited.trackId } });
        REQUIRE (edited.project.tracks.size() == 2);

        std::set<ObjectId> first;
        forEachNode (edited.project.tracks[0]->graph, [&] (const GraphNode& node, bool) { first.insert (node.id); });
        auto shared = false;
        forEachNode (edited.project.tracks[1]->graph,
                     [&] (const GraphNode& node, bool) { shared = shared || first.contains (node.id); });
        CHECK_FALSE (shared);

        const auto copied = edited.project.tracks[1]->automation.at (0).target.nodeId;
        CHECK (copied != plugin);
        CHECK (edited.project.tracks[1]->findNode (copied) != nullptr);
    }

    SECTION ("a key send reaches a compressor inside, and sounds as it would outside")
    {
        auto project = apply (edited.project, "addTrack", {});
        const auto other = project.tracks[1]->id;
        const auto compressor = project.nextId;
        project =
            apply (project, "addNode",
                   { { "trackId", edited.trackId }, { "voiceNodeId", edited.box }, { "type", nodetype::compressor } });
        CHECK_NOTHROW (apply (project, "addSend",
                              { { "trackId", other }, { "target", edited.trackId }, { "targetNode", compressor } }));

        // What isn't keyable, or runs per voice, stays out of reach.
        const auto gain = project.nextId;
        project = apply (project, "addNode",
                         { { "trackId", edited.trackId }, { "voiceNodeId", edited.box }, { "type", nodetype::gain } });
        CHECK_THROWS_AS (
            apply (project, "addSend", { { "trackId", other }, { "target", edited.trackId }, { "targetNode", gain } }),
            CommandError);

        const auto keyed = apply (project, "addSend",
                                  { { "trackId", other }, { "target", edited.trackId }, { "targetNode", compressor } });
        GraphCompiler compiler;
        CHECK (compiler.compile (keyed, spec) != nullptr);
    }
}

TEST_CASE ("a component's ports can be added, named and removed", "[component][commands]")
{
    Edited edited;
    const auto eq = edited.chainNode (nodetype::eq);
    const auto gain = edited.chainNode (nodetype::gain);
    const auto in = edited.fixed (edited.box, nodetype::componentInput);
    const auto out = edited.fixed (edited.box, nodetype::componentOutput);

    const auto addPort = [&] (const char* side, const char* type, const char* name)
    {
        edited.project = apply (edited.project, "addComponentPort",
                                { { "trackId", edited.trackId },
                                  { "nodeId", edited.box },
                                  { "side", side },
                                  { "type", type },
                                  { "name", name } });
    };

    SECTION ("a new port can be cabled at once, on both sides, and only with its own kind")
    {
        addPort ("input", "audio", "Key");
        CHECK (edited.inside (edited.box).inputs.back() == ComponentPort { PortType::audio, "Key" });
        CHECK_NOTHROW (edited.connect (eq, 0, edited.box, 1));

        const auto inner = edited.add (nodetype::gain, edited.box);
        CHECK_NOTHROW (edited.connect (in, 1, inner, 0, edited.box));

        addPort ("input", "events", "Notes");
        CHECK_THROWS_AS (edited.connect (in, 2, inner, 0, edited.box), CommandError);
        CHECK_THROWS_AS (edited.connect (eq, 0, edited.box, 2), CommandError);
    }

    SECTION ("renaming")
    {
        edited.project = apply (edited.project, "renameComponentPort",
                                { { "trackId", edited.trackId },
                                  { "nodeId", edited.box },
                                  { "side", "output" },
                                  { "index", 0 },
                                  { "name", "Wet" } });
        CHECK (edited.inside (edited.box).outputs[0].name == "Wet");
    }

    SECTION ("removing takes its cables, inside and out, and the later ports move up")
    {
        addPort ("input", "audio", "Second");
        addPort ("input", "audio", "Third");

        const auto inner = edited.add (nodetype::gain, edited.box);
        edited.connect (in, 2, inner, 0, edited.box);
        edited.connect (inner, 0, out, 0, edited.box);
        edited.connect (eq, 0, edited.box, 1);
        edited.connect (eq, 0, edited.box, 2);

        const auto cabled = [&] (Connection cable)
        {
            const auto& cables = edited.track().graph.connections;
            return std::ranges::find (cables, cable) != cables.end();
        };
        REQUIRE (cabled ({ eq, 0, edited.box, 2 }));

        edited.project =
            apply (edited.project, "removeComponentPort",
                   { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "side", "input" }, { "index", 1 } });

        const auto& inside = edited.inside (edited.box);
        REQUIRE (inside.inputs.size() == 2);
        CHECK (inside.inputs[1].name == "Third");
        CHECK (std::ranges::find (inside.graph.connections, Connection { in, 1, inner, 0 })
               != inside.graph.connections.end());
        CHECK (cabled ({ eq, 0, edited.box, 1 }));
        CHECK (! cabled ({ eq, 0, edited.box, 2 }));

        // And an output's.
        edited.connect (edited.box, 0, gain, 0);
        edited.project =
            apply (edited.project, "removeComponentPort",
                   { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "side", "output" }, { "index", 0 } });
        CHECK (edited.inside (edited.box).outputs.empty());
        CHECK (! cabled ({ edited.box, 0, gain, 0 }));
        CHECK (std::ranges::none_of (edited.inside (edited.box).graph.connections,
                                     [out] (const Connection& c) { return c.toNode == out; }));
    }

    SECTION ("removing a port a modulation cable came in by drops that slot")
    {
        addPort ("input", "modulation", "Dark");
        const auto darkened = edited.add (nodetype::filter, edited.box);
        edited.connect (in, 1, darkened, 1, edited.box);
        edited.project = apply (edited.project, "setModulation",
                                { { "trackId", edited.trackId },
                                  { "nodeId", darkened },
                                  { "port", 1 },
                                  { "param", "cutoff" },
                                  { "depth", 0.1 } });
        REQUIRE (edited.track().findNode (darkened)->modulations.contains (1));

        edited.project =
            apply (edited.project, "removeComponentPort",
                   { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "side", "input" }, { "index", 1 } });
        CHECK_FALSE (edited.track().findNode (darkened)->modulations.contains (1));
    }

    SECTION ("ports that aren't there, or nodes that aren't components")
    {
        CHECK_THROWS_AS (
            apply (edited.project, "removeComponentPort",
                   { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "side", "input" }, { "index", 7 } }),
            CommandError);
        CHECK_THROWS_AS (
            apply (edited.project, "addComponentPort",
                   { { "trackId", edited.trackId }, { "nodeId", eq }, { "side", "input" }, { "type", "audio" } }),
            CommandError);
    }

    SECTION ("a cable closing a loop through a box gets a Feedback node, and only then")
    {
        edited.connect (eq, 0, edited.box, 0);
        edited.connect (edited.box, 0, gain, 0);

        const auto drag = [&] (ObjectId from, ObjectId to)
        {
            return apply (edited.project, "connectOrCloseLoop",
                          { { "trackId", edited.trackId },
                            { "fromNode", from },
                            { "fromPort", 0 },
                            { "toNode", to },
                            { "toPort", 0 },
                            { "x", 0 },
                            { "y", 0 } });
        };

        const auto looped = drag (gain, edited.box);
        const auto& nodes = looped.tracks[0]->graph.nodes;
        CHECK (nodes.back().type == nodetype::feedback);

        const auto before = edited.track().graph.nodes.size();
        const auto plain = drag (edited.chainNode (nodetype::compressor), edited.box);
        CHECK (plain.tracks[0]->graph.nodes.size() == before);

        // What's refused for another reason stays refused.
        CHECK_THROWS_AS (drag (eq, edited.box), CommandError);
    }
}

namespace
{
// A real track playing a note: Synth -> EQ -> Mix -> Saturator (with latency) -> Compressor -> Gain, a loop from the Saturator through a Feedback
// node to the Mix, a dry path past the Saturator, and an LFO on the EQ's neighbour, a Filter.
struct Song
{
    Song()
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        project = apply (setup.project, "addNotes",
                         { { "clipId", setup.clipId },
                           { "notes", juce::var (juce::Array<juce::var> { noteArgs (0, ticksPerBeat, 57.0) }) } });

        const auto chain = [this] (const char* type)
        {
            const auto& nodes = project.tracks[0]->graph.nodes;
            return std::ranges::find (nodes, juce::String (type), &GraphNode::type)->id;
        };

        synth = chain (nodetype::mpeSynth);
        eq = chain (nodetype::eq);
        compressorNode = chain (nodetype::compressor);
        gainNode = chain (nodetype::gain);

        project = apply (project, "setNodeBypass",
                         { { "trackId", trackId }, { "nodeId", compressorNode }, { "bypassed", false } });

        mixer = add (nodetype::mix);
        lowpass = add (nodetype::filter);
        saturatorNode = add (nodetype::saturator);
        echo = add (nodetype::feedback);
        lfoNode = add (nodetype::lfo);

        set (lowpass, "cutoff", 3000.0);
        set (saturatorNode, "oversampling", 1.0);
        set (echo, "time", 20.0);
        set (echo, "amount", 60.0);
        set (lfoNode, "rateMs", 100.0);

        disconnect (eq, compressorNode);
        connect (eq, 0, mixer, 0);
        connect (mixer, 0, lowpass, 0);
        connect (lowpass, 0, saturatorNode, 0);
        connect (saturatorNode, 0, compressorNode, 0);
        connect (saturatorNode, 0, echo, 0);
        connect (echo, 0, mixer, 1);
        connect (lfoNode, 0, lowpass, 1);
        project = apply (
            project, "setModulation",
            { { "trackId", trackId }, { "nodeId", lowpass }, { "port", 1 }, { "param", "cutoff" }, { "depth", 0.02 } });
    }

    ObjectId add (const char* type)
    {
        const auto next = project.nextId;
        project = apply (
            project, "addNode",
            { { "trackId", trackId }, { "type", type }, { "x", 100 + 200 * static_cast<int> (next) }, { "y", 50 } });
        return next;
    }

    void set (ObjectId node, const char* param, double value)
    {
        project = apply (project, "setNodeParam",
                         { { "trackId", trackId }, { "nodeId", node }, { "param", param }, { "value", value } });
    }

    void connect (ObjectId from, int fromPort, ObjectId to, int toPort)
    {
        project = apply (project, "connect",
                         { { "trackId", trackId },
                           { "fromNode", from },
                           { "fromPort", fromPort },
                           { "toNode", to },
                           { "toPort", toPort } });
    }

    void disconnect (ObjectId from, ObjectId to)
    {
        project = apply (
            project, "disconnect",
            { { "trackId", trackId }, { "fromNode", from }, { "fromPort", 0 }, { "toNode", to }, { "toPort", 0 } });
    }

    [[nodiscard]] Project grouped (std::initializer_list<ObjectId> ids) const
    {
        juce::Array<juce::var> list;

        for (const auto id : ids)
            list.add (id);

        return apply (project, "groupNodes", { { "trackId", trackId }, { "nodeIds", list } });
    }

    [[nodiscard]] static std::vector<float> played (const Project& from)
    {
        GraphCompiler compiler;
        AudioEngine engine;
        engine.setEngine (compiler.compile (from, spec));
        engine.play();
        return render (engine, 24000).left();
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId synth = 0;
    ObjectId eq = 0;
    ObjectId compressorNode = 0;
    ObjectId gainNode = 0;
    ObjectId mixer = 0;
    ObjectId lowpass = 0;
    ObjectId saturatorNode = 0;
    ObjectId echo = 0;
    ObjectId lfoNode = 0;
};

// What a graph's cables are, whatever their order.
[[nodiscard]] std::vector<std::tuple<ObjectId, int, ObjectId, int>> sorted (const TrackGraph& graph)
{
    std::vector<std::tuple<ObjectId, int, ObjectId, int>> cables;

    for (const auto& c : graph.connections)
        cables.emplace_back (c.fromNode, c.fromPort, c.toNode, c.toPort);

    std::ranges::sort (cables);
    return cables;
}
} // namespace

TEST_CASE ("grouping and ungrouping with the commands doesn't change the sound", "[component][group]")
{
    const Song song;
    const auto plain = Song::played (song.project);
    REQUIRE (loudest (plain) > 0.01f);

    const auto nulls = [&] (const Project& project)
    { return largestDifference (Song::played (project), plain) < 1.0e-5f; };

    SECTION ("whatever is selected")
    {
        CHECK (nulls (song.grouped ({ song.lowpass })));
        CHECK (nulls (song.grouped ({ song.lowpass, song.lfoNode })));
        CHECK (nulls (song.grouped ({ song.mixer, song.lowpass, song.saturatorNode, song.echo })));
        CHECK (nulls (song.grouped ({ song.echo })));          // the loop crosses the edge
        CHECK (nulls (song.grouped ({ song.saturatorNode }))); // latency
        CHECK (nulls (song.grouped ({ song.synth, song.eq })));
        CHECK (nulls (song.grouped (
            { song.eq, song.mixer, song.lowpass, song.saturatorNode, song.echo, song.lfoNode, song.compressorNode })));
    }

    SECTION ("a group inside a group")
    {
        auto project = song.grouped ({ song.lowpass, song.saturatorNode });
        const auto box = project.tracks[0]->graph.nodes.back().id;
        juce::Array<juce::var> list;
        list.add (box);
        list.add (song.mixer);
        project = apply (project, "groupNodes", { { "trackId", song.trackId }, { "nodeIds", list } });
        CHECK (nulls (project));
    }

    SECTION ("ungrouping gives back the same cables, nodes and sound")
    {
        const auto before = sorted (song.project.tracks[0]->graph);

        for (const auto& ids :
             std::vector<std::vector<ObjectId>> { { song.lowpass, song.saturatorNode, song.echo, song.mixer },
                                                  { song.echo },
                                                  { song.lfoNode, song.lowpass } })
        {
            juce::Array<juce::var> list;

            for (const auto id : ids)
                list.add (id);

            const auto group = apply (song.project, "groupNodes", { { "trackId", song.trackId }, { "nodeIds", list } });
            const auto box = group.tracks[0]->graph.nodes.back().id;
            const auto back = apply (group, "ungroupNode", { { "trackId", song.trackId }, { "nodeId", box } });

            CHECK (sorted (back.tracks[0]->graph) == before);
            CHECK (back.tracks[0]->graph.nodes.size() == song.project.tracks[0]->graph.nodes.size());
            CHECK (back.tracks[0]->findNode (box) == nullptr);
            CHECK (nulls (back));

            // The nodes come back where they were.
            for (const auto id : ids)
            {
                CHECK_THAT (back.tracks[0]->findNode (id)->x,
                            WithinAbs (song.project.tracks[0]->findNode (id)->x, 0.01));
                CHECK_THAT (back.tracks[0]->findNode (id)->y,
                            WithinAbs (song.project.tracks[0]->findNode (id)->y, 0.01));
            }
        }
    }

    SECTION ("modulation stays, and so do automation lanes on what's inside")
    {
        auto project = apply (
            song.project, "addAutomationLane",
            { { "trackId", song.trackId },
              { "target", makeObject ({ { "kind", "node" }, { "nodeId", song.lowpass }, { "param", "cutoff" } }) } });
        juce::Array<juce::var> list;
        list.add (song.lowpass);
        project = apply (project, "groupNodes", { { "trackId", song.trackId }, { "nodeIds", list } });

        CHECK (project.tracks[0]->automation.size() == 1);
        CHECK (project.tracks[0]->findNode (song.lowpass)->modulations.contains (1));
    }
}

TEST_CASE ("what a group's ports are", "[component][group]")
{
    const Song song;

    SECTION ("an outside output feeding two nodes inside is one input, and an inside output feeding two outside is one "
             "output")
    {
        const auto project = song.grouped ({ song.lowpass, song.echo });
        const auto& box = project.tracks[0]->graph.nodes.back();
        REQUIRE (box.component != nullptr);

        // The Saturator feeds the Feedback node, the Mix the Filter, and the LFO the Filter's modulation.
        CHECK (box.component->inputs.size() == 3);
        CHECK (box.component->outputs.size() == 2);

        const auto modulation = std::ranges::find (box.component->inputs, PortType::modulation, &ComponentPort::type);
        REQUIRE (modulation != box.component->inputs.end());
        CHECK (modulation->name == "LFO");

        const auto oneOutput = song.grouped ({ song.saturatorNode });
        const auto& saturated = oneOutput.tracks[0]->graph.nodes.back();
        CHECK (saturated.component->outputs.size() == 1);
        CHECK (std::ranges::count_if (oneOutput.tracks[0]->graph.connections,
                                      [&] (const Connection& c) { return c.fromNode == saturated.id; })
               == 2);
    }

    SECTION ("the fixed nodes stay out")
    {
        const auto sink = std::ranges::find (song.project.tracks[0]->graph.nodes, juce::String (nodetype::trackOutput),
                                             &GraphNode::type)
                              ->id;
        CHECK_THROWS_AS (song.grouped ({ song.eq, sink }), CommandError);
        CHECK_THROWS_AS (song.grouped ({ 99999 }), CommandError);
        CHECK_THROWS_AS (apply (song.project, "groupNodes",
                                { { "trackId", song.trackId }, { "nodeIds", juce::Array<juce::var>() } }),
                         CommandError);
    }

    SECTION ("in a component, a group is one more level, and in a voice graph it stays per voice")
    {
        const auto outer = song.grouped ({ song.lowpass, song.saturatorNode });
        const auto box = outer.tracks[0]->graph.nodes.back().id;
        juce::Array<juce::var> list;
        list.add (song.lowpass);
        const auto inner =
            apply (outer, "groupNodes", { { "trackId", song.trackId }, { "voiceNodeId", box }, { "nodeIds", list } });
        const auto owner = inner.tracks[0]->graphOwnerOf (song.lowpass);
        CHECK (owner != box);
        CHECK (inner.tracks[0]->graphOwnerOf (owner) == box);
        CHECK (largestDifference (Song::played (inner), Song::played (song.project)) < 1.0e-5f);
    }

    SECTION ("a bypassed component can't be ungrouped")
    {
        auto project = song.grouped ({ song.lowpass });
        const auto box = project.tracks[0]->graph.nodes.back().id;
        project =
            apply (project, "setNodeBypass", { { "trackId", song.trackId }, { "nodeId", box }, { "bypassed", true } });
        CHECK_THROWS_AS (apply (project, "ungroupNode", { { "trackId", song.trackId }, { "nodeId", box } }),
                         CommandError);
        CHECK_THROWS_AS (apply (project, "ungroupNode", { { "trackId", song.trackId }, { "nodeId", song.eq } }),
                         CommandError);
    }

    SECTION ("a group survives a save and open")
    {
        const auto project = song.grouped ({ song.lowpass, song.echo });
        CHECK (juce::JSON::toString (toVar (roundTrip (project))) == juce::JSON::toString (toVar (project)));
    }
}

TEST_CASE ("a component has a name, and shows inner parameters", "[component][exposed]")
{
    Edited edited;
    const auto inner = edited.add (nodetype::component, edited.box);
    const auto eqInside = edited.add (nodetype::eq, edited.box);
    const auto deep = edited.add (nodetype::gain, inner);

    const auto expose = [&] (ObjectId target, const char* param)
    {
        edited.project = apply (
            edited.project, "exposeParam",
            { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "target", target }, { "param", param } });
    };
    const auto exposedOf = [&] (ObjectId id) { return edited.inside (id).exposed; };

    SECTION ("the name")
    {
        edited.project = apply (edited.project, "renameComponent",
                                { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "name", "Dub Echo" } });
        CHECK (edited.inside (edited.box).name == "Dub Echo");

        // A nested one finds its graph.
        edited.project = apply (edited.project, "renameComponent",
                                { { "trackId", edited.trackId }, { "nodeId", inner }, { "name", "Tone" } });
        CHECK (edited.inside (inner).name == "Tone");
    }

    SECTION ("a parameter takes its own name, and can be given another")
    {
        expose (deep, "gainDb");
        REQUIRE (exposedOf (edited.box).size() == 1);
        CHECK (exposedOf (edited.box)[0].name == "Gain");

        edited.project = apply (edited.project, "renameExposedParam",
                                { { "trackId", edited.trackId },
                                  { "nodeId", edited.box },
                                  { "target", deep },
                                  { "param", "gainDb" },
                                  { "name", "Dark" } });
        CHECK (exposedOf (edited.box)[0].name == "Dark");

        edited.project = apply (
            edited.project, "unexposeParam",
            { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "target", deep }, { "param", "gainDb" } });
        CHECK (exposedOf (edited.box).empty());
    }

    SECTION ("what isn't there, isn't inside, or is there already, is refused")
    {
        CHECK_THROWS_AS (expose (deep, "nonsense"), CommandError);
        CHECK_THROWS_AS (expose (edited.chainNode (nodetype::gain), "gainDb"), CommandError);
        expose (deep, "gainDb");
        CHECK_THROWS_AS (expose (deep, "gainDb"), CommandError);
    }

    SECTION ("it's the inner parameter: moving it changes the inner node, and needs no compilation")
    {
        expose (deep, "gainDb");
        GraphCompiler compiler;
        AudioEngine engine;
        engine.setEngine (compiler.compile (edited.project, spec));

        edited.project = apply (edited.project, "renameComponent",
                                { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "name", "A" } });
        CHECK (compiler.update (edited.project, spec) == nullptr);

        edited.project =
            apply (edited.project, "setNodeParam",
                   { { "trackId", edited.trackId }, { "nodeId", deep }, { "param", "gainDb" }, { "value", -12.0 } });
        CHECK (compiler.update (edited.project, spec) == nullptr);
        CHECK_THAT (edited.track().findNode (deep)->params.at ("gainDb"), WithinAbs (-12.0, 1.0e-9));
    }

    SECTION ("removing the node forgets it, whatever the depth, and ungrouping forgets them all")
    {
        expose (eqInside, "hpFreq");
        expose (deep, "gainDb");
        REQUIRE (exposedOf (edited.box).size() == 2);

        edited.project = apply (edited.project, "removeNode", { { "trackId", edited.trackId }, { "nodeId", deep } });
        REQUIRE (exposedOf (edited.box).size() == 1);
        CHECK (exposedOf (edited.box)[0].nodeId == eqInside);

        edited.project = apply (edited.project, "removeNode", { { "trackId", edited.trackId }, { "nodeId", inner } });
        edited.project =
            apply (edited.project, "ungroupNode", { { "trackId", edited.trackId }, { "nodeId", edited.box } });
        CHECK (edited.track().findNode (eqInside) != nullptr);
        CHECK (edited.track().findNode (edited.box) == nullptr);
    }

    SECTION ("a node that's grouped further inside stays shown")
    {
        expose (eqInside, "hpFreq");
        juce::Array<juce::var> list;
        list.add (eqInside);
        edited.project = apply (edited.project, "groupNodes",
                                { { "trackId", edited.trackId }, { "voiceNodeId", edited.box }, { "nodeIds", list } });
        REQUIRE (exposedOf (edited.box).size() == 1);
        CHECK (edited.track().graphOwnerOf (eqInside) != edited.box);
    }

    SECTION ("they survive a save and open, and a copy aims at its own nodes")
    {
        expose (deep, "gainDb");
        edited.project = apply (edited.project, "renameComponent",
                                { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "name", "Dub Echo" } });

        const auto loaded = roundTrip (edited.project);
        CHECK (juce::JSON::toString (toVar (loaded)) == juce::JSON::toString (toVar (edited.project)));
        const auto* node = loaded.tracks[0]->findNode (edited.box);
        REQUIRE (node != nullptr);
        CHECK (node->component->name == "Dub Echo");
        CHECK (node->component->exposed.size() == 1);

        const auto copied = apply (edited.project, "duplicateTrack", { { "id", edited.trackId } });
        const auto& copy = *copied.tracks[1];
        const GraphNode* box = nullptr;

        for (const auto& candidate : copy.graph.nodes)
            if (candidate.component != nullptr)
                box = &candidate;

        REQUIRE (box != nullptr);
        REQUIRE (box->component->exposed.size() == 1);
        CHECK (box->component->exposed[0].nodeId != deep);
        CHECK (copy.findNode (box->component->exposed[0].nodeId) != nullptr);
    }
}

TEST_CASE ("a saved component comes back as it was, with new ids and its plugins' state", "[component][saved]")
{
    Edited edited;
    const auto gain = edited.add (nodetype::gain, edited.box);
    auto pluginArgs = makeObject (
        { { "trackId", edited.trackId }, { "voiceNodeId", edited.box }, { "type", nodetype::pluginEffect } });
    pluginArgs.getDynamicObject()->setProperty ("plugin", makeObject ({ { "identifier", "x" }, { "name", "X" } }));
    const auto plugin = edited.project.nextId;
    edited.project = applyCommand (edited.project, "addNode", pluginArgs);

    const auto state = std::make_shared<const juce::MemoryBlock> ("\x01\x02\x03\x04", 4);
    edited.project = withPluginState (edited.project, edited.trackId, plugin, state);
    edited.project = apply (edited.project, "renameComponent",
                            { { "trackId", edited.trackId }, { "nodeId", edited.box }, { "name", "Mine" } });
    edited.project = apply (edited.project, "exposeParam",
                            { { "trackId", edited.trackId },
                              { "nodeId", edited.box },
                              { "target", gain },
                              { "param", "gainDb" },
                              { "name", "Level" } });

    // As it goes through a file.
    const auto saved = juce::JSON::parse (
        juce::JSON::toString (projectfile::savedComponentVar (*edited.track().findNode (edited.box))));

    const auto drop = [&] (const char* command, const juce::var& args)
    {
        args.getDynamicObject()->setProperty ("trackId", edited.trackId);
        return applyCommand (edited.project, command, args);
    };

    const auto boxIn = [&] (const Project& project)
    {
        const GraphNode* latest = nullptr;

        for (const auto& node : project.tracks[0]->graph.nodes)
            if (node.component != nullptr && node.id != edited.box)
                latest = &node;

        REQUIRE (latest != nullptr);
        return latest;
    };

    const auto check = [&] (const Project& project)
    {
        const auto* box = boxIn (project);
        CHECK (box->component->name == "Mine");
        REQUIRE (box->component->exposed.size() == 1);
        CHECK (box->component->exposed[0].name == "Level");
        CHECK (box->component->exposed[0].nodeId != gain);

        const auto* inner = project.tracks[0]->findNode (box->component->exposed[0].nodeId);
        REQUIRE (inner != nullptr);
        CHECK (inner->type == juce::String (nodetype::gain));

        // Each copy is independent, with its plugin's state.
        const GraphNode* copied = nullptr;
        forEachNode (box->component->graph,
                     [&] (const GraphNode& node, bool)
                     {
                         if (node.plugin.has_value())
                             copied = &node;
                     });
        REQUIRE (copied != nullptr);
        CHECK (copied->id != plugin);
        REQUIRE (copied->pluginState != nullptr);
        CHECK (*copied->pluginState == *state);
    };

    SECTION ("dropped on a graph")
    {
        juce::Array<juce::var> nodes;
        nodes.add (makeObject ({ { "type", nodetype::component }, { "component", saved } }));
        check (drop ("addNodes", makeObject ({ { "nodes", nodes } })));
    }

    SECTION ("put in a chain, with one audio input and output, and it joins the chain")
    {
        const auto project = applyCommand (edited.project, "removeNode",
                                           makeObject ({ { "trackId", edited.trackId }, { "nodeId", edited.box } }));
        const auto before = edited.project;
        edited.project = project;
        const auto inserted = drop (
            "insertNode", makeObject ({ { "type", nodetype::component }, { "component", saved }, { "index", 0 } }));
        const auto* box = boxIn (inserted);
        CHECK (box->component->name == "Mine");
        const auto& cables = inserted.tracks[0]->graph.connections;
        CHECK (std::ranges::any_of (cables, [box] (const Connection& c) { return c.toNode == box->id; }));
        CHECK (std::ranges::any_of (cables, [box] (const Connection& c) { return c.fromNode == box->id; }));
        edited.project = before;
    }

    SECTION ("an empty one in a chain gets its ports")
    {
        const auto project = applyCommand (edited.project, "removeNode",
                                           makeObject ({ { "trackId", edited.trackId }, { "nodeId", edited.box } }));
        const auto inserted = applyCommand (
            project, "insertNode",
            makeObject ({ { "trackId", edited.trackId }, { "type", nodetype::component }, { "index", 0 } }));
        const auto* box = boxIn (inserted);
        CHECK (box->component->inputs.size() == 1);
    }

    SECTION ("refused in a voice graph, with a plugin in it")
    {
        const auto voice = edited.add (nodetype::voiceInstrument);
        juce::Array<juce::var> nodes;
        nodes.add (makeObject ({ { "type", nodetype::component }, { "component", saved } }));
        CHECK_THROWS_AS (drop ("addNodes", makeObject ({ { "voiceNodeId", voice }, { "nodes", nodes } })),
                         CommandError);
    }

    SECTION ("a damaged one is refused")
    {
        juce::Array<juce::var> nodes;
        nodes.add (
            makeObject ({ { "type", nodetype::component }, { "component", makeObject ({ { "name", "Broken" } }) } }));
        CHECK_THROWS_AS (drop ("addNodes", makeObject ({ { "nodes", nodes } })), CommandError);
    }
}
