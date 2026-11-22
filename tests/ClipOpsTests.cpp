// Reverse and normalise (docs/design/audio-features.md, AU1 and AU2).

#include "TestUtils.h"
#include "dawg/ClipOps.h"
#include "dawg/ProjectFile.h"
#include "dawg/TimingMap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr int samplesPerBeat = 24000;     // 120 BPM at 48 kHz
constexpr juce::int64 fileFrames = 48000; // one second: two beats at 120 BPM

[[nodiscard]] juce::var fileArgs (const juce::String& path, juce::int64 frames = fileFrames)
{
    return makeObject (
        { { "path", path }, { "name", "ramp" }, { "sampleRate", 48000.0 }, { "channels", 1 }, { "frames", frames } });
}

[[nodiscard]] juce::AudioBuffer<float> ramp (int frames)
{
    juce::AudioBuffer<float> buffer (1, frames);

    for (int i = 0; i < frames; ++i)
        buffer.setSample (0, i, -0.5f + static_cast<float> (i) / static_cast<float> (frames));

    return buffer;
}

[[nodiscard]] juce::AudioBuffer<float> reversed (juce::AudioBuffer<float> buffer)
{
    std::reverse (buffer.getWritePointer (0), buffer.getWritePointer (0) + buffer.getNumSamples());
    return buffer;
}

// One audio track with a clip of a ramp file at beat 1, and its reversed copy served by path.
struct ReverseRig
{
    explicit ReverseRig (Ticks offset = 0, Ticks length = 2 * beat)
        : forward (AudioSource::fromBuffer (ramp (static_cast<int> (fileFrames)), 48000.0)),
          backward (AudioSource::fromBuffer (reversed (ramp (static_cast<int> (fileFrames))), 48000.0))
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project = apply (project, "addAudioClip",
                         { { "trackId", trackId },
                           { "start", beat },
                           { "offset", offset },
                           { "length", length },
                           { "file", fileArgs ("Audio/ramp.wav") } });
        clipId = project.tracks[0]->clips[0]->id;
        fileId = project.tracks[0]->clips[0]->fileId;
        compiler.setAudioSourceResolver ([this] (const AudioFile& file)
                                         { return file.path.contains ("reversed") ? backward : forward; });
    }

    [[nodiscard]] Project reverse (const Project& from, const std::vector<ObjectId>& ids) const
    {
        juce::Array<juce::var> clipIds;
        for (const auto id : ids)
            clipIds.add (id);

        const auto key = juce::String (fileId);
        const auto files = makeObject ({ { key.toRawUTF8(), fileArgs ("Audio/ramp (reversed).wav") } });
        return apply (from, "reverseClips", { { "clipIds", clipIds }, { "files", files } });
    }

    [[nodiscard]] std::vector<float> play (const Project& p)
    {
        AudioEngine engine;
        engine.setEngine (compiler.compile (p, spec));
        engine.play();
        auto output = render (engine, 4 * samplesPerBeat);
        return output.left();
    }

    std::shared_ptr<const AudioSource> forward;
    std::shared_ptr<const AudioSource> backward;
    Project project;
    ObjectId trackId = 0;
    ObjectId clipId = 0;
    ObjectId fileId = 0;
    GraphCompiler compiler;
};

[[nodiscard]] const NoteClip& firstClip (const Project& project)
{
    return *project.tracks[0]->clips[0];
}
} // namespace

TEST_CASE ("mirroring a clip plays the same audio backwards and twice gives it back", "[clipops]")
{
    NoteClip clip;
    clip.start = 3 * beat;
    clip.offset = beat / 2;
    clip.length = 2 * beat;
    clip.fadeIn = 10;
    clip.fadeOut = 20;
    clip.fadeOutShape = FadeShape::equalPower;
    const auto original = clip;

    mirrorClip (clip, 4.0 * beat);
    CHECK (clip.offset == 4 * beat - beat / 2 - 2 * beat);
    CHECK (clip.start == original.start);
    CHECK (clip.length == original.length);
    CHECK (clip.fadeIn == 20);
    CHECK (clip.fadeOut == 10);
    CHECK (clip.fadeInShape == FadeShape::equalPower);
    CHECK (clip.fadeOutShape == FadeShape::linear);

    mirrorClip (clip, 4.0 * beat);
    CHECK (clip.offset == original.offset);
    CHECK (clip.fadeIn == original.fadeIn);
    CHECK (clip.fadeInShape == original.fadeInShape);

    SECTION ("a looped clip mirrors its loop, and each pass plays backwards")
    {
        NoteClip looped;
        looped.offset = beat + beat / 4;
        looped.loopStart = beat;
        looped.loopLength = beat;
        looped.length = 5 * beat;
        const auto before = looped;

        mirrorClip (looped, 4.0 * beat);
        CHECK (looped.loopStart == 2 * beat);
        CHECK (looped.loopLength == beat);
        // The first pass is as long as before (3/4 beat), ending at the mirrored loop's end.
        CHECK (looped.loopStart + looped.loopLength - looped.offset == 3 * beat / 4);

        mirrorClip (looped, 4.0 * beat);
        CHECK (looped.offset == before.offset);
        CHECK (looped.loopStart == before.loopStart);
    }

    SECTION ("a clip reaching past its file's end starts at the copy's start")
    {
        NoteClip longer;
        longer.offset = beat;
        longer.length = 6 * beat;
        mirrorClip (longer, 4.0 * beat);
        CHECK (longer.offset == 0);
    }
}

