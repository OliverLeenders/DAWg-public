#include "TestUtils.h"
#include "dawg/AudioPlayer.h"
#include "dawg/ProjectFile.h"
#include "dawg/TimingMap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Time stretching (docs/design/time-stretching.md): the timing map, the timing commands and repitch playback.

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr double dbeat = static_cast<double> (ticksPerBeat);

[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] juce::var fileArgs (const char* path, double rate, juce::int64 frames)
{
    return makeObject (
        { { "path", path }, { "name", "tone" }, { "sampleRate", rate }, { "channels", 1 }, { "frames", frames } });
}

[[nodiscard]] juce::AudioBuffer<float> sine (double frequency, double rate, int frames, float amplitude = 0.5f)
{
    juce::AudioBuffer<float> buffer (1, frames);

    for (int i = 0; i < frames; ++i)
        buffer.setSample (
            0, i,
            amplitude * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate)));

    return buffer;
}

// An audio track with one clip of `seconds` of audio at `start`.
[[nodiscard]] Project projectWithAudio (Ticks start = beat, double seconds = 2.0, Ticks offset = 0)
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    return apply (project, "addAudioClip",
                  { { "trackId", project.tracks[0]->id },
                    { "start", start },
                    { "offset", offset },
                    { "file", fileArgs ("Audio/tone.wav", 48000.0, static_cast<juce::int64> (seconds * 48000.0)) } });
}

[[nodiscard]] const NoteClip& firstClip (const Project& project)
{
    return *project.tracks[0]->clips[0];
}

[[nodiscard]] ObjectId fileIdOf (const Project& project)
{
    return project.audioFiles[0].id;
}

// The timeline second at which a clip plays a file frame (the first pass of an unlooped clip).
[[nodiscard]] double secondsOfFrame (const Project& project, const NoteClip& clip, double frame)
{
    const auto& file = *project.findAudioFile (clip.fileId);
    const auto& map = project.tempoMap;

    if (file.timing.musical())
    {
        const TimingMap timing (file.timing, file.sampleRate);
        return map.secondsAt (static_cast<double> (clip.start - clip.offset) + timing.tickAt (frame));
    }

    // A one-shot file's markers are in content seconds.
    const TimingMap timing (file.timing, file.sampleRate);
    const auto spt = 60.0 / (map.bpmAt (static_cast<double> (clip.start)) * dbeat);
    return map.secondsAt (static_cast<double> (clip.start)) + timing.tickAt (frame)
           - static_cast<double> (clip.offset) * spt;
}

// One audio track with a generated tone, a compiler that serves it and an engine.
struct Rig
{
    explicit Rig (juce::AudioBuffer<float> audio, Ticks start = beat)
        : source (AudioSource::fromBuffer (std::move (audio), 48000.0))
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        project = apply (project, "addAudioClip",
                         { { "trackId", project.tracks[0]->id },
                           { "start", start },
                           { "file", fileArgs ("Audio/tone.wav", 48000.0, source->frames()) } });
        clipId = firstClip (project).id;
        fileId = fileIdOf (project);
        compiler.setAudioSourceResolver ([this] (const AudioFile&) { return source; });
        engine.setEngine (compiler.compile (project, spec));
    }

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);

        if (auto compiled = compiler.update (project, spec))
            engine.setEngine (std::move (compiled));
    }

    std::shared_ptr<const AudioSource> source;
    Project project;
    ObjectId clipId = 0;
    ObjectId fileId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

