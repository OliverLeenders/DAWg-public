#include "TestUtils.h"
#include "dawg/AudioSource.h"
#include "dawg/GraphCompiler.h"
#include "dawg/NodeRegistry.h"
#include "dawg/PluginNode.h"
#include "dawg/ProjectFile.h"
#include "dawg/Sampler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Multi-output instruments (docs/design/multi-output-instruments.md): output tracks, Output to track nodes and a
// Sampler's zone outputs.

namespace
{
[[nodiscard]] ObjectId idOf (const Project& project, const juce::String& name)
{
    for (const auto& track : project.tracks)
        if (track->name == name)
            return track->id;

    FAIL ("No track named " << name); // [[noreturn]]
}

[[nodiscard]] const Track& trackNamed (const Project& project, const char* name)
{
    return *project.findTrack (idOf (project, name));
}

[[nodiscard]] juce::String order (const Project& project)
{
    juce::StringArray names;

    for (const auto& track : project.tracks)
        names.add (track->name);

    return names.joinIntoString (" ");
}

[[nodiscard]] const GraphNode* nodeOfType (const Track& track, const char* type)
{
    for (const auto& node : track.graph.nodes)
        if (node.type == type)
            return &node;

    return nullptr;
}

constexpr float level = 0.25f;
constexpr int frames = 2048;
constexpr size_t probe = 100;

// Two monitored audio tracks with a constant input (each adds `level`). Track "1" copies its input to an output
// track "1 out" through an Output to track node, so it is heard twice: there and through its Main output track
// "1 Main", as it is only a header now (MO12). There is a master track.
struct OutputRig
{
    OutputRig()
        : input (frames, level)
    {
        for (const auto* name : { "1", "2" })
        {
            project = apply (project, "addTrack", { { "kind", "audio" }, { "name", name } });
            project = apply (project, "setTrackSwitches",
                             { { "id", project.tracks.back()->id }, { "inputChannel", 0 }, { "monitor", 2 } });
        }

        project = apply (project, "addTrack", { { "kind", "output" }, { "source", id ("1") }, { "name", "1 out" } });
        project = apply (project, "addNode", { { "trackId", id ("1") }, { "type", nodetype::busOutput } });
        const auto& one = trackNamed (project, "1");
        const auto player = nodeOfType (one, nodetype::audioPlayer)->id;
        const auto output = nodeOfType (one, nodetype::busOutput)->id;
        project = apply (project, "setNodeTarget",
                         { { "trackId", id ("1") }, { "nodeId", output }, { "target", id ("1 out") } });
        project = apply (project, "connect",
                         { { "trackId", id ("1") },
                           { "fromNode", player },
                           { "fromPort", 0 },
                           { "toNode", output },
                           { "toPort", 0 } });
        project = withMaster (project);
        engine.setEngine (compiler.compile (project, spec));
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);

        if (auto compiled = compiler.update (project, spec))
            engine.setEngine (std::move (compiled));
    }

    [[nodiscard]] float render()
    {
        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (frames);
        engine.renderOffline (out.channels(), 2, frames, sampleRate, inputs.data(), 1);
        return out.left()[probe];
    }

    [[nodiscard]] ObjectId id (const juce::String& name) const { return idOf (project, name); }

