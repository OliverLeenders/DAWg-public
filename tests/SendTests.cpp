#include "TestUtils.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Return tracks and sends (docs/design/groups-and-sends.md, step 2): the model, the commands, the file and the
// engine.

namespace
{
[[nodiscard]] ObjectId idOf (const Project& project, const juce::String& name)
{
    for (const auto& track : project.tracks)
        if (track->name == name)
            return track->id;

    FAIL ("No track named " << name); // [[noreturn]]: the loop above either returns or fails
}

[[nodiscard]] const Track& trackNamed (const Project& project, const juce::String& name)
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

[[nodiscard]] juce::var ids (std::initializer_list<ObjectId> values)
{
    juce::Array<juce::var> result;

    for (const auto id : values)
        result.add (id);

    return result;
}

// Tracks "1" and "2" and returns "A" and "B".
[[nodiscard]] Project withReturns()
{
    Project project;
    project = apply (project, "addTrack", { { "name", "1" } });
    project = apply (project, "addTrack", { { "kind", "return" }, { "name", "A" } });
    project = apply (project, "addTrack", { { "name", "2" } }); // goes before the returns
    project = apply (project, "addTrack", { { "kind", "return" }, { "name", "B" } });
    return project;
}

[[nodiscard]] Project send (const Project& project, const juce::String& from, const juce::String& to,
                            std::initializer_list<std::pair<const char*, juce::var>> extra = {})
{
    auto args = makeObject ({ { "trackId", idOf (project, from) }, { "target", idOf (project, to) } });

    for (const auto& [key, value] : extra)
        args.getDynamicObject()->setProperty (key, value);

    return applyCommand (project, "addSend", args);
}
} // namespace

TEST_CASE ("return tracks come after every other track", "[sends]")
{
    const auto project = withReturns();
    CHECK (order (project) == "1 2 A B");
    CHECK (trackNamed (project, "A").kind == TrackKind::returnTrack);
    REQUIRE (trackNamed (project, "A").graph.nodes.size() == 5);
    CHECK (trackNamed (project, "A").graph.nodes[0].type == nodetype::mixInput);

    // Returns reorder among themselves; other tracks stay out of their section.
    auto moved = apply (project, "moveTracks", { { "ids", ids ({ idOf (project, "A") }) }, { "parent", 0 } });
    CHECK (order (moved) == "1 2 B A");
    moved = apply (moved, "moveTracks", { { "ids", ids ({ idOf (moved, "1") }) }, { "parent", 0 } });
    CHECK (order (moved) == "2 1 B A");

    CHECK_THROWS_AS (
        apply (project, "moveTracks",
               { { "ids", ids ({ idOf (project, "1") }) }, { "parent", 0 }, { "before", idOf (project, "A") } }),
        CommandError);
    CHECK_THROWS_AS (apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "A") }) } }), CommandError);

    const auto grouped = apply (project, "groupTracks", { { "ids", ids ({ idOf (project, "1") }) }, { "name", "G" } });
    CHECK_THROWS_AS (
        apply (grouped, "moveTracks", { { "ids", ids ({ idOf (grouped, "A") }) }, { "parent", idOf (grouped, "G") } }),
        CommandError);
}

TEST_CASE ("sends go to returns, once each, without loops", "[sends]")
{
    auto project = withReturns();
    project = send (project, "1", "A");

    const auto& sends = trackNamed (project, "1").sends;
    REQUIRE (sends.size() == 1);
    CHECK (sends[0].target == idOf (project, "A"));
    CHECK_THAT (sends[0].levelDb, WithinAbs (0.0, 1e-9));
    CHECK_FALSE (sends[0].preFader);
    CHECK (sends[0].active);

    CHECK_THROWS_AS (send (project, "1", "A"), CommandError); // already there
    CHECK_THROWS_AS (send (project, "1", "2"), CommandError); // not a return
    CHECK_THROWS_AS (send (project, "A", "A"), CommandError); // itself

    // A return may feed another, but not back.
    project = send (project, "A", "B");
    CHECK_THROWS_AS (send (project, "B", "A"), CommandError);

    // Changing a send.
    const auto sendId = trackNamed (project, "1").sends[0].id;
    project =
        apply (project, "setSend",
               { { "trackId", idOf (project, "1") }, { "sendId", sendId }, { "levelDb", 99.0 }, { "preFader", true } });
    CHECK_THAT (trackNamed (project, "1").sends[0].levelDb, WithinAbs (limits::maxGainDb, 1e-9));
    CHECK (trackNamed (project, "1").sends[0].preFader);
}

