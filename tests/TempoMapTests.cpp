#include "TestUtils.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// The tempo map and time signatures (docs/design/tempo-map.md).

namespace
{
constexpr Ticks beat = ticksPerBeat;

[[nodiscard]] juce::var fileArgs (juce::int64 frames)
{
    return makeObject (
        { { "path", "a.wav" }, { "name", "a" }, { "sampleRate", 48000 }, { "channels", 2 }, { "frames", frames } });
}
} // namespace

TEST_CASE ("a tempo map converts between ticks and seconds over steps and ramps", "[tempo]")
{
    SECTION ("one tempo")
    {
        const TempoMap map;
        CHECK_THAT (map.secondsAt (4.0 * beat), WithinAbs (2.0, 1e-12));
        CHECK_THAT (map.tickAt (2.0), WithinAbs (4.0 * beat, 1e-6));
        CHECK_THAT (map.secondsAt (-2.0 * beat), WithinAbs (-1.0, 1e-12));
    }

    SECTION ("a step")
    {
        // 4 beats at 120 (2 s), then 60 BPM.
        const TempoMap map ({ { 0, 120.0 }, { 4 * beat, 60.0 } }, {});
        CHECK_THAT (map.secondsAt (6.0 * beat), WithinAbs (4.0, 1e-12));
        CHECK_THAT (map.tickAt (4.0), WithinAbs (6.0 * beat, 1e-6));
        CHECK_THAT (map.bpmAt (3.9 * beat), WithinAbs (120.0, 1e-12));
        CHECK_THAT (map.bpmAt (4.0 * beat), WithinAbs (60.0, 1e-12));
    }

    SECTION ("a ramp")
    {
        // 60 to 120 BPM over 4 beats: the integral of 60 / b over b = 60 + 15 x is 4 ln 2 seconds.
        const TempoMap map ({ { 0, 60.0, true }, { 4 * beat, 120.0 } }, {});
        CHECK_THAT (map.bpmAt (2.0 * beat), WithinAbs (90.0, 1e-9));
        CHECK_THAT (map.secondsAt (4.0 * beat), WithinAbs (4.0 * std::log (2.0), 1e-9));
        CHECK_THAT (map.secondsAt (5.0 * beat), WithinAbs (4.0 * std::log (2.0) + 0.5, 1e-9));

        for (const auto tick : { 0.0, 1000.0, 1.5 * beat, 3.99 * beat, 4.0 * beat, 9.0 * beat })
            CHECK_THAT (map.tickAt (map.secondsAt (tick)), WithinAbs (tick, 1e-6));
    }
}

TEST_CASE ("tempo points and signatures are normalised", "[tempo]")
{
    const TempoMap map ({ { 8 * beat, 5000.0 }, { 2 * beat, 90.0 }, { 2 * beat, 100.0 } },
                        { { 4, 7, 7 }, { 4, 3, 8 }, { 2, 0, 4 } });

    REQUIRE (map.points().size() == 3);
    CHECK (map.points()[0].tick == 0);                         // the first point moves to tick 0
    CHECK_THAT (map.points()[0].bpm, WithinAbs (100.0, 1e-9)); // the later of two points on one tick wins
    CHECK_THAT (map.points()[2].bpm, WithinAbs (limits::maxTempo, 1e-9));

    REQUIRE (map.signatures().size() == 3);
    CHECK (map.signatures()[0] == TimeSignature { 0, 4, 4 }); // 4/4 fills in bar 0
    CHECK (map.signatures()[1] == TimeSignature { 2, 1, 4 });
    CHECK (map.signatures()[2] == TimeSignature { 4, 3, 8 });
}

TEST_CASE ("bars follow time signature changes", "[tempo]")
{
    // Two bars of 4/4, two of 7/8 (3.5 beats), then 6/8.
    const TempoMap map ({}, { { 0, 4, 4 }, { 2, 7, 8 }, { 4, 6, 8 } });

    CHECK (map.tickOfBar (2) == 8 * beat);
    CHECK (map.tickOfBar (3) == 8 * beat + 7 * beat / 2);
    CHECK (map.tickOfBar (4) == 15 * beat);
    CHECK (map.tickOfBar (5) == 18 * beat);
    CHECK (map.tickOfBar (-1) == -4 * beat);

    const auto position = map.barAt (8.0 * beat + 7.0 * beat / 2 + 2.5 * beat);
    CHECK (position.bar == 3);
    CHECK (position.beat == 5); // eighths
    CHECK_THAT (position.tickInBeat, WithinAbs (0.0, 1e-9));
    CHECK (map.signatureAt (16.0 * beat).numerator == 6);
    CHECK (map.barAt (-1.0).bar == -1);
}

