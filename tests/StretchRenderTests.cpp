#include "TestUtils.h"
#include "dawg/AudioPlayer.h"
#include "dawg/StretchRenderer.h"
#include "dawg/TimingMap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// Offline stretch rendering (docs/design/time-stretching.md, TS7–TS10): renders, the cache and the job queue.

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr double rate = 48000.0;

[[nodiscard]] juce::AudioBuffer<float> sine (double frequency, int frames, float amplitude = 0.5f)
{
    juce::AudioBuffer<float> buffer (1, frames);

    for (int i = 0; i < frames; ++i)
        buffer.setSample (
            0, i,
            amplitude * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate)));

    return buffer;
}

// Short clicks every `spacing` frames.
[[nodiscard]] juce::AudioBuffer<float> clicks (int count, int spacing)
{
    juce::AudioBuffer<float> buffer (1, count * spacing);
    buffer.clear();

    for (int c = 0; c < count; ++c)
        for (int i = 0; i < 48; ++i)
            buffer.setSample (0, c * spacing + i,
                              0.8f * static_cast<float> (std::sin (i * 0.9)) * (1.0f - static_cast<float> (i) / 48.0f));

    return buffer;
}

// The frequency of a sine from its zero crossings over [from, to).
[[nodiscard]] double frequencyOf (const std::vector<float>& samples, size_t from, size_t to)
{
    return zeroCrossings (samples, from, to) * rate / (2.0 * static_cast<double> (to - from));
}

[[nodiscard]] int lastAudible (const std::vector<float>& samples, float threshold = 1.0e-3f)
{
    for (auto i = static_cast<int> (samples.size()) - 1; i >= 0; --i)
        if (std::abs (samples[static_cast<size_t> (i)]) > threshold)
            return i;

    return -1;
}

[[nodiscard]] float peakOf (const std::vector<float>& samples, size_t from, size_t to)
{
    auto result = 0.0f;

    for (auto i = from; i < to && i < samples.size(); ++i)
        result = std::max (result, std::abs (samples[i]));

    return result;
}

// An audio track with one clip of generated audio, a renderer with its own cache folder, a compiler and an engine.
struct StretchRig
{
    explicit StretchRig (juce::AudioBuffer<float> audio, Ticks start = beat)
        : buffer (std::move (audio)),
          source (AudioSource::fromBuffer (buffer, rate))
    {
        renderer = std::make_unique<StretchRenderer> (
            folder.folder, makeRubberBandStretcher(),
            [this] (const AudioFile&, juce::int64 from, juce::AudioBuffer<float>& into)
            {
                into.clear();

                for (int i = 0; i < into.getNumSamples(); ++i)
                    if (const auto frame = from + i; frame >= 0 && frame < buffer.getNumSamples())
                        into.setSample (0, i, buffer.getSample (0, static_cast<int> (frame)));

                return true;
            },
            [] (AudioFile& file) { return file.path; }, 2);

        project = apply ({}, "addTrack", { { "kind", "audio" } });
        project =
            apply (project, "addAudioClip",
                   { { "trackId", project.tracks[0]->id },
                     { "start", start },
                     { "file", makeObject ({ { "path", "Audio/test.wav" },
                                             { "name", "test" },
                                             { "sampleRate", rate },
                                             { "channels", 1 },
                                             { "frames", static_cast<juce::int64> (buffer.getNumSamples()) } }) } });
        clipId = project.tracks[0]->clips[0]->id;
        fileId = project.audioFiles[0].id;
        compiler.setAudioSourceResolver ([this] (const AudioFile&) { return source; });
        compiler.setStretchRenderer (renderer.get());
        recompile();
    }

    ~StretchRig() = default;
    StretchRig (const StretchRig&) = delete;
    StretchRig& operator= (const StretchRig&) = delete;
    StretchRig (StretchRig&&) = delete;
    StretchRig& operator= (StretchRig&&) = delete;

    void edit (const char* command, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        project = apply (project, command, args);
        recompile();
    }