    std::vector<float> input;
    Project project;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

namespace
{
[[nodiscard]] juce::var idList (const Project& project, std::initializer_list<const char*> names)
{
    juce::Array<juce::var> ids;

    for (const auto* name : names)
        ids.add (idOf (project, name));

    return ids;
}
} // namespace

TEST_CASE ("an output track starts under its track, can be grouped apart, and is removed with it", "[outputs]")
{
    Project project;
    project = apply (project, "addTrack", { { "name", "Kit" } });
    project = apply (project, "addTrack", { { "name", "Bass" } });

    for (const auto* name : { "Kick", "Snare" })
        project = apply (project, "addTrack",
                         { { "kind", "output" }, { "source", idOf (project, "Kit") }, { "name", name } });

    // Its first output track gives it a Main output track too: it is only a header now (MO12).
    CHECK (order (project) == "Kit Kit Main Kick Snare Bass");
    CHECK (trackNamed (project, "Kit Main").mainOutput);
    CHECK (! trackNamed (project, "Kick").mainOutput);
    CHECK (trackNamed (project, "Kick").kind == TrackKind::output);
    CHECK (trackNamed (project, "Kick").source == idOf (project, "Kit"));
    CHECK (trackNamed (project, "Kick").graph.nodes[0].type == nodetype::mixInput);

    // Output tracks group like any track, apart from the rest of the kit.
    project =
        apply (project, "groupTracks", { { "ids", idList (project, { "Kick", "Snare" }) }, { "name", "Shells" } });
    CHECK (order (project) == "Kit Kit Main Shells Kick Snare Bass");
    CHECK (trackNamed (project, "Kick").parent == idOf (project, "Shells"));

    // Moving the instrument leaves them where they are; a new output track starts right after it.
    project = apply (project, "moveTracks",
                     { { "ids", idList (project, { "Kit" }) }, { "before", idOf (project, "Bass") }, { "parent", 0 } });
    CHECK (order (project) == "Kit Main Shells Kick Snare Kit Bass");
    project =
        apply (project, "addTrack", { { "kind", "output" }, { "source", idOf (project, "Kit") }, { "name", "Hats" } });
    CHECK (order (project) == "Kit Main Shells Kick Snare Kit Hats Bass");

    CHECK_THROWS (apply (project, "addTrack", { { "kind", "output" }, { "source", idOf (project, "Shells") } }));

    // A duplicate gets copies of all its output tracks, right after it.
    const auto copied = apply (project, "duplicateTrack", { { "id", idOf (project, "Kit") } });
    CHECK (order (copied) == "Kit Main Shells Kick Snare Kit Hats Kit Kit Main Kick Snare Hats Bass");
    const auto copy = copied.tracks[6]->id;

    for (size_t i = 7; i < 11; ++i)
    {
        CHECK (copied.tracks[i]->source == copy);
        CHECK (copied.tracks[i]->parent == 0);
    }

    // Removing the instrument removes its output tracks, wherever they are.
    const auto removed = apply (project, "removeTrack", { { "id", idOf (project, "Kit") } });
    CHECK (order (removed) == "Shells Bass");
}

TEST_CASE ("an Output to track only feeds an output track of its own track", "[outputs]")
{
    OutputRig rig;
    const auto output = nodeOfType (trackNamed (rig.project, "1"), nodetype::busOutput)->id;
    CHECK_THROWS (apply (rig.project, "setNodeTarget",
                         { { "trackId", rig.id ("1") }, { "nodeId", output }, { "target", rig.id ("2") } }));
    CHECK_THROWS (apply (rig.project, "addNode",
                         { { "trackId", rig.id ("1") }, { "type", nodetype::busOutput }, { "voiceNodeId", 12345 } }));
}

TEST_CASE ("an output track hears its Output to track, whatever the track's fader", "[outputs][engine]")
{
    OutputRig rig;
    // Track 1 through its Main and its output track, track 2 dry.
    CHECK (order (rig.project) == "1 1 Main 1 out 2");
    CHECK_THAT (rig.render(), WithinAbs (3 * level, 1e-6));

    // The track is only a header: its fader plays no part. The Main's fader is the Main's, the output track's its own.
    rig.edit ("setTrackGain", { { "id", rig.id ("1") }, { "gainDb", -96.0 } });
    CHECK_THAT (rig.render(), WithinAbs (3 * level, 1e-4));
    rig.edit ("setTrackGain", { { "id", rig.id ("1") }, { "gainDb", 0.0 } });
    rig.edit ("setTrackGain", { { "id", rig.id ("1 Main") }, { "gainDb", -96.0 } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-4));
    rig.edit ("setTrackGain", { { "id", rig.id ("1 Main") }, { "gainDb", 0.0 } });
    rig.edit ("setTrackGain", { { "id", rig.id ("1 out") }, { "gainDb", -96.0 } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-4));
    rig.edit ("setTrackGain", { { "id", rig.id ("1 out") }, { "gainDb", 0.0 } });
    std::ignore = rig.render();
    CHECK_THAT (rig.compiler.trackState (rig.id ("1 out"))->peakLeft.load(), WithinAbs (level, 1e-6));
}

TEST_CASE ("muting or soloing a track takes its output tracks along", "[outputs][engine]")
{
    OutputRig rig;

    // Muting the track mutes its output; muting the output track mutes only it.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "mute", true } });
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "mute", false } });
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1 out") }, { "mute", true } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1 out") }, { "mute", false } });

    // Soloing the track keeps its output track; soloing the output track plays only it.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "solo", true } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "solo", false } });
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1 out") }, { "solo", true } });
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-6));
}