// The same cases as ui/src/time/timingMap.test.ts, so the UI and the engine agree.
TEST_CASE ("the timing map runs through its markers and on at the source tempo", "[stretch]")
{
    // Without markers: the file at its source tempo (120 BPM at 48 kHz is 1.5625 frames per tick).
    const TimingMap plain (FileTiming { TimingMode::musical, MusicalStyle::stretch, 120.0, {} }, 48000.0);
    CHECK_THAT (plain.frameAt (dbeat), WithinAbs (24000.0, 1e-9));
    CHECK_THAT (plain.tickAt (24000.0), WithinAbs (dbeat, 1e-9));
    CHECK_THAT (plain.frameAt (-dbeat), WithinAbs (-24000.0, 1e-9));
    CHECK_THAT (plain.rateAt (0.0), WithinAbs (1.5625, 1e-12));

    // Two markers: the first beat holds half a beat of audio, the second a beat and a half.
    const TimingMap marked (
        FileTiming {
            TimingMode::musical, MusicalStyle::stretch, 120.0, { { dbeat, 12000.0 }, { 2 * dbeat, 48000.0 } } },
        48000.0);
    CHECK_THAT (marked.frameAt (0.5 * dbeat), WithinAbs (6000.0, 1e-9));
    CHECK_THAT (marked.frameAt (1.5 * dbeat), WithinAbs (30000.0, 1e-9));
    CHECK_THAT (marked.frameAt (3 * dbeat), WithinAbs (72000.0, 1e-9));
    CHECK_THAT (marked.rateAt (0.5 * dbeat), WithinAbs (12000.0 / dbeat, 1e-12));
    CHECK_THAT (marked.rateAt (3 * dbeat), WithinAbs (1.5625, 1e-12));

    for (const auto tick : { 0.0, 100.0, 0.99 * dbeat, dbeat, 1.7 * dbeat, 2 * dbeat, 5.5 * dbeat })
        CHECK_THAT (marked.tickAt (marked.frameAt (tick)), WithinAbs (tick, 1e-6));

    // Monotonic lookups with a hint agree with the search.
    size_t hint = 0;

    for (auto tick = -100.0; tick < 4 * dbeat; tick += 997.0)
        CHECK_THAT (marked.frameAt (tick, hint), WithinAbs (marked.frameAt (tick), 1e-9));

    // Markers are sorted, and those that don't increase in both tick and frame are dropped.
    std::vector<TimingMarker> markers {
        { 2 * dbeat, 48000.0 }, { dbeat, 12000.0 }, { 1.5 * dbeat, 10000.0 }, { 0.0, 0.0 }, { 3 * dbeat, 48000.0 }
    };
    normaliseTimingMarkers (markers);
    CHECK (markers == std::vector<TimingMarker> { { dbeat, 12000.0 }, { 2 * dbeat, 48000.0 } });
}

TEST_CASE ("markers from the tempo map follow steps and ramps", "[stretch]")
{
    // One tempo at the source tempo needs no markers.
    CHECK (markersFromTempo (TempoMap(), 0.0, 96000, 48000.0, 120.0).empty());

    // A file at 100 BPM in a 120 BPM song is pinned at its end.
    CHECK (markersFromTempo (TempoMap(), 0.0, 96000, 48000.0, 100.0).size() == 1);

    const auto check = [] (const TempoMap& map, double anchorTick, juce::int64 frames)
    {
        const auto markers = markersFromTempo (map, anchorTick, frames, 48000.0, map.bpmAt (anchorTick));
        const TimingMap timing (
            FileTiming { TimingMode::musical, MusicalStyle::stretch, map.bpmAt (anchorTick), markers }, 48000.0);
        const auto anchorSeconds = map.secondsAt (anchorTick);
        auto worst = 0.0;

        for (juce::int64 frame = 0; frame <= frames; frame += 997)
        {
            const auto seconds = map.secondsAt (anchorTick + timing.tickAt (static_cast<double> (frame)));
            worst = std::max (worst, std::abs (seconds - anchorSeconds - static_cast<double> (frame) / 48000.0));
        }

        CHECK (worst < 2.0e-5);
        return markers.size();
    };

    const TempoMap step ({ { 0, 120.0, false }, { 4 * beat, 60.0, false } }, {});
    CHECK (check (step, 0.0, 8 * 48000) == 2);   // the step, and the end at 60 BPM
    CHECK (check (step, 6 * dbeat, 48000) == 0); // wholly after the step, at its own tempo

    const TempoMap ramp ({ { 0, 120.0, false }, { 2 * beat, 120.0, true }, { 6 * beat, 80.0, false } }, {});
    CHECK (check (ramp, 0.5 * dbeat, 6 * 48000) > 3);
}

TEST_CASE ("new files are one-shot and only setFileTiming changes the mode", "[stretch][commands]")
{
    auto project = projectWithAudio();
    CHECK_FALSE (project.audioFiles[0].timing.musical());
    CHECK (firstClip (project).transpose == 0.0);

    project = apply (project, "setTempo", { { "bpm", 90.0 } });
    project = apply (project, "splitClips", { { "ids", array ({ firstClip (project).id }) }, { "at", 2 * beat } });
    CHECK_FALSE (project.audioFiles[0].timing.musical());

    // Transpose needs a musical file.
    CHECK_THROWS_AS (
        apply (project, "setClipTranspose", { { "clipId", firstClip (project).id }, { "semitones", 2.0 } }),
        CommandError);

    project = apply (project, "setFileTiming", { { "fileId", fileIdOf (project) }, { "mode", "musical" } });
    CHECK (project.audioFiles[0].timing.musical());
    CHECK_THAT (project.audioFiles[0].timing.sourceBpm, WithinAbs (90.0, 1e-9));
    project = apply (project, "setClipTranspose", { { "clipId", firstClip (project).id }, { "semitones", 2.5 } });
    CHECK (firstClip (project).transpose == 2.5);

    CHECK_THROWS_AS (apply (project, "setFileTiming", { { "fileId", fileIdOf (project) }, { "mode", "warped" } }),
                     CommandError);
}

