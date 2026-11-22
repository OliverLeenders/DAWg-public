#include "TestUtils.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] const GraphNode& nodeOfType (const Track& track, const char* type)
{
    const auto it = std::ranges::find_if (track.graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
    REQUIRE (it != track.graph.nodes.end());
    return *it;
}
} // namespace

TEST_CASE ("a new track gets the default graph", "[commands][graph]")
{
    // Sequencer -> Synth -> EQ -> Compressor (bypassed) -> Gain -> Output (docs/design/channel-strip.md, C2).
    const auto project = apply ({}, "addTrack", {});
    const auto& graph = project.tracks[0]->graph;

    REQUIRE (graph.nodes.size() == 6);
    CHECK (graph.connections.size() == 5);
    CHECK (nodeOfType (*project.tracks[0], nodetype::trackOutput).id > 0);
    CHECK (graph.nodes[2].type == nodetype::eq);
    CHECK_FALSE (graph.nodes[2].bypassed);
    CHECK (graph.nodes[3].type == nodetype::compressor);
    CHECK (graph.nodes[3].bypassed);

    // Groups and returns get them too; the master doesn't.
    for (const auto* kind : { "audio", "group", "return" })
    {
        const auto bus = apply ({}, "addTrack", { { "kind", kind } }); // outlives `track` (it points into it)
        const auto& track = *bus.tracks[0];
        CHECK (nodeOfType (track, nodetype::eq).id > 0);
        CHECK (nodeOfType (track, nodetype::compressor).bypassed);
    }

    CHECK (withMaster ({}).master->graph.nodes.size() == 3);
}

TEST_CASE ("a device dropped where there is no track makes a track that holds it", "[commands][graph]")
{
    // An instrument takes the MPE Synth's place; an effect goes after the inserts, on an audio track.
    const auto withSampler = apply ({}, "addTrack", { { "device", makeObject ({ { "type", nodetype::sampler } }) } });
    const auto& instrument = *withSampler.tracks[0];
    CHECK (nodeOfType (instrument, nodetype::sampler).id > 0);
    CHECK (std::ranges::none_of (instrument.graph.nodes,
                                 [] (const GraphNode& n) { return n.type == nodetype::mpeSynth; }));

    const auto withDelay =
        apply ({}, "addTrack", { { "kind", "audio" }, { "device", makeObject ({ { "type", nodetype::delay } }) } });
    const auto& audio = *withDelay.tracks[0];
    CHECK (audio.kind == TrackKind::audio);
    CHECK (audio.graph.nodes.size() == 6);
    const auto compressor = nodeOfType (audio, nodetype::compressor).id;
    const auto delay = nodeOfType (audio, nodetype::delay).id;
    CHECK (std::ranges::any_of (audio.graph.connections,
                                [&] (const Connection& c) { return c.fromNode == compressor && c.toNode == delay; }));

    // One undo step.
    Session session;
    REQUIRE (
        session.apply ("addTrack", makeObject ({ { "device", makeObject ({ { "type", nodetype::sampler } }) } })).ok);
    REQUIRE (session.undo().ok);
    CHECK (session.project()->tracks.empty());
}

TEST_CASE ("track offsets are clamped and the master rejects them", "[commands][tracks][offset]")
{
    auto project = withMaster (apply ({}, "addTrack", {}));
    const auto trackId = project.tracks[0]->id;

    CHECK_THAT (project.tracks[0]->offsetMs, WithinAbs (0.0, 1e-9));

    project = apply (project, "setTrackOffset", { { "id", trackId }, { "offsetMs", 123.25 } });
    CHECK_THAT (project.tracks[0]->offsetMs, WithinAbs (123.25, 1e-9));

    project = apply (project, "setTrackOffset", { { "id", trackId }, { "offsetMs", 99999.0 } });
    CHECK_THAT (project.tracks[0]->offsetMs, WithinAbs (limits::maxOffsetMs, 1e-9));

    project = apply (project, "setTrackOffset", { { "id", trackId }, { "offsetMs", -99999.0 } });
    CHECK_THAT (project.tracks[0]->offsetMs, WithinAbs (limits::minOffsetMs, 1e-9));

    REQUIRE (project.master != nullptr);
    CHECK_THROWS_AS (apply (project, "setTrackOffset", { { "id", project.master->id }, { "offsetMs", 10.0 } }),
                     CommandError);
}