TEST_CASE ("an output track in a group of its own plays through it, and its track's solo keeps it heard",
           "[outputs][engine]")
{
    OutputRig rig;
    rig.edit ("groupTracks", { { "ids", idList (rig.project, { "1 out" }) }, { "name", "Outs" } });
    CHECK (trackNamed (rig.project, "1 out").parent == rig.id ("Outs"));
    CHECK_THAT (rig.render(), WithinAbs (3 * level, 1e-6));

    // The group's fader is the output's.
    rig.edit ("setTrackGain", { { "id", rig.id ("Outs") }, { "gainDb", -96.0 } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-4));
    rig.edit ("setTrackGain", { { "id", rig.id ("Outs") }, { "gainDb", 0.0 } });

    // Soloing the track keeps the group that holds its output heard; soloing the group plays only the output.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "solo", true } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "solo", false } });
    rig.edit ("setTrackSwitches", { { "id", rig.id ("Outs") }, { "solo", true } });
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-6));
}

TEST_CASE ("removing an output track takes its Output to track nodes and sets their zones back to Main", "[outputs]")
{
    OutputRig rig;
    const auto removed = apply (rig.project, "removeTrack", { { "id", rig.id ("1 out") } });
    CHECK (order (removed) == "1 1 Main 2");
    CHECK (nodeOfType (trackNamed (removed, "1"), nodetype::busOutput) == nullptr);
    CHECK (std::ranges::none_of (
        trackNamed (removed, "1").graph.connections, [&] (const Connection& c)
        { return c.toNode == nodeOfType (trackNamed (rig.project, "1"), nodetype::busOutput)->id; }));
}

TEST_CASE ("a duplicate feeds the duplicates of its output tracks", "[outputs]")
{
    OutputRig rig;
    const auto copied = apply (rig.project, "duplicateTrack", { { "id", rig.id ("1") } });
    REQUIRE (copied.tracks.size() == rig.project.tracks.size() + 3);
    // 1, 1 Main, 1 out, then the copies.
    const auto& copy = *copied.tracks[3];
    CHECK (copied.tracks[4]->mainOutput);
    CHECK (copied.tracks[4]->source == copy.id);
    const auto& copyOut = *copied.tracks[5];
    CHECK (copyOut.kind == TrackKind::output);
    CHECK (copyOut.source == copy.id);
    CHECK (nodeOfType (copy, nodetype::busOutput)->target == copyOut.id);
    CHECK (nodeOfType (trackNamed (copied, "1"), nodetype::busOutput)->target == idOf (copied, "1 out"));
}

TEST_CASE ("output tracks, their nodes and zone outputs survive a save and open", "[outputs][file]")
{
    OutputRig rig;
    const juce::TemporaryFile temporary (juce::String ("Outputs") + projectfile::extension);
    REQUIRE (projectfile::write (rig.project, { "id", "v" }, temporary.getFile(), false).isEmpty());
    const auto loaded = projectfile::read (temporary.getFile()).project;

    CHECK (trackNamed (loaded, "1 out").kind == TrackKind::output);
    CHECK (trackNamed (loaded, "1 out").source == idOf (loaded, "1"));
    CHECK (nodeOfType (trackNamed (loaded, "1"), nodetype::busOutput)->target == idOf (loaded, "1 out"));
    CHECK (trackNamed (loaded, "1 Main").mainOutput);
    CHECK (! trackNamed (loaded, "1 out").mainOutput);
}

TEST_CASE ("a track with output tracks is only a header: its Main plays on an output track", "[outputs][engine]")
{
    OutputRig rig;

    // The header's sends and pan play no part either.
    rig.edit ("setTrackPan", { { "id", rig.id ("1") }, { "pan", -1.0 } });
    CHECK_THAT (rig.render(), WithinAbs (3 * level, 1e-6));

    // Muting the Main leaves the output track and track 2.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1 Main") }, { "mute", true } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1 Main") }, { "mute", false } });

    // Soloing the Main plays only it.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1 Main") }, { "solo", true } });
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1 Main") }, { "solo", false } });

    // A Main removed stays removed: the header plays only through its other output track.
    rig.edit ("removeTrack", { { "id", rig.id ("1 Main") } });
    rig.edit ("setTrackGain", { { "id", rig.id ("2") }, { "gainDb", 0.0 } });
    CHECK (order (rig.project) == "1 1 out 2");
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));

    // Without output tracks, the track plays on its own line again.
    rig.edit ("removeTrack", { { "id", rig.id ("1 out") } });
    CHECK (order (rig.project) == "1 2");
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
}