TEST_CASE ("switching a file to musical and back moves no audio", "[stretch][commands]")
{
    const auto checkRoundTrip = [] (const Project& project)
    {
        const auto& before = project;
        const auto fileId = fileIdOf (project);
        const auto musical = apply (project, "setFileTiming", { { "fileId", fileId }, { "mode", "musical" } });
        const auto back = apply (musical, "setFileTiming", { { "fileId", fileId }, { "mode", "oneShot" } });

        for (const auto* version : { &musical, &back })
        {
            for (size_t c = 0; c < before.tracks[0]->clips.size(); ++c)
            {
                const auto& old = *before.tracks[0]->clips[c];
                const auto& now = *version->tracks[0]->clips[c];
                CHECK (now.start == old.start);
                CHECK (now.length == old.length);

                for (const auto frame : { 0.0, 10000.0, 30000.0, 60000.0, 90000.0 })
                    CHECK_THAT (secondsOfFrame (*version, now, frame),
                                WithinAbs (secondsOfFrame (before, old, frame), 4.0e-5));
            }
        }

        // Back at one-shot, the markers the switch added are gone again: the file plays at its own speed.
        CHECK (back.audioFiles[0].timing.markers.empty());
    };

    SECTION ("one tempo")
    {
        checkRoundTrip (projectWithAudio (beat, 2.0, beat / 3));
    }

    SECTION ("a step")
    {
        auto project = projectWithAudio (beat, 2.0, beat / 2);
        project = apply (project, "addTempoPoint", { { "tick", 2 * beat }, { "bpm", 80.0 } });
        checkRoundTrip (project);
    }

    SECTION ("a ramp")
    {
        auto project = projectWithAudio (beat, 2.0);
        project = apply (project, "addTempoPoint", { { "tick", 0 }, { "bpm", 100.0 }, { "ramp", true } });
        project = apply (project, "addTempoPoint", { { "tick", 3 * beat }, { "bpm", 150.0 } });
        checkRoundTrip (project);
    }

    SECTION ("two clips at one tempo")
    {
        auto project = projectWithAudio (beat, 2.0);
        project = apply (project, "addAudioClip",
                         { { "trackId", project.tracks[0]->id },
                           { "start", 8 * beat },
                           { "offset", beat },
                           { "fileId", fileIdOf (project) } });
        checkRoundTrip (project);
    }
}

TEST_CASE ("a tempo change keeps a musical file's ticks and refits one-shot clips", "[stretch][commands]")
{
    auto project = projectWithAudio (4 * beat, 2.0);
    project = apply (project, "addAudioClip",
                     { { "trackId", project.tracks[0]->id },
                       { "start", 12 * beat },
                       { "file", fileArgs ("Audio/other.wav", 48000.0, 96000) } });
    project = apply (project, "setFileTiming", { { "fileId", fileIdOf (project) }, { "mode", "musical" } });
    project = apply (project, "setClipProps", { { "id", firstClip (project).id }, { "fadeIn", beat / 2 } });
    const auto musical = firstClip (project);
    project = apply (project, "setTempo", { { "bpm", 60.0 } });

    // The musical clip keeps its ticks; the one-shot one keeps its two seconds (two beats at 60 BPM).
    CHECK (firstClip (project).length == musical.length);
    CHECK (firstClip (project).fadeIn == beat / 2);
    CHECK (project.tracks[0]->clips[1]->length == 2 * beat);

    // A musical file's new clips take their length from the timing map.
    project =
        apply (project, "addAudioClip",
               { { "trackId", project.tracks[0]->id }, { "start", 20 * beat }, { "fileId", fileIdOf (project) } });
    CHECK (project.tracks[0]->clips[2]->length == 4 * beat); // two seconds at the source tempo of 120 BPM
}

