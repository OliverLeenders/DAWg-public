#include "TestUtils.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"
#include "dawg/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <map>
#include <numbers>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Phase 8: graph commands, free routing and the built-in effects (docs/design/phase8-node-graph.md).

namespace
{
constexpr int frames = 4096;
constexpr float level = 0.25f;

[[nodiscard]] const GraphNode& nodeOfType (const Track& track, const char* type)
{
    const auto it = std::ranges::find_if (track.graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
    REQUIRE (it != track.graph.nodes.end());
    return *it;
}

[[nodiscard]] juce::var list (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] juce::var nodeSpec (const char* type, double x = 0.0)
{
    return makeObject ({ { "type", type }, { "x", x }, { "y", 0.0 } });
}

[[nodiscard]] juce::var link (int from, int fromPort, int to, int toPort)
{
    return makeObject ({ { "from", from }, { "fromPort", fromPort }, { "to", to }, { "toPort", toPort } });
}

// A sine of the given frequency at 48 kHz.
[[nodiscard]] std::vector<float> sine (double frequency, float amplitude = 0.5f, size_t length = frames)
{
    std::vector<float> result (length);

    for (size_t i = 0; i < result.size(); ++i)
        result[i] = amplitude
                    * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequency
                                                    * static_cast<double> (i) / sampleRate));

    return result;
}

[[nodiscard]] float peak (const std::vector<float>& samples, size_t from = frames / 2)
{
    float result = 0.0f;

    for (auto i = from; i < samples.size(); ++i)
        result = std::max (result, std::abs (samples[i]));

    return result;
}

// Runs one node over whole buffers (in blocks), with parameters set by id.
struct NodeRun
{
    std::vector<float> left;
    std::vector<float> right;
};

[[nodiscard]] NodeRun runNode (const char* type, const std::map<juce::String, float>& values,
                               const std::vector<float>& inLeft, const std::vector<float>& inRight,
                               bool bypassed = false)
{
    auto node = NodeRegistry::builtin().create (type);
    REQUIRE (node != nullptr);

    for (const auto& [id, value] : values)
    {
        const auto index = node->descriptor().indexOfParam (id);
        REQUIRE (index >= 0);
        node->params().set (static_cast<size_t> (index), value);
    }

    GraphNode model;
    model.type = type;
    model.bypassed = bypassed;
    node->configure (model);
    node->prepare (spec);

    NodeRun result { std::vector<float> (inLeft.size()), std::vector<float> (inLeft.size()) };
    const std::vector<float> silence (static_cast<size_t> (blockSize));

    for (size_t offset = 0; offset < inLeft.size(); offset += blockSize)
    {
        const auto count = std::min (static_cast<size_t> (blockSize), inLeft.size() - offset);
        std::vector<AudioInput> inputs (node->descriptor().inputs.size(),
                                        AudioInput { silence.data(), silence.data() });
        inputs[0] = { inLeft.data() + offset, inRight.data() + offset };
        const std::array<AudioOutput, 1> outputs { AudioOutput { result.left.data() + offset,
                                                                 result.right.data() + offset } };

        ProcessContext context;
        context.numSamples = static_cast<int> (count);
        context.audioInputs = inputs;
        context.audioOutputs = outputs;
        node->process (context);
    }

    return result;
}

// One node instance processed buffer by buffer, so parameters can change between buffers.
struct TestRig
{
    TestRig (const char* type, const std::map<juce::String, float>& values)
        : node (NodeRegistry::builtin().create (type))
    {
        REQUIRE (node != nullptr);

        for (const auto& [id, value] : values)
            setNodeParam (id, value);

        GraphNode model;
        model.type = type;
        node->configure (model);
        node->prepare (spec);
    }

    void setNodeParam (const juce::String& id, float value)
    {
        const auto index = node->descriptor().indexOfParam (id);
        REQUIRE (index >= 0);
        node->params().set (static_cast<size_t> (index), value);
    }

    [[nodiscard]] NodeRun process (const std::vector<float>& input)
    {
        NodeRun result { std::vector<float> (input.size()), std::vector<float> (input.size()) };
        const std::vector<float> silence (static_cast<size_t> (blockSize));

        for (size_t offset = 0; offset < input.size(); offset += blockSize)
        {
            const auto count = std::min (static_cast<size_t> (blockSize), input.size() - offset);
            std::vector<AudioInput> inputs (node->descriptor().inputs.size(),
                                            AudioInput { silence.data(), silence.data() });
            inputs[0] = { input.data() + offset, input.data() + offset };
            const std::array<AudioOutput, 1> outputs { AudioOutput { result.left.data() + offset,
                                                                     result.right.data() + offset } };

            ProcessContext context;
            context.numSamples = static_cast<int> (count);
            context.audioInputs = inputs;
            context.audioOutputs = outputs;
            node->process (context);
        }

        return result;
    }

    std::unique_ptr<Node> node;
};

[[nodiscard]] TestRig testRig (const char* type, const std::map<juce::String, float>& values)
{
    return TestRig (type, values);
}

// An audio track that always monitors a constant input, so its output level is known.
struct GraphRig
{
    GraphRig()
        : input (frames, level)
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project = apply (project, "setTrackSwitches", { { "id", trackId }, { "inputChannel", 0 }, { "monitor", 2 } });
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);
    }

    [[nodiscard]] StereoBuffer render()
    {
        engine.setEngine (compiler.compile (project, spec));
        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (frames);
        engine.renderOffline (out.channels(), 2, frames, sampleRate, inputs.data(), 1);
        return out;
    }

    [[nodiscard]] const Track& track() const { return *project.tracks[0]; }

    std::vector<float> input;
    Project project;
    ObjectId trackId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Registry

TEST_CASE ("the registry describes the built-in effects for the UI", "[graph]")
{
    const auto& registry = NodeRegistry::builtin();

    for (const auto* type : { nodetype::eq, nodetype::filter, nodetype::saturator, nodetype::clipper,
                              nodetype::waveshaper, nodetype::compressor, nodetype::utility, nodetype::mix })
        REQUIRE (registry.find (type) != nullptr);

    CHECK (registry.find (nodetype::eq)->category == NodeCategory::effect);
    CHECK (registry.find (nodetype::mix)->inputs.size() == 4 + maxModulationInputs);
    CHECK (registry.find (nodetype::trackOutput)->category == NodeCategory::io);
    CHECK (registry.find (nodetype::mpeSynth)->category == NodeCategory::instrument);
    CHECK (registry.descriptors().size() >= 12);

    const auto filter = toVar (*registry.find (nodetype::filter));
    CHECK (filter["category"].toString() == "effect");
    CHECK (filter["params"][0]["kind"].toString() == "choice");
    CHECK (filter["params"][0]["choices"][1].toString() == "High-pass");
    CHECK (filter["params"][1]["unit"].toString() == "Hz");
    CHECK (filter["params"][4]["id"].toString() == "slope");
    CHECK (filter["params"][4]["kind"].toString() == "choice");
    CHECK (filter["params"][4]["choices"][0].toString() == "12 dB/oct");
    CHECK (filter["params"][4]["choices"][1].toString() == "24 dB/oct");
    CHECK (filter["params"][4]["choices"][2].toString() == "36 dB/oct");
    CHECK (filter["params"][4]["choices"][3].toString() == "48 dB/oct");
}

// ---------------------------------------------------------------------------------------------------------
// Commands

TEST_CASE ("addNodes adds a selection with its connections in one step", "[graph][commands]")
{
    GraphRig rig;
    const auto before = rig.track().graph.nodes.size();

    rig.edit ("addNodes", { { "trackId", rig.trackId },
                            { "nodes", list ({ nodeSpec (nodetype::eq), nodeSpec (nodetype::filter, 200) }) },
                            { "connections", list ({ link (0, 0, 1, 0) }) } });
    CHECK (rig.track().graph.nodes.size() == before + 2);
    CHECK (rig.track().graph.connections.back().fromNode == rig.track().graph.nodes[before].id);

    CHECK_THROWS_AS (apply (rig.project, "addNodes",
                            { { "trackId", rig.trackId },
                              { "nodes", list ({ nodeSpec (nodetype::eq) }) },
                              { "connections", list ({ link (0, 0, 3, 0) }) } }),
                     CommandError);

    // Pasted nodes keep their parameters (clamped) and bypass.
    const auto pasted =
        makeObject ({ { "type", nodetype::filter },
                      { "params", makeObject ({ { "cutoff", 250.0 }, { "drive", 99.0 }, { "nope", 1.0 } }) },
                      { "bypassed", true } });
    rig.edit ("addNodes", { { "trackId", rig.trackId }, { "nodes", list ({ pasted }) } });
    const auto& filter = rig.track().graph.nodes.back();
    CHECK_THAT (filter.params.at ("cutoff"), WithinAbs (250.0, 1e-9));
    CHECK_THAT (filter.params.at ("drive"), WithinAbs (24.0, 1e-9));
    CHECK_FALSE (filter.params.contains ("nope"));
    CHECK (filter.bypassed);
    CHECK_THROWS_AS (apply (rig.project, "addNodes",
                            { { "trackId", rig.trackId }, { "nodes", list ({ nodeSpec (nodetype::trackOutput) }) } }),
                     CommandError);
    CHECK_THROWS_AS (apply (rig.project, "addNode", { { "trackId", rig.trackId }, { "type", nodetype::mpeSynth } }),
                     CommandError);
}

