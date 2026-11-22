#include "TestUtils.h"
#include "dawg/AudioPlayer.h"
#include "dawg/NodeRegistry.h"
#include "dawg/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr int samplesPerBeat = 24000; // 120 BPM at 48 kHz

[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}

[[nodiscard]] juce::var fileArgs (const char* path, double rate, int channels, juce::int64 frames)
{
    return makeObject ({ { "path", path },
                         { "name", "tone" },
                         { "sampleRate", rate },
                         { "channels", channels },
                         { "frames", frames } });
}

// A sine of the given frequency and amplitude.
[[nodiscard]] juce::AudioBuffer<float> sine (double frequency, double rate, int frames, float amplitude = 0.5f)
{
    juce::AudioBuffer<float> buffer (1, frames);

    for (int i = 0; i < frames; ++i)
        buffer.setSample (
            0, i,
            amplitude * static_cast<float> (std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate)));

    return buffer;
}

// One audio track with one clip of a generated tone, a compiler that serves it and an engine.
struct AudioRig
{
    AudioRig (double fileRate = 48000.0, double seconds = 2.0, Ticks start = beat)
        : source (AudioSource::fromBuffer (sine (1000.0, fileRate, static_cast<int> (seconds * fileRate)), fileRate))
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        project = apply (project, "addAudioClip",
                         { { "trackId", trackId },
                           { "start", start },
                           { "file", fileArgs ("Audio/tone.wav", fileRate, 1, source->frames()) } });
        clipId = project.tracks[0]->clips[0]->id;
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
    ObjectId trackId = 0;
    ObjectId clipId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};

[[nodiscard]] float peak (const std::vector<float>& samples, size_t from, size_t to)
{
    float result = 0.0f;
    for (auto i = from; i < to; ++i)
        result = std::max (result, std::abs (samples[i]));
    return result;
}
} // namespace

TEST_CASE ("audio tracks have their own chain and kind rules", "[audio]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto& track = *project.tracks[0];
    CHECK (track.kind == TrackKind::audio);
    CHECK (track.graph.nodes.size() == 5); // player -> EQ -> compressor -> gain -> output

    // Effects go after the player; instruments and note clips don't belong here.
    const auto trackId = track.id;
    project = apply (project, "insertNode",
                     { { "trackId", trackId },
                       { "type", nodetype::pluginEffect },
                       { "plugin", makeObject ({ { "identifier", "fx" } }) } });
    CHECK (project.tracks[0]->graph.connections.size() == 5);
    CHECK_THROWS_AS (apply (project, "insertNode", { { "trackId", trackId }, { "type", nodetype::mpeSynth } }),
                     CommandError);
    CHECK_THROWS_AS (apply (project, "addClip", { { "trackId", trackId }, { "start", 0 }, { "length", beat } }),
                     CommandError);

    // Clips can't move between track kinds.
    project = apply (project, "addAudioClip",
                     { { "trackId", trackId }, { "start", 0 }, { "file", fileArgs ("a.wav", 48000, 2, 48000) } });
    project = apply (project, "addTrack", {});
    const auto clip = project.tracks[0]->clips[0]->id;
    CHECK_THROWS_AS (apply (project, "moveClips", { { "ids", array ({ clip }) }, { "delta", 0 }, { "trackDelta", 1 } }),
                     CommandError);
}

TEST_CASE ("audio clips take their length from the file and reuse pool entries", "[audio]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addAudioClip",
                     { { "trackId", trackId }, { "start", 0 }, { "file", fileArgs ("a.wav", 48000, 2, 48000) } });
    project =
        apply (project, "addAudioClip",
               { { "trackId", trackId }, { "start", 4 * beat }, { "file", fileArgs ("a.wav", 48000, 2, 48000) } });

    REQUIRE (project.audioFiles.size() == 1);
    const auto& clips = project.tracks[0]->clips;
    REQUIRE (clips.size() == 2);
    CHECK (clips[0]->length == 2 * beat); // one second at 120 BPM
    CHECK (clips[0]->fileId == clips[1]->fileId);
}