    // Sets the file's timing directly (the marker commands are tested elsewhere).
    void setTiming (const FileTiming& timing)
    {
        project.audioFiles[0].timing = timing;
        recompile();
    }

    void recompile() { engine.setEngine (compiler.compile (project, spec)); }

    // Waits for the queued renders and compiles again, so they play.
    void settle()
    {
        REQUIRE (renderer->waitUntilIdle (60000));
        recompile();
    }

    [[nodiscard]] const CompiledAudioClip& compiledClip()
    {
        compiledSequence = std::make_shared<AudioSequence> (
            compileAudio (*project.tracks[0], project, [this] (const AudioFile&) { return source; }, renderer.get()));
        return compiledSequence->clips[0];
    }

    [[nodiscard]] std::vector<float> play (int samples, double fromBeats = 0.0)
    {
        engine.seek (fromBeats);
        engine.play();
        auto output = render (engine, samples);
        engine.stop();
        return output.left();
    }

    // Destroyed last, once nothing has its renders open.
    struct TempFolder
    {
        juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("dawg-stretch-" + juce::Uuid().toString());

        TempFolder() = default;
        TempFolder (const TempFolder&) = delete;
        TempFolder& operator= (const TempFolder&) = delete;
        TempFolder (TempFolder&&) = delete;
        TempFolder& operator= (TempFolder&&) = delete;
        ~TempFolder() { std::ignore = folder.deleteRecursively(); }
    } folder;

    juce::AudioBuffer<float> buffer;
    std::shared_ptr<const AudioSource> source;
    std::unique_ptr<StretchRenderer> renderer;
    Project project;
    ObjectId clipId = 0;
    ObjectId fileId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
    std::shared_ptr<AudioSequence> compiledSequence;
};

[[nodiscard]] FileTiming musicalAt (double bpm, std::vector<TimingMarker> markers = {})
{
    return { TimingMode::musical, MusicalStyle::stretch, bpm, std::move (markers) };
}
} // namespace

TEST_CASE ("a musical file at its source tempo renders nothing and plays as one-shot", "[stretch][render]")
{
    const auto run = [] (bool ramp)
    {
        StretchRig oneShot (sine (1000.0, 4 * 48000));
        StretchRig musical (sine (1000.0, 4 * 48000));

        for (auto* rig : { &oneShot, &musical })
        {
            if (ramp)
            {
                rig->edit ("addTempoPoint", { { "tick", 0 }, { "bpm", 120.0 }, { "ramp", true } });
                rig->edit ("addTempoPoint", { { "tick", 6 * beat }, { "bpm", 90.0 } });
            }
        }

        musical.edit ("setFileTiming", { { "fileId", musical.fileId }, { "mode", "musical" } });
        CHECK (stretchJobs (musical.project).empty());
        CHECK (musical.renderer->pendingCount() == 0);

        for (const auto& piece : musical.compiledClip().pieces)
            CHECK (piece.kind == CompiledAudioPiece::Kind::file);

        const auto a = oneShot.play (5 * 48000);
        const auto b = musical.play (5 * 48000);
        auto worst = 0.0f;

        for (size_t i = 0; i < a.size(); ++i)
            worst = std::max (worst, std::abs (a[i] - b[i]));

        CHECK (worst < 1.0e-3f); // the declick follows the timeline's ticks, a little off one-shot's along a ramp
        CHECK (musical.renderer->renderedCount() == 0);
    };

    SECTION ("one tempo")
    {
        run (false);
    }
    SECTION ("a ramp")
    {
        run (true);
    }
}

