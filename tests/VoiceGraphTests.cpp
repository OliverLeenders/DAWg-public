#include "TestUtils.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"
#include "dawg/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <functional>
#include <map>
#include <set>

// The voice graph's model, commands and file (docs/design/phase12-voice-scope.md, V1, V4, V6, V16).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
[[nodiscard]] const GraphNode& nodeOfType (const TrackGraph& graph, const char* type)
{
    const auto it = std::ranges::find_if (graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
    REQUIRE (it != graph.nodes.end());
    return *it;
}

// A project with one instrument track holding a Voice Instrument beside its MPE Synth.
struct VoiceProject
{
    VoiceProject()
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        project = apply (setup.project, "addNode", { { "trackId", trackId }, { "type", nodetype::voiceInstrument } });
        voiceId = graph().nodes.back().id;
    }

    [[nodiscard]] const TrackGraph& graph() const { return project.tracks[0]->graph; }

    [[nodiscard]] const TrackGraph& voice() const
    {
        const auto& node = graph().nodes[static_cast<size_t> (graph().indexOfNode (voiceId))];
        REQUIRE (node.voiceGraph != nullptr);
        return *node.voiceGraph;
    }

    // Applies a graph command to the voice graph.
    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("trackId", trackId);
        object.getDynamicObject()->setProperty ("voiceNodeId", voiceId);
        project = applyCommand (project, command, object);
    }

    [[nodiscard]] bool fails (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args) const
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("trackId", trackId);
        object.getDynamicObject()->setProperty ("voiceNodeId", voiceId);

        try
        {
            std::ignore = applyCommand (project, command, object);
            return false;
        }
        catch (const CommandError&)
        {
            return true;
        }
    }

    Project project;
    ObjectId trackId = 0;
    ObjectId voiceId = 0;
};

// Every node id in a project's track graphs and voice graphs.
[[nodiscard]] std::vector<ObjectId> allNodeIds (const Project& project)
{
    std::vector<ObjectId> ids;

    for (const auto& track : project.tracks)
        for (const auto& node : track->graph.nodes)
        {
            ids.push_back (node.id);

            if (node.voiceGraph != nullptr)
                for (const auto& voiceNode : node.voiceGraph->nodes)
                    ids.push_back (voiceNode.id);
        }

    return ids;
}

[[nodiscard]] bool unique (const std::vector<ObjectId>& ids)
{
    return std::set<ObjectId> (ids.begin(), ids.end()).size() == ids.size();
}
} // namespace

TEST_CASE ("a new Voice Instrument has a playable default voice graph", "[voice][commands]")
{
    const VoiceProject setup;
    const auto& voice = setup.voice();

    std::vector<juce::String> types;
    types.reserve (voice.nodes.size());

    for (const auto& node : voice.nodes)
        types.push_back (node.type);

    CHECK (types
           == std::vector<juce::String> { nodetype::voiceInput, nodetype::noteExpression, nodetype::oscillator,
                                          nodetype::adsr, nodetype::gain, nodetype::voiceOutput });
    CHECK (voice.connections.size() == 6);

    // Note Expression's pitch drives the oscillator at 100 %; the ADSR opens the gain from 0 % by +100 %.
    const auto& oscillator = nodeOfType (voice, nodetype::oscillator);
    REQUIRE (oscillator.modulations.contains (0));
    CHECK (oscillator.modulations.at (0).param == "pitch");
    CHECK_THAT (oscillator.modulations.at (0).depth, WithinAbs (1.0, 1e-9));
    const auto& gain = nodeOfType (voice, nodetype::gain);
    CHECK_THAT (gain.params.at ("amplitude"), WithinAbs (0.0, 1e-9));
    CHECK_THAT (gain.modulations.at (1).depth, WithinAbs (0.5, 1e-9));

    // Node ids stay unique across the project.
    const auto ids = allNodeIds (setup.project);
    CHECK (unique (ids));
    CHECK (*std::ranges::max_element (ids) < setup.project.nextId);
}

