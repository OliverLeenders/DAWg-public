#include "TestUtils.h"
#include "dawg/Onsets.h"
#include "dawg/Session.h"
#include "dawg/TimingMap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Timing markers, "Make copy" and onsets (docs/design/time-stretching.md, TS2, TS11, TS13, TS14).

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr double dbeat = static_cast<double> (ticksPerBeat);

[[nodiscard]] Project musicalProject()
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    project = apply (project, "addAudioClip",
                     { { "trackId", project.tracks[0]->id },
                       { "start", beat },
                       { "file", makeObject ({ { "path", "Audio/take.wav" },
                                               { "name", "take" },
                                               { "sampleRate", 48000.0 },
                                               { "channels", 1 },
                                               { "frames", 4 * 48000 } }) } });
    return apply (project, "setFileTiming", { { "fileId", project.audioFiles[0].id }, { "mode", "musical" } });
}

[[nodiscard]] const std::vector<TimingMarker>& markersOf (const Project& project, size_t file = 0)
{
    return project.audioFiles[file].timing.markers;
}
} // namespace

TEST_CASE ("timing markers are added, moved, removed and reset", "[stretch][markers]")
{
    auto project = musicalProject();
    const auto fileId = project.audioFiles[0].id;

    // Added at the current mapping by default, sorted.
    project = apply (project, "addTimingMarker", { { "fileId", fileId }, { "tick", 4 * beat } });
    project = apply (project, "addTimingMarker", { { "fileId", fileId }, { "tick", 2 * beat }, { "frame", 20000.0 } });
    REQUIRE (markersOf (project).size() == 2);
    CHECK (markersOf (project)[0] == TimingMarker { 2 * dbeat, 20000.0 });
    CHECK (markersOf (project)[1] == TimingMarker { 4 * dbeat, 96000.0 });
    CHECK_THROWS_AS (
        apply (project, "addTimingMarker", { { "fileId", fileId }, { "tick", 3 * beat }, { "frame", 10000.0 } }),
        CommandError); // out of order
    CHECK_THROWS_AS (apply (project, "addTimingMarker", { { "fileId", fileId }, { "tick", 2 * beat } }), CommandError);

    // A move stays between its neighbours, and changes the map only beside the marker.
    const TimingMap before (project.audioFiles[0].timing, 48000.0);
    project = apply (project, "moveTimingMarker", { { "fileId", fileId }, { "index", 0 }, { "tick", 9 * beat } });
    CHECK (markersOf (project)[0].tick < 4 * dbeat);
    project = apply (project, "moveTimingMarker", { { "fileId", fileId }, { "index", 0 }, { "tick", 2.5 * beat } });
    const TimingMap after (project.audioFiles[0].timing, 48000.0);
    CHECK_THAT (after.frameAt (5 * dbeat), WithinAbs (before.frameAt (5 * dbeat), 1e-6));
    CHECK (after.frameAt (dbeat) != before.frameAt (dbeat));

    // Shift: the audio slides under a fixed marker (its frame moves).
    project = apply (project, "moveTimingMarker", { { "fileId", fileId }, { "index", 0 }, { "frame", 30000.0 } });
    CHECK (markersOf (project)[0] == TimingMarker { 2.5 * dbeat, 30000.0 });

    project = apply (project, "removeTimingMarker", { { "fileId", fileId }, { "index", 0 } });
    CHECK (markersOf (project).size() == 1);
    CHECK_THROWS_AS (apply (project, "removeTimingMarker", { { "fileId", fileId }, { "index", 3 } }), CommandError);
    project = apply (project, "resetTimingMarkers", { { "fileId", fileId } });
    CHECK (markersOf (project).empty());
}

TEST_CASE ("one-shot files take markers in seconds, which stay put in real time", "[stretch][markers]")
{
    auto project = musicalProject();
    project = apply (project, "setFileTiming", { { "fileId", project.audioFiles[0].id }, { "mode", "oneShot" } });
    const auto fileId = project.audioFiles[0].id;
    const auto clipId = project.tracks[0]->clips[0]->id;
    REQUIRE (markersOf (project).empty());

    // Content second 1 plays frame 60000: the first second is sped up, the rest plays at the file's own speed.
    project = apply (project, "addTimingMarker", { { "fileId", fileId }, { "tick", 1.0 }, { "frame", 60000.0 } });
    CHECK_FALSE (project.audioFiles[0].timing.musical());
    REQUIRE (markersOf (project).size() == 1);
    CHECK (markersOf (project)[0] == TimingMarker { 1.0, 60000.0 });
    const TimingMap map (project.audioFiles[0].timing, 48000.0);
    CHECK_THAT (map.frameAt (0.5), WithinAbs (30000.0, 1e-6));
    CHECK_THAT (map.frameAt (2.0), WithinAbs (108000.0, 1e-6));

    // Markers closer than a millisecond are the same marker; a tempo change leaves them where they are.
    CHECK_THROWS_AS (apply (project, "addTimingMarker", { { "fileId", fileId }, { "tick", 1.0005 } }), CommandError);
    project = apply (project, "setTempo", { { "bpm", 60.0 } });
    CHECK (markersOf (project)[0] == TimingMarker { 1.0, 60000.0 });
    CHECK_NOTHROW (apply (project, "copyPoolFile", { { "clipId", clipId } }));

    // Switching to musical converts the marker: the clip starts on beat 1, so content second 1 is one beat later
    // at 60 BPM.
    const auto musical = apply (project, "setFileTiming", { { "fileId", fileId }, { "mode", "musical" } });
    REQUIRE_FALSE (markersOf (musical).empty());
    CHECK_THAT (markersOf (musical)[0].tick, WithinAbs (dbeat, 1e-6));
    CHECK_THAT (markersOf (musical)[0].frame, WithinAbs (60000.0, 1e-6));
    CHECK (apply (project, "resetTimingMarkers", { { "fileId", fileId } }).audioFiles[0].timing.markers.empty());
}