TEST_CASE ("notes are added sorted and edited by id", "[commands][notes]")
{
    auto [project, trackId, clipId] = projectWithClip();
    project =
        apply (project, "addNotes",
               { { "clipId", clipId },
                 { "notes", array ({ noteArgs (ticksPerBeat, ticksPerBeat, 64), noteArgs (0, ticksPerBeat, 60) }) } });

    const auto& clip = *project.tracks[0]->clips[0];
    REQUIRE (clip.notes.size() == 2);
    CHECK (clip.notes[0]->start == 0);
    CHECK (clip.notes[1]->start == ticksPerBeat);

    const auto firstId = clip.notes[0]->id;
    project = apply (project, "updateNotes",
                     { { "clipId", clipId },
                       { "notes", array ({ makeObject ({ { "id", firstId }, { "start", 2 * ticksPerBeat } }) }) } });

    const auto& updated = *project.tracks[0]->clips[0];
    CHECK (updated.notes[1]->id == firstId); // re-sorted after the move
    CHECK (updated.notes[1]->pitch > 59.0);

    project = apply (project, "removeNotes", { { "clipId", clipId }, { "ids", array ({ firstId }) } });
    CHECK (project.tracks[0]->clips[0]->notes.size() == 1);
}

TEST_CASE ("invalid notes are rejected", "[commands][notes]")
{
    const auto [project, trackId, clipId] = projectWithClip();

    CHECK_THROWS_AS (
        apply (project, "addNotes", { { "clipId", clipId }, { "notes", array ({ noteArgs (0, 0, 60) }) } }),
        CommandError);
    CHECK_THROWS_AS (
        apply (project, "addNotes", { { "clipId", clipId }, { "notes", array ({ makeObject ({ { "start", 0 } }) }) } }),
        CommandError);
    CHECK_THROWS_AS (apply (project, "removeNotes", { { "clipId", clipId }, { "ids", array ({ 12345 }) } }),
                     CommandError);
    CHECK_THROWS_AS (apply (project, "addNotes", { { "clipId", 999 }, { "notes", array ({}) } }), CommandError);
}

TEST_CASE ("MPE lanes start with a point; the note end has one only when added", "[commands][curve]")
{
    auto [project, trackId, clipId] = projectWithClip();
    project =
        apply (project, "addNotes", { { "clipId", clipId }, { "notes", array ({ noteArgs (0, ticksPerBeat, 60) }) } });
    const auto note = [&] { return *project.tracks[0]->clips[0]->notes[0]; };
    const auto noteId = note().id;

    for (const auto lane : { Lane::pitch, Lane::pressure, Lane::timbre })
    {
        const auto curve = note().expression[static_cast<size_t> (lane)];
        REQUIRE (curve.size() == 1);
        CHECK (curve.front().time == 0);
        CHECK_THAT (curve.front().value, WithinAbs (laneInfos()[static_cast<size_t> (lane)].defaultValue, 1e-9));
    }

    CHECK (note().expression[static_cast<size_t> (Lane::vibrato)].empty());

    // A curve without a start point gets one, with the value it already has there; nothing is added at the end.
    project = apply (project, "setCurve",
                     { { "clipId", clipId },
                       { "noteId", noteId },
                       { "lane", "pitch" },
                       { "points", array ({ array ({ 100, 1.0 }), array ({ 200, 2.0 }) }) } });
    const auto pitch = note().expression[static_cast<size_t> (Lane::pitch)];
    REQUIRE (pitch.size() == 3);
    CHECK (pitch.front().time == 0);
    CHECK_THAT (pitch.front().value, WithinAbs (1.0, 1e-9));
    CHECK (pitch.back().time == 200);

    // A point the user put at the note end moves with it when the note is resized.
    project = apply (project, "setCurve",
                     { { "clipId", clipId },
                       { "noteId", noteId },
                       { "lane", "pressure" },
                       { "points", array ({ array ({ 0, 0.2 }), array ({ ticksPerBeat, 0.8 }) }) } });
    project = apply (project, "updateNotes",
                     { { "clipId", clipId },
                       { "notes", array ({ makeObject ({ { "id", noteId }, { "length", 2 * ticksPerBeat } }) }) } });
    const auto longer = note().expression[static_cast<size_t> (Lane::pressure)];
    REQUIRE (longer.size() == 2);
    CHECK (longer.back().time == 2 * ticksPerBeat);
    CHECK (note().expression[static_cast<size_t> (Lane::pitch)].back().time == 200);
}