TEST_CASE ("a reversed file's timing plays the original's backwards", "[clipops]")
{
    FileTiming timing;
    timing.mode = TimingMode::musical;
    timing.sourceBpm = 100.0;
    timing.markers = { { 480.0, 12000.0 }, { 1500.0, 30000.0 }, { 2000.0, 41000.0 } };
    constexpr juce::int64 frames = 60000;

    const auto mirrored = reversedTiming (timing, 48000.0, frames);
    const TimingMap a (timing, 48000.0);
    const TimingMap b (mirrored, 48000.0);
    const auto end = a.tickAt (static_cast<double> (frames));

    CHECK_THAT (b.tickAt (static_cast<double> (frames)), WithinAbs (end, 1e-6));

    for (double tick = 0.0; tick <= end; tick += 37.0)
        CHECK_THAT (b.frameAt (end - tick), WithinAbs (static_cast<double> (frames) - a.frameAt (tick), 1e-6));

    SECTION ("a one-shot file without markers keeps its timing")
    {
        CHECK (reversedTiming (FileTiming {}, 48000.0, frames) == FileTiming {});
    }
}

TEST_CASE ("reverseClips points clips at a reversed copy, shared, and back", "[clipops][commands]")
{
    ReverseRig rig;
    auto project = apply (rig.project, "addAudioClip",
                          { { "trackId", rig.trackId }, { "start", 8 * beat }, { "fileId", rig.fileId } });
    const auto second = project.tracks[0]->clips[1]->id;

    const auto once = rig.reverse (project, { rig.clipId });
    REQUIRE (once.audioFiles.size() == 2);
    const auto& copy = once.audioFiles[1];
    CHECK (copy.reverseOf == rig.fileId);
    CHECK (copy.name == "ramp (reversed)");
    CHECK (firstClip (once).fileId == copy.id);

    // The second clip reuses the copy, without being handed a file.
    const auto both = apply (once, "reverseClips", { { "clipIds", juce::Array<juce::var> { second } } });
    CHECK (both.audioFiles.size() == 2);
    CHECK (both.tracks[0]->clips[1]->fileId == copy.id);

    // Reversing again goes back to the original, exactly.
    const auto back = apply (both, "reverseClips", { { "clipIds", juce::Array<juce::var> { rig.clipId, second } } });
    CHECK (back.audioFiles.size() == 2);
    CHECK (juce::JSON::toString (toVar (*back.tracks[0]->clips[0]))
           == juce::JSON::toString (toVar (*project.tracks[0]->clips[0])));
    CHECK (juce::JSON::toString (toVar (*back.tracks[0]->clips[1]))
           == juce::JSON::toString (toVar (*project.tracks[0]->clips[1])));

    SECTION ("without a copy, it is refused")
    {
        CHECK_THROWS_AS (apply (rig.project, "reverseClips", { { "clipIds", juce::Array<juce::var> { rig.clipId } } }),
                         CommandError);
    }

    SECTION ("note clips can't be reversed")
    {
        const auto notes = projectWithClip();
        CHECK_THROWS_AS (
            apply (notes.project, "reverseClips", { { "clipIds", juce::Array<juce::var> { notes.clipId } } }),
            CommandError);
    }

    SECTION ("reverseOf round-trips through project files")
    {
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("dawg-reverse-" + juce::Uuid().toString() + ".dawg");
        REQUIRE (projectfile::write (once, { "id", "v" }, file, false).isEmpty());
        const auto loaded = projectfile::read (file).project;
        std::ignore = file.deleteFile();
        REQUIRE (loaded.audioFiles.size() == 2);
        CHECK (loaded.audioFiles[1].reverseOf == rig.fileId);
    }
}

TEST_CASE ("a reversed clip plays its audio backwards in the same place", "[clipops][engine]")
{
    // Half a second from a quarter second in: frames 12000 to 36000.
    ReverseRig rig (beat / 2, beat);
    const auto& project = rig.project;
    const auto& clip = firstClip (project);
    REQUIRE (clip.offset == beat / 2);
    REQUIRE (clip.length == beat);

    const auto reversedProject = rig.reverse (project, { rig.clipId });
    const auto output = rig.play (reversedProject);

    // Past the 2 ms declick, sample t of the clip is the original's frame 35999 - t.
    for (int t = 200; t < samplesPerBeat - 200; t += 97)
        CHECK_THAT (output[static_cast<size_t> (samplesPerBeat + t)], WithinAbs (-0.5 + (35999.0 - t) / 48000.0, 1e-4));

    CHECK (firstAudible ({ output.begin(), output.begin() + samplesPerBeat }) == -1);
}