TEST_CASE ("marker edits undo", "[stretch][markers]")
{
    Session session;
    session.reset (musicalProject());
    const auto id = session.project()->audioFiles[0].id;
    std::ignore = session.apply ("addTimingMarker", makeObject ({ { "fileId", id }, { "tick", 2 * beat } }));
    std::ignore =
        session.apply ("moveTimingMarker", makeObject ({ { "fileId", id }, { "index", 0 }, { "tick", 3 * beat } }));
    CHECK (session.project()->audioFiles[0].timing.markers[0].tick == 3 * dbeat);
    CHECK (session.undo().ok);
    CHECK (session.project()->audioFiles[0].timing.markers[0].tick == 2 * dbeat);
    CHECK (session.undo().ok);
    CHECK (session.project()->audioFiles[0].timing.markers.empty());
}

TEST_CASE ("make copy gives a clip its own pool entry", "[stretch][markers]")
{
    auto project = musicalProject();
    const auto fileId = project.audioFiles[0].id;
    project = apply (project, "addTimingMarker", { { "fileId", fileId }, { "tick", 2 * beat } });
    project = apply (project, "addAudioClip",
                     { { "trackId", project.tracks[0]->id }, { "start", 12 * beat }, { "fileId", fileId } });
    const auto copied = project.tracks[0]->clips[1]->id;
    project = apply (project, "copyPoolFile", { { "clipId", copied } });

    REQUIRE (project.audioFiles.size() == 2);
    const auto& copy = project.audioFiles[1];
    CHECK (copy.name == "take (2)");
    CHECK (copy.path == project.audioFiles[0].path);
    CHECK (copy.timing == project.audioFiles[0].timing);
    CHECK (project.tracks[0]->clips[1]->fileId == copy.id);
    CHECK (project.tracks[0]->clips[0]->fileId == fileId);

    // The copy's markers move on their own.
    project = apply (project, "moveTimingMarker", { { "fileId", copy.id }, { "index", 0 }, { "tick", 3 * beat } });
    CHECK (markersOf (project, 0)[0].tick == 2 * dbeat);
    CHECK (markersOf (project, 1)[0].tick == 3 * dbeat);

    // Another copy is named on.
    project = apply (project, "copyPoolFile", { { "clipId", project.tracks[0]->clips[0]->id } });
    CHECK (project.audioFiles[2].name == "take (3)");
}

TEST_CASE ("onsets of clicks and plucks are found within 2 ms", "[stretch][onsets]")
{
    constexpr double rate = 48000.0;
    juce::AudioBuffer<float> audio (1, 6 * 48000);
    audio.clear();
    std::vector<juce::int64> expected;
    juce::Random random (7);

    // Clicks of different levels, then plucked tones (sharp attack, exponential decay) over a little noise.
    for (int i = 0; i < 6; ++i)
    {
        const auto at = 12000 + i * 21000 + random.nextInt (3000);
        expected.push_back (at);
        const auto level = 0.2f + 0.12f * static_cast<float> (i);

        for (int n = 0; n < 64; ++n)
            audio.setSample (
                0, at + n, level * std::sin (static_cast<float> (n) * 1.3f) * (1.0f - static_cast<float> (n) / 64.0f));
    }

    for (int i = 0; i < 5; ++i)
    {
        const auto at = 150000 + i * 26000 + random.nextInt (3000);
        expected.push_back (at);

        for (int n = 0; n < 24000 && at + n < audio.getNumSamples(); ++n)
            audio.addSample (0, at + n,
                             0.5f * std::exp (static_cast<float> (-n) / 6000.0f)
                                 * static_cast<float> (
                                     std::sin (juce::MathConstants<double>::twoPi * (220.0 + 55.0 * i) * n / rate)));
    }

    for (int n = 0; n < audio.getNumSamples(); ++n)
        audio.addSample (0, n, 0.002f * (random.nextFloat() - 0.5f));

    const auto onsets = detectOnsets (audio, rate);
    CHECK (onsets.size() == expected.size());

    for (const auto frame : expected)
    {
        CAPTURE (frame);
        const auto nearest =
            std::ranges::min_element (onsets, {}, [&] (const Onset& o) { return std::abs (o.frame - frame); });
        REQUIRE (nearest != onsets.end());
        CHECK (std::abs (nearest->frame - frame) <= 96);
        CHECK (nearest->strength > 0.0f);
    }
}