TEST_CASE ("an end point that only holds the value is dropped when a note is read", "[commands][curve]")
{
    const auto read = [] (const char* pitch)
    {
        return noteFromVar (
                   juce::JSON::parse (juce::String (R"({"start":0,"length":480,"pitch":60,"expression":{"pitch":)")
                                      + pitch + "}}"))
            .expression[static_cast<size_t> (Lane::pitch)];
    };

    CHECK (read ("[[0,0],[480,0]]").size() == 1);
    CHECK (read ("[[0,0],[240,2],[480,2]]").size() == 2);
    CHECK (read ("[[0,0],[480,2]]").size() == 2);         // a ramp to the end stays
    CHECK (read ("[[0,0],[480,0],[600,1]]").size() == 3); // so does a point with a release after it
}

TEST_CASE ("notes split in two, curves and all", "[commands][notes]")
{
    auto [project, trackId, clipId] = projectWithClip();
    auto fields = noteArgs (0, 4 * ticksPerBeat, 60);
    fields.getDynamicObject()->setProperty (
        "expression", makeObject ({ { "pitch", array ({ array ({ 0, 0.0 }), array ({ 2 * ticksPerBeat, 4.0 }),
                                                        array ({ 3 * ticksPerBeat, 4.0 }) }) } }));
    fields.getDynamicObject()->setProperty ("vibrato",
                                            makeObject ({ { "delay", ticksPerBeat }, { "fadeIn", 2 * ticksPerBeat } }));
    project = apply (
        project, "addNotes",
        { { "clipId", clipId }, { "notes", array ({ fields, noteArgs (8 * ticksPerBeat, ticksPerBeat, 62) }) } });
    const auto notes = [&] { return project.tracks[0]->clips[0]->notes; };
    const auto noteId = notes()[0]->id;
    const auto otherId = notes()[1]->id;

    // Halfway up the pitch ramp; the other note is not under the split and stays as it is.
    project = apply (project, "splitNotes",
                     { { "clipId", clipId }, { "ids", array ({ noteId, otherId }) }, { "at", ticksPerBeat } });
    REQUIRE (notes().size() == 3);
    const auto left = *notes()[0];
    const auto right = *notes()[1];
    CHECK (left.id == noteId);
    CHECK (left.length == ticksPerBeat);
    CHECK (right.start == ticksPerBeat);
    CHECK (right.length == 3 * ticksPerBeat);
    CHECK_THAT (right.pitch, WithinAbs (60.0, 1e-9));
    CHECK (notes()[2]->id == otherId);
    CHECK (notes()[2]->length == ticksPerBeat);

    const auto& leftPitch = left.expression[static_cast<size_t> (Lane::pitch)];
    REQUIRE (leftPitch.size() == 2); // ends where the ramp had got to
    CHECK (leftPitch.back().time == ticksPerBeat);
    CHECK_THAT (leftPitch.back().value, WithinAbs (2.0, 1e-9));

    const auto& rightPitch = right.expression[static_cast<size_t> (Lane::pitch)];
    REQUIRE (rightPitch.size() == 3);
    CHECK (rightPitch[0].time == 0);
    CHECK_THAT (rightPitch[0].value, WithinAbs (2.0, 1e-9));
    CHECK (rightPitch[1].time == ticksPerBeat);
    CHECK (rightPitch[2].time == 2 * ticksPerBeat);

    // The vibrato's delay had run out at the split; its fade-in carries on.
    CHECK (right.vibrato.delay == 0);
    CHECK (right.vibrato.fadeIn == 2 * ticksPerBeat);

    // On the flat part, the left half needs no point at the split.
    project = apply (project, "splitNotes",
                     { { "clipId", clipId }, { "ids", array ({ right.id }) }, { "at", 7 * ticksPerBeat / 2 } });
    CHECK (notes()[1]->expression[static_cast<size_t> (Lane::pitch)].size() == 3);

    CHECK_THROWS_AS (
        apply (project, "splitNotes", { { "clipId", clipId }, { "ids", array ({ otherId }) }, { "at", 0 } }),
        CommandError);
}