TEST_CASE ("a cable keeps its bends in the project until it is disconnected", "[graph][commands][file]")
{
    GraphRig rig;
    const auto cable = rig.track().graph.connections.front();
    const auto ends = [&] (std::initializer_list<std::pair<const char*, juce::var>> more)
    {
        auto args = makeObject ({ { "trackId", rig.trackId },
                                  { "fromNode", cable.fromNode },
                                  { "fromPort", cable.fromPort },
                                  { "toNode", cable.toNode },
                                  { "toPort", cable.toPort } });

        for (const auto& [key, value] : more)
            args.getDynamicObject()->setProperty (key, value);

        return args;
    };
    const auto bends =
        list ({ makeObject ({ { "x", 140 }, { "y", 60 } }), makeObject ({ { "x", 140 }, { "y", 120.5 } }) });

    rig.project = applyCommand (rig.project, "setConnectionBends", ends ({ { "bends", bends } }));
    const auto& bent = rig.track().graph.connections.front();
    REQUIRE (bent.bends.size() == 2);
    CHECK (bent.bends[1] == CablePoint { 140.0f, 120.5f });
    // Still the same cable: bends are not part of what it connects.
    CHECK (bent == cable);
    CHECK_THROWS_AS (applyCommand (rig.project, "connect", ends ({})), CommandError);

    // Saved only where there are any, and loaded back.
    const auto json = toVar (rig.project);
    const auto file = makeObject ({ { "format", projectfile::formatName },
                                    { "version", projectfile::version },
                                    { "nextId", rig.project.nextId },
                                    { "project", json } });
    const auto loaded = projectfile::fromVar (file, [] (const juce::String&) { return nullptr; });
    CHECK (loaded.project.tracks[0]->graph.connections.front().bends == bent.bends);
    CHECK (juce::JSON::toString (toVar (loaded.project)) == juce::JSON::toString (json));
    CHECK (juce::JSON::toString (json).contains ("\"bends\""));

    // None clears them; too many, or a cable that isn't there, is refused.
    CHECK (
        applyCommand (rig.project, "setConnectionBends", ends ({})).tracks[0]->graph.connections.front().bends.empty());
    juce::Array<juce::var> many;

    for (size_t i = 0; i <= limits::maxCableBends; ++i)
        many.add (makeObject ({ { "x", 0 }, { "y", 0 } }));

    CHECK_THROWS_AS (applyCommand (rig.project, "setConnectionBends", ends ({ { "bends", many } })), CommandError);
    CHECK_THROWS_AS (applyCommand (rig.project, "setConnectionBends", ends ({ { "toPort", 7 }, { "bends", bends } })),
                     CommandError);

    // Disconnecting drops them: connected again, the cable is a curve.
    rig.edit ("disconnect", { { "trackId", rig.trackId },
                              { "fromNode", cable.fromNode },
                              { "fromPort", cable.fromPort },
                              { "toNode", cable.toNode },
                              { "toPort", cable.toPort } });
    rig.edit ("connect", { { "trackId", rig.trackId },
                           { "fromNode", cable.fromNode },
                           { "fromPort", cable.fromPort },
                           { "toNode", cable.toNode },
                           { "toPort", cable.toPort } });
    CHECK (rig.track().graph.connections.back().bends.empty());
}

TEST_CASE ("setting a cable's bends is one undo step", "[graph][commands][session]")
{
    Session session;
    REQUIRE (session.apply ("addTrack", makeObject ({ { "kind", "audio" } })).ok);
    const auto& track = *session.project()->tracks[0];
    const auto cable = track.graph.connections.front();
    const auto args = makeObject ({ { "trackId", track.id },
                                    { "fromNode", cable.fromNode },
                                    { "fromPort", cable.fromPort },
                                    { "toNode", cable.toNode },
                                    { "toPort", cable.toPort },
                                    { "bends", list ({ makeObject ({ { "x", 10 }, { "y", 20 } }) }) } });

    REQUIRE (session.apply ("setConnectionBends", args).ok);
    CHECK (session.project()->tracks[0]->graph.connections.front().bends.size() == 1);
    CHECK (session.undoLabel() == "Edit cable");
    REQUIRE (session.undo().ok);
    CHECK (session.project()->tracks[0]->graph.connections.front().bends.empty());
}

TEST_CASE ("addNode can put the new node in a cable's path", "[graph][commands]")
{
    GraphRig rig;
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::eq } });
    const auto a = rig.track().graph.nodes.back().id;
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::filter } });
    const auto b = rig.track().graph.nodes.back().id;
    rig.edit ("connect",
              { { "trackId", rig.trackId }, { "fromNode", a }, { "fromPort", 0 }, { "toNode", b }, { "toPort", 0 } });

    const auto into = [&] (ObjectId to)
    {
        return makeObject ({ { "fromNode", a },
                             { "fromPort", 0 },
                             { "toNode", to },
                             { "toPort", 0 },
                             { "input", 0 },
                             { "output", 0 } });
    };

    // A cable that doesn't exist changes nothing.
    CHECK_THROWS_AS (apply (rig.project, "addNode",
                            { { "trackId", rig.trackId }, { "type", nodetype::utility }, { "insertInto", into (a) } }),
                     CommandError);

    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::utility }, { "insertInto", into (b) } });
    const auto& graph = rig.track().graph;
    const auto c = graph.nodes.back().id;
    const auto has = [&] (ObjectId from, ObjectId to)
    {
        return std::ranges::any_of (graph.connections,
                                    [&] (const Connection& e) { return e.fromNode == from && e.toNode == to; });
    };

    CHECK (has (a, c));
    CHECK (has (c, b));
    CHECK_FALSE (has (a, b));
}

TEST_CASE ("addNodes checks pasted modulations like setModulation", "[graph][commands]")
{
    GraphRig rig;
    const auto filterSpec = [] (const juce::String& port, const char* param)
    {
        return makeObject (
            { { "type", nodetype::filter },
              { "modulations", makeObject ({ { port.toRawUTF8(), makeObject ({ { "param", param } }) } }) } });
    };
    const auto& ports = NodeRegistry::builtin().find (nodetype::filter)->inputs;
    const auto first = std::ranges::find_if (ports, [] (const PortInfo& p) { return p.type == PortType::modulation; });
    REQUIRE (first != ports.end());
    const auto port = juce::String (static_cast<int> (first - ports.begin()));
    const auto paste = [&] (const juce::var& pasted)
    { return apply (rig.project, "addNodes", { { "trackId", rig.trackId }, { "nodes", list ({ pasted }) } }); };

    CHECK_NOTHROW (paste (filterSpec (port, "cutoff")));
    CHECK_THROWS_AS (paste (filterSpec (port, "nope")), CommandError);
    CHECK_THROWS_AS (paste (filterSpec (port, "mode")), CommandError);  // a choice can't be modulated
    CHECK_THROWS_AS (paste (filterSpec ("0", "cutoff")), CommandError); // the audio input
    CHECK_THROWS_AS (paste (filterSpec ("99", "cutoff")), CommandError);
}

TEST_CASE ("addNodes keeps state that is not a parameter", "[graph][commands]")
{
    GraphRig rig;
    const auto shape = juce::Array<juce::var> {
        juce::Array<juce::var> { 0, 0.0 },
        juce::Array<juce::var> { modulationShapeTicks / 2, 1.0 },
        juce::Array<juce::var> { modulationShapeTicks, 0.0 },
    };
    const auto lfoSpec = makeObject ({ { "type", nodetype::lfo }, { "modulationShape", juce::var (shape) } });
    const auto filterSpec = makeObject (
        { { "type", nodetype::filter },
          { "modulations",
            makeObject (
                { { "1", makeObject ({ { "param", "cutoff" }, { "depth", 0.5 }, { "active", false } }) } }) } });
    const auto faustSpec = makeObject ({ { "type", nodetype::faust }, { "code", "process = _;" } });

    rig.edit ("addNodes", { { "trackId", rig.trackId }, { "nodes", list ({ lfoSpec, filterSpec, faustSpec }) } });

    const auto& nodes = rig.track().graph.nodes;
    const auto& lfo = nodes[nodes.size() - 3];
    REQUIRE (lfo.modulationShape.size() == 3);
    CHECK (lfo.modulationShape[1].time == modulationShapeTicks / 2);
    CHECK_THAT (lfo.modulationShape[1].value, WithinAbs (1.0, 1e-9));

    const auto& filter = nodes[nodes.size() - 2];
    REQUIRE (filter.modulations.contains (1));
    CHECK (filter.modulations.at (1).param == "cutoff");
    CHECK_THAT (filter.modulations.at (1).depth, WithinAbs (0.5, 1e-9));
    CHECK_FALSE (filter.modulations.at (1).active);

    CHECK (nodes.back().code == "process = _;");
}

TEST_CASE ("addNodes keeps an ADSR's handle parameters", "[graph][commands]")
{
    GraphRig rig;
    const auto adsrSpec = makeObject ({ { "type", nodetype::adsr },
                                        { "params", makeObject ({ { "attackHandleTime", 10.0 },
                                                                  { "attackHandleLevel", 20.0 },
                                                                  { "decayHandleTime", 30.0 },
                                                                  { "decayHandleLevel", 40.0 },
                                                                  { "releaseHandleTime", 60.0 },
                                                                  { "releaseHandleLevel", 70.0 } }) } });

    rig.edit ("addNodes", { { "trackId", rig.trackId }, { "nodes", list ({ adsrSpec }) } });

    const auto& adsr = rig.track().graph.nodes.back();
    CHECK_THAT (adsr.params.at ("attackHandleTime"), WithinAbs (10.0, 1e-9));
    CHECK_THAT (adsr.params.at ("attackHandleLevel"), WithinAbs (20.0, 1e-9));
    CHECK_THAT (adsr.params.at ("decayHandleTime"), WithinAbs (30.0, 1e-9));
    CHECK_THAT (adsr.params.at ("decayHandleLevel"), WithinAbs (40.0, 1e-9));
    CHECK_THAT (adsr.params.at ("releaseHandleTime"), WithinAbs (60.0, 1e-9));
    CHECK_THAT (adsr.params.at ("releaseHandleLevel"), WithinAbs (70.0, 1e-9));
}