TEST_CASE ("graph commands with a voiceNodeId edit the voice graph", "[voice][commands]")
{
    VoiceProject setup;
    const auto trackNodes = setup.graph().nodes.size();

    setup.edit ("addNode", { { "type", nodetype::filter }, { "x", 10 }, { "y", 20 } });
    REQUIRE (setup.voice().nodes.size() == 7);
    CHECK (setup.graph().nodes.size() == trackNodes);
    const auto filter = setup.voice().nodes.back().id;

    // Put the filter between the gain and the output.
    const auto gain = nodeOfType (setup.voice(), nodetype::gain).id;
    const auto output = nodeOfType (setup.voice(), nodetype::voiceOutput).id;
    setup.edit ("disconnect", { { "fromNode", gain }, { "fromPort", 0 }, { "toNode", output }, { "toPort", 0 } });
    setup.edit ("connect", { { "fromNode", gain }, { "fromPort", 0 }, { "toNode", filter }, { "toPort", 0 } });
    setup.edit ("connect", { { "fromNode", filter }, { "fromPort", 0 }, { "toNode", output }, { "toPort", 0 } });

    setup.edit ("setNodeParam", { { "nodeId", filter }, { "param", "cutoff" }, { "value", 500.0 } });
    setup.edit ("setNodeBypass", { { "nodeId", filter }, { "bypassed", true } });
    setup.edit ("setNodePosition", { { "nodeId", filter }, { "x", 5 }, { "y", 6 } });
    setup.edit ("setConnectionBends",
                { { "fromNode", gain },
                  { "fromPort", 0 },
                  { "toNode", filter },
                  { "toPort", 0 },
                  { "bends", juce::Array<juce::var> { makeObject ({ { "x", 7 }, { "y", 8 } }) } } });
    CHECK (std::ranges::any_of (setup.voice().connections,
                                [&] (const Connection& c) { return c.toNode == filter && c.bends.size() == 1; }));

    // An envelope on the cutoff, from the voice's ADSR.
    const auto adsr = nodeOfType (setup.voice(), nodetype::adsr).id;
    setup.edit ("connect", { { "fromNode", adsr }, { "fromPort", 0 }, { "toNode", filter }, { "toPort", 1 } });
    setup.edit ("setModulation", { { "nodeId", filter }, { "port", 1 }, { "param", "cutoff" }, { "depth", 0.25 } });

    const auto& added = nodeOfType (setup.voice(), nodetype::filter);
    CHECK_THAT (added.params.at ("cutoff"), WithinAbs (500.0, 1e-9));
    CHECK (added.bypassed);
    CHECK (added.x == 5.0f);
    CHECK (added.modulations.at (1).param == "cutoff");
    CHECK (setup.voice().connections.size() == 8);

    setup.edit ("removeNode", { { "nodeId", filter } });
    CHECK (setup.voice().nodes.size() == 6);
    CHECK (setup.voice().connections.size() == 5);
}

TEST_CASE ("a voice graph takes only nodes that can run per voice", "[voice][commands]")
{
    const VoiceProject setup;

    // Instruments, plugins and nodes that belong to a track (V6), and the fixed nodes.
    CHECK (setup.fails ("addNode", { { "type", nodetype::mpeSynth } }));
    CHECK (setup.fails ("addNode", { { "type", nodetype::voiceInstrument } }));
    CHECK (setup.fails (
        "addNode", { { "type", nodetype::pluginEffect }, { "plugin", makeObject ({ { "identifier", "test-A" } }) } }));
    CHECK (setup.fails ("addNode", { { "type", nodetype::voiceOutput } }));
    CHECK (setup.fails ("removeNode", { { "nodeId", nodeOfType (setup.voice(), nodetype::voiceInput).id } }));

    // The chain tools are track-scope.
    CHECK (setup.fails ("insertNode", { { "type", nodetype::eq } }));

    // Everything else built in is fine.
    for (const auto* type : { nodetype::oscillator, nodetype::noteExpression, nodetype::lfo, nodetype::eq,
                              nodetype::compressor, nodetype::utility, nodetype::loopReverb, nodetype::faust })
    {
        INFO (type);
        CHECK_FALSE (setup.fails ("addNode", { { "type", type } }));
    }

    // A voiceNodeId must name a Voice Instrument.
    const auto eq = nodeOfType (setup.graph(), nodetype::eq).id;
    CHECK_THROWS_AS (apply (setup.project, "addNode",
                            { { "trackId", setup.trackId }, { "voiceNodeId", eq }, { "type", nodetype::lfo } }),
                     CommandError);

    // The fixed nodes stay out of track graphs.
    CHECK_THROWS_AS (
        apply (setup.project, "addNode", { { "trackId", setup.trackId }, { "type", nodetype::voiceInput } }),
        CommandError);
}