TEST_CASE ("an audio clip added as a new track lands before the returns", "[audio]")
{
    // The new track goes at the end of the top level, which is before the returns (R10).
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    project = apply (project, "addTrack", { { "kind", "return" } });
    REQUIRE (project.tracks[1]->kind == TrackKind::returnTrack);

    project = apply (project, "addAudioClip",
                     { { "newTrack", true }, { "start", 0 }, { "file", fileArgs ("a.wav", 48000, 2, 48000) } });

    REQUIRE (project.tracks.size() == 3);
    CHECK (project.tracks[1]->kind == TrackKind::audio);
    CHECK (project.tracks[1]->name == "tone"); // named after the file
    CHECK (project.tracks[2]->kind == TrackKind::returnTrack);
    REQUIRE (project.tracks[1]->clips.size() == 1);
    CHECK (project.tracks[1]->clips[0]->fileId == project.audioFiles[0].id);
}

TEST_CASE ("audio clips keep their duration when the tempo changes", "[audio]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project =
        apply (project, "addAudioClip",
               { { "trackId", trackId }, { "start", 4 * beat }, { "file", fileArgs ("a.wav", 48000, 2, 48000) } });
    const auto clipId = project.tracks[0]->clips[0]->id;
    project = apply (project, "setClipProps", { { "id", clipId }, { "fadeIn", beat / 2 } });
    project = apply (project, "setTempo", { { "bpm", 60 } });

    const auto& clip = *project.tracks[0]->clips[0];
    CHECK (clip.start == 4 * beat); // the musical position stays
    CHECK (clip.length == beat);    // one second at 60 BPM
    CHECK (clip.fadeIn == beat / 4);
}

TEST_CASE ("an audio clip plays from its start position", "[audio][engine]")
{
    AudioRig rig;
    rig.engine.play();
    const auto output = render (rig.engine, 2 * samplesPerBeat);
    const auto& left = output.left();

    CHECK (firstAudible ({ left.begin(), left.begin() + samplesPerBeat }) == -1);
    CHECK_THAT (peak (left, samplesPerBeat + 1000, samplesPerBeat + 4000), WithinAbs (0.5, 0.01));
    CHECK (zeroCrossings (left, samplesPerBeat + 1000, samplesPerBeat + 5800) == 200); // 1 kHz for 0.1 s
}

TEST_CASE ("audio at another sample rate is resampled to the device rate", "[audio][engine]")
{
    AudioRig rig (44100.0);
    rig.engine.play();
    const auto output = render (rig.engine, 2 * samplesPerBeat);
    const auto& left = output.left();

    // Still 1 kHz and full level at 48 kHz.
    CHECK (std::abs (zeroCrossings (left, samplesPerBeat + 1000, samplesPerBeat + 5800) - 200) <= 1);
    CHECK_THAT (peak (left, samplesPerBeat + 1000, samplesPerBeat + 4000), WithinAbs (0.5, 0.01));
}

TEST_CASE ("clip gain and fades shape the level", "[audio][engine]")
{
    AudioRig rig;
    rig.edit ("setClipProps", { { "id", rig.clipId }, { "gainDb", -6.0206 }, { "fadeIn", beat / 2 } });
    rig.engine.play();
    const auto output = render (rig.engine, 2 * samplesPerBeat);
    const auto& left = output.left();

    // Halfway through the linear fade-in: half of the half-gain level.
    const auto mid = static_cast<size_t> (samplesPerBeat) + static_cast<size_t> (samplesPerBeat / 4);
    CHECK_THAT (peak (left, mid - 50, mid + 50), WithinAbs (0.125, 0.01));
    CHECK_THAT (peak (left, samplesPerBeat + 15000, samplesPerBeat + 20000), WithinAbs (0.25, 0.01));

    CHECK (fadeGain (FadeShape::equalPower, 0.5) > 0.7f);
    CHECK_THAT (fadeGain (FadeShape::exponential, 0.5), WithinAbs (0.25, 1e-6));
}

TEST_CASE ("seeking into an audio clip plays from the middle at once", "[audio][engine]")
{
    AudioRig rig;
    rig.engine.seek (1.5);
    rig.engine.play();
    const auto output = render (rig.engine, 4096);
    CHECK (firstAudible (output.left()) <= 2);
}

TEST_CASE ("mute and solo decide which tracks are heard", "[audio][engine]")
{
    AudioRig rig (48000.0, 2.0, 0);
    rig.edit ("addTrack", { { "kind", "audio" } });
    const auto second = rig.project.tracks[1]->id;

    rig.edit ("setTrackSwitches", { { "id", second }, { "solo", true } });
    rig.engine.play();
    CHECK (firstAudible (render (rig.engine, 4096).left()) == -1); // only the empty, soloed track plays

    rig.edit ("setTrackSwitches", { { "id", second }, { "solo", false } });
    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "mute", true } });
    CHECK (firstAudible (render (rig.engine, 4096).left()) == -1);

    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "mute", false } });
    CHECK (firstAudible (render (rig.engine, 4096).left()) >= 0);
}