TEST_CASE ("the ADSR's handles are ordinary parameters", "[graph][panels]")
{
    const auto* descriptor = NodeRegistry::builtin().find (nodetype::adsr);
    REQUIRE (descriptor != nullptr);

    // Presets save every descriptor parameter, so the handles need no separate preset state.
    for (const auto* id : { "attackHandleTime", "attackHandleLevel", "decayHandleTime", "decayHandleLevel",
                            "releaseHandleTime", "releaseHandleLevel" })
        CHECK (descriptor->indexOfParam (id) >= 0);
}

TEST_CASE ("removeNodes removes a selection but never fixed nodes", "[graph][commands]")
{
    GraphRig rig;
    rig.edit ("addNodes", { { "trackId", rig.trackId },
                            { "nodes", list ({ nodeSpec (nodetype::eq), nodeSpec (nodetype::mix) }) } });
    const auto eq = nodeOfType (rig.track(), nodetype::eq).id;
    const auto mix = nodeOfType (rig.track(), nodetype::mix).id;
    const auto output = nodeOfType (rig.track(), nodetype::trackOutput).id;

    CHECK_THROWS_AS (
        apply (rig.project, "removeNodes", { { "trackId", rig.trackId }, { "nodeIds", list ({ eq, output }) } }),
        CommandError);

    rig.edit ("removeNodes", { { "trackId", rig.trackId }, { "nodeIds", list ({ eq, mix }) } });
    CHECK (rig.track().graph.indexOfNode (eq) < 0);
    CHECK (rig.track().graph.indexOfNode (mix) < 0);
}

TEST_CASE ("a chain without its gain is still a chain", "[graph][commands]")
{
    GraphRig rig;
    const auto gain = nodeOfType (rig.track(), nodetype::gain).id;
    const auto player = nodeOfType (rig.track(), nodetype::audioPlayer).id;
    const auto eq = nodeOfType (rig.track(), nodetype::eq).id;
    const auto compressor = nodeOfType (rig.track(), nodetype::compressor).id;
    const auto output = nodeOfType (rig.track(), nodetype::trackOutput).id;
    rig.edit ("removeNode", { { "trackId", rig.trackId }, { "nodeId", gain } });

    // The neighbours are joined up, and inserts still go in before the output.
    CHECK (rig.track().graph.indexOfNode (gain) < 0);
    CHECK (rig.track().graph.connections
           == std::vector<Connection> { { player, 0, eq, 0 }, { eq, 0, compressor, 0 }, { compressor, 0, output, 0 } });
    rig.edit ("insertNode", { { "trackId", rig.trackId }, { "type", nodetype::utility } });
    const auto utility = nodeOfType (rig.track(), nodetype::utility).id;
    CHECK (
        rig.track().graph.connections
        == std::vector<Connection> {
            { player, 0, eq, 0 }, { eq, 0, compressor, 0 }, { compressor, 0, utility, 0 }, { utility, 0, output, 0 } });
}

TEST_CASE ("a graph without a chain shape is a custom one", "[graph][commands]")
{
    GraphRig rig;
    const auto gain = nodeOfType (rig.track(), nodetype::gain).id;
    const auto output = nodeOfType (rig.track(), nodetype::trackOutput).id;
    rig.edit (
        "disconnect",
        { { "trackId", rig.trackId }, { "fromNode", gain }, { "fromPort", 0 }, { "toNode", output }, { "toPort", 0 } });

    const auto plugin = makeObject ({ { "identifier", "x" }, { "name", "X" } });
    CHECK_THROWS_AS (apply (rig.project, "insertNode",
                            { { "trackId", rig.trackId }, { "type", nodetype::pluginEffect }, { "plugin", plugin } }),
                     CommandError);

    // Plugins can be placed anywhere with addNode. Unconnected, they still get an instance (so they load), but
    // never run.
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::pluginEffect }, { "plugin", plugin } });
    const auto& node = nodeOfType (rig.track(), nodetype::pluginEffect);
    CHECK (node.plugin.value_or (PluginRef {}).identifier == "x");

    const auto engine = rig.compiler.compile (rig.project, spec);
    CHECK (rig.compiler.findInstance (rig.trackId, node.id) != nullptr);
    CHECK (std::ranges::none_of (engine->tracks[0].steps, [&] (const CompiledStep& step)
                                 { return step.node == rig.compiler.findInstance (rig.trackId, node.id); }));
}

// ---------------------------------------------------------------------------------------------------------
// Routing

TEST_CASE ("parallel branches are summed where they meet", "[graph][engine]")
{
    GraphRig rig;
    const auto player = nodeOfType (rig.track(), nodetype::audioPlayer).id;
    const auto output = nodeOfType (rig.track(), nodetype::trackOutput).id;
    rig.edit ("removeNode", { { "trackId", rig.trackId }, { "nodeId", nodeOfType (rig.track(), nodetype::gain).id } });
    const auto compressor = nodeOfType (rig.track(), nodetype::compressor).id;
    rig.edit ("disconnect", { { "trackId", rig.trackId },
                              { "fromNode", compressor },
                              { "fromPort", 0 },
                              { "toNode", output },
                              { "toPort", 0 } });

    // Player -> Utility (0 dB) -> Output and Player -> Utility (-6 dB) -> Output.
    rig.edit ("addNodes", { { "trackId", rig.trackId },
                            { "nodes", list ({ nodeSpec (nodetype::utility), nodeSpec (nodetype::utility) }) } });
    const auto& nodes = rig.track().graph.nodes;
    const auto first = nodes[nodes.size() - 2].id;
    const auto second = nodes.back().id;

    for (const auto& [from, to] : std::array<std::pair<ObjectId, ObjectId>, 4> {
             { { player, first }, { player, second }, { first, output }, { second, output } } })
        rig.edit (
            "connect",
            { { "trackId", rig.trackId }, { "fromNode", from }, { "fromPort", 0 }, { "toNode", to }, { "toPort", 0 } });

    rig.edit ("setNodeParam",
              { { "trackId", rig.trackId }, { "nodeId", second }, { "param", "gain" }, { "value", -6 } });

    const auto out = rig.render();
    CHECK_THAT (out.left()[frames - 1], WithinAbs (level * (1.0 + std::pow (10.0, -6.0 / 20.0)), 1e-4));
}

TEST_CASE ("one sequencer can play several instruments", "[graph][engine]")
{
    auto setup = projectWithClip();
    auto project = apply (setup.project, "addNotes",
                          { { "clipId", setup.clipId }, { "notes", list ({ noteArgs (0, 4 * ticksPerBeat, 69) }) } });

    const auto renderPeak = [] (const Project& p)
    {
        GraphCompiler compiler;
        AudioEngine engine;
        engine.setEngine (compiler.compile (p, spec));
        engine.play();
        return peak (render (engine, frames).left());
    };

    const auto single = renderPeak (project);

    const auto& track = *project.tracks[0];
    const auto sequencer = nodeOfType (track, nodetype::sequencer).id;
    const auto gain = nodeOfType (track, nodetype::gain).id;
    project = apply (project, "addNode", { { "trackId", setup.trackId }, { "type", nodetype::mpeSynth } });
    const auto layer = project.tracks[0]->graph.nodes.back().id;

    for (const auto& [from, to] :
         std::array<std::pair<ObjectId, ObjectId>, 2> { { { sequencer, layer }, { layer, gain } } })
        project = apply (project, "connect",
                         { { "trackId", setup.trackId },
                           { "fromNode", from },
                           { "fromPort", 0 },
                           { "toNode", to },
                           { "toPort", 0 } });

    CHECK_THAT (renderPeak (project), WithinAbs (2.0 * single, 0.02 * single));
}

// ---------------------------------------------------------------------------------------------------------
// Effects

TEST_CASE ("the EQ boosts and cuts around its band frequencies", "[graph][effects]")
{
    const auto tone = sine (1000.0);
    const auto boosted =
        runNode (nodetype::eq, { { "mid1Freq", 1000.0f }, { "mid1Used", 1.0f }, { "mid1Gain", 12.0f } }, tone, tone);
    CHECK_THAT (peak (boosted.left), WithinAbs (0.5 * std::pow (10.0, 12.0 / 20.0), 0.02));

    const auto flat = runNode (nodetype::eq, {}, tone, tone);
    CHECK_THAT (peak (flat.left), WithinAbs (0.5, 0.01));

    const auto low = sine (50.0);
    const auto shelf =
        runNode (nodetype::eq, { { "lowFreq", 200.0f }, { "lowUsed", 1.0f }, { "lowGain", -12.0f } }, low, low);
    CHECK (peak (shelf.left) < 0.5f * 0.3f);

    // The third and fourth bells boost and cut around their frequencies like the first two.
    const auto high = sine (6000.0);
    const auto bell3 =
        runNode (nodetype::eq, { { "mid3Freq", 6000.0f }, { "mid3Used", 1.0f }, { "mid3Gain", 12.0f } }, high, high);
    CHECK_THAT (peak (bell3.left), WithinAbs (0.5 * std::pow (10.0, 12.0 / 20.0), 0.02));

    const auto top = sine (12000.0);
    const auto bell4 =
        runNode (nodetype::eq, { { "mid4Freq", 12000.0f }, { "mid4Used", 1.0f }, { "mid4Gain", -12.0f } }, top, top);
    CHECK_THAT (peak (bell4.left), WithinAbs (0.5 * 0.25, 0.01));
}