TEST_CASE ("stretching a clip scales its file's timing, and is refused for one-shot files", "[stretch][markers]")
{
    auto project = musicalProject(); // 4 s at 120 BPM: 8 beats from beat 1
    const auto fileId = project.audioFiles[0].id;
    project = apply (project, "addTimingMarker", { { "fileId", fileId }, { "tick", 2 * beat } });
    project = apply (project, "addAudioClip",
                     { { "trackId", project.tracks[0]->id }, { "start", 20 * beat }, { "fileId", fileId } });
    project = apply (project, "setClipProps", { { "id", project.tracks[0]->clips[1]->id }, { "fadeIn", beat } });
    const auto clipId = project.tracks[0]->clips[0]->id;
    REQUIRE (project.tracks[0]->clips[0]->length == 8 * beat);

    // Ten beats instead of eight: everything in the file's content time scales by 10 / 8.
    project = apply (project, "stretchClip", { { "clipId", clipId }, { "length", 10 * beat } });
    const auto& timing = project.audioFiles[0].timing;
    CHECK_THAT (timing.sourceBpm, WithinAbs (150.0, 1e-9));
    CHECK_THAT (timing.markers[0].tick, WithinAbs (2.5 * dbeat, 1e-6));
    CHECK (project.tracks[0]->clips[0]->length == 10 * beat);

    // The file's other clip keeps its start and stretches too.
    const auto& other = *project.tracks[0]->clips[1];
    CHECK (other.start == 20 * beat);
    CHECK (other.length == 10 * beat);
    CHECK (other.fadeIn == beat * 5 / 4);

    // The audio fills the new length: the file's end lands at the clip's end.
    const TimingMap map (timing, 48000.0);
    CHECK_THAT (map.frameAt (10 * dbeat), WithinAbs (4 * 48000.0, 1.0));

    project = apply (project, "setFileTiming", { { "fileId", fileId }, { "mode", "oneShot" } });
    CHECK_THROWS_AS (apply (project, "stretchClip", { { "clipId", clipId }, { "length", 12 * beat } }), CommandError);
}

TEST_CASE ("source tempo suggestions come from metadata, the name or the length", "[stretch][suggest]")
{
    CHECK (tempoFromName ("Drums 128bpm") == 128.0);
    CHECK (tempoFromName ("loop_95_BPM_Am") == 95.0);
    CHECK (tempoFromName ("bass 87.5 bpm") == 87.5);
    CHECK (tempoFromName ("vocal take 3") == 0.0);
    CHECK (tempoFromName ("x1280bpm") == 0.0);
    CHECK (tempoFromName ("Kick 250bpm") == 0.0);

    // Two bars of 4/4 in 4 s is 120 BPM; four bars would be 240, one bar 60: only 120 is in range.
    CHECK (tempoFromLength (4.0, 4.0, 100.0) == 120.0);
    // 6 s of 4/4: 80 (2 bars) or 160 (4 bars); the project's 150 BPM picks 160.
    CHECK (tempoFromLength (6.0, 4.0, 150.0) == 160.0);
    CHECK (tempoFromLength (6.0, 4.0, 90.0) == 80.0);
    CHECK (tempoFromLength (40.0, 4.0, 120.0) == 0.0);

    CHECK (suggestTempo (126.0, "Drums 128bpm", 4.0, 4.0, 120.0) == 126.0);
    CHECK (suggestTempo (0.0, "Drums 128bpm", 4.0, 4.0, 120.0) == 128.0);
    CHECK (suggestTempo (0.0, "Drums", 4.0, 4.0, 120.0) == 120.0);

    // Imported files get one; it never changes the mode.
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    project = apply (project, "addAudioClip",
                     { { "trackId", project.tracks[0]->id },
                       { "start", 0 },
                       { "file", makeObject ({ { "path", "Audio/Loop 100bpm.wav" },
                                               { "name", "Loop 100bpm" },
                                               { "sampleRate", 48000.0 },
                                               { "channels", 2 },
                                               { "frames", 4 * 48000 } }) } });
    CHECK (project.audioFiles[0].suggestedBpm == 100.0);
    CHECK_FALSE (project.audioFiles[0].timing.musical());
}