TEST_CASE ("audio tracks and the pool round-trip through project files", "[audio][file]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addAudioClip",
                     { { "trackId", trackId }, { "start", 0 }, { "file", fileArgs ("Audio/a.wav", 44100, 1, 44100) } });
    project = apply (
        project, "setClipProps",
        { { "id", project.tracks[0]->clips[0]->id }, { "gainDb", -3 }, { "fadeOut", beat }, { "fadeOutShape", 1 } });
    project = apply (
        project, "setTrackSwitches",
        { { "id", trackId }, { "mute", true }, { "inputChannel", 2 }, { "inputStereo", true }, { "monitor", 2 } });

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-audio-" + juce::Uuid().toString() + ".dawg");
    REQUIRE (projectfile::write (project, { "id", "v" }, file, false).isEmpty());
    const auto loaded = projectfile::read (file).project;
    std::ignore = file.deleteFile();

    CHECK (juce::JSON::toString (toVar (loaded)) == juce::JSON::toString (toVar (project)));
}

// ---------------------------------------------------------------------------------------------------------
// Recording and monitoring

TEST_CASE ("monitoring passes the input through the track", "[audio][record]")
{
    AudioRig rig (48000.0, 1.0, 64 * beat); // the clip is far away; only the input is heard
    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "inputChannel", 0 }, { "monitor", 2 } });

    std::vector<float> input (4096, 0.25f);
    const std::array<const float*, 1> inputs { input.data() };
    StereoBuffer out (4096);
    rig.engine.renderOffline (out.channels(), 2, 4096, sampleRate, inputs.data(), 1);
    CHECK_THAT (out.left()[100], WithinAbs (0.25, 1e-6));

    // Automatic monitoring needs the track armed.
    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "monitor", 1 } });
    StereoBuffer quiet (4096);
    rig.engine.renderOffline (quiet.channels(), 2, 4096, sampleRate, inputs.data(), 1);
    CHECK (firstAudible (quiet.left()) == -1);

    rig.compiler.trackState (rig.trackId)->armed.store (true);
    StereoBuffer armed (4096);
    rig.engine.renderOffline (armed.channels(), 2, 4096, sampleRate, inputs.data(), 1);
    CHECK_THAT (armed.left()[100], WithinAbs (0.25, 1e-6));
}

TEST_CASE ("recording writes the input from the record position on", "[audio][record]")
{
    AudioRig rig (48000.0, 1.0, 64 * beat);
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-rec-" + juce::Uuid().toString() + ".wav");

    juce::TimeSliceThread diskThread ("test recorder");
    diskThread.startThread();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (
        stream,
        juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), diskThread, 1 << 16);

    const RecordTarget target { rig.trackId, 0, false, threaded.get() };
    rig.engine.startRecording ({ &target, 1 }, 0.5); // a count-in: recording starts half a beat in
    rig.engine.play();

    std::vector<float> input (static_cast<size_t> (samplesPerBeat), 0.5f);
    const std::array<const float*, 1> inputs { input.data() };
    StereoBuffer out (samplesPerBeat);
    rig.engine.renderOffline (out.channels(), 2, samplesPerBeat, sampleRate, inputs.data(), 1);
    rig.engine.stopRecording();

    CHECK_THAT (rig.engine.recordStartBeats(), WithinAbs (0.5, 1e-3));
    CHECK (rig.engine.recordedFrames (0) == samplesPerBeat / 2);

    threaded.reset(); // flushes and closes the file
    diskThread.stopThread (1000);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    CHECK (reader->lengthInSamples == samplesPerBeat / 2);
    reader.reset();
    std::ignore = file.deleteFile();
}