TEST_CASE ("the filter passes or removes content by mode", "[graph][effects]")
{
    const auto high = sine (8000.0);
    const auto low = sine (100.0);

    CHECK (peak (runNode (nodetype::filter, { { "cutoff", 500.0f } }, high, high).left) < 0.02f);
    CHECK_THAT (peak (runNode (nodetype::filter, { { "cutoff", 500.0f } }, low, low).left), WithinAbs (0.5, 0.03));
    CHECK (peak (runNode (nodetype::filter, { { "mode", 1.0f }, { "cutoff", 2000.0f } }, low, low).left) < 0.02f);
    // 8 kHz has six samples a period, whose peak depends on the phase: within what that allows.
    CHECK_THAT (peak (runNode (nodetype::filter, { { "mode", 1.0f }, { "cutoff", 2000.0f } }, high, high).left),
                WithinAbs (0.5, 0.07));
}

TEST_CASE ("the compressor follows its static curve", "[graph][effects]")
{
    CHECK_THAT (compressorGainChangeDb (-30.0f, -18.0f, 4.0f), WithinAbs (0.0, 1e-6));
    CHECK_THAT (compressorGainChangeDb (-6.0f, -18.0f, 4.0f), WithinAbs (-9.0, 1e-4));
    CHECK (compressorGainChangeDb (-18.0f, -18.0f, 4.0f) < 0.0f); // inside the soft knee

    // A steady tone 12 dB over the threshold settles at 9 dB of gain reduction (ratio 4).
    const auto tone = sine (1000.0, 0.5f); // about -6 dBFS peak
    const auto out = runNode (nodetype::compressor, { { "threshold", -18.0f }, { "attack", 1.0f } }, tone, tone);
    CHECK_THAT (juce::Decibels::gainToDecibels (peak (out.left)), WithinAbs (-15.0, 0.5));

    const auto dry = runNode (nodetype::compressor, { { "threshold", -18.0f }, { "mix", 0.0f } }, tone, tone);
    CHECK_THAT (peak (dry.left), WithinAbs (0.5, 1e-3));
}

TEST_CASE ("the utility pans, narrows and flips phase", "[graph][effects]")
{
    const std::vector<float> left (frames, 0.4f);
    const std::vector<float> right (frames, 0.2f);

    const auto mono = runNode (nodetype::utility, { { "mono", 1.0f } }, left, right);
    CHECK_THAT (mono.left.back(), WithinAbs (0.3, 1e-4));
    CHECK_THAT (mono.right.back(), WithinAbs (0.3, 1e-4));

    const auto flipped = runNode (nodetype::utility, { { "invertRight", 1.0f } }, left, right);
    CHECK_THAT (flipped.right.back(), WithinAbs (-0.2, 1e-4));

    const auto panned = runNode (nodetype::utility, { { "pan", -100.0f }, { "gain", -6.0f } }, left, right);
    CHECK_THAT (panned.left.back(), WithinAbs (0.4 * std::pow (10.0, -6.0 / 20.0), 1e-4));
    CHECK_THAT (panned.right.back(), WithinAbs (0.0, 1e-6));
}

TEST_CASE ("the mix node sums its inputs with their gains", "[graph][effects]")
{
    const std::vector<float> ones (frames, 1.0f);
    const auto out = runNode (nodetype::mix, { { "gain1", -6.0f } }, ones, ones);
    CHECK_THAT (out.left.back(), WithinAbs (std::pow (10.0, -6.0 / 20.0), 1e-4)); // the other inputs are silent
}

TEST_CASE ("bypassed effects pass their input through", "[graph][effects]")
{
    const auto tone = sine (8000.0);
    const auto out = runNode (nodetype::filter, { { "cutoff", 100.0f } }, tone, tone, true);
    CHECK (out.left == tone);
}

// ---------------------------------------------------------------------------------------------------------
// Device panels (docs/design/phase11-faust-panels.md, P1 and P2)

namespace
{
// The gain in dB of a sine passed through a node, measured in the second half.
[[nodiscard]] double measuredGainDb (const char* type, const std::map<juce::String, float>& values, double frequency)
{
    const auto in = sine (frequency, 0.25f);
    const auto out = runNode (type, values, in, in);
    return 20.0 * std::log10 (static_cast<double> (peak (out.left)) / static_cast<double> (peak (in)));
}

[[nodiscard]] double curveAt (const juce::var& curve, double x)
{
    // The point nearest to x.
    const auto& xs = *curve["x"].getArray();
    int best = 0;

    for (int i = 1; i < xs.size(); ++i)
        if (std::abs (static_cast<double> (xs[i]) - x) < std::abs (static_cast<double> (xs[best]) - x))
            best = i;

    return static_cast<double> (curve["y"][best]);
}

// The same for one band's own curve, which shares the summed curve's x grid.
[[nodiscard]] double bandCurveAt (const juce::var& curve, int band, double x)
{
    const auto& xs = *curve["x"].getArray();
    const auto& ys = *curve["bands"][band].getArray();
    int best = 0;

    for (int i = 1; i < xs.size(); ++i)
        if (std::abs (static_cast<double> (xs[i]) - x) < std::abs (static_cast<double> (xs[best]) - x))
            best = i;

    return static_cast<double> (ys[best]);
}
} // namespace

TEST_CASE ("response curves come from the coefficients the effects run", "[graph][panels]")
{
    // A flat EQ is flat; a boosted band shows its gain.
    const auto flat = effectResponse (nodetype::eq, {}, sampleRate);
    REQUIRE (flat["x"].size() == responsePoints);
    CHECK (flat["bands"].size() == 8);
    CHECK_THAT (curveAt (flat, 1000.0), WithinAbs (0.0, 0.01));

    const auto boosted = effectResponse (
        nodetype::eq, { { "mid1Freq", 1000.0 }, { "mid1Used", 1.0 }, { "mid1Gain", 12.0 } }, sampleRate);
    CHECK_THAT (curveAt (boosted, 1000.0), WithinAbs (12.0, 0.3));

    // Bells 3 and 4 are bands of their own: their curves show the boost and cut where their handles sit.
    const auto bell3 =
        effectResponse (nodetype::eq, { { "mid3Freq", 6000.0 }, { "mid3Used", 1.0 }, { "mid3Gain", 6.0 } }, sampleRate);
    CHECK_THAT (curveAt (bell3, 6000.0), WithinAbs (6.0, 0.3));

    const auto bell4 = effectResponse (
        nodetype::eq, { { "mid4Freq", 12000.0 }, { "mid4Used", 1.0 }, { "mid4Gain", -6.0 } }, sampleRate);
    CHECK_THAT (curveAt (bell4, 12000.0), WithinAbs (-6.0, 0.3));

    const auto bells = effectResponse (nodetype::eq,
                                       { { "mid3Freq", 6000.0 },
                                         { "mid3Used", 1.0 },
                                         { "mid3Gain", 6.0 },
                                         { "mid4Freq", 12000.0 },
                                         { "mid4Used", 1.0 },
                                         { "mid4Gain", -6.0 } },
                                       sampleRate);
    CHECK_THAT (bandCurveAt (bells, 4, 6000.0), WithinAbs (6.0, 0.3));
    CHECK_THAT (bandCurveAt (bells, 5, 12000.0), WithinAbs (-6.0, 0.3));

    // The curve matches what the node does to a sine (frequencies off the curve's grid are close enough).
    const auto measured =
        measuredGainDb (nodetype::eq, { { "mid1Freq", 1000.0f }, { "mid1Used", 1.0f }, { "mid1Gain", 12.0f } }, 1500.0);
    CHECK_THAT (curveAt (boosted, 1500.0), WithinAbs (measured, 0.5));

    // A low-pass at 1 kHz without resonance (k = 2) is 6 dB down at the cutoff and passes low frequencies.
    const auto lowPass = effectResponse (nodetype::filter, { { "cutoff", 1000.0 }, { "resonance", 0.0 } }, sampleRate);
    CHECK_THAT (curveAt (lowPass, 20.0), WithinAbs (0.0, 0.05));
    CHECK_THAT (curveAt (lowPass, 1000.0), WithinAbs (-6.0, 0.3));
    const auto filtered = measuredGainDb (nodetype::filter, { { "cutoff", 1000.0f }, { "resonance", 0.0f } }, 4000.0);
    CHECK_THAT (curveAt (lowPass, 4000.0), WithinAbs (filtered, 0.6));

    // The compressor's transfer curve: 4:1 above -20 dB, with makeup.
    const auto compressor = effectResponse (
        nodetype::compressor, { { "threshold", -20.0 }, { "ratio", 4.0 }, { "makeup", 3.0 } }, sampleRate);
    CHECK_THAT (curveAt (compressor, 0.0), WithinAbs (-15.0 + 3.0, 0.01));
    CHECK_THAT (curveAt (compressor, -40.0), WithinAbs (-40.0 + 3.0, 0.01));

    CHECK (effectResponse (nodetype::utility, {}, sampleRate).isVoid());
}