TEST_CASE ("a new return can come with a send to it", "[sends]")
{
    auto project = withReturns();
    project =
        apply (project, "addTrack", { { "kind", "return" }, { "name", "C" }, { "sendFrom", idOf (project, "2") } });
    REQUIRE (trackNamed (project, "2").sends.size() == 1);
    CHECK (trackNamed (project, "2").sends[0].target == idOf (project, "C"));
}

TEST_CASE ("send levels can be automated, and the lanes go with their sends", "[sends][automation]")
{
    auto project = withReturns();
    project = send (project, "1", "A");
    const auto sendId = trackNamed (project, "1").sends[0].id;

    project = apply (project, "addAutomationLane",
                     { { "trackId", idOf (project, "1") },
                       { "target", makeObject ({ { "kind", "send" }, { "param", juce::String (sendId) } }) } });
    REQUIRE (trackNamed (project, "1").automation.size() == 1);
    CHECK (trackNamed (project, "1").automation[0].target.kind == AutomationTargetKind::send);

    CHECK_THROWS_AS (apply (project, "addAutomationLane",
                            { { "trackId", idOf (project, "2") },
                              { "target", makeObject ({ { "kind", "send" }, { "param", juce::String (sendId) } }) } }),
                     CommandError);

    // Removing the return removes the send and its lane.
    const auto removed = apply (project, "removeTrack", { { "id", idOf (project, "A") } });
    CHECK (trackNamed (removed, "1").sends.empty());
    CHECK (trackNamed (removed, "1").automation.empty());

    // So does removing the send.
    const auto unsent = apply (project, "removeSend", { { "trackId", idOf (project, "1") }, { "sendId", sendId } });
    CHECK (trackNamed (unsent, "1").sends.empty());
    CHECK (trackNamed (unsent, "1").automation.empty());
}

TEST_CASE ("duplicates keep their sends, with lanes that follow", "[sends]")
{
    auto project = withReturns();
    project = send (project, "1", "A");
    const auto sendId = trackNamed (project, "1").sends[0].id;
    project = apply (project, "addAutomationLane",
                     { { "trackId", idOf (project, "1") },
                       { "target", makeObject ({ { "kind", "send" }, { "param", juce::String (sendId) } }) } });

    project = apply (project, "duplicateTrack", { { "id", idOf (project, "1") } });
    REQUIRE (project.tracks.size() == 5);
    const auto& copy = *project.tracks[1];
    REQUIRE (copy.sends.size() == 1);
    CHECK (copy.sends[0].id != sendId);
    CHECK (copy.sends[0].target == idOf (project, "A"));
    CHECK (copy.automation[0].target.param == juce::String (copy.sends[0].id));

    // A duplicated return stays in the returns section.
    project = apply (project, "duplicateTrack", { { "id", idOf (project, "A") } });
    CHECK (project.tracks[4]->kind == TrackKind::returnTrack);
    CHECK (project.tracks.back()->name == "B");
}

TEST_CASE ("returns and sends survive a save and open, and bad ones are repaired", "[sends][file]")
{
    auto project = withReturns();
    project = send (project, "1", "A", { { "levelDb", -6.0 }, { "preFader", true } });

    const juce::TemporaryFile temporary (juce::String ("Sends") + projectfile::extension);
    REQUIRE (projectfile::write (project, { "id", "v" }, temporary.getFile(), false).isEmpty());
    const auto loaded = projectfile::read (temporary.getFile()).project;

    CHECK (order (loaded) == "1 2 A B");
    REQUIRE (trackNamed (loaded, "1").sends.size() == 1);
    CHECK_THAT (trackNamed (loaded, "1").sends[0].levelDb, WithinAbs (-6.0, 1e-9));
    CHECK (trackNamed (loaded, "1").sends[0].preFader);

    // A return out of place goes back to the end; a send to something that isn't a return is dropped.
    auto broken = project;
    std::swap (broken.tracks[1], broken.tracks[2]); // 1 A 2 B
    auto track = *broken.tracks[0];
    track.sends.push_back ({ 999, idOf (broken, "2") });
    broken.tracks[0] = std::make_shared<const Track> (std::move (track));
    normaliseGroups (broken);
    CHECK (order (broken) == "1 2 A B");
    CHECK (trackNamed (broken, "1").sends.size() == 1);
}