// ---------------------------------------------------------------------------------------------------------
// The Sampler's outputs

namespace
{
constexpr auto sampleFrames = 24000;
constexpr auto samplesPerBeat = 24000;

// A sample of constant level, so the output tells which zone played where.
[[nodiscard]] std::shared_ptr<AudioSource> constantSample()
{
    juce::AudioBuffer<float> buffer (1, sampleFrames);

    for (int i = 0; i < sampleFrames; ++i)
        buffer.setSample (0, i, 0.5f);

    return AudioSource::fromBuffer (std::move (buffer), 48000.0);
}

[[nodiscard]] juce::var files (std::initializer_list<const char*> names)
{
    juce::Array<juce::var> list;

    for (const auto* name : names)
        list.add (makeObject ({ { "path", juce::String ("Audio/") + name + ".wav" },
                                { "name", name },
                                { "sampleRate", 48000.0 },
                                { "channels", 1 },
                                { "frames", sampleFrames } }));

    return list;
}

// A Sampler track "Kit" with a kick on key 36 and a snare on key 38, both played at once; no master track.
struct KitRig
{
    KitRig()
    {
        auto setup = projectWithClip();
        project = setup.project;
        trackId = setup.trackId;
        juce::Array<juce::var> notes;
        notes.add (noteArgs (0, ticksPerBeat, 36));
        notes.add (noteArgs (0, ticksPerBeat, 38));
        project = apply (project, "addNotes", { { "clipId", setup.clipId }, { "notes", notes } });
        project = apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::sampler } });
        project = apply (project, "renameTrack", { { "id", trackId }, { "name", "Kit" } });

        for (const auto& node : project.tracks[0]->graph.nodes)
            if (node.type == nodetype::sampler)
                samplerId = node.id;

        edit ("setNodeParam", { { "param", "velocity" }, { "value", 0.0 } });
        edit ("addSampleZones", { { "files", files ({ "kick", "snare" }) }, { "key", 36 } });
        edit ("setSampleZone", { { "zoneId", zones()[1].id }, { "lowKey", 38 }, { "highKey", 38 }, { "rootKey", 38 } });
        compiler.setAudioSourceResolver ([this] (const AudioFile&) { return source; });
    }

    [[nodiscard]] const std::vector<SampleZone>& zones() const
    {
        return project.tracks[0]->findNode (samplerId)->sampleZones;
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("trackId", trackId);
        object.getDynamicObject()->setProperty ("nodeId", samplerId);
        project = applyCommand (project, command, object);
    }

    // The level after the attack, with the track's main output and an output track's gains.
    [[nodiscard]] float play()
    {
        engine.setEngine (compiler.compile (project, spec));
        engine.play();
        return render (engine, samplesPerBeat / 4).left()[2000];
    }

    std::shared_ptr<AudioSource> source = constantSample();
    Project project;
    ObjectId trackId = 0;
    ObjectId samplerId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

TEST_CASE ("a zone on an output that leads nowhere plays in Main", "[outputs][sampler]")
{
    KitRig rig;
    const auto both = rig.play();
    CHECK (both > 0.5f);

    rig.edit ("setSampleZone", { { "zoneId", rig.zones()[0].id }, { "output", 5 } });
    CHECK (rig.zones()[0].output == 5);
    CHECK_THAT (rig.play(), WithinAbs (both, 1e-5));
}

