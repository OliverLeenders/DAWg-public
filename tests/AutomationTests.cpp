#include "TestUtils.h"
#include "dawg/MidiInput.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Phase 10: automation lanes and clip controllers (docs/design/phase10-automation.md).

namespace
{
constexpr float level = 0.25f;
constexpr int frames = 4096;
constexpr size_t probe = 2000;

[[nodiscard]] juce::var list (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] juce::var point (Ticks time, double value, int shape = 0)
{
    return list ({ time, value, shape });
}

[[nodiscard]] juce::var target (const char* kind, ObjectId nodeId = 0, const char* param = "")
{
    return makeObject ({ { "kind", kind }, { "nodeId", nodeId }, { "param", param } });
}

[[nodiscard]] const GraphNode& nodeOfType (const Track& track, const char* type)
{
    const auto it = std::ranges::find_if (track.graph.nodes, [type] (const GraphNode& n) { return n.type == type; });
    REQUIRE (it != track.graph.nodes.end());
    return *it;
}

// An audio track that always monitors a constant input, so its level is known.
struct Rig
{
    Rig()
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

    [[nodiscard]] ObjectId lane (int index = 0) const
    {
        return project.tracks[0]->automation[static_cast<size_t> (index)].id;
    }

    [[nodiscard]] StereoBuffer render()
    {
        if (auto compiled = compiler.update (project, spec); compiled != nullptr)
            engine.setEngine (compiled);

        const std::array<const float*, 1> inputs { input.data() };
        StereoBuffer out (frames);
        engine.renderOffline (out.channels(), 2, frames, sampleRate, inputs.data(), 1);
        return out;
    }

    std::vector<float> input;
    Project project;
    ObjectId trackId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

// ---------------------------------------------------------------------------------------------------------
// Commands

TEST_CASE ("automation lanes are one per parameter and go with their node", "[automation][commands]")
{
    Rig rig;
    rig.edit ("addAutomationLane", { { "trackId", rig.trackId }, { "target", target ("gain") } });
    CHECK (rig.project.tracks[0]->automationExpanded);
    CHECK_THROWS_AS (
        apply (rig.project, "addAutomationLane", { { "trackId", rig.trackId }, { "target", target ("gain") } }),
        CommandError);

    rig.edit ("addNode", { { "trackId", rig.trackId }, { "type", nodetype::utility } });
    const auto utility = nodeOfType (*rig.project.tracks[0], nodetype::utility).id;
    CHECK_THROWS_AS (apply (rig.project, "addAutomationLane",
                            { { "trackId", rig.trackId }, { "target", target ("node", utility, "nope") } }),
                     CommandError);

    rig.edit ("addAutomationLane", { { "trackId", rig.trackId }, { "target", target ("node", utility, "gain") } });
    REQUIRE (rig.project.tracks[0]->automation.size() == 2);

    rig.edit ("removeNode", { { "trackId", rig.trackId }, { "nodeId", utility } });
    CHECK (rig.project.tracks[0]->automation.size() == 1);

    // Points: clamped to 0..1; writing replaces a range.
    rig.edit ("setAutomationPoints", { { "trackId", rig.trackId },
                                       { "laneId", rig.lane() },
                                       { "points", list ({ point (0, 2.0), point (1000, 0.2) }) } });
    CHECK_THAT (rig.project.tracks[0]->automation[0].points[0].value, WithinAbs (1.0, 1e-9));

    rig.edit ("writeAutomation", { { "trackId", rig.trackId },
                                   { "laneId", rig.lane() },
                                   { "from", 500 },
                                   { "to", 1500 },
                                   { "points", list ({ point (600, 0.5), point (1400, 0.6) }) } });
    const auto& points = rig.project.tracks[0]->automation[0].points;
    REQUIRE (points.size() == 3);
    CHECK (points[1].time == 600);
    CHECK (points[2].time == 1400);
}

TEST_CASE ("automation and clip controllers round-trip through project files", "[automation][file]")
{
    auto setup = projectWithClip();
    auto project = apply (
        setup.project, "addAutomationLane",
        { { "trackId", setup.trackId }, { "target", target ("pan") }, { "points", list ({ point (0, 0.25) }) } });
    project = apply (project, "setTrackAutomation", { { "id", setup.trackId }, { "mode", 3 } });
    project = apply (project, "setClipController",
                     { { "clipId", setup.clipId },
                       { "controller", 64 },
                       { "points", list ({ point (0, 1.0, 1), point (960, 0.0, 1) }) } });

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-automation-" + juce::Uuid().toString() + ".dawg");
    REQUIRE (projectfile::write (project, { "id", "v" }, file, false).isEmpty());
    const auto loaded = projectfile::read (file).project;
    std::ignore = file.deleteFile();

    CHECK (juce::JSON::toString (toVar (loaded)) == juce::JSON::toString (toVar (project)));
    CHECK (loaded.tracks[0]->automationMode == AutomationMode::latch);
    CHECK (loaded.nextId == project.nextId);
}

// ---------------------------------------------------------------------------------------------------------
// Playback

TEST_CASE ("gain and pan lanes override the fader while the mode plays them", "[automation][engine]")
{
    Rig rig;
    rig.edit ("addAutomationLane",
              { { "trackId", rig.trackId }, { "target", target ("gain") }, { "points", list ({ point (0, 0.5) }) } });

    // 0.5 on the fader curve is 2 * 0.25 = 0.5 linear.
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * 0.5, 1e-4));
    CHECK_THAT (rig.compiler.laneState (rig.lane())->value.load(), WithinAbs (0.5, 1e-6));