TEST_CASE ("a stretched sine keeps its frequency and fills the expected length", "[stretch][render]")
{
    for (const auto ratio : { 0.5, 0.8, 1.25, 2.0 })
    {
        CAPTURE (ratio);
        // Two seconds of 1 kHz from beat 1 at 120 BPM; a tempo of 120 / ratio stretches it by the ratio.
        StretchRig rig (sine (1000.0, 2 * 48000));
        rig.setTiming (musicalAt (120.0));
        rig.edit ("setTempo", { { "bpm", 120.0 / ratio } });
        CHECK (rig.renderer->pendingCount() == 1);
        rig.settle();
        REQUIRE (rig.compiledClip().pieces.size() == 1);

        const auto start = static_cast<size_t> (48000 * 0.5 * ratio); // beat 1
        const auto length = static_cast<size_t> (2 * 48000 * ratio);
        const auto output = rig.play (static_cast<int> (start + length + 24000));

        CHECK_THAT (frequencyOf (output, start + length / 4, start + length * 3 / 4), WithinAbs (1000.0, 5.0));
        CHECK (std::abs (lastAudible (output) - static_cast<int> (start + length)) < 480);
        CHECK_THAT (peakOf (output, start + length / 4, start + length / 2), WithinAbs (0.5, 0.08));
    }
}

TEST_CASE ("transpose shifts the pitch at an unchanged length", "[stretch][render]")
{
    StretchRig rig (sine (500.0, 2 * 48000));
    rig.setTiming (musicalAt (120.0));
    rig.edit ("setClipTranspose", { { "clipId", rig.clipId }, { "semitones", 12.0 } });
    rig.settle();

    const auto output = rig.play (24000 + 96000 + 24000);
    CHECK_THAT (frequencyOf (output, 40000, 100000), WithinAbs (1000.0, 5.0));
    CHECK (std::abs (lastAudible (output) - (24000 + 96000)) < 480);
}

TEST_CASE ("clicks at 120 BPM land on the beats of a 140 BPM song", "[stretch][render]")
{
    StretchRig rig (clicks (8, 24000), 0);
    rig.setTiming (musicalAt (120.0));
    rig.edit ("setTempo", { { "bpm", 140.0 } });
    rig.settle();

    const auto output = rig.play (8 * 20572 + 4800);
    const auto spacing = 48000.0 * 60.0 / 140.0;

    for (int c = 1; c < 8; ++c)
    {
        CAPTURE (c);
        const auto expected = static_cast<size_t> (c * spacing);
        const auto found = firstAudible ({ output.begin() + static_cast<std::ptrdiff_t> (expected - 480),
                                           output.begin() + static_cast<std::ptrdiff_t> (expected + 480) },
                                         0.05f);
        REQUIRE (found >= 0);
        CHECK (std::abs (found - 480) <= 48); // within 1 ms
    }
}

TEST_CASE ("a moved marker moves its click and re-renders only the segments beside it", "[stretch][render]")
{
    // Clicks on every beat at 120 BPM, a marker on each beat, the song at 100 BPM.
    StretchRig rig (clicks (8, 24000), 0);
    std::vector<TimingMarker> markers;

    for (int m = 1; m < 8; ++m)
        markers.push_back ({ static_cast<double> (m * beat), m * 24000.0 });

    rig.setTiming (musicalAt (120.0, markers));
    rig.edit ("setTempo", { { "bpm", 100.0 } });
    rig.settle();
    const auto rendered = rig.renderer->renderedCount();
    CHECK (rendered == 8);
    const auto before = rig.play (8 * 28800 + 4800);

    // Pin the click of beat 4 to beat 4.25: the segments 3-4 and 4-5 change.
    markers[3].tick = 4.25 * beat;
    rig.setTiming (musicalAt (120.0, markers));
    rig.settle();
    CHECK (rig.renderer->renderedCount() == rendered + 2);
    const auto after = rig.play (8 * 28800 + 4800);

    const auto clickNear = [] (const std::vector<float>& samples, double at)
    {
        const auto from = static_cast<std::ptrdiff_t> (at) - 480;
        return from + firstAudible ({ samples.begin() + from, samples.begin() + from + 960 }, 0.05f);
    };
    CHECK (std::abs (clickNear (after, 4.25 * 28800) - static_cast<std::ptrdiff_t> (4.25 * 28800)) <= 48);

    // Before the two segments nothing changed. After them, pieces may be offset afresh to line up with the new
    // renders (CompiledAudioPiece::shift), by at most 10 ms: the clicks stay on their beats within that.
    auto worst = 0.0f;

    for (size_t i = 0; i < 3 * 28800 - 480; ++i)
        worst = std::max (worst, std::abs (before[i] - after[i]));

    CHECK (worst == 0.0f);

    for (int b = 5; b < 8; ++b)
    {
        CAPTURE (b);
        CHECK (std::abs (clickNear (after, b * 28800.0) - static_cast<std::ptrdiff_t> (b * 28800)) <= 528);
    }
}