// ---------------------------------------------------------------------------------------------------------
// The engine

namespace
{
constexpr float level = 0.25f;
constexpr int frames = 2048;
constexpr size_t probe = 100;

// Two monitored audio tracks with a constant input (each adds `level`), a return "R" and a master track.
struct SendRig
{
    SendRig()
        : input (frames, level)
    {
        for (const auto* name : { "1", "2" })
        {
            project = apply (project, "addTrack", { { "kind", "audio" }, { "name", name } });
            project = apply (project, "setTrackSwitches",
                             { { "id", project.tracks.back()->id }, { "inputChannel", 0 }, { "monitor", 2 } });
        }

        project = apply (project, "addTrack", { { "kind", "return" }, { "name", "R" } });
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
    [[nodiscard]] ObjectId sendOf (const juce::String& name) const { return trackNamed (project, name).sends[0].id; }

    std::vector<float> input;
    Project project;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

TEST_CASE ("sends feed their return, before or after the fader", "[sends][engine]")
{
    SendRig rig;
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));

    // Post-fader at 0 dB: track 1 is heard dry and through the return.
    rig.edit ("addSend", { { "trackId", rig.id ("1") }, { "target", rig.id ("R") } });
    CHECK_THAT (rig.render(), WithinAbs (3 * level, 1e-6));

    // Level and switches change without recompiling.
    rig.edit ("setSend", { { "trackId", rig.id ("1") }, { "sendId", rig.sendOf ("1") }, { "levelDb", -96.0 } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-4));
    rig.edit ("setSend",
              { { "trackId", rig.id ("1") }, { "sendId", rig.sendOf ("1") }, { "levelDb", 0.0 }, { "active", false } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
    rig.edit ("setSend", { { "trackId", rig.id ("1") }, { "sendId", rig.sendOf ("1") }, { "active", true } });

    // Pulling the fader down takes a post-fader send with it, but not a pre-fader one.
    rig.edit ("setTrackGain", { { "id", rig.id ("1") }, { "gainDb", -96.0 } });
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-4));
    rig.edit ("setSend", { { "trackId", rig.id ("1") }, { "sendId", rig.sendOf ("1") }, { "preFader", true } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-4));

    // The return's fader and meter.
    rig.edit ("setTrackGain", { { "id", rig.id ("R") }, { "gainDb", -96.0 } });
    CHECK_THAT (rig.render(), WithinAbs (level, 1e-4));
    rig.edit ("setTrackGain", { { "id", rig.id ("R") }, { "gainDb", 0.0 } });
    std::ignore = rig.render();
    CHECK_THAT (rig.compiler.trackState (rig.id ("R"))->peakLeft.load(), WithinAbs (level, 1e-6));
}

TEST_CASE ("returns feed returns, in order", "[sends][engine]")
{
    SendRig rig;
    rig.edit ("addTrack", { { "kind", "return" }, { "name", "S" } });
    rig.edit ("addSend", { { "trackId", rig.id ("1") }, { "target", rig.id ("S") } });
    rig.edit ("addSend", { { "trackId", rig.id ("S") }, { "target", rig.id ("R") } });

    // Track 1 dry, through S, and through S into R; track 2 dry.
    CHECK_THAT (rig.render(), WithinAbs (4 * level, 1e-6));
}

TEST_CASE ("returns are solo-safe, and soloing one keeps its senders audible", "[sends][engine]")
{
    SendRig rig;
    rig.edit ("addSend", { { "trackId", rig.id ("1") }, { "target", rig.id ("R") } });

    // Soloing the sender: it and the return stay, track 2 goes.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "solo", true } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("1") }, { "solo", false } });

    // Soloing the return: its sender stays too.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("R") }, { "solo", true } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
    rig.edit ("setTrackSwitches", { { "id", rig.id ("R") }, { "solo", false } });

    // A muted return passes nothing on.
    rig.edit ("setTrackSwitches", { { "id", rig.id ("R") }, { "mute", true } });
    CHECK_THAT (rig.render(), WithinAbs (2 * level, 1e-6));
}