TEST_CASE ("tempo commands edit the map", "[tempo][commands]")
{
    auto project = apply ({}, "addTempoPoint", { { "tick", 8 * beat }, { "bpm", 90 } });
    project = apply (project, "addTempoPoint", { { "tick", 16 * beat }, { "bpm", 140 }, { "ramp", true } });
    REQUIRE (project.tempoMap.points().size() == 3);

    // The BPM field edits the tempo in force at the playhead.
    project = apply (project, "setTempo", { { "bpm", 100 }, { "at", 10 * beat } });
    CHECK_THAT (project.tempoMap.points()[1].bpm, WithinAbs (100.0, 1e-9));
    project = apply (project, "setTempo", { { "bpm", 130 } });
    CHECK_THAT (project.tempoMap.points()[0].bpm, WithinAbs (130.0, 1e-9));

    // A point stays between its neighbours; the first stays at 0.
    project = apply (project, "setTempoPoint", { { "index", 1 }, { "tick", 20 * beat }, { "ramp", true } });
    CHECK (project.tempoMap.points()[1].tick == 16 * beat - 1);
    CHECK (project.tempoMap.points()[1].ramp);
    project = apply (project, "setTempoPoint", { { "index", 0 }, { "tick", 4 * beat } });
    CHECK (project.tempoMap.points()[0].tick == 0);

    project = apply (project, "removeTempoPoint", { { "index", 1 } });
    CHECK (project.tempoMap.points().size() == 2);
    CHECK_THROWS_AS (applyCommand (project, "removeTempoPoint", makeObject ({ { "index", 0 } })), CommandError);
}

TEST_CASE ("time signature commands edit the signatures", "[tempo][commands]")
{
    auto project = apply ({}, "setSignature", { { "bar", 4 }, { "numerator", 7 }, { "denominator", 8 } });
    project = apply (project, "setSignature", { { "bar", 0 }, { "numerator", 3 }, { "denominator", 4 } });
    REQUIRE (project.tempoMap.signatures().size() == 2);
    CHECK (project.tempoMap.signatures()[0] == TimeSignature { 0, 3, 4 });

    project = apply (project, "moveSignature", { { "from", 4 }, { "to", 6 } });
    CHECK (project.tempoMap.signatures()[1] == TimeSignature { 6, 7, 8 });
    CHECK_THROWS_AS (applyCommand (project, "moveSignature", makeObject ({ { "from", 0 }, { "to", 2 } })),
                     CommandError);

    project = apply (project, "removeSignature", { { "bar", 6 } });
    CHECK (project.tempoMap.signatures().size() == 1);
    CHECK_THROWS_AS (applyCommand (project, "removeSignature", makeObject ({ { "bar", 0 } })), CommandError);
}

TEST_CASE ("audio clips keep their seconds through tempo map edits and moves", "[tempo][audio]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    // One second of audio at 120 BPM: two beats.
    project = apply (project, "addAudioClip",
                     { { "trackId", trackId }, { "start", 4 * beat }, { "file", fileArgs (48000) } });
    const auto clip = [&project] { return *project.tracks[0]->clips[0]; };
    REQUIRE (clip().length == 2 * beat);

    SECTION ("a tempo change inside the clip stretches its timeline length, not its content")
    {
        // From beat 5 on, 60 BPM: half a second fills beat 4, the other half takes half a beat at 60.
        project = apply (project, "addTempoPoint", { { "tick", 5 * beat }, { "bpm", 60 } });
        CHECK (clip().length == beat + beat / 2);
        CHECK (clip().offset == 0);
    }

    SECTION ("a tempo change before the clip rescales its content ticks")
    {
        project = apply (project, "addTempoPoint", { { "tick", 2 * beat }, { "bpm", 60 }, { "ramp", false } });
        CHECK (clip().start == 4 * beat);
        CHECK (clip().length == beat);
    }

    SECTION ("moving the clip to another tempo keeps its seconds")
    {
        project = apply (project, "addTempoPoint", { { "tick", 16 * beat }, { "bpm", 240 } });
        const auto id = clip().id;
        project = apply (project, "moveClips", { { "ids", juce::Array<juce::var> { id } }, { "delta", 16 * beat } });
        CHECK (clip().start == 20 * beat);
        CHECK (clip().length == 4 * beat);
    }

    SECTION ("a clip added after a tempo change measures its content at the tempo where it starts")
    {
        project = apply (project, "addTempoPoint", { { "tick", 16 * beat }, { "bpm", 60 } });
        project = apply (project, "addAudioClip",
                         { { "trackId", trackId }, { "start", 20 * beat }, { "file", fileArgs (48000) } });
        CHECK (project.tracks[0]->clips[1]->length == beat);
    }
}