TEST_CASE ("joins between renders never change the pitch", "[stretch][render]")
{
    // A sine with markers stretching its parts differently: its frequency holds everywhere, also just before each
    // join (an earlier alignment slid the tails there, which bent the pitch by up to a semitone).
    StretchRig rig (sine (500.0, 4 * 48000), 0);
    rig.setTiming (musicalAt (120.0, { { 1.0 * beat, 26000.0 },
                                       { 2.0 * beat, 47000.0 },
                                       { 3.0 * beat, 73000.0 },
                                       { 4.0 * beat, 96000.0 },
                                       { 5.0 * beat, 121000.0 } }));
    rig.edit ("setTempo", { { "bpm", 110.0 } });
    rig.settle();
    const auto output = rig.play (5 * 48000);
    const auto beatSamples = 48000.0 * 60.0 / 110.0;

    for (int b = 1; b <= 5; ++b)
    {
        CAPTURE (b);
        // 60 ms ending 10 ms before the join.
        const auto end = static_cast<size_t> (b * beatSamples) - 480;
        CHECK_THAT (frequencyOf (output, end - 2880, end), WithinAbs (500.0, 500.0 * 0.012));
    }
}

TEST_CASE ("a sine across a join has no step in level or phase", "[stretch][render]")
{
    // A marker in the middle of a two-second sine, with the two halves stretched differently.
    StretchRig rig (sine (1000.0, 2 * 48000), 0);
    rig.setTiming (musicalAt (120.0, { { 2.0 * beat, 52000.0 } }));
    rig.edit ("setTempo", { { "bpm", 100.0 } });
    rig.settle();
    REQUIRE (rig.compiledClip().pieces.size() == 2);

    const auto output = rig.play (3 * 48000);
    const auto join = static_cast<size_t> (2.0 * 0.6 * 48000); // beat 2 at 100 BPM

    // The level holds through the crossfade, and the frequency stays.
    for (auto at = join - 960; at < join + 960; at += 48)
        CHECK_THAT (peakOf (output, at, at + 96), WithinAbs (0.5, 0.06));

    CHECK_THAT (frequencyOf (output, join - 4800, join + 4800), WithinAbs (1000.0, 6.0));
}

TEST_CASE ("stretched audio plays after a seek and across loop wraps", "[stretch][render]")
{
    StretchRig rig (sine (1000.0, 4 * 48000), 0);
    rig.setTiming (musicalAt (120.0));
    rig.edit ("setTempo", { { "bpm", 90.0 } });
    rig.settle();

    // Mid-clip at once.
    const auto seeked = rig.play (4800, 2.0);
    CHECK (firstAudible (seeked, 0.1f) <= 4);

    // A clip loop of two beats, played across its wrap.
    rig.edit ("setClipLoop", { { "id", rig.clipId }, { "loopStart", 0 }, { "loopLength", 2 * beat } });
    rig.settle();
    const auto looped = rig.play (static_cast<int> (4 * 48000 * 120.0 / 90.0));
    const auto wrap = static_cast<size_t> (2 * 48000 * 60.0 / 90.0);
    CHECK_THAT (peakOf (looped, wrap - 480, wrap + 480), WithinAbs (0.5, 0.08));
    CHECK_THAT (peakOf (looped, 2 * wrap - 4800, 2 * wrap - 480), WithinAbs (0.5, 0.08));
}