TEST_CASE ("an audio clip starts at its first sample when the loop wraps ahead of the playhead",
           "[audio][engine][loop]")
{
    // A 10 ms negative offset makes the track run 480 samples ahead of the playhead. The source's impulse is just
    // past its start; without an early wrap the second iteration would start 480 samples into the source and skip it.
    constexpr int ahead = 480;
    constexpr int impulse = 100; // just past the clip's declick fade-in

    juce::AudioBuffer<float> data (1, 2 * samplesPerBeat);
    data.clear();
    data.setSample (0, impulse, 0.8f);
    const auto source = AudioSource::fromBuffer (std::move (data), sampleRate);

    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addAudioClip",
                     { { "trackId", trackId },
                       { "start", 0 },
                       { "file", fileArgs ("Audio/impulse.wav", sampleRate, 1, source->frames()) } });
    project = apply (project, "setTrackOffset", { { "id", trackId }, { "offsetMs", -1000.0 * ahead / sampleRate } });
    project = apply (project, "setLoop", { { "enabled", true }, { "start", 0 }, { "end", beat } });

    GraphCompiler compiler;
    compiler.setAudioSourceResolver ([source] (const AudioFile&) { return source; });
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();

    const auto output = render (engine, 2 * samplesPerBeat);
    const auto& left = output.left();

    // The first iteration's impulse is 10 ms before sample 0. Each looped iteration must play it 10 ms before the
    // loop end, at the start of the source.
    CHECK (firstAudible ({ left.begin() + samplesPerBeat - 1000, left.begin() + samplesPerBeat })
           == 1000 - ahead + impulse);
    CHECK (firstAudible ({ left.begin() + static_cast<std::ptrdiff_t> (2) * samplesPerBeat - 1000, left.end() })
           == 1000 - ahead + impulse);
}

TEST_CASE ("a lead longer than the loop wraps into the right iteration", "[audio][engine][loop]")
{
    // 750 ms early is one and a half beats at 120 BPM: the track starts half a beat into the loop, wraps at actual
    // sample 12000 and plays the source's impulse 100 samples later. Without the modulo, the lead would sit past the
    // loop end and never wrap.
    constexpr int ahead = 36000;
    constexpr int impulse = 100;

    juce::AudioBuffer<float> data (1, 2 * samplesPerBeat);
    data.clear();
    data.setSample (0, impulse, 0.8f);
    const auto source = AudioSource::fromBuffer (std::move (data), sampleRate);

    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addAudioClip",
                     { { "trackId", trackId },
                       { "start", 0 },
                       { "file", fileArgs ("Audio/impulse.wav", sampleRate, 1, source->frames()) } });
    project = apply (project, "setTrackOffset", { { "id", trackId }, { "offsetMs", -1000.0 * ahead / sampleRate } });
    project = apply (project, "setLoop", { { "enabled", true }, { "start", 0 }, { "end", beat } });

    GraphCompiler compiler;
    compiler.setAudioSourceResolver ([source] (const AudioFile&) { return source; });
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();

    const auto output = render (engine, 2 * samplesPerBeat);
    const auto& left = output.left();
    const auto expected = samplesPerBeat / 2 + impulse;

    CHECK (firstAudible ({ left.begin() + expected - 300, left.begin() + expected + 300 }) == 300);
    CHECK (firstAudible (
               { left.begin() + samplesPerBeat + expected - 300, left.begin() + samplesPerBeat + expected + 300 })
           == 300);
}

TEST_CASE ("the loop seam is declicked without repeating audio", "[audio][engine][loop]")
{
    // A rising ramp, looped over its second beat: the wrap jumps from about 2/3 back to 1/3. The step is faded out
    // after the seam, and the loop's content then plays exactly as it did the first time, not shifted or repeated.
    constexpr int declick = 64;
    const auto length = 3 * samplesPerBeat;

    juce::AudioBuffer<float> data (1, length);

    for (int i = 0; i < length; ++i)
        data.setSample (0, i, static_cast<float> (i) / static_cast<float> (length));

    const auto source = AudioSource::fromBuffer (std::move (data), sampleRate);

    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addAudioClip",
                     { { "trackId", trackId },
                       { "start", 0 },
                       { "file", fileArgs ("Audio/ramp.wav", sampleRate, 1, source->frames()) } });
    project = apply (project, "setLoop", { { "enabled", true }, { "start", beat }, { "end", 2 * beat } });

    GraphCompiler compiler;
    compiler.setAudioSourceResolver ([source] (const AudioFile&) { return source; });
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();

    const auto output = render (engine, 3 * samplesPerBeat);
    const auto& left = output.left();
    const auto seam = 2 * static_cast<size_t> (samplesPerBeat);
    const auto loopStart = static_cast<size_t> (samplesPerBeat);

    // Without the declick the seam would jump by a third of full scale.
    REQUIRE (left[seam - 1] - left[loopStart] > 0.2f);
    CHECK (std::abs (left[seam] - left[seam - 1]) < 0.01f);

    for (size_t i = declick; i < declick + 1000; ++i)
        CHECK_THAT (left[seam + i], WithinAbs (left[loopStart + i], 1.0e-5));
}