    rig.edit ("setTrackAutomation", { { "id", rig.trackId }, { "mode", 0 } });
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level, 1e-4));

    // While a lane is written, the manual value plays.
    rig.edit ("setTrackAutomation", { { "id", rig.trackId }, { "mode", 1 } });
    rig.compiler.laneState (rig.lane())->overridden.store (true);
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level, 1e-4));
    rig.compiler.laneState (rig.lane())->overridden.store (false);

    rig.edit ("addAutomationLane",
              { { "trackId", rig.trackId }, { "target", target ("pan") }, { "points", list ({ point (0, 0.0) }) } });
    const auto out = rig.render();
    CHECK_THAT (out.left()[probe], WithinAbs (level * 0.5, 1e-4));
    CHECK_THAT (out.right()[probe], WithinAbs (0.0, 1e-6));

    // Switching a lane off leaves the parameter alone.
    rig.edit ("setAutomationLane", { { "trackId", rig.trackId }, { "laneId", rig.lane (1) }, { "active", false } });
    CHECK_THAT (rig.render().right()[probe], WithinAbs (level * 0.5, 1e-4));
}

TEST_CASE ("node parameter lanes move the parameter along their curve", "[automation][engine]")
{
    Rig rig;
    const auto gain = nodeOfType (*rig.project.tracks[0], nodetype::gain).id;
    const auto& info = NodeRegistry::builtin().find (nodetype::gain)->params[0];
    const auto halfGain = paramToNormalised (info, -6.0);
    CHECK_THAT (paramFromNormalised (info, halfGain), WithinAbs (-6.0, 1e-4));

    rig.edit ("addAutomationLane", { { "trackId", rig.trackId },
                                     { "target", target ("node", gain, "gainDb") },
                                     { "points", list ({ point (0, halfGain) }) } });
    std::ignore = rig.render(); // the gain node ramps to its new value over a block
    CHECK_THAT (rig.render().left()[probe], WithinAbs (level * std::pow (10.0, -6.0 / 20.0), 1e-4));
}

TEST_CASE ("a clip's sustain curve holds notes and lifts outside the clip", "[automation][engine][midi]")
{
    auto setup = projectWithClip(); // a 16-beat clip from 0
    auto project = apply (setup.project, "addNotes",
                          { { "clipId", setup.clipId }, { "notes", list ({ noteArgs (0, ticksPerBeat / 4, 69) }) } });

    // Pedal down for the first two beats.
    project = apply (project, "setClipController",
                     { { "clipId", setup.clipId },
                       { "controller", 64 },
                       { "points", list ({ point (0, 1.0, 1), point (2 * ticksPerBeat, 0.0, 1) }) } });

    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();

    // The note ends after a quarter beat, but sounds on while the pedal is down...
    std::ignore = render (engine, 24000);
    CHECK (firstAudible (render (engine, 1024).left()) >= 0);

    // ...and fades after it lifts at beat 2 (the synth's release takes about half a second).
    std::ignore = render (engine, 4 * 24000);
    CHECK (firstAudible (render (engine, 1024).left()) == -1);
}

TEST_CASE ("the recorder keeps the pedal and controllers as clip curves", "[automation][midi][record]")
{
    NoteRecorder recorder;
    LiveInput pedal;
    pedal.type = NoteEventType::sustain;
    pedal.value = 1.0f;
    recorder.add ({ pedal, 1000.0 });
    recorder.add ({ pedal, 1500.0 }); // repeated value: dropped
    pedal.value = 0.0f;
    recorder.add ({ pedal, 3000.0 });

    LiveInput bend;
    bend.type = NoteEventType::controller;
    bend.lane = controllerPitchBend;
    bend.value = 1.0f;
    recorder.add ({ bend, 2000.0 });

    CHECK_FALSE (recorder.empty());
    const auto controllers = recorder.controllers (1000, 4000);
    const auto& sustain = controllers["64"];
    REQUIRE (sustain.size() == 2);
    CHECK (static_cast<int> (sustain[0][0]) == 0);
    CHECK (static_cast<int> (sustain[1][0]) == 2000);
    CHECK (static_cast<int> (sustain[0][2]) == 1); // a step

    const auto& pitch = controllers[juce::Identifier (juce::String (controllerKeyPitchBend))];
    REQUIRE (pitch.size() == 1);
    CHECK_THAT (static_cast<double> (pitch[0][1]), WithinAbs (1.0, 1e-9));
}