TEST_CASE ("an MPE Synth converts to a Voice Instrument in place, as one undo step", "[voice][commands]")
{
    auto setup = projectWithClip();
    const auto trackId = setup.trackId;
    const auto& graph = setup.project.tracks[0]->graph;
    const auto synth = nodeOfType (graph, nodetype::mpeSynth).id;
    const auto connections = graph.connections;

    // An LFO on the synth's gain, which the Voice Instrument won't have.
    auto project = apply (setup.project, "addNode", { { "trackId", trackId }, { "type", nodetype::lfo } });
    const auto lfo = project.tracks[0]->graph.nodes.back().id;
    project = apply (
        project, "connect",
        { { "trackId", trackId }, { "fromNode", lfo }, { "fromPort", 0 }, { "toNode", synth }, { "toPort", 1 } });
    project =
        apply (project, "setModulation",
               { { "trackId", trackId }, { "nodeId", synth }, { "port", 1 }, { "param", "gain" }, { "depth", 0.1 } });

    Session session (project);
    REQUIRE (session.apply ("convertToVoiceGraph", makeObject ({ { "trackId", trackId }, { "nodeId", synth } })).ok);
    CHECK (session.undoLabel() == "Convert to voice graph");

    const auto& converted = session.project()->tracks[0]->graph;
    const auto& node = converted.nodes[static_cast<size_t> (converted.indexOfNode (synth))];
    CHECK (node.type == nodetype::voiceInstrument);
    CHECK (node.params.empty());
    CHECK (node.modulations.empty());
    REQUIRE (node.voiceGraph != nullptr);
    CHECK (node.voiceGraph->nodes.size() == 6);

    // The notes in and the audio out stay connected; the modulation input goes.
    CHECK (converted.connections == connections);

    REQUIRE (session.undo().ok);
    CHECK (nodeOfType (session.project()->tracks[0]->graph, nodetype::mpeSynth).id == synth);

    // Only an MPE Synth converts.
    const auto eq = nodeOfType (graph, nodetype::eq).id;
    CHECK_THROWS_AS (apply (project, "convertToVoiceGraph", { { "trackId", trackId }, { "nodeId", eq } }),
                     CommandError);
}

TEST_CASE ("removing a Voice Instrument takes its voice graph", "[voice][commands]")
{
    const VoiceProject setup;
    const auto project =
        apply (setup.project, "removeNode", { { "trackId", setup.trackId }, { "nodeId", setup.voiceId } });
    CHECK (project.tracks[0]->graph.indexOfNode (setup.voiceId) < 0);
    CHECK (allNodeIds (project).size() == setup.graph().nodes.size() - 1);
}

TEST_CASE ("a duplicated track gets new ids for its voice graph", "[voice][commands]")
{
    const VoiceProject setup;
    const auto project = apply (setup.project, "duplicateTrack", { { "id", setup.trackId } });
    REQUIRE (project.tracks.size() == 2);

    const auto ids = allNodeIds (project);
    CHECK (unique (ids));

    const auto& copy = *project.tracks[1]->graph.nodes.back().voiceGraph;
    CHECK (copy.nodes.size() == 6);

    for (const auto& connection : copy.connections)
    {
        CHECK (copy.indexOfNode (connection.fromNode) >= 0);
        CHECK (copy.indexOfNode (connection.toNode) >= 0);
    }
}

TEST_CASE ("voice graphs round-trip through project files, and damaged ones are refused", "[voice][file]")
{
    VoiceProject setup;
    setup.edit ("addNode", { { "type", nodetype::lfo } });

    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> pluginStates;
    const auto json = projectfile::toVar (setup.project, { "id", "v" }, pluginStates);
    const auto states = [] (const juce::String&) { return nullptr; };
    const auto loaded = projectfile::fromVar (projectfile::migrate (json), states).project;
    CHECK (juce::JSON::toString (toVar (loaded)) == juce::JSON::toString (toVar (setup.project)));

    // A version 10 file has no voice graphs and opens as it is.
    auto older = json.clone();
    older.getDynamicObject()->setProperty ("version", 10);
    CHECK (static_cast<int> (projectfile::migrate (older)["version"]) == projectfile::version);

    // A sequencer inside a voice graph, a voice graph without its output, and a Voice Input on a track are refused.
    const auto damaged = [&] (const std::function<void (juce::var & track)>& damage)
    {
        auto file = projectfile::migrate (json.clone());
        auto track = file["project"]["tracks"][juce::Identifier (juce::String (setup.trackId))];
        damage (track);
        return file;
    };
    const auto voiceNodes = [&] (juce::var& track)
    {
        return track["graph"]["nodes"][juce::Identifier (juce::String (setup.voiceId))]["voice"]["nodes"]
            .getDynamicObject();
    };

    const auto sequencer = damaged (
        [&] (juce::var& track)
        {
            const auto lfo = juce::String (setup.voice().nodes.back().id);
            voiceNodes (track)->getProperty (lfo).getDynamicObject()->setProperty ("type", nodetype::sequencer);
        });
    CHECK_THROWS_AS (projectfile::fromVar (sequencer, states), CommandError);

    const auto noOutput = damaged (
        [&] (juce::var& track)
        {
            const auto output = nodeOfType (setup.voice(), nodetype::voiceOutput).id;
            voiceNodes (track)
                ->getProperty (juce::String (output))
                .getDynamicObject()
                ->setProperty ("type", nodetype::gain);
        });
    CHECK_THROWS_AS (projectfile::fromVar (noOutput, states), CommandError);

    const auto stray = damaged (
        [&] (juce::var& track)
        {
            const auto eq = juce::String (nodeOfType (setup.graph(), nodetype::eq).id);
            track["graph"]["nodes"][juce::Identifier (eq)].getDynamicObject()->setProperty ("type",
                                                                                            nodetype::voiceInput);
        });
    CHECK_THROWS_AS (projectfile::fromVar (stray, states), CommandError);
}