TEST_CASE ("the frames a clip plays", "[clipops]")
{
    AudioFile file;
    file.sampleRate = 48000.0;
    file.frames = 96000; // four beats at 120 BPM
    const TempoMap map;

    NoteClip clip;
    clip.fileId = 1;
    clip.start = 0;
    clip.offset = beat;
    clip.length = beat;
    CHECK (playedFrameRanges (clip, file, map)
           == std::vector<std::pair<juce::int64, juce::int64>> { { 24000, 48000 } });

    // Past the file's end: cut there.
    clip.length = 8 * beat;
    CHECK (playedFrameRanges (clip, file, map)
           == std::vector<std::pair<juce::int64, juce::int64>> { { 24000, 96000 } });

    SECTION ("a loop plays its first pass and the loop range")
    {
        clip.offset = 3 * beat;
        clip.loopStart = beat / 2;
        clip.loopLength = beat;
        clip.length = 4 * beat;
        // The offset past the loop end starts at the loop start: only the loop range plays.
        CHECK (playedFrameRanges (clip, file, map)
               == std::vector<std::pair<juce::int64, juce::int64>> { { 12000, 36000 } });

        clip.offset = beat;
        clip.loopStart = 2 * beat;
        // From the offset to the loop end, then the loop: one range.
        CHECK (playedFrameRanges (clip, file, map)
               == std::vector<std::pair<juce::int64, juce::int64>> { { 24000, 72000 } });

        clip.offset = 0;
        clip.loopStart = 3 * beat;
        clip.length = 3 * beat + beat / 4;
        // The clip ends inside its first pass.
        CHECK (playedFrameRanges (clip, file, map)
               == std::vector<std::pair<juce::int64, juce::int64>> { { 0, 78000 } });
    }

    SECTION ("musical files go through the timing map")
    {
        file.timing.mode = TimingMode::musical;
        file.timing.sourceBpm = 60.0; // a beat is 48000 frames
        clip.offset = beat / 2;
        clip.length = beat / 2;
        CHECK (playedFrameRanges (clip, file, map)
               == std::vector<std::pair<juce::int64, juce::int64>> { { 24000, 48000 } });
    }
}

TEST_CASE ("normalising sets each clip, or all together, to the target peak", "[clipops]")
{
    const std::vector<float> peaks { 0.25f, 0.5f, 0.0f, 1.0e-6f };

    const auto each = normaliseChanges (peaks, 0.0, false);
    CHECK_THAT (each[0], WithinAbs (12.0412, 1e-3));
    CHECK_THAT (each[1], WithinAbs (6.0206, 1e-3));
    CHECK (each[2] == 0.0);
    CHECK_THAT (each[3], WithinAbs (120.0, 1e-3));

    const auto together = normaliseChanges (peaks, -1.0, true);
    for (const auto change : together)
        CHECK_THAT (change, WithinAbs (5.0206, 1e-3));
}

TEST_CASE ("writeReversed writes a file backwards", "[clipops][file]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("dawg-reverse-" + juce::Uuid().toString());
    REQUIRE (folder.createDirectory());
    const auto source = folder.getChildFile ("ramp.wav");
    const auto target = folder.getChildFile ("ramp (reversed).wav");
    // Longer than one chunk, and not a multiple of it.
    constexpr int frames = 150001;
    juce::AudioBuffer<float> audio (2, frames);

    for (int i = 0; i < frames; ++i)
    {
        audio.setSample (0, i, static_cast<float> (i % 1000) / 1000.0f);
        audio.setSample (1, i, -static_cast<float> (i % 777) / 777.0f);
    }

    {
        std::unique_ptr<juce::OutputStream> stream (source.createOutputStream());
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (
            stream, juce::AudioFormatWriterOptions {}
                        .withSampleRate (44100.0)
                        .withNumChannels (2)
                        .withBitsPerSample (32)
                        .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        REQUIRE (writer != nullptr);
        REQUIRE (writer->writeFromAudioSampleBuffer (audio, 0, frames));
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));
        REQUIRE (reader != nullptr);
        REQUIRE (writeReversed (*reader, target));
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (target));
    REQUIRE (reader != nullptr);
    CHECK (reader->lengthInSamples == frames);
    CHECK (reader->numChannels == 2);
    CHECK (reader->sampleRate == 44100.0);
    juce::AudioBuffer<float> back (2, frames);
    REQUIRE (reader->read (&back, 0, frames, 0, true, true));
    reader.reset();

    auto mismatches = 0;
    for (int i = 0; i < frames; ++i)
        for (int ch = 0; ch < 2; ++ch)
            if (back.getSample (ch, i) != audio.getSample (ch, frames - 1 - i))
                ++mismatches;

    CHECK (mismatches == 0);
    std::ignore = folder.deleteRecursively();
}