TEST_CASE ("each pad to its own track: an output track per pad, wired in one step", "[outputs][sampler]")
{
    KitRig rig;
    const auto both = rig.play();
    const auto kickId = rig.zones()[0].id;

    juce::Array<juce::var> outputs;
    juce::Array<juce::var> kickZones;
    kickZones.add (kickId);
    outputs.add (makeObject ({ { "name", "Kick" }, { "zoneIds", kickZones } }));
    rig.edit ("splitInstrumentOutputs", { { "outputs", outputs } });

    CHECK (order (rig.project) == "Kit Kit Main Kick");
    CHECK (rig.zones()[0].output == 2);
    CHECK (rig.zones()[1].output == 1);
    const auto& kit = trackNamed (rig.project, "Kit");
    const auto* node = nodeOfType (kit, nodetype::busOutput);
    REQUIRE (node != nullptr);
    CHECK (node->target == idOf (rig.project, "Kick"));
    CHECK (std::ranges::any_of (kit.graph.connections, [&] (const Connection& c)
                                { return c.fromNode == rig.samplerId && c.fromPort == 1 && c.toNode == node->id; }));

    // The track is only a header now: effects go on its output tracks, MIDI effects and instruments still on it.
    CHECK_THROWS_AS (apply (rig.project, "insertNode", { { "trackId", rig.trackId }, { "type", nodetype::delay } }),
                     CommandError);
    CHECK_NOTHROW (apply (rig.project, "insertNode", { { "trackId", rig.trackId }, { "type", nodetype::chord } }));
    CHECK_NOTHROW (apply (rig.project, "insertNode",
                          { { "trackId", idOf (rig.project, "Kit Main") }, { "type", nodetype::delay } }));

    // Both still sound, the kick through its own track: muting that leaves the snare.
    CHECK_THAT (rig.play(), WithinAbs (both, 1e-4));
    const auto kick = idOf (rig.project, "Kick");
    rig.project = apply (rig.project, "setTrackSwitches", { { "id", kick }, { "mute", true } });
    CHECK_THAT (rig.play(), WithinAbs (both / 2, 1e-4));

    // Removing the output track puts the kick back on Main.
    rig.project = apply (rig.project, "removeTrack", { { "id", kick } });
    CHECK (rig.zones()[0].output == 1);
    CHECK_THAT (rig.play(), WithinAbs (both, 1e-4));
}

TEST_CASE ("a track that becomes a header moves its effects to its Main output track", "[outputs][sampler]")
{
    KitRig rig;
    const auto both = rig.play();
    rig.project = apply (rig.project, "insertNode", { { "trackId", rig.trackId }, { "type", nodetype::utility } });
    const auto utility = nodeOfType (*rig.project.tracks[0], nodetype::utility)->id;
    rig.project =
        apply (rig.project, "setNodeParam",
               { { "trackId", rig.trackId }, { "nodeId", utility }, { "param", "gain" }, { "value", -6.0206 } });
    rig.project =
        apply (rig.project, "addAutomationLane",
               { { "trackId", rig.trackId },
                 { "target", makeObject ({ { "kind", "node" }, { "nodeId", utility }, { "param", "pan" } }) } });
    CHECK_THAT (rig.play(), WithinAbs (both / 2, 1e-4));

    juce::Array<juce::var> outputs;
    juce::Array<juce::var> kickZones;
    kickZones.add (rig.zones()[0].id);
    outputs.add (makeObject ({ { "name", "Kick" }, { "zoneIds", kickZones } }));
    rig.edit ("splitInstrumentOutputs", { { "outputs", outputs } });

    // The Utility and its lane are on Kit Main now: it halves the snare, and the kick plays on its own line.
    const auto& kit = trackNamed (rig.project, "Kit");
    const auto& main = trackNamed (rig.project, "Kit Main");
    CHECK (kit.findNode (utility) == nullptr);
    CHECK (nodeOfType (kit, nodetype::eq) == nullptr);
    CHECK (kit.automation.empty());
    // The Kit's default EQ and Compressor came along in place of Kit Main's own.
    CHECK (std::ranges::count (main.graph.nodes, juce::String (nodetype::eq), &GraphNode::type) == 1);
    REQUIRE (main.findNode (utility) != nullptr);
    CHECK_THAT (main.findNode (utility)->params.at ("gain"), WithinAbs (-6.0206, 1e-6));
    REQUIRE (main.automation.size() == 1);
    CHECK (main.automation[0].target.nodeId == utility);
    CHECK_THAT (rig.play(), WithinAbs (both * 0.75, 1e-4));
}