TEST_CASE ("a command naming a voice-graph node edits its voice graph without a voiceNodeId", "[voice][commands]")
{
    VoiceProject setup;
    const auto oscillator = nodeOfType (setup.voice(), nodetype::oscillator).id;
    setup.project =
        apply (setup.project, "setNodeParam",
               { { "trackId", setup.trackId }, { "nodeId", oscillator }, { "param", "coarse" }, { "value", 7.0 } });
    CHECK_THAT (nodeOfType (setup.voice(), nodetype::oscillator).params.at ("coarse"), WithinAbs (7.0, 1e-9));

    // addNode names no node: it stays in the track's graph.
    const auto before = setup.graph().nodes.size();
    setup.project = apply (setup.project, "addNode", { { "trackId", setup.trackId }, { "type", nodetype::lfo } });
    CHECK (setup.graph().nodes.size() == before + 1);
}

TEST_CASE ("voice-graph nodes can be automated", "[voice][commands]")
{
    VoiceProject setup;
    const auto oscillator = nodeOfType (setup.voice(), nodetype::oscillator).id;
    setup.project =
        apply (setup.project, "addAutomationLane",
               { { "trackId", setup.trackId },
                 { "target", makeObject ({ { "kind", "node" }, { "nodeId", oscillator }, { "param", "fine" } }) } });
    REQUIRE (setup.project.tracks[0]->automation.size() == 1);
    CHECK (setup.project.tracks[0]->automation[0].target.nodeId == oscillator);

    // Removing the node takes its lane.
    setup.edit ("removeNodes", { { "nodeIds", juce::Array<juce::var> { juce::var (oscillator) } } });
    CHECK (setup.project.tracks[0]->automation.empty());
}

TEST_CASE ("a pasted Voice Instrument brings its voice graph with new ids", "[voice][commands]")
{
    VoiceProject setup;
    setup.edit ("addNode", { { "type", nodetype::lfo } });
    const auto voice =
        toVar (*setup.project.tracks[0])["graph"]["nodes"][juce::Identifier (juce::String (setup.voiceId))]["voice"];
    REQUIRE (voice.isObject());
    const auto pastedSpec = makeObject ({ { "type", nodetype::voiceInstrument }, { "voice", voice } });
    setup.project = apply (setup.project, "addNodes",
                           { { "trackId", setup.trackId }, { "nodes", juce::Array<juce::var> { pastedSpec } } });

    const auto& pasted = setup.graph().nodes.back();
    REQUIRE (pasted.voiceGraph != nullptr);
    CHECK (pasted.voiceGraph->nodes.size() == setup.voice().nodes.size());
    CHECK (pasted.voiceGraph->connections.size() == setup.voice().connections.size());
    CHECK (unique (allNodeIds (setup.project)));

    // A voice graph that could not run is refused.
    auto damaged = juce::JSON::parse (juce::JSON::toString (voice));
    damaged["nodes"].getDynamicObject()->clear();
    const auto bad = makeObject ({ { "type", nodetype::voiceInstrument }, { "voice", damaged } });
    CHECK_THROWS_AS (
        applyCommand (setup.project, "addNodes",
                      makeObject ({ { "trackId", setup.trackId }, { "nodes", juce::Array<juce::var> { bad } } })),
        CommandError);
}

TEST_CASE ("a Voice Instrument chosen as a track's instrument gets the default voice graph", "[voice][commands]")
{
    auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::voiceInstrument } });

    const auto& voice = nodeOfType (project.tracks[0]->graph, nodetype::voiceInstrument);
    REQUIRE (voice.voiceGraph != nullptr);
    CHECK (voice.voiceGraph->nodes.size() == 6);
    CHECK (std::ranges::none_of (project.tracks[0]->graph.nodes,
                                 [] (const GraphNode& n) { return n.type == nodetype::mpeSynth; }));
}