TEST_CASE ("repitch and the preview of a pending render follow the tempo in pitch", "[stretch][render]")
{
    StretchRig rig (sine (1000.0, 4 * 48000), 0);
    rig.setTiming (musicalAt (120.0));
    rig.compiler.setStretchRenderer (nullptr); // nothing renders: the preview plays
    rig.edit ("setTempo", { { "bpm", 180.0 } });
    REQUIRE (rig.compiledClip().pieces[0].kind == CompiledAudioPiece::Kind::repitch);

    const auto output = rig.play (48000);
    CHECK_THAT (frequencyOf (output, 4800, 43200), WithinAbs (1500.0, 3.0));
}

TEST_CASE ("export renders what is missing and gives the same audio twice", "[stretch][render]")
{
    StretchRig rig (sine (1000.0, 2 * 48000));
    rig.compiler.setStretchRenderer (nullptr);
    rig.setTiming (musicalAt (120.0, { { 2.0 * beat, 50000.0 } }));
    rig.edit ("setTempo", { { "bpm", 133.0 } });

    const auto jobs = stretchJobs (rig.project);
    CHECK (jobs.size() == 2);
    REQUIRE (rig.renderer->renderNow (jobs, nullptr));
    CHECK (rig.renderer->renderedCount() == 2);

    for (const auto& job : jobs)
        CHECK (rig.renderer->lookup (job) != nullptr);

    rig.compiler.setStretchRenderer (rig.renderer.get());
    rig.recompile();
    CHECK (rig.renderer->pendingCount() == 0);
    const auto first = rig.play (3 * 48000);
    rig.recompile();
    const auto second = rig.play (3 * 48000);
    CHECK (first == second);
}

TEST_CASE ("the render cache key follows what a render depends on", "[stretch][cache]")
{
    StretchRig rig (sine (1000.0, 2 * 48000));
    rig.setTiming (musicalAt (120.0));
    rig.edit ("setTempo", { { "bpm", 100.0 } });
    const auto keyNow = [&] { return rig.renderer->keyOf (stretchJobs (rig.project).at (0)); };
    const auto base = keyNow();

    // What changes the render changes the key ...
    const auto changes = [&] (const char* what, auto&& edit)
    {
        CAPTURE (what);
        const auto saved = rig.project;
        edit();
        CHECK (keyNow() != base);
        rig.project = saved;
    };
    changes (
        "transpose", [&]
        { rig.project = apply (rig.project, "setClipTranspose", { { "clipId", rig.clipId }, { "semitones", 1.0 } }); });
    changes ("tempo", [&] { rig.project = apply (rig.project, "setTempo", { { "bpm", 101.0 } }); });
    changes ("source tempo", [&] { rig.project.audioFiles[0].timing.sourceBpm = 119.0; });
    changes ("markers", [&] { rig.project.audioFiles[0].timing.markers = { { 1.0 * beat, 23000.0 } }; });
    changes ("file", [&] { rig.project.audioFiles[0].path = "Audio/other.wav"; });

    // ... and nothing else does: gain, name, or a move to another bar at the same tempo.
    rig.project = apply (rig.project, "setClipProps", { { "id", rig.clipId }, { "gainDb", -3.0 }, { "name", "x" } });
    rig.project = apply (rig.project, "moveClip", { { "id", rig.clipId }, { "start", 9 * beat } });
    CHECK (keyNow() == base);
}

TEST_CASE ("undo finds its render in the cache, and the size limit drops the oldest", "[stretch][cache]")
{
    StretchRig rig (sine (1000.0, 2 * 48000));
    rig.setTiming (musicalAt (120.0));
    const auto before = rig.project;
    rig.edit ("setTempo", { { "bpm", 100.0 } });
    rig.settle();
    const auto tempo100 = rig.project;
    rig.edit ("setTempo", { { "bpm", 140.0 } });
    rig.settle();
    CHECK (rig.renderer->renderedCount() == 2);

    // Undo to 100 BPM: found, not rendered again.
    rig.project = tempo100;
    rig.recompile();
    CHECK (rig.renderer->pendingCount() == 0);
    CHECK (rig.renderer->renderedCount() == 2);

    // A limit below both renders keeps only the one in use.
    const auto job100 = stretchJobs (tempo100).at (0);
    rig.compiler.reset();
    rig.project = before;
    rig.recompile();
    {
        const auto keep = rig.renderer->lookup (job100); // in use
        rig.renderer->setSizeLimit (1);
        CHECK (rig.renderer->lookup (job100) != nullptr);
    }
    CHECK (rig.renderer->cacheSize() < 2 * 4 * 48000 * 2);
}