TEST_CASE ("a high-Q response is sampled through its peak", "[graph][panels]")
{
    // A +12 dB bell at 1 kHz with Q 10 is narrower than the fixed grid's 2.7% steps: without extra points the curve
    // both missed the peak's height and had only a few corners to draw between. The engine now adds log-spaced points
    // around each running band, with one exactly on its centre.
    const auto curve = effectResponse (
        nodetype::eq, { { "mid1Freq", 1000.0 }, { "mid1Used", 1.0 }, { "mid1Gain", 12.0 }, { "mid1Q", 10.0 } },
        sampleRate);
    CHECK_THAT (curveAt (curve, 1000.0), WithinAbs (12.0, 0.1));

    // Inside the peak's skirt no two samples are more than 0.5% apart in frequency.
    const auto& xs = *curve["x"].getArray();
    auto widest = 0.0;
    for (int i = 1; i < xs.size(); ++i)
    {
        const auto low = static_cast<double> (xs[i - 1]);
        const auto high = static_cast<double> (xs[i]);

        if (low > 900.0 && high < 1100.0)
            widest = std::max (widest, high / low - 1.0);
    }

    CHECK (widest < 0.005);

    // The filter's resonance gets the same dense points around its cutoff: Q 25 (resonance 100%) is +28 dB there,
    // where the fixed grid's nearest sample would read about 23 dB.
    const auto resonant =
        effectResponse (nodetype::filter, { { "cutoff", 1000.0 }, { "resonance", 100.0 } }, sampleRate);
    CHECK (curveAt (resonant, 1000.0) > 27.0);
}

TEST_CASE ("an analyser tap copies a node's output while it is on", "[graph][panels]")
{
    GraphRig rig;
    rig.edit ("insertNode", { { "trackId", rig.trackId }, { "type", nodetype::compressor } });
    const auto nodeId = rig.track().graph.nodes.back().id; // not the track's default (bypassed) compressor

    std::ignore = rig.render();
    auto* node = rig.compiler.findInstance (rig.trackId, nodeId);
    REQUIRE (node != nullptr);
    CHECK (node->activeTap() == nullptr);

    node->setTapEnabled (true);
    REQUIRE (node->activeTap() != nullptr);
    rig.engine.renderOffline (StereoBuffer (frames).channels(), 2, frames, sampleRate,
                              std::array<const float*, 1> { rig.input.data() }.data(), 1);

    // The compressor reports its input level and gain reduction (threshold -18 dB, input -12 dB), and the tap holds
    // its (compressed) output.
    const auto meters = node->meters();
    CHECK_THAT (static_cast<double> (meters["input"]), WithinAbs (20.0 * std::log10 (level), 0.1));
    CHECK (static_cast<double> (meters["reduction"]) < -1.0);

    // The history holds an [input, output, reduction, key] quadruple per 10 ms slot rendered since the last read (the
    // key is the detector's level: here the input's), and the peaks are taken over it. A read with no new slots keeps
    // the last peaks.
    const auto* history = meters["history"].getArray();
    REQUIRE (history != nullptr);
    REQUIRE (history->size() % 4 == 0);
    CHECK (history->size() / 4 >= 2 * frames / static_cast<int> (sampleRate * 0.01) - 1);
    auto mostReduction = 0.0;
    for (int i = 0; i < history->size(); i += 4)
    {
        CHECK_THAT (static_cast<double> ((*history)[i]), WithinAbs (20.0 * std::log10 (level), 0.1));
        CHECK_THAT (static_cast<double> ((*history)[i + 3]), WithinAbs (20.0 * std::log10 (level), 0.1));
        mostReduction = std::min (mostReduction, static_cast<double> ((*history)[i + 2]));
    }
    CHECK_THAT (mostReduction, WithinAbs (static_cast<double> (meters["reduction"]), 1.0e-6));

    const auto again = node->meters();
    CHECK (again["history"].getArray()->isEmpty());
    CHECK_THAT (static_cast<double> (again["reduction"]),
                WithinAbs (static_cast<double> (meters["reduction"]), 1.0e-6));

    std::vector<float> left (1024);
    std::vector<float> right (1024);
    node->activeTap()->read (left, right);
    CHECK_THAT (20.0 * std::log10 (left.back()),
                WithinAbs (static_cast<double> ((*history)[history->size() - 3]), 0.1));
    CHECK_THAT (right.front(), WithinAbs (left.back(), 1.0e-4));

    node->setTapEnabled (false);
    CHECK (node->activeTap() == nullptr);
}

TEST_CASE ("EQ bands keep their analogue shape up to Nyquist", "[graph][effects]")
{
    // A peak at 15 kHz, +12 dB, Q 1 at 48 kHz: the matched design stays within 1 dB of the analogue prototype. The
    // bilinear (RBJ) design it replaced was 2.8 dB off at 10 kHz and 6 dB at 20 kHz, squeezed towards Nyquist.
    const auto analogueDb = [] (double f, double f0, double gainDb, double q)
    {
        const auto a = std::pow (10.0, gainDb / 40.0);
        const std::complex<double> s (0.0, f / f0);
        return 20.0 * std::log10 (std::abs ((s * s + s * (a / q) + 1.0) / (s * s + s / (a * q) + 1.0)));
    };
    const auto curve = effectResponse (
        nodetype::eq, { { "mid2Freq", 15000.0 }, { "mid2Used", 1.0 }, { "mid2Gain", 12.0 }, { "mid2Q", 1.0 } },
        sampleRate);

    for (const auto f : { 1000.0, 5000.0, 10000.0, 15000.0, 20000.0 })
        CHECK_THAT (curveAt (curve, f), WithinAbs (analogueDb (f, 15000.0, 12.0, 1.0), 1.0));

    // The shelves keep half their gain at their frequency and all of it far away.
    const auto shelves = effectResponse (nodetype::eq,
                                         { { "lowFreq", 100.0 },
                                           { "lowUsed", 1.0 },
                                           { "lowGain", -12.0 },
                                           { "highFreq", 12000.0 },
                                           { "highUsed", 1.0 },
                                           { "highGain", 6.0 } },
                                         sampleRate);
    CHECK_THAT (curveAt (shelves, 20.0), WithinAbs (-12.0, 0.5));
    CHECK_THAT (curveAt (shelves, 1000.0), WithinAbs (0.0, 0.3));
    CHECK_THAT (curveAt (shelves, 12000.0), WithinAbs (3.0, 0.5));
    CHECK_THAT (curveAt (shelves, 20000.0), WithinAbs (6.0, 0.6));
}

TEST_CASE ("EQ bands filter both channels, only the mid or only the side", "[graph][effects]")
{
    // A tone the same on both channels is all mid; one with the right channel inverted is all side.
    const auto tone = sine (1000.0);
    std::vector<float> inverted (tone.size());
    std::ranges::transform (tone, inverted.begin(), [] (float x) { return -x; });
    const auto boost = 0.5 * std::pow (10.0, 12.0 / 20.0);
    const auto bell = [] (float mode)
    {
        return std::map<juce::String, float> {
            { "mid1Freq", 1000.0f }, { "mid1Used", 1.0f }, { "mid1Gain", 12.0f }, { "mid1Mode", mode }
        };
    };

    // Mid: boosts the mid, leaves the side.
    const auto midOnMid = runNode (nodetype::eq, bell (1.0f), tone, tone);
    CHECK_THAT (peak (midOnMid.left), WithinAbs (boost, 0.02));
    CHECK_THAT (peak (midOnMid.right), WithinAbs (boost, 0.02));
    const auto midOnSide = runNode (nodetype::eq, bell (1.0f), tone, inverted);
    CHECK_THAT (peak (midOnSide.left), WithinAbs (0.5, 0.01));
    CHECK_THAT (peak (midOnSide.right), WithinAbs (0.5, 0.01));

    // Side: the other way round.
    const auto sideOnMid = runNode (nodetype::eq, bell (2.0f), tone, tone);
    CHECK_THAT (peak (sideOnMid.left), WithinAbs (0.5, 0.01));
    const auto sideOnSide = runNode (nodetype::eq, bell (2.0f), tone, inverted);
    CHECK_THAT (peak (sideOnSide.left), WithinAbs (boost, 0.02));
    CHECK_THAT (peak (sideOnSide.right), WithinAbs (boost, 0.02));

    // A band on both channels does the same beside a mid band (the EQ then runs on mid and side) as on its own.
    std::map<juce::String, float> both { { "mid2Freq", 1000.0f }, { "mid2Used", 1.0f }, { "mid2Gain", -6.0f } };
    auto withSideBand = both;
    withSideBand.insert ({ { "mid1Freq", 100.0f }, { "mid1Used", 1.0f }, { "mid1Gain", 12.0f }, { "mid1Mode", 2.0f } });
    const auto alone = runNode (nodetype::eq, both, tone, tone);
    const auto beside = runNode (nodetype::eq, withSideBand, tone, tone);
    CHECK_THAT (peak (beside.left), WithinAbs (peak (alone.left), 0.002));
    CHECK_THAT (peak (beside.right), WithinAbs (peak (alone.right), 0.002));

    // The panel gets the mid's curve as y and the side's as side, once a band is on one of them.
    const auto curve = effectResponse (
        nodetype::eq, { { "mid1Freq", 1000.0 }, { "mid1Used", 1.0 }, { "mid1Gain", 12.0 }, { "mid1Mode", 2.0 } },
        sampleRate);
    REQUIRE (curve.hasProperty ("side"));
    CHECK_THAT (curveAt (curve, 1000.0), WithinAbs (0.0, 0.01));
    CHECK_THAT (curveAt (makeObject ({ { "x", curve["x"] }, { "y", curve["side"] } }), 1000.0), WithinAbs (12.0, 0.3));
    CHECK_FALSE (
        effectResponse (nodetype::eq, { { "mid1Used", 1.0 }, { "mid1Gain", 12.0 } }, sampleRate).hasProperty ("side"));
}