TEST_CASE ("curves are sorted, clamped and validated", "[commands][curve]")
{
    auto [project, trackId, clipId] = projectWithClip();
    project =
        apply (project, "addNotes", { { "clipId", clipId }, { "notes", array ({ noteArgs (0, ticksPerBeat, 60) }) } });
    const auto noteId = project.tracks[0]->clips[0]->notes[0]->id;

    project = apply (project, "setCurve",
                     { { "clipId", clipId },
                       { "noteId", noteId },
                       { "lane", "pressure" },
                       { "points", array ({ array ({ 100, 0.5 }), array ({ 0, 2.0, 2, 3.0 }) }) } });

    const auto& curve = project.tracks[0]->clips[0]->notes[0]->expression[static_cast<size_t> (Lane::pressure)];
    REQUIRE (curve.size() == 2); // no point added at the note end
    CHECK (curve[0].time == 0);
    CHECK (curve[1].time == 100);
    CHECK (curve[0].value > 0.99); // clamped to the pressure range
    CHECK (curve[0].shape == CurveShape::power);
    CHECK (curve[0].tension == 3.0); // the free-handle range goes past the usual -1..1

    CHECK_THROWS_AS (apply (project, "setCurve",
                            { { "clipId", clipId },
                              { "noteId", noteId },
                              { "lane", "pitch" },
                              { "points", array ({ array ({ 0, 1 }), array ({ 0, 2 }) }) } }),
                     CommandError);
    CHECK_THROWS_AS (
        apply (project, "setCurve",
               { { "clipId", clipId }, { "noteId", noteId }, { "lane", "nope" }, { "points", array ({}) } }),
        CommandError);
}

TEST_CASE ("free curve handles are parsed and clamped", "[commands][curve]")
{
    auto [project, trackId, clipId] = projectWithClip();
    project =
        apply (project, "addNotes", { { "clipId", clipId }, { "notes", array ({ noteArgs (0, ticksPerBeat, 60) }) } });
    const auto noteId = project.tracks[0]->clips[0]->notes[0]->id;

    project = apply (project, "setCurve",
                     { { "clipId", clipId },
                       { "noteId", noteId },
                       { "lane", "pressure" },
                       { "points", array ({ array ({ 0, 0.0, 3, 0, 0.25, 0.75 }), array ({ 100, 1.0 }) }) } });

    const auto& curve = project.tracks[0]->clips[0]->notes[0]->expression[static_cast<size_t> (Lane::pressure)];
    REQUIRE (curve.size() == 2);
    CHECK (curve[0].shape == CurveShape::free);
    CHECK_THAT (curve[0].handleTime, WithinAbs (0.25, 1.0e-12));
    CHECK_THAT (curve[0].handleValue, WithinAbs (0.75, 1.0e-12));
    // The handles point at the tension point, so the curve bends towards it rather than through it.
    CHECK_THAT (evaluateCurve (curve, 25.0, 0.0), WithinAbs (0.513502, 1.0e-4)); // the handle rule, sampled

    project = apply (project, "setCurve",
                     { { "clipId", clipId },
                       { "noteId", noteId },
                       { "lane", "pressure" },
                       { "points", array ({ array ({ 0, 0.0, 3, 0, -1, 2 }), array ({ 100, 1.0 }) }) } });

    const auto& clamped = project.tracks[0]->clips[0]->notes[0]->expression[static_cast<size_t> (Lane::pressure)];
    CHECK_THAT (clamped[0].handleTime, WithinAbs (0.0, 1.0e-12));
    CHECK_THAT (clamped[0].handleValue, WithinAbs (1.0, 1.0e-12));
}

TEST_CASE ("clips move between tracks", "[commands][clips]")
{
    auto [project, trackId, clipId] = projectWithClip();
    project = apply (project, "addTrack", {});
    const auto otherTrack = project.tracks[1]->id;

    project =
        apply (project, "moveClip", { { "id", clipId }, { "start", 4 * ticksPerBeat }, { "trackId", otherTrack } });

    CHECK (project.tracks[0]->clips.empty());
    REQUIRE (project.tracks[1]->clips.size() == 1);
    CHECK (project.tracks[1]->clips[0]->start == 4 * ticksPerBeat);
}