TEST_CASE ("every pad on its own track: a header and one line per pad, no Main", "[outputs][sampler]")
{
    KitRig rig;
    const auto both = rig.play();
    juce::Array<juce::var> outputs;

    for (size_t z = 0; z < 2; ++z)
    {
        juce::Array<juce::var> zoneIds;
        zoneIds.add (rig.zones()[z].id);
        outputs.add (makeObject ({ { "name", z == 0 ? "Kick" : "Snare" }, { "zoneIds", zoneIds } }));
    }

    // Two outputs, three lines: nothing is left in Main, so it gets no line (MO12).
    rig.edit ("splitInstrumentOutputs", { { "outputs", outputs } });
    CHECK (order (rig.project) == "Kit Kick Snare");
    CHECK_THAT (rig.play(), WithinAbs (both, 1e-4));

    // The header's fader plays no part.
    rig.project = apply (rig.project, "setTrackGain", { { "id", rig.trackId }, { "gainDb", -96.0 } });
    CHECK_THAT (rig.play(), WithinAbs (both, 1e-4));

    // A zone that plays in Main again gives the Main a line.
    rig.edit ("setSampleZone", { { "zoneId", rig.zones()[1].id }, { "output", 1 } });
    CHECK (order (rig.project) == "Kit Kit Main Kick Snare");
    CHECK (trackNamed (rig.project, "Kit Main").mainOutput);
    CHECK_THAT (rig.play(), WithinAbs (both, 1e-4));
}

TEST_CASE ("a Sampler has 15 outputs besides Main to split into", "[outputs][sampler]")
{
    KitRig rig;
    juce::Array<juce::var> outputs;

    for (int i = 0; i < maxSampleOutputs; ++i)
        outputs.add (makeObject ({ { "name", "Out " + juce::String (i) } }));

    CHECK_THROWS (apply (rig.project, "splitInstrumentOutputs",
                         { { "trackId", rig.trackId }, { "nodeId", rig.samplerId }, { "outputs", outputs } }));
}

// ---------------------------------------------------------------------------------------------------------
// Instrument plugins' output buses (MO1, MO2)

namespace
{
// An instrument with a stereo main bus, a stereo "Kick" bus and a mono "Hat" bus, both off at first. Each bus that is
// on plays a constant: 0.1, 0.2 and 0.3.
class MultiOutProcessor final : public juce::AudioProcessor
{
public:
    MultiOutProcessor()
        : juce::AudioProcessor (BusesProperties()
                                    .withOutput ("Main", juce::AudioChannelSet::stereo())
                                    .withOutput ("Kick", juce::AudioChannelSet::stereo(), false)
                                    .withOutput ("Hat", juce::AudioChannelSet::mono(), false))
    {
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
               && (layouts.outputBuses[1].isDisabled() || layouts.outputBuses[1] == juce::AudioChannelSet::stereo())
               && (layouts.outputBuses[2].isDisabled() || layouts.outputBuses[2] == juce::AudioChannelSet::mono());
    }