TEST_CASE ("the EQ's high-pass and low-pass have 12 or 24 dB per octave, and bands switch off", "[graph][effects]")
{
    // Butterworth: 3 dB down at the frequency, then 12 or 24 dB per octave.
    const auto gentle = effectResponse (nodetype::eq, { { "hpUsed", 1.0 }, { "hpFreq", 200.0 } }, sampleRate);
    CHECK_THAT (curveAt (gentle, 200.0), WithinAbs (-3.0, 0.4));
    CHECK_THAT (curveAt (gentle, 50.0), WithinAbs (-24.0, 1.0));
    CHECK_THAT (curveAt (gentle, 2000.0), WithinAbs (0.0, 0.1));

    const auto steep =
        effectResponse (nodetype::eq, { { "hpUsed", 1.0 }, { "hpFreq", 200.0 }, { "hpSlope", 3.0 } }, sampleRate);
    CHECK_THAT (curveAt (steep, 200.0), WithinAbs (-3.0, 0.4));
    CHECK_THAT (curveAt (steep, 50.0), WithinAbs (-48.0, 1.5));

    const auto lowPass =
        effectResponse (nodetype::eq, { { "lpUsed", 1.0 }, { "lpFreq", 5000.0 }, { "lpSlope", 3.0 } }, sampleRate);
    CHECK_THAT (curveAt (lowPass, 5000.0), WithinAbs (-3.0, 0.5));
    CHECK_THAT (curveAt (lowPass, 500.0), WithinAbs (0.0, 0.1));
    CHECK (curveAt (lowPass, 20000.0) < -40.0);

    // A band in use but bypassed is flat, whatever its gain; so is one not in use.
    const auto off =
        effectResponse (nodetype::eq, { { "mid1Used", 1.0 }, { "mid1On", 0.0 }, { "mid1Gain", 12.0 } }, sampleRate);
    CHECK_THAT (curveAt (off, 500.0), WithinAbs (0.0, 0.01));
    const auto unused = effectResponse (nodetype::eq, { { "mid1Gain", 12.0 } }, sampleRate);
    CHECK_THAT (curveAt (unused, 500.0), WithinAbs (0.0, 0.01));

    // And the node does what the curve says.
    const auto measured = measuredGainDb (nodetype::eq, { { "hpUsed", 1.0f }, { "hpFreq", 200.0f } }, 100.0);
    CHECK_THAT (measured, WithinAbs (curveAt (gentle, 100.0), 0.5));
}

TEST_CASE ("the EQ's high-pass and low-pass have 6 to 48 dB per octave", "[graph][effects]")
{
    // Butterworth of order n (6n dB/octave): 3 dB down at the frequency, 10 log10 (1 + r^2n) down at r times or
    // 1/r times it, for the HP and the LP, with odd orders too.
    const auto butterworthDb = [] (double r, int order)
    { return -10.0 * std::log10 (1.0 + std::pow (r, 2.0 * order)); };

    for (int slope = 0; slope < 8; ++slope)
    {
        const auto order = slope + 1;
        CAPTURE (order);

        const auto highPass = effectResponse (
            nodetype::eq, { { "hpUsed", 1.0 }, { "hpFreq", 1000.0 }, { "hpSlope", static_cast<double> (slope) } },
            sampleRate);
        CHECK_THAT (curveAt (highPass, 1000.0), WithinAbs (-3.0, 0.4));
        CHECK_THAT (curveAt (highPass, 500.0), WithinAbs (butterworthDb (2.0, order), 0.6));
        CHECK_THAT (curveAt (highPass, 250.0), WithinAbs (butterworthDb (4.0, order), 1.0));
        CHECK_THAT (curveAt (highPass, 8000.0), WithinAbs (0.0, 0.1));

        const auto lowPass = effectResponse (
            nodetype::eq, { { "lpUsed", 1.0 }, { "lpFreq", 1000.0 }, { "lpSlope", static_cast<double> (slope) } },
            sampleRate);
        CHECK_THAT (curveAt (lowPass, 1000.0), WithinAbs (-3.0, 0.4));
        CHECK_THAT (curveAt (lowPass, 2000.0), WithinAbs (butterworthDb (2.0, order), 0.6));
        CHECK_THAT (curveAt (lowPass, 125.0), WithinAbs (0.0, 0.1));
    }

    // The node does what the curve says at the steepest slope.
    const auto steepest =
        effectResponse (nodetype::eq, { { "hpUsed", 1.0 }, { "hpFreq", 1000.0 }, { "hpSlope", 7.0 } }, sampleRate);
    const auto measured =
        measuredGainDb (nodetype::eq, { { "hpUsed", 1.0f }, { "hpFreq", 1000.0f }, { "hpSlope", 7.0f } }, 700.0);
    CHECK_THAT (measured, WithinAbs (curveAt (steepest, 700.0), 0.5));

    // Switching slopes does not produce an explosive transient from stale section state.
    auto rig = testRig (nodetype::eq, { { "hpUsed", 1.0f }, { "hpFreq", 200.0f }, { "hpSlope", 1.0f } });
    std::ignore = rig.process (sine (100.0, 1.0, 512));
    rig.setNodeParam ("hpSlope", 2.0f);
    const auto afterSwitch = rig.process (std::vector<float> (512, 0.0f));
    CHECK (peak (afterSwitch.left, 0) < 1.0e-3);
    CHECK (peak (afterSwitch.right, 0) < 1.0e-3);
}

TEST_CASE ("the EQ's shelves take a Q", "[graph][effects]")
{
    // The analogue low shelf A (s^2 + sqrt A / Q s + A) / (A s^2 + sqrt A / Q s + 1): a higher Q dips just above a
    // boost's frequency and bumps below it.
    const auto analogueDb = [] (double f, double f0, double gainDb, double q)
    {
        const auto a = std::pow (10.0, gainDb / 40.0);
        const std::complex<double> s (0.0, f / f0);
        const auto root = std::sqrt (a) / q;
        return 20.0 * std::log10 (std::abs (a * (s * s + root * s + a) / (a * s * s + root * s + 1.0)));
    };

    for (const auto q : { 0.3, 0.7071, 2.0, 5.0 })
    {
        CAPTURE (q);
        const auto curve = effectResponse (
            nodetype::eq, { { "lowUsed", 1.0 }, { "lowFreq", 200.0 }, { "lowGain", 9.0 }, { "lowQ", q } }, sampleRate);

        for (const auto f : { 30.0, 100.0, 200.0, 300.0, 600.0, 3000.0 })
            CHECK_THAT (curveAt (curve, f), WithinAbs (analogueDb (f, 200.0, 9.0, q), 0.3));
    }

    // At the default Q the shelves are as before: half the gain at the frequency.
    const auto plain =
        effectResponse (nodetype::eq, { { "highUsed", 1.0 }, { "highFreq", 2000.0 }, { "highGain", 6.0 } }, sampleRate);
    CHECK_THAT (curveAt (plain, 2000.0), WithinAbs (3.0, 0.2));
    CHECK_THAT (curveAt (plain, 200.0), WithinAbs (0.0, 0.1));
}

TEST_CASE ("every EQ band can be any type: bell, shelf, high- or low-pass, notch or tilt", "[graph][effects]")
{
    // The types by their type parameter's value (eqTypes).
    constexpr double bell = 0.0;
    constexpr double highPass = 3.0;
    constexpr double lowPass = 4.0;
    constexpr double notch = 5.0;
    constexpr double tilt = 6.0;

    // A middle band as a high-pass: 3 dB down at its frequency, 12 dB/octave below.
    const auto passBand = effectResponse (
        nodetype::eq, { { "mid2Used", 1.0 }, { "mid2Type", highPass }, { "mid2Freq", 1000.0 } }, sampleRate);
    CHECK_THAT (curveAt (passBand, 1000.0), WithinAbs (-3.0, 0.4));
    CHECK_THAT (curveAt (passBand, 250.0), WithinAbs (-24.0, 1.0));

    // The first band (a high-pass by default) as a bell, with its own gain and Q.
    const auto bellBand = effectResponse (
        nodetype::eq, { { "hpUsed", 1.0 }, { "hpType", bell }, { "hpFreq", 500.0 }, { "hpGain", 9.0 }, { "hpQ", 2.0 } },
        sampleRate);
    CHECK_THAT (curveAt (bellBand, 500.0), WithinAbs (9.0, 0.3));
    CHECK_THAT (curveAt (bellBand, 5000.0), WithinAbs (0.0, 0.2));

    // The last band as a low-pass at 24 dB/octave, from its slope.
    const auto lowPassBand = effectResponse (
        nodetype::eq, { { "highUsed", 1.0 }, { "highType", lowPass }, { "highFreq", 2000.0 }, { "highSlope", 3.0 } },
        sampleRate);
    CHECK_THAT (curveAt (lowPassBand, 2000.0), WithinAbs (-3.0, 0.4));
    CHECK_THAT (curveAt (lowPassBand, 4000.0), WithinAbs (-24.3, 1.0));

    // A notch: deep at its frequency, untouched two octaves away, whatever its gain.
    const auto notchBand = effectResponse (
        nodetype::eq,
        { { "mid1Used", 1.0 }, { "mid1Type", notch }, { "mid1Freq", 1000.0 }, { "mid1Q", 4.0 }, { "mid1Gain", 0.0 } },
        sampleRate);
    CHECK (curveAt (notchBand, 1000.0) < -30.0);
    CHECK_THAT (curveAt (notchBand, 250.0), WithinAbs (0.0, 0.2));
    CHECK_THAT (curveAt (notchBand, 4000.0), WithinAbs (0.0, 0.2));

    // A tilt: half its gain cut at the bottom, half boosted at the top, flat at its frequency.
    const auto tiltBand = effectResponse (
        nodetype::eq, { { "mid3Used", 1.0 }, { "mid3Type", tilt }, { "mid3Freq", 1000.0 }, { "mid3Gain", 12.0 } },
        sampleRate);
    CHECK_THAT (curveAt (tiltBand, 20.0), WithinAbs (-6.0, 0.3));
    CHECK_THAT (curveAt (tiltBand, 1000.0), WithinAbs (0.0, 0.2));
    CHECK_THAT (curveAt (tiltBand, 20000.0), WithinAbs (6.0, 0.6));

    // And the node does what the curves say.
    const auto measured = measuredGainDb (
        nodetype::eq, { { "mid1Used", 1.0f }, { "mid1Type", 5.0f }, { "mid1Freq", 1000.0f }, { "mid1Q", 4.0f } },
        700.0);
    CHECK_THAT (measured, WithinAbs (curveAt (notchBand, 700.0), 0.5));
}

