#include "TestUtils.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Mixing basics: pan, meters and the master track (docs/design/mixing-basics.md).

namespace
{
constexpr float level = 0.25f;
constexpr int frames = 4096;
constexpr size_t probe = 100;

// An audio track that always monitors a constant input, so its output level is known; and an engine.
struct MixRig
{
    explicit MixRig (bool master = false)
        : input (frames, level)
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project = apply (project, "setTrackSwitches", { { "id", trackId }, { "inputChannel", 0 }, { "monitor", 2 } });

        if (master)
            project = withMaster (project);

        engine.setEngine (compiler.compile (project, spec));
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);

        if (auto compiled = compiler.update (project, spec))
            engine.setEngine (std::move (compiled));
    }

    [[nodiscard]] StereoBuffer render()
    {
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

TEST_CASE ("pan follows an equal-power law with unity at the centre", "[mixing]")
{
    CHECK_THAT (panGains (0.0f)[0], WithinAbs (1.0, 1e-6));
    CHECK_THAT (panGains (0.0f)[1], WithinAbs (1.0, 1e-6));
    CHECK_THAT (panGains (-1.0f)[0], WithinAbs (1.0, 1e-6));
    CHECK_THAT (panGains (-1.0f)[1], WithinAbs (0.0, 1e-6));
    CHECK_THAT (panGains (0.5f)[0],
                WithinAbs (std::sqrt (2.0) * std::cos (0.375 * juce::MathConstants<double>::pi), 1e-6));
    CHECK_THAT (panGains (0.5f)[1], WithinAbs (1.0, 1e-6));
}

TEST_CASE ("pan is clamped, stored and serialised", "[mixing][commands]")
{
    auto project = apply ({}, "addTrack", {});
    const auto id = project.tracks[0]->id;

    CHECK_THAT (apply (project, "setTrackPan", { { "id", id }, { "pan", 3 } }).tracks[0]->pan, WithinAbs (1.0, 1e-9));
    project = apply (project, "setTrackPan", { { "id", id }, { "pan", -0.4 } });
    CHECK_THAT (project.tracks[0]->pan, WithinAbs (-0.4, 1e-9));
    CHECK_THAT (static_cast<double> (toVar (*project.tracks[0])["pan"]), WithinAbs (-0.4, 1e-9));
}

TEST_CASE ("panned tracks are louder on one side", "[mixing][engine]")
{
    MixRig rig;
    const auto centre = rig.render();
    CHECK_THAT (centre.left()[probe], WithinAbs (level, 1e-6));
    CHECK_THAT (centre.right()[probe], WithinAbs (level, 1e-6));

    rig.edit ("setTrackPan", { { "id", rig.trackId }, { "pan", -1 } });
    const auto left = rig.render();
    CHECK_THAT (left.left()[probe], WithinAbs (level, 1e-6));
    CHECK_THAT (left.right()[probe], WithinAbs (0.0, 1e-6));

    rig.edit ("setTrackPan", { { "id", rig.trackId }, { "pan", 0.5 } });
    const auto right = rig.render();
    CHECK_THAT (right.left()[probe], WithinAbs (level * panGains (0.5f)[0], 1e-6));
    CHECK_THAT (right.right()[probe], WithinAbs (level, 1e-6));
}

TEST_CASE ("track meters show the level after fader and pan, and the input level", "[mixing][engine]")
{
    MixRig rig;
    rig.edit ("setTrackGain", { { "id", rig.trackId }, { "gainDb", -6 } });
    rig.edit ("setTrackPan", { { "id", rig.trackId }, { "pan", 1 } });
    std::ignore = rig.render();

    const auto state = rig.compiler.trackState (rig.trackId);
    const auto gain = std::pow (10.0, -6.0 / 20.0);
    CHECK_THAT (state->peakLeft.exchange (0.0f), WithinAbs (0.0, 1e-6));
    CHECK_THAT (state->peakRight.exchange (0.0f), WithinAbs (level * gain, 1e-5));
    CHECK_THAT (state->inputPeak.exchange (0.0f), WithinAbs (level, 1e-6));

    // Muted tracks show nothing after the fader, but their input is still metered.
    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "mute", true } });
    std::ignore = rig.render();
    CHECK_THAT (state->peakRight.load(), WithinAbs (0.0, 1e-9));
    CHECK_THAT (state->inputPeak.load(), WithinAbs (level, 1e-6));
}