TEST_CASE ("file timing and transpose round-trip through project files", "[stretch][file]")
{
    auto project = projectWithAudio (beat, 2.0);
    project = apply (project, "addTempoPoint", { { "tick", 2 * beat }, { "bpm", 90.0 } });
    project = apply (project, "setFileTiming",
                     { { "fileId", fileIdOf (project) }, { "mode", "musical" }, { "musicalStyle", "repitch" } });
    project = apply (project, "setClipTranspose", { { "clipId", firstClip (project).id }, { "semitones", -3.25 } });
    REQUIRE_FALSE (project.audioFiles[0].timing.markers.empty());

    std::map<juce::String, std::shared_ptr<const juce::MemoryBlock>> states;
    const auto json = projectfile::toVar (project, { "id", "v" }, states);
    const auto loaded = projectfile::fromVar (projectfile::migrate (json), {}).project;
    CHECK (loaded.audioFiles[0].timing == project.audioFiles[0].timing);
    CHECK (firstClip (loaded).transpose == -3.25);

    // Files from before version 16 are one-shot without transpose.
    auto old = json.clone();
    old.getDynamicObject()->setProperty ("version", 15);
    auto* file = old["project"]["audioFiles"].getDynamicObject()->getProperties().getValueAt (0).getDynamicObject();
    file->removeProperty ("timing");
    const auto migrated = projectfile::fromVar (projectfile::migrate (old), {}).project;
    CHECK_FALSE (migrated.audioFiles[0].timing.musical());
}

TEST_CASE ("repitch plays a musical file faster and higher at a faster tempo", "[stretch][engine]")
{
    // Two seconds of 1 kHz from beat 1, made musical at 120 BPM, then played at 180 BPM.
    Rig rig (sine (1000.0, 48000.0, 96000));
    rig.edit ("setFileTiming", { { "fileId", rig.fileId }, { "mode", "musical" }, { "musicalStyle", "repitch" } });
    rig.edit ("setTempo", { { "bpm", 180.0 } });
    REQUIRE (firstClip (rig.project).length == 4 * beat);

    rig.engine.play();
    const auto output = render (rig.engine, 48000 * 2);
    const auto& left = output.left();

    // Beat 1 at 180 BPM is sample 16000; four beats of clip last 64000 samples, at 1.5 kHz.
    CHECK (firstAudible (left) >= 15990);
    CHECK (firstAudible (left) <= 16010);
    CHECK (std::abs (zeroCrossings (left, 20000, 24800) - 300) <= 2);
    CHECK (firstAudible ({ left.begin() + 16000 + 64000 + 200, left.end() }) == -1);
}

TEST_CASE ("a musical file at its source tempo plays as a one-shot one", "[stretch][engine]")
{
    Rig oneShot (sine (1000.0, 48000.0, 96000));
    Rig musical (sine (1000.0, 48000.0, 96000));
    musical.edit ("setFileTiming",
                  { { "fileId", musical.fileId }, { "mode", "musical" }, { "musicalStyle", "repitch" } });
    oneShot.engine.play();
    musical.engine.play();
    const auto a = render (oneShot.engine, 48000 * 3);
    const auto b = render (musical.engine, 48000 * 3);
    auto worst = 0.0f;

    for (size_t i = 0; i < a.left().size(); ++i)
        worst = std::max (worst, std::abs (a.left()[i] - b.left()[i]));

    CHECK (worst < 1.0e-5f);
}

TEST_CASE ("a one-shot file plays through its markers in real time, whatever the tempo", "[stretch][engine]")
{
    // Two seconds of 1 kHz from beat 1. A marker squeezes the first second of audio into half a second (2 kHz); the
    // rest plays at its own speed. The tempo then halves: the clip's start moves, the markers don't.
    Rig rig (sine (1000.0, 48000.0, 96000));
    rig.edit ("setFileTiming", { { "fileId", rig.fileId }, { "musicalStyle", "repitch" } });
    rig.edit ("addTimingMarker", { { "fileId", rig.fileId }, { "tick", 0.5 }, { "frame", 48000.0 } });
    rig.edit ("setTempo", { { "bpm", 60.0 } });
    REQUIRE_FALSE (rig.project.audioFiles[0].timing.musical());

    rig.engine.play();
    const auto output = render (rig.engine, 48000 * 3);
    const auto& left = output.left();

    // Beat 1 at 60 BPM is 1 s; the clip lasts 0.5 s at 2 kHz, then 1 s at 1 kHz.
    CHECK (std::abs (firstAudible (left) - 48000) <= 10);
    CHECK (std::abs (zeroCrossings (left, 50400, 69600) - 1600) <= 4);
    CHECK (std::abs (zeroCrossings (left, 76800, 115200) - 1600) <= 4);
    CHECK (firstAudible ({ left.begin() + 48000 + 72000 + 200, left.end() }) == -1);
}