TEST_CASE ("connections are validated", "[commands][graph]")
{
    const auto project = apply ({}, "addTrack", {});
    const auto& track = *project.tracks[0];
    const auto trackId = track.id;
    const auto sequencer = nodeOfType (track, nodetype::sequencer).id;
    const auto synth = nodeOfType (track, nodetype::mpeSynth).id;
    const auto gain = nodeOfType (track, nodetype::gain).id;
    const auto output = nodeOfType (track, nodetype::trackOutput).id;

    const auto connect = [&] (const Project& p, ObjectId from, int fromPort, ObjectId to, int toPort)
    {
        return applyCommand (p, "connect",
                             makeObject ({ { "trackId", trackId },
                                           { "fromNode", from },
                                           { "fromPort", fromPort },
                                           { "toNode", to },
                                           { "toPort", toPort } }));
    };

    SECTION ("type mismatch")
    {
        CHECK_THROWS_AS (connect (project, sequencer, 0, gain, 0), CommandError);
    }

    SECTION ("feedback loop")
    {
        CHECK_THROWS_AS (connect (project, gain, 0, synth, 0), CommandError);
    }

    SECTION ("duplicate")
    {
        const auto eq = nodeOfType (track, nodetype::eq).id;
        CHECK_THROWS_AS (connect (project, synth, 0, eq, 0), CommandError);
    }

    SECTION ("fixed nodes cannot be removed")
    {
        CHECK_THROWS_AS (apply (project, "removeNode", { { "trackId", trackId }, { "nodeId", output } }), CommandError);
    }

    SECTION ("parallel audio paths are allowed")
    {
        auto withGain = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::gain } });
        const auto newGain = withGain.tracks[0]->graph.nodes.back().id;
        withGain = connect (withGain, synth, 0, newGain, 0);
        withGain = connect (withGain, newGain, 0, output, 0);
        CHECK (withGain.tracks[0]->graph.connections.size() == 7);
    }
}

TEST_CASE ("node parameters are validated and clamped", "[commands][graph]")
{
    const auto project = apply ({}, "addTrack", {});
    const auto trackId = project.tracks[0]->id;
    const auto gain = nodeOfType (*project.tracks[0], nodetype::gain).id;

    const auto updated =
        apply (project, "setNodeParam",
               { { "trackId", trackId }, { "nodeId", gain }, { "param", "gainDb" }, { "value", 100.0 } });
    CHECK (nodeOfType (*updated.tracks[0], nodetype::gain).params.at ("gainDb") > 11.9);

    CHECK_THROWS_AS (apply (project, "setNodeParam",
                            { { "trackId", trackId }, { "nodeId", gain }, { "param", "volume" }, { "value", 0.0 } }),
                     CommandError);
}

TEST_CASE ("a batch is one undo step and cannot nest", "[commands][session]")
{
    const auto [project, trackId, clipId] = projectWithClip();
    Session session (project);

    const auto commands = array ({
        makeObject ({ { "command", "setTempo" }, { "args", makeObject ({ { "bpm", 100 } }) } }),
        makeObject (
            { { "command", "addNotes" },
              { "args", makeObject ({ { "clipId", clipId }, { "notes", array ({ noteArgs (0, 960, 60) }) } }) } }),
    });

    REQUIRE (session.apply ("batch", makeObject ({ { "label", "Paste" }, { "commands", commands } })).ok);
    CHECK (session.undoLabel() == "Paste");
    CHECK (session.project()->tracks[0]->clips[0]->notes.size() == 1);

    REQUIRE (session.undo().ok);
    CHECK (session.project()->tracks[0]->clips[0]->notes.empty());
    CHECK_FALSE (session.canUndo());

    const auto nested = array ({ makeObject ({ { "command", "batch" }, { "args", makeObject ({}) } }) });
    CHECK_FALSE (session.apply ("batch", makeObject ({ { "commands", nested } })).ok);
}

TEST_CASE ("collections are serialised by id so one added note is one patch op", "[commands][serialisation]")
{
    const auto [project, trackId, clipId] = projectWithClip();
    Session session (project);
    juce::var lastPatch;
    session.setChangeListener ([&] (const ProjectPtr&, const ProjectPtr&, const juce::var& patch)
                               { lastPatch = patch; });

    REQUIRE (
        session
            .apply ("addNotes", makeObject ({ { "clipId", clipId }, { "notes", array ({ noteArgs (0, 960, 60) }) } }))
            .ok);
    const auto noteId = session.project()->tracks[0]->clips[0]->notes[0]->id;

    REQUIRE (lastPatch["ops"].size() == 1);
    CHECK (lastPatch["ops"][0]["op"] == "add");
    CHECK (lastPatch["ops"][0]["path"].toString()
           == "/tracks/" + juce::String (trackId) + "/clips/" + juce::String (clipId) + "/notes/"
                  + juce::String (noteId));

    const auto json = toVar (*session.project());
    CHECK (json["trackOrder"].size() == 1);
    CHECK (json["tracks"][juce::Identifier (juce::String (trackId))]["graph"]["nodes"].getDynamicObject() != nullptr);
}