// The speed the UI may promise (docs/design/time-stretching.md, section 8). Hidden: run with "[.benchmark]".
TEST_CASE ("offline R3 speed on a three-minute stereo take", "[.benchmark][stretch]")
{
    const auto frames = 180 * 48000;
    juce::AudioBuffer<float> input (2, frames);
    juce::Random random (1);

    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < frames; ++i)
            input.setSample (c, i,
                             0.3f * static_cast<float> (std::sin (i * 0.0571 * (c + 1)))
                                 + 0.05f * (random.nextFloat() - 0.5f));

    const auto library = makeRubberBandStretcher();
    StretchSpec stretch;
    stretch.outputFrames = static_cast<juce::int64> (frames * 1.1);

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto output = library->render (input, stretch, nullptr);
    const auto whole = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    CHECK (output.getNumSamples() == stretch.outputFrames);

    // A re-render after a marker move: two segments of four seconds with their context.
    juce::AudioBuffer<float> segment (2, 5 * 48000);
    segment.copyFrom (0, 0, input, 0, 0, segment.getNumSamples());
    segment.copyFrom (1, 0, input, 1, 0, segment.getNumSamples());
    stretch.outputFrames = static_cast<juce::int64> (segment.getNumSamples() * 1.1);
    const auto again = juce::Time::getMillisecondCounterHiRes();
    std::ignore = library->render (segment, stretch, nullptr);
    std::ignore = library->render (segment, stretch, nullptr);
    const auto marker = (juce::Time::getMillisecondCounterHiRes() - again) / 1000.0;

    WARN ("Three minutes of stereo at 48 kHz: " << whole << " s (" << 180.0 / whole
                                                << "x real time); a marker move: " << marker << " s");
}

TEST_CASE ("renders wait for a drag to end and for a quiet moment", "[stretch][cache]")
{
    StretchRig rig (sine (1000.0, 2 * 48000));
    rig.setTiming (musicalAt (120.0));

    // A drag holds them back: the repitch preview plays meanwhile.
    rig.renderer->setHeld (true);
    for (const auto bpm : { 101.0, 103.0, 105.0, 107.0 })
        rig.edit ("setTempo", { { "bpm", bpm } });

    juce::Thread::sleep (StretchRenderer::quietMilliseconds + 300);
    CHECK (rig.renderer->renderedCount() == 0);
    CHECK (rig.renderer->pendingCount() == 1); // only the last state is still wanted

    // Released: only the last state renders.
    rig.renderer->setHeld (false);
    rig.settle();
    CHECK (rig.renderer->renderedCount() == 1);
    REQUIRE (rig.compiledClip().pieces.size() == 1);
    CHECK (rig.compiledClip().pieces[0].kind == CompiledAudioPiece::Kind::render);

    // Quick edits without a hold: nothing starts before the quiet time has passed.
    rig.edit ("setTempo", { { "bpm", 111.0 } });
    rig.edit ("setTempo", { { "bpm", 113.0 } });
    juce::Thread::sleep (StretchRenderer::quietMilliseconds / 3);
    CHECK (rig.renderer->renderedCount() == 1);
    rig.settle();
    CHECK (rig.renderer->renderedCount() == 2);
}