TEST_CASE ("an audio clip across a tempo change plays on at its own speed", "[audio][engine][tempo]")
{
    // Two seconds of 1 kHz from beat 1 (sample 24000); from beat 2 the tempo halves.
    AudioRig rig;
    rig.edit ("addTempoPoint", { { "tick", 2 * beat }, { "bpm", 60.0 } });
    rig.engine.play();
    const auto output = render (rig.engine, 24000 + 96000 + 4800);
    const auto& left = output.left();

    // Unbroken 1 kHz across the change at sample 48000, and the clip still lasts two seconds.
    CHECK (std::abs (zeroCrossings (left, 44000, 52000) - 333) <= 2);
    CHECK_THAT (peak (left, 47000, 49000), WithinAbs (0.5, 0.01));
    CHECK_THAT (peak (left, 110000, 115000), WithinAbs (0.5, 0.01));
    CHECK (firstAudible ({ left.begin() + 24000 + 96000 + 200, left.end() }) == -1);
}

namespace
{
// The width of a track's Audio Player after the last compile (docs/design/mono-ports.md, MP15): 1 or 2.
[[nodiscard]] int playerWidthOf (const AudioRig& rig)
{
    const auto& nodes = rig.project.tracks[0]->graph.nodes;
    const auto player =
        std::ranges::find_if (nodes, [] (const GraphNode& n) { return n.type == nodetype::audioPlayer; });
    REQUIRE (player != nodes.end());
    const auto it = rig.compiler.portWidths().find ({ rig.trackId, player->id });
    return it == rig.compiler.portWidths().end() || it->second.outputs.empty() ? 2 : it->second.outputs[0];
}
} // namespace

TEST_CASE ("a track of mono clips plays mono, heard on both sides", "[audio][engine][mono]")
{
    AudioRig rig; // its tone is a mono file
    CHECK (playerWidthOf (rig) == 1);

    rig.engine.play();
    const auto output = render (rig.engine, 2 * samplesPerBeat);
    CHECK_THAT (peak (output.left(), samplesPerBeat + 1000, samplesPerBeat + 4000), WithinAbs (0.5, 0.01));
    CHECK (output.left() == output.right());
}

TEST_CASE ("a stereo clip makes the player stereo", "[audio][mono]")
{
    AudioRig rig;
    rig.edit ("addAudioClip", { { "trackId", rig.trackId },
                                { "start", 16 * beat },
                                { "file", fileArgs ("Audio/wide.wav", 48000.0, 2, rig.source->frames()) } });
    CHECK (playerWidthOf (rig) == 2);
}

TEST_CASE ("the track's input decides the player's width with its clips", "[audio][mono][record]")
{
    AudioRig rig;

    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "inputChannel", 0 }, { "inputStereo", true } });
    CHECK (playerWidthOf (rig) == 2);

    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "inputStereo", false } });
    CHECK (playerWidthOf (rig) == 1);

    // Another mono channel keeps the width, so it needs no compilation.
    const auto moved = apply (rig.project, "setTrackSwitches", { { "id", rig.trackId }, { "inputChannel", 3 } });
    CHECK (rig.compiler.update (moved, spec) == nullptr);

    // Without clips, a mono input alone makes it mono; with nothing at all it stays stereo.
    rig.edit ("deleteClips", { { "ids", array ({ rig.clipId }) } });
    CHECK (playerWidthOf (rig) == 1);
    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "inputChannel", -1 } });
    CHECK (playerWidthOf (rig) == 2);
}

TEST_CASE ("a mono input is monitored on both sides", "[audio][mono][record]")
{
    AudioRig rig (48000.0, 1.0, 64 * beat);
    rig.edit ("setTrackSwitches", { { "id", rig.trackId }, { "inputChannel", 0 }, { "monitor", 2 } });
    REQUIRE (playerWidthOf (rig) == 1);

    std::vector<float> input (4096, 0.25f);
    const std::array<const float*, 1> inputs { input.data() };
    StereoBuffer out (4096);
    rig.engine.renderOffline (out.channels(), 2, 4096, sampleRate, inputs.data(), 1);
    CHECK_THAT (out.left()[100], WithinAbs (0.25, 1e-6));
    CHECK_THAT (out.right()[100], WithinAbs (0.25, 1e-6));
}