TEST_CASE ("the master track has a fixed chain that takes effects only", "[mixing][commands]")
{
    const auto project = withMaster (apply ({}, "addTrack", {}));
    REQUIRE (project.master != nullptr);
    CHECK (project.master->kind == TrackKind::master);
    CHECK (project.master->graph.nodes.size() == 3);
    CHECK (project.findTrack (project.master->id) == nullptr);
    CHECK (project.findTrackOrMaster (project.master->id) == project.master.get());
    CHECK (withMaster (project).master == project.master);

    const auto masterId = project.master->id;
    const auto pluginRef = makeObject ({ { "identifier", "x" }, { "name", "X" } });
    CHECK_THROWS_AS (
        apply (project, "insertNode",
               { { "trackId", masterId }, { "type", nodetype::pluginInstrument }, { "plugin", pluginRef } }),
        CommandError);
    CHECK_THROWS_AS (apply (project, "addClip", { { "trackId", masterId }, { "start", 0 }, { "length", 100 } }),
                     CommandError);
    CHECK_THROWS_AS (apply (project, "removeTrack", { { "id", masterId } }), CommandError);

    const auto withEffect =
        apply (project, "insertNode",
               { { "trackId", masterId }, { "type", nodetype::pluginEffect }, { "plugin", pluginRef } });
    CHECK (withEffect.master->graph.nodes.size() == 4);
    CHECK_THAT (apply (withEffect, "setTrackGain", { { "id", masterId }, { "gainDb", -3 } }).master->gainDb,
                WithinAbs (-3.0, 1e-9));
}

TEST_CASE ("the master fader and pan apply to the whole mix", "[mixing][engine]")
{
    MixRig rig (true);
    const auto masterId = rig.project.master->id;
    rig.edit ("addTrack", { { "kind", "audio" } });
    const auto second = rig.project.tracks[1]->id;
    rig.edit ("setTrackSwitches", { { "id", second }, { "inputChannel", 0 }, { "monitor", 2 } });

    const auto both = rig.render();
    CHECK_THAT (both.left()[probe], WithinAbs (2 * level, 1e-6));

    rig.edit ("setTrackGain", { { "id", masterId }, { "gainDb", -96 } });
    CHECK (firstAudible (rig.render().left(), 1.0e-3f) == -1);

    rig.edit ("setTrackGain", { { "id", masterId }, { "gainDb", 0 } });
    rig.edit ("setTrackPan", { { "id", masterId }, { "pan", -1 } });
    const auto panned = rig.render();
    CHECK_THAT (panned.left()[probe], WithinAbs (2 * level, 1e-6));
    CHECK_THAT (panned.right()[probe], WithinAbs (0.0, 1e-6));

    const auto meters = rig.compiler.trackState (masterId);
    CHECK_THAT (meters->peakLeft.load(), WithinAbs (2 * level, 1e-6));
}

TEST_CASE ("the master track round-trips through project files", "[mixing][file]")
{
    auto project = withMaster (apply ({}, "addTrack", {}));
    project = apply (project, "setTrackGain", { { "id", project.master->id }, { "gainDb", -2 } });
    project = apply (project, "setTrackPan", { { "id", project.tracks[0]->id }, { "pan", 0.25 } });

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-mix-" + juce::Uuid().toString() + ".dawg");
    REQUIRE (projectfile::write (project, { "id", "v" }, file, false).isEmpty());
    const auto loaded = projectfile::read (file).project;
    std::ignore = file.deleteFile();

    CHECK (juce::JSON::toString (toVar (loaded)) == juce::JSON::toString (toVar (project)));
    CHECK (loaded.nextId == project.nextId);
}