    const juce::String getName() const override { return "Multi"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override {}
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/) override
    {
        for (int bus = 0; bus < getBusCount (false); ++bus)
        {
            auto channels = getBusBuffer (buffer, false, bus);

            for (int ch = 0; ch < channels.getNumChannels(); ++ch)
                juce::FloatVectorOperations::fill (channels.getWritePointer (ch), 0.1f * static_cast<float> (bus + 1),
                                                   channels.getNumSamples());
        }
    }

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

// The last sample of each side of each port, after one block.
using PortLevels = std::array<std::pair<float, float>, maxPluginOutputs>;

// Runs an instrument plugin node on one block with the ports in `wired` wired.
[[nodiscard]] PortLevels runPlugin (PluginNode& node, juce::uint32 wired)
{
    constexpr int n = 64;
    std::vector<std::vector<float>> channels (2 * maxPluginOutputs, std::vector<float> (n, -1.0f));
    std::array<AudioOutput, maxPluginOutputs> outputs;

    for (size_t port = 0; port < outputs.size(); ++port)
        outputs[port] = { channels[2 * port].data(), channels[2 * port + 1].data() };

    const Transport transport;
    ProcessContext context;
    context.numSamples = n;
    context.audioOutputs = outputs;
    context.wiredOutputs = wired;
    context.transport = &transport;
    node.process (context);

    PortLevels levels;

    for (size_t port = 0; port < outputs.size(); ++port)
        levels[port] = { channels[2 * port].back(), channels[2 * port + 1].back() };

    return levels;
}

[[nodiscard]] GraphNode pluginSettings (bool allOutputs)
{
    GraphNode node;
    node.type = nodetype::pluginInstrument;
    node.allOutputs = allOutputs;
    return node;
}
} // namespace

TEST_CASE ("an instrument plugin's output buses play from their ports while wired, else in Main", "[outputs][plugins]")
{
    PluginNode node (true);
    node.prepare (spec);
    node.attach (std::make_unique<MultiOutProcessor>());
    REQUIRE (node.descriptor().outputs.size() == static_cast<size_t> (maxPluginOutputs));

    // As the plugin starts: only Main is on.
    const auto buses = node.outputBuses();
    REQUIRE (buses.size() == 3);
    CHECK (buses[1]["name"].toString() == "Kick");
    CHECK (! static_cast<bool> (buses[1]["on"]));
    const auto first = runPlugin (node, 1u | 2u);
    CHECK_THAT (first[0].first, WithinAbs (0.1, 1e-6));
    CHECK_THAT (first[1].first, WithinAbs (0.0, 1e-6));

    // Use all outputs: unwired, everything plays in Main.
    node.configure (pluginSettings (true));
    CHECK (static_cast<bool> (node.outputBuses()[2]["on"]));
    CHECK_THAT (runPlugin (node, 1u)[0].first, WithinAbs (0.6, 1e-6));

    // Wired, a bus leaves Main for its port; a mono bus plays on both sides.
    const auto kick = runPlugin (node, 1u | 2u);
    CHECK_THAT (kick[0].first, WithinAbs (0.4, 1e-6));
    CHECK_THAT (kick[1].first, WithinAbs (0.2, 1e-6));
    CHECK_THAT (kick[1].second, WithinAbs (0.2, 1e-6));
    const auto hat = runPlugin (node, 1u | 4u);
    CHECK_THAT (hat[0].first, WithinAbs (0.3, 1e-6));
    CHECK_THAT (hat[2].first, WithinAbs (0.3, 1e-6));
    CHECK_THAT (hat[2].second, WithinAbs (0.3, 1e-6));

    // A wired port whose bus is off again is silent.
    node.configure (pluginSettings (false));
    const auto off = runPlugin (node, 1u | 2u);
    CHECK_THAT (off[0].first, WithinAbs (0.1, 1e-6));
    CHECK_THAT (off[1].first, WithinAbs (0.0, 1e-6));
}

TEST_CASE ("each plugin output to its own track: buses on, an output track per port, in one step", "[outputs][plugins]")
{
    Project project;
    project = apply (project, "addTrack", { { "name", "Drums" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "insertNode",
                     { { "trackId", trackId },
                       { "type", nodetype::pluginInstrument },
                       { "plugin", makeObject ({ { "identifier", "drums" } }) } });
    const auto pluginId = nodeOfType (*project.tracks[0], nodetype::pluginInstrument)->id;

    juce::Array<juce::var> outputs;
    outputs.add (makeObject ({ { "port", 1 }, { "name", "Kick" } }));
    outputs.add (makeObject ({ { "port", 2 }, { "name", "Hat" } }));
    project = apply (project, "splitPluginOutputs",
                     { { "trackId", trackId }, { "nodeId", pluginId }, { "outputs", outputs } });

    // Main, Kick and Hat each get a line, under the header (MO12).
    CHECK (order (project) == "Drums Drums Main Kick Hat");
    CHECK (trackNamed (project, "Drums Main").mainOutput);
    const auto& drums = trackNamed (project, "Drums");
    CHECK (nodeOfType (drums, nodetype::pluginInstrument)->allOutputs);
    CHECK (std::ranges::count_if (drums.graph.connections, [&] (const Connection& c)
                                  { return c.fromNode == pluginId && (c.fromPort == 1 || c.fromPort == 2); })
           == 2);

    // Ports already wired are left alone; Main is not an extra output.
    CHECK (apply (project, "splitPluginOutputs",
                  { { "trackId", trackId }, { "nodeId", pluginId }, { "outputs", outputs } })
               .tracks.size()
           == project.tracks.size());
    juce::Array<juce::var> main;
    main.add (makeObject ({ { "port", 0 } }));
    CHECK_THROWS (
        apply (project, "splitPluginOutputs", { { "trackId", trackId }, { "nodeId", pluginId }, { "outputs", main } }));

    // The setting survives a save and open, and can be turned off.
    const juce::TemporaryFile temporary (juce::String ("PluginOutputs") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "v" }, temporary.getFile(), false).isEmpty());
    const auto loaded = projectfile::read (temporary.getFile()).project;
    CHECK (nodeOfType (trackNamed (loaded, "Drums"), nodetype::pluginInstrument)->allOutputs);
    project = apply (project, "setPluginOutputs", { { "trackId", trackId }, { "nodeId", pluginId }, { "all", false } });
    CHECK (! nodeOfType (trackNamed (project, "Drums"), nodetype::pluginInstrument)->allOutputs);
}