TEST_CASE ("a duplicated track is an independent copy right below the original", "[commands][tracks]")
{
    auto [project, trackId, clipId] = projectWithClip();
    project = apply (project, "addTrack", {});
    project =
        apply (project, "addNotes", { { "clipId", clipId }, { "notes", array ({ noteArgs (0, ticksPerBeat, 60) }) } });
    const auto synthNode = nodeOfType (*project.tracks[0], nodetype::mpeSynth).id;
    project =
        apply (project, "addAutomationLane",
               { { "trackId", trackId },
                 { "target", makeObject ({ { "kind", "node" }, { "nodeId", synthNode }, { "param", "gain" } }) } });
    project = apply (project, "setTrackGain", { { "id", trackId }, { "gainDb", -6.0 } });

    project = apply (project, "duplicateTrack", { { "id", trackId } });
    REQUIRE (project.tracks.size() == 3);
    const auto& original = *project.tracks[0];
    const auto& copy = *project.tracks[1];

    CHECK (copy.id != original.id);
    CHECK (copy.name == original.name);
    CHECK_THAT (copy.gainDb, WithinAbs (-6.0, 1e-9));

    // New node ids, wired the same way.
    REQUIRE (copy.graph.nodes.size() == original.graph.nodes.size());
    REQUIRE (copy.graph.connections.size() == original.graph.connections.size());
    for (const auto& node : copy.graph.nodes)
        CHECK (original.graph.indexOfNode (node.id) < 0);
    for (const auto& connection : copy.graph.connections)
        CHECK ((copy.graph.indexOfNode (connection.fromNode) >= 0 && copy.graph.indexOfNode (connection.toNode) >= 0));

    // Clips and notes are copies with their own ids, not linked to the originals.
    REQUIRE (copy.clips.size() == 1);
    CHECK (copy.clips[0]->id != clipId);
    CHECK (copy.clips[0]->linkId == 0);
    REQUIRE (copy.clips[0]->notes.size() == 1);
    CHECK (copy.clips[0]->notes[0]->id != original.clips[0]->notes[0]->id);

    // Automation follows the copied synth node.
    REQUIRE (copy.automation.size() == 1);
    CHECK (copy.automation[0].id != original.automation[0].id);
    CHECK (copy.automation[0].target.nodeId == nodeOfType (copy, nodetype::mpeSynth).id);

    CHECK_THROWS_AS (apply (project, "duplicateTrack", { { "id", 999999 } }), CommandError);
}

TEST_CASE ("a duplicated track and a pasted plugin node take the states the app gives for copies",
           "[commands][plugins]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "insertNode",
                     { { "trackId", trackId },
                       { "type", nodetype::pluginEffect },
                       { "plugin", makeObject ({ { "identifier", "fx" } }) } });
    const auto pluginNode = nodeOfType (*project.tracks[0], nodetype::pluginEffect).id;
    const juce::MemoryBlock forCopy ("copy", 4);

    auto states = makeObject ({ { juce::String (pluginNode).toRawUTF8(), forCopy.toBase64Encoding() } });
    project = apply (project, "duplicateTrack", { { "id", trackId }, { "pluginStates", states } });
    REQUIRE (project.tracks.size() == 2);
    CHECK (nodeOfType (*project.tracks[0], nodetype::pluginEffect).pluginState == nullptr);
    const auto& copied = nodeOfType (*project.tracks[1], nodetype::pluginEffect);
    REQUIRE (copied.pluginState != nullptr);
    CHECK (*copied.pluginState == forCopy);

    const auto pasteSpec = makeObject ({ { "type", nodetype::pluginEffect },
                                         { "plugin", makeObject ({ { "identifier", "fx" } }) },
                                         { "pluginState", forCopy.toBase64Encoding() } });
    project = apply (project, "addNodes",
                     { { "trackId", trackId }, { "nodes", array ({ pasteSpec }) }, { "connections", array ({}) } });
    const auto& nodes = project.tracks[0]->graph.nodes;
    const auto pasted = std::ranges::count_if (nodes, [&] (const GraphNode& n)
                                               { return n.pluginState != nullptr && *n.pluginState == forCopy; });
    CHECK (pasted == 1);
}