TEST_CASE ("the filter follows its analogue prototype up to Nyquist, in every mode", "[graph][effects]")
{
    // A low-pass at 12 kHz with some resonance (k = 1.02): the matched SVF stays close to the analogue curve, where
    // the bilinear one fell to nothing at Nyquist.
    const auto k = 2.0 - 1.96 * 0.5;
    const auto analogueDb = [k] (double f, double f0, std::complex<double> numerator)
    {
        const std::complex<double> s (0.0, f / f0);
        return 20.0 * std::log10 (std::abs (numerator / (s * s + k * s + 1.0)));
    };
    // Without oversampling, the matched design.
    const auto lowPass = effectResponse (
        nodetype::filter, { { "mode", 0.0 }, { "cutoff", 12000.0 }, { "resonance", 50.0 }, { "oversampling", 0.0 } },
        sampleRate);

    for (const auto f : { 1000.0, 6000.0, 12000.0, 18000.0, 22000.0 })
        CHECK_THAT (curveAt (lowPass, f), WithinAbs (analogueDb (f, 12000.0, 1.0), 1.0));

    // The node realises the curve in each mode, also near Nyquist.
    for (const auto mode : { 0.0f, 1.0f, 2.0f, 3.0f })
    {
        const std::map<juce::String, double> params { { "mode", mode }, { "cutoff", 9000.0 }, { "resonance", 50.0 } };
        const auto curve = effectResponse (nodetype::filter, params, sampleRate);

        for (const auto f : { 2000.0, 15000.0 })
        {
            const auto measured = measuredGainDb (
                nodetype::filter, { { "mode", mode }, { "cutoff", 9000.0f }, { "resonance", 50.0f } }, f);
            CHECK_THAT (measured, WithinAbs (curveAt (curve, f), 0.6));
        }
    }
}

TEST_CASE ("the filter realises selectable slopes with resonance and drive", "[graph][effects]")
{
    // A low-pass at 1 kHz: each slope increases steepness by 12 dB/oct (12, 24, 36, 48 dB/oct).
    for (int slope = 0; slope < 4; ++slope)
    {
        const auto curve = effectResponse (
            nodetype::filter,
            { { "mode", 0.0 }, { "cutoff", 1000.0 }, { "resonance", 0.0 }, { "slope", static_cast<double> (slope) } },
            sampleRate);

        // Low frequencies pass unattenuated.
        CHECK_THAT (curveAt (curve, 50.0), WithinAbs (0.0, 0.1));

        // Cutoff attenuation stays consistent (-6 dB).
        CHECK_THAT (curveAt (curve, 1000.0), WithinAbs (-6.0, 0.4));

        // Attenuation 2 octaves above cutoff (4000 Hz) gets progressively steeper.
        CHECK (curveAt (curve, 4000.0) < -18.0 * (slope + 1));

        // The node's actual measured output matches the response curve.
        const auto measured = measuredGainDb (
            nodetype::filter,
            { { "mode", 0.0f }, { "cutoff", 1000.0f }, { "resonance", 0.0f }, { "slope", static_cast<float> (slope) } },
            2000.0);
        CHECK_THAT (measured, WithinAbs (curveAt (curve, 2000.0), 0.6));
    }

    // High-pass mode steepness increases with slope.
    for (int slope = 0; slope < 4; ++slope)
    {
        const auto hpCurve = effectResponse (
            nodetype::filter,
            { { "mode", 1.0 }, { "cutoff", 2000.0 }, { "resonance", 0.0 }, { "slope", static_cast<double> (slope) } },
            sampleRate);
        // Each matched stage warps slightly near Nyquist, so the cascade accumulates a little error high up.
        CHECK_THAT (curveAt (hpCurve, 10000.0), WithinAbs (0.0, 0.5));
        CHECK_THAT (curveAt (hpCurve, 2000.0), WithinAbs (-6.0, 0.4));
        CHECK (curveAt (hpCurve, 500.0) < -18.0 * (slope + 1));
    }

    // Resonance peak height stays consistent and stable across slopes: the damping is redistributed between the
    // stages so the cascade peaks like the 12 dB/oct filter (1/k = 1/0.432 = 7.3 dB at 80 % resonance).
    for (int slope = 0; slope < 4; ++slope)
    {
        const auto resonant = effectResponse (
            nodetype::filter, { { "cutoff", 1000.0 }, { "resonance", 80.0 }, { "slope", static_cast<double> (slope) } },
            sampleRate);
        const auto peakGain = curveAt (resonant, 1000.0);
        CHECK_THAT (peakGain, WithinAbs (7.3, 0.3));
    }

    // At resonance 0 no slope rises above its passband, in either direction, and a low-pass only ever falls.
    for (int slope = 0; slope < 4; ++slope)
    {
        for (const auto mode : { 0.0, 1.0 })
        {
            const auto curve = effectResponse (nodetype::filter,
                                               { { "mode", mode },
                                                 { "cutoff", 1000.0 },
                                                 { "resonance", 0.0 },
                                                 { "slope", static_cast<double> (slope) } },
                                               sampleRate);
            const auto& gains = *curve["y"].getArray();

            for (int i = 0; i < gains.size(); ++i)
                CHECK (static_cast<double> (gains[i]) < 0.05);

            if (mode == 0.0)
                for (int i = 1; i < gains.size(); ++i)
                    CHECK (static_cast<double> (gains[i]) <= static_cast<double> (gains[i - 1]) + 1.0e-6);
        }

        // The node itself, where the bump was: about an octave below the cutoff.
        const auto measured = measuredGainDb (
            nodetype::filter,
            { { "mode", 0.0f }, { "cutoff", 1000.0f }, { "resonance", 0.0f }, { "slope", static_cast<float> (slope) } },
            500.0);
        CHECK (measured < 0.1);
    }

    // With resonance, the peak is near the cutoff's gain: within 1.4 dB at 48 dB/oct, where equal stages were 3 dB above it, well below the cutoff.
    for (int slope = 0; slope < 4; ++slope)
    {
        const auto curve = effectResponse (
            nodetype::filter, { { "cutoff", 1000.0 }, { "resonance", 80.0 }, { "slope", static_cast<double> (slope) } },
            sampleRate);
        const auto& gains = *curve["y"].getArray();
        double highest = -1000.0;

        for (int i = 0; i < gains.size(); ++i)
            highest = std::max (highest, static_cast<double> (gains[i]));

        INFO ("slope " << slope << " peaks at " << highest);
        CHECK (highest < curveAt (curve, 1000.0) + 1.5);
    }

    // Switching slopes does not produce an explosive transient from stale section state.
    auto rig = testRig (nodetype::filter, { { "cutoff", 500.0f }, { "slope", 0.0f } });
    std::ignore = rig.process (sine (200.0, 1.0, 512));
    rig.setNodeParam ("slope", 3.0f);
    const auto afterSwitch = rig.process (std::vector<float> (512, 0.0f));
    // The oversampler's halfband filters play out what they hold first, which a 48 dB/oct filter at 500 Hz rings out.
    CHECK (peak (afterSwitch.left, 0) < 1.0f);
    CHECK (peak (afterSwitch.left, 384) < 1.0e-3);
    CHECK (peak (afterSwitch.right, 384) < 1.0e-3);
}

TEST_CASE ("the filter meters report active parameters and modulated response curve", "[graph][panels]")
{
    GraphRig rig;
    rig.edit ("insertNode", { { "trackId", rig.trackId }, { "type", nodetype::filter } });
    const auto nodeId = rig.track().graph.nodes.back().id;

    std::ignore = rig.render();
    auto* node = rig.compiler.findInstance (rig.trackId, nodeId);
    REQUIRE (node != nullptr);

    const auto meters = node->meters();
    CHECK (meters.hasProperty ("cutoff"));
    CHECK (meters.hasProperty ("resonance"));
    CHECK (meters.hasProperty ("drive"));
    CHECK (meters.hasProperty ("slope"));
    CHECK (meters.hasProperty ("mode"));
    CHECK (meters.hasProperty ("isModulated"));
    CHECK_FALSE (meters.hasProperty ("curve")); // read for every voice, so only the panel gets the curve
    const auto panel = node->panelMeters();
    const auto expected = effectResponse (nodetype::filter, {}, sampleRate);
    CHECK (panel["curve"]["x"].size() == expected["x"].size());
    CHECK (panel["curve"]["y"].size() == expected["y"].size());
    CHECK_FALSE (static_cast<bool> (meters["isModulated"]));
}