namespace
{
// A steady pitch track at `pitch`, voiced between `from` and `to` seconds.
[[nodiscard]] std::shared_ptr<const PitchTrack> steadyTrack (double seconds, double pitch, double from, double to)
{
    auto track = std::make_shared<PitchTrack>();
    track->sampleRate = rate;
    track->startSeconds = 0.005;

    for (auto t = track->startSeconds; t < seconds; t += track->hopSeconds)
    {
        const auto voiced = t >= from && t < to;
        track->pitch.push_back (voiced ? static_cast<float> (pitch) : std::numeric_limits<float>::quiet_NaN());
        track->voicedProb.push_back (voiced ? 0.9f : 0.0f);
        track->levelDb.push_back (-10.0f);
    }

    return track;
}

[[nodiscard]] std::vector<float> leftOf (const AudioSource& source, int frames)
{
    std::vector<float> left (static_cast<size_t> (frames));
    std::vector<float> right (static_cast<size_t> (frames));
    REQUIRE (source.readBlocking ({ left.data(), right.data() }, 0, frames));
    return left;
}
} // namespace

TEST_CASE ("pitch corrections render, play over the file and feed stretch renders", "[pitch][render]")
{
    // 220 Hz is MIDI 57; one note from 0.5 s to 1.5 s, moved up 2 semitones.
    constexpr int frames = 2 * 48000;
    StretchRig rig (sine (220.0, frames));
    rig.renderer->setPitchTracks ([track = steadyTrack (2.0, 57.0, 0.0, 2.0)] (const AudioFile&) { return track; });
    rig.edit ("seedPitchNotes", { { "fileId", rig.fileId },
                                  { "notes", juce::Array<juce::var> { makeObject (
                                                 { { "start", 24000 }, { "end", 72000 }, { "centre", 57.0 } }) } } });
    rig.edit ("setNotePitch", { { "fileId", rig.fileId }, { "target", 59.0 } });

    // Queued at first, playing the file meanwhile.
    CHECK (rig.compiledClip().pendingRenders == 1);
    CHECK (rig.compiledClip().source == rig.source);
    rig.settle();
    const auto& clip = rig.compiledClip();
    CHECK (clip.pendingRenders == 0);
    REQUIRE (clip.source != rig.source);

    const auto played = leftOf (*clip.source, frames);
    const auto up = 220.0 * std::pow (2.0, 2.0 / 12.0);
    CHECK_THAT (frequencyOf (played, 33600, 62400), WithinAbs (up, up * 0.01));

    // Outside the note (and its 10 ms ramps) the file plays as it was.
    auto worst = 0.0f;

    for (const auto& [from, to] : { std::pair { 0, 23000 }, std::pair { 73000, frames } })
        for (auto i = from; i < to; ++i)
            worst = std::max (worst, std::abs (played[static_cast<size_t> (i)] - rig.buffer.getSample (0, i)));

    CHECK (worst < 1.0e-3f);

    // A stretch of the corrected file reads the corrected audio (here at ratio 1, so the pitch is easy to check).
    StretchJob job;
    job.file = rig.project.audioFiles[0];
    job.inputFrames = frames;
    job.spec.sampleRate = rate;
    job.spec.outputFrames = frames;
    REQUIRE (rig.renderer->renderNow ({ job }, {}));
    const auto stretched = rig.renderer->lookup (job);
    REQUIRE (stretched != nullptr);
    const auto out = leftOf (*stretched, frames);
    CHECK_THAT (frequencyOf (out, 33600, 62400), WithinAbs (up, up * 0.01));
    CHECK_THAT (frequencyOf (out, 2400, 21600), WithinAbs (220.0, 2.2));

    // Export finds the pitch render (cached already) before the stretch jobs.
    CHECK (rig.renderer->pitchJobs (rig.project).size() == 1);

    // A pitch edit alone recompiles (the app's incremental update), so the new correction is requested and played.
    REQUIRE (rig.compiler.update (rig.project, spec) == nullptr);
    const auto edited = apply (rig.project, "setNotePitch", { { "fileId", rig.fileId }, { "target", 58.0 } });
    CHECK (rig.compiler.update (edited, spec) != nullptr);
}