TEST_CASE ("the filter meters follow an LFO modulating the cutoff", "[graph][panels][modulation]")
{
    GraphRig rig;
    rig.edit ("insertNode", { { "trackId", rig.trackId }, { "type", nodetype::filter } });
    const auto filter = rig.track().graph.nodes.back().id;
    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::lfo } });
    const auto lfo = rig.track().graph.nodes.back().id;
    rig.edit ("setNodeParam", { { "trackId", rig.trackId }, { "nodeId", lfo }, { "param", "sync" }, { "value", 0.0 } });
    rig.edit (
        "connect",
        { { "trackId", rig.trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", filter }, { "toPort", 1 } });
    rig.edit (
        "setModulation",
        { { "trackId", rig.trackId }, { "nodeId", filter }, { "port", 1 }, { "param", "cutoff" }, { "depth", 0.5 } });

    std::ignore = rig.render();

    auto* node = rig.compiler.findInstance (rig.trackId, filter);
    REQUIRE (node != nullptr);

    const auto meters = node->meters();
    CHECK (static_cast<bool> (meters["hasModulationInput"]));
    CHECK (static_cast<bool> (meters["isModulated"]));
    CHECK (std::abs (static_cast<double> (meters["cutoff"]) - 1000.0) > 1.0);
}

TEST_CASE ("a low-pass near Nyquist doesn't rise above its passband at low resonance, at any slope", "[graph][effects]")
{
    // At 20 kHz the 24 dB/oct low-pass used to rise 1.6 dB at the default resonance, and the 48 dB/oct one 4 dB: each
    // stage's numerator was matched at the cutoff, where its impulse-invariant poles peak more than the analogue's.
    // Oversampled (2x, 4x) the filter is bilinear at the high rate, which never rises above its prototype.
    for (const auto oversampling : { 0.0, 1.0, 2.0 })
        for (const auto slope : { 0.0, 1.0, 2.0, 3.0 })
            for (const auto resonance : { 0.0, 20.0 })
                for (const auto cutoff : { 16000.0, 18000.0, 20000.0 })
                {
                    const auto curve = effectResponse (nodetype::filter,
                                                       { { "mode", 0.0 },
                                                         { "cutoff", cutoff },
                                                         { "resonance", resonance },
                                                         { "slope", slope },
                                                         { "oversampling", oversampling } },
                                                       sampleRate);
                    auto highest = -100.0;

                    for (auto f = 100.0; f < 23500.0; f *= 1.02)
                        highest = std::max (highest, curveAt (curve, f));

                    INFO ("oversampling " << oversampling << ", slope " << slope << ", resonance " << resonance
                                          << ", cutoff " << cutoff);
                    CHECK (highest < 0.1);
                }

    // The node does what the curve says, high up too. Its RMS over the second half, not its peak: a sine this high
    // has three or four samples a period, which miss its peak by up to 1.25 dB.
    const auto rmsGainDb = [] (const std::map<juce::String, float>& values, double frequency)
    {
        const auto in = sine (frequency, 0.25f);
        const auto out = runNode (nodetype::filter, values, in, in);
        const auto rms = [] (const std::vector<float>& signal)
        {
            double sum = 0.0;

            for (auto i = signal.size() / 2; i < signal.size(); ++i)
                sum += static_cast<double> (signal[i]) * static_cast<double> (signal[i]);

            return std::sqrt (sum / static_cast<double> (signal.size() - signal.size() / 2));
        };
        return 20.0 * std::log10 (rms (out.left) / rms (in));
    };
    const std::map<juce::String, float> params {
        { "mode", 0.0f }, { "cutoff", 20000.0f }, { "resonance", 20.0f }, { "slope", 1.0f }
    };
    const auto curve = effectResponse (
        nodetype::filter, { { "mode", 0.0 }, { "cutoff", 20000.0 }, { "resonance", 20.0 }, { "slope", 1.0 } },
        sampleRate);

    for (const auto f : { 8000.0, 12000.0, 16000.0 })
        CHECK_THAT (rmsGainDb (params, f), WithinAbs (curveAt (curve, f), 0.3));
}

TEST_CASE ("an oversampled filter follows its analogue prototype up to 20 kHz", "[graph][effects]")
{
    // The prototype's stages: the Filter's damping per stage over s^2 + k s + 1.
    const auto analogueDb = [] (int mode, double f, double f0, double resonance, int stages)
    {
        const std::complex<double> s (0.0, f / f0);
        std::complex<double> h = 1.0;
        const std::array numerators { std::complex<double> (1.0), s * s, s, s * s + 1.0 };

        // Butterworth's proportions, scaled so the stages' dampings multiply to the single stage's (the Filter's).
        static constexpr std::array<std::array<double, 4>, 4> butterworth { {
            { 1.414213562, 0.0, 0.0, 0.0 },
            { 1.847759065, 0.765366865, 0.0, 0.0 },
            { 1.931851653, 1.414213562, 0.517638090, 0.0 },
            { 1.961570561, 1.662939224, 1.111140466, 0.390180644 },
        } };
        const auto single = std::max (0.04, 2.0 - 1.96 * resonance / 100.0);
        const auto scale = std::pow (single / std::numbers::sqrt2, 1.0 / static_cast<double> (stages));

        for (int stage = 0; stage < stages; ++stage)
        {
            const auto k = butterworth[static_cast<size_t> (stages - 1)][static_cast<size_t> (stage)] * scale;
            h *= numerators[static_cast<size_t> (mode)] / (s * s + k * s + 1.0);
        }

        return 20.0 * std::log10 (std::max (std::abs (h), 1.0e-9));
    };

    // Within a tolerance wherever the prototype is above -6 dB: the passband, the cutoff and the resonance. At 2x a cutoff
    // near 20 kHz is close enough to the high rate's Nyquist for the bilinear design's warping to show: a high- or
    // band-pass falls up to 1.5 dB faster just below it, 2 dB at 48 dB/oct, whose four stages each warp. 4x halves the
    // warping.
    for (const auto& [oversampling, tolerance] : { std::pair { 1.0, 2.0 }, std::pair { 2.0, 0.5 } })
        for (const auto mode : { 0, 1, 2 })
            for (const auto slope : { 0, 1, 3 })
                for (const auto cutoff : { 1000.0, 8000.0, 18000.0 })
                {
                    const auto curve = effectResponse (nodetype::filter,
                                                       { { "mode", static_cast<double> (mode) },
                                                         { "cutoff", cutoff },
                                                         { "resonance", 50.0 },
                                                         { "slope", static_cast<double> (slope) },
                                                         { "oversampling", oversampling } },
                                                       sampleRate);

                    for (auto f = 50.0; f <= 20000.0; f *= 1.05)
                    {
                        const auto analogue = analogueDb (mode, f, cutoff, 50.0, slope + 1);

                        if (analogue < -6.0)
                            continue;

                        INFO ("oversampling " << oversampling << ", mode " << mode << ", slope " << slope << ", cutoff "
                                              << cutoff << ", at " << f << " Hz");
                        CHECK_THAT (curveAt (curve, f), WithinAbs (analogue, tolerance));
                    }
                }

    // The node does what the curve says.
    const auto rmsGainDb = [] (const std::map<juce::String, float>& values, double frequency)
    {
        const auto in = sine (frequency, 0.25f);
        const auto out = runNode (nodetype::filter, values, in, in);
        const auto rms = [] (const std::vector<float>& signal)
        {
            double sum = 0.0;

            for (auto i = signal.size() / 2; i < signal.size(); ++i)
                sum += static_cast<double> (signal[i]) * static_cast<double> (signal[i]);

            return std::sqrt (sum / static_cast<double> (signal.size() - signal.size() / 2));
        };
        return 20.0 * std::log10 (rms (out.left) / rms (in));
    };

    for (const auto oversampling : { 1.0f, 2.0f })
    {
        const std::map<juce::String, float> params {
            { "cutoff", 12000.0f }, { "resonance", 60.0f }, { "slope", 1.0f }, { "oversampling", oversampling }
        };
        const auto curve = effectResponse (
            nodetype::filter,
            { { "cutoff", 12000.0 }, { "resonance", 60.0 }, { "slope", 1.0 }, { "oversampling", oversampling } },
            sampleRate);

        for (const auto f : { 1000.0, 9000.0, 12000.0, 15000.0 })
        {
            // The curve leaves out the halfband filters, which droop a little near the top.
            INFO ("oversampling " << oversampling << " at " << f << " Hz");
            CHECK_THAT (rmsGainDb (params, f), WithinAbs (curveAt (curve, f), 0.5));
        }
    }

    // It reports its latency: the halfband filters', a few samples (low enough for a filter in a feedback loop).
    for (const auto& [oversampling, latency] : { std::pair { 0.0f, 0 }, std::pair { 1.0f, 4 }, std::pair { 2.0f, 6 } })
    {
        auto node = NodeRegistry::builtin().create (nodetype::filter);
        node->prepare ({ sampleRate, 512 });
        GraphNode model;
        model.type = nodetype::filter;
        model.params["oversampling"] = oversampling;
        node->params().set (5, oversampling);
        node->configure (model);
        CHECK (node->latencySamples() == latency);
    }
}
