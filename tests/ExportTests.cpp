#include "TestUtils.h"
#include "dawg/Export.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Phase 9: export rendering, loudness and file formats (docs/design/phase9-export-midi-input.md, X1–X6).

namespace
{
// Measures a sine in the left channel only (the right is silent).
[[nodiscard]] double lufsOfSine (double frequency, float amplitude, double seconds)
{
    LoudnessMeter meter;
    meter.prepare (sampleRate);
    const auto frames = static_cast<int> (seconds * sampleRate);
    std::vector<float> left (static_cast<size_t> (frames));
    const std::vector<float> right (static_cast<size_t> (frames));

    for (size_t i = 0; i < left.size(); ++i)
        left[i] = amplitude
                  * static_cast<float> (
                      std::sin (juce::MathConstants<double>::twoPi * frequency * static_cast<double> (i) / sampleRate));

    meter.process ({ left.data(), right.data() }, frames);
    return meter.integratedLufs();
}

// A project with one note playing for the first beat, and an engine for it.
struct ExportRig
{
    ExportRig()
    {
        auto setup = projectWithClip();
        trackId = setup.trackId;
        project = apply (
            setup.project, "addNotes",
            { { "clipId", setup.clipId }, { "notes", juce::Array<juce::var> { noteArgs (0, ticksPerBeat, 69) } } });
        engine.setEngine (compiler.compile (project, spec));
    }

    Project project;
    ObjectId trackId = 0;
    GraphCompiler compiler;
    AudioEngine engine;
};
} // namespace

TEST_CASE ("loudness follows BS.1770 for reference tones", "[export]")
{
    // EBU Tech 3341: a 1 kHz sine at 0 dBFS in one channel reads −3.01 LUFS.
    CHECK_THAT (lufsOfSine (1000.0, 1.0f, 5.0), WithinAbs (-3.01, 0.05));
    CHECK_THAT (lufsOfSine (1000.0, 0.1f, 5.0), WithinAbs (-23.01, 0.05));

    // Silence is gated away entirely.
    LoudnessMeter silence;
    silence.prepare (sampleRate);
    const std::vector<float> zeros (48000);
    silence.process ({ zeros.data(), zeros.data() }, 48000);
    CHECK (std::isinf (silence.integratedLufs()));
}

TEST_CASE ("a range renders without looping, then a tail until silence", "[export][engine]")
{
    ExportRig rig;
    rig.project = apply (rig.project, "setLoop", { { "enabled", true }, { "start", 0 }, { "end", ticksPerBeat } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    juce::int64 audibleUntil = 0;
    juce::int64 position = 0;
    const auto frames = renderRange (rig.engine, { 0.0, 2.0, true, 30.0, {}, false, {} }, sampleRate, blockSize,
                                     [&] (std::array<const float*, 2> channels, int count)
                                     {
                                         for (int i = 0; i < count; ++i)
                                             if (std::abs (channels[0][i]) > 1.0e-4f)
                                                 audibleUntil = position + i;

                                         position += count;
                                         return true;
                                     });

    // Two beats (48000 frames), then the synth's release and a quiet second; the loop didn't repeat the note.
    CHECK (frames >= 48000 + 48000);
    CHECK (frames < 48000 + 3 * 48000);
    CHECK (audibleUntil < 48000);
    CHECK_FALSE (rig.engine.isPlaying());

    const auto fixed = renderRange (rig.engine, { 0.0, 2.0, false, 0.5, {}, false, {} }, sampleRate, blockSize,
                                    [] (std::array<const float*, 2>, int) { return true; });
    CHECK (fixed >= 48000 + 24000);
    CHECK (fixed < 48000 + 24000 + 2 * blockSize);

    const auto cancelled = renderRange (rig.engine, { 0.0, 2.0, true, 30.0, {}, false, {} }, sampleRate, blockSize,
                                        [] (std::array<const float*, 2>, int) { return false; });
    CHECK (cancelled == -1);
}

TEST_CASE ("a stem renders one track, ignoring mute and the other tracks", "[export][engine]")
{
    ExportRig rig;
    rig.project = apply (rig.project, "addTrack", {});
    const auto other = rig.project.tracks[1]->id;
    rig.project = apply (rig.project, "setTrackSwitches", { { "id", rig.trackId }, { "mute", true } });
    rig.engine.setEngine (rig.compiler.compile (rig.project, spec));

    float peak = 0.0f;
    const auto sink = [&] (std::array<const float*, 2> channels, int count)
    {
        for (int i = 0; i < count; ++i)
            peak = std::max (peak, std::abs (channels[0][i]));
        return true;
    };

    std::ignore =
        renderRange (rig.engine, { 0.0, 1.0, true, 30.0, { rig.trackId }, false, {} }, sampleRate, blockSize, sink);
    CHECK (peak > 0.01f);

    peak = 0.0f;
    std::ignore = renderRange (rig.engine, { 0.0, 1.0, true, 30.0, { other }, false, {} }, sampleRate, blockSize, sink);
    CHECK (peak < 1.0e-6f);
}

TEST_CASE ("rendered audio is written in every format", "[export][file]")
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("dawg-export-" + juce::Uuid().toString());
    REQUIRE (folder.createDirectory());
    const auto source = folder.getChildFile ("render.wav");

    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (source.createOutputStream());
        auto writer = wav.createWriterFor (
            stream, juce::AudioFormatWriterOptions {}
                        .withSampleRate (sampleRate)
                        .withNumChannels (2)
                        .withBitsPerSample (32)
                        .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        REQUIRE (writer != nullptr);
        juce::AudioBuffer<float> buffer (2, 4800);
        buffer.clear();

        for (int i = 0; i < 4800; ++i)
            buffer.setSample (0, i, 0.25f);

        REQUIRE (writer->writeFromAudioSampleBuffer (buffer, 0, 4800));
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    for (const auto& [format, name, bits] :
         std::array<std::tuple<ExportFormat, const char*, int>, 5> { { { ExportFormat::wav16, "a.wav", 16 },
                                                                       { ExportFormat::wav24, "b.wav", 24 },
                                                                       { ExportFormat::wav32, "c.wav", 32 },
                                                                       { ExportFormat::flac16, "d.flac", 16 },
                                                                       { ExportFormat::flac24, "e.flac", 24 } } })
    {
        const auto target = folder.getChildFile (name);
        CHECK (writeExport (source, target, format, 2.0f).isEmpty());

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (target));
        REQUIRE (reader != nullptr);
        CHECK (static_cast<int> (reader->bitsPerSample) == bits);
        CHECK (reader->lengthInSamples == 4800);

        juce::AudioBuffer<float> read (2, 4800);
        REQUIRE (reader->read (&read, 0, 4800, 0, true, true));
        CHECK_THAT (read.getSample (0, 100), WithinAbs (0.5, 1.0e-3)); // gain applied
        CHECK_THAT (read.getSample (1, 100), WithinAbs (0.0, 1.0e-3));
    }

    std::ignore = folder.deleteRecursively();
}

// ---------------------------------------------------------------------------------------------------------
// MP3 (X6)

namespace
{
// A 32-bit float stereo render at `rate`, each channel a sum of sines (frequency, amplitude).
void writeRender (const juce::File& file, double rate, double seconds,
                  const std::vector<std::pair<double, float>>& left, const std::vector<std::pair<double, float>>& right)
{
    REQUIRE (file.deleteFile()); // a file's output stream appends
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (
        stream, juce::AudioFormatWriterOptions {}
                    .withSampleRate (rate)
                    .withNumChannels (2)
                    .withBitsPerSample (32)
                    .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    REQUIRE (writer != nullptr);
    const auto frames = static_cast<int> (seconds * rate);
    juce::AudioBuffer<float> buffer (2, frames);

    for (int i = 0; i < frames; ++i)
    {
        const auto t = static_cast<double> (i) / rate;
        const auto sum = [t] (const std::vector<std::pair<double, float>>& tones)
        {
            auto value = 0.0;

            for (const auto& [frequency, amplitude] : tones)
                value += amplitude * std::sin (juce::MathConstants<double>::twoPi * frequency * t);

            return static_cast<float> (value);
        };
        buffer.setSample (0, i, sum (left));
        buffer.setSample (1, i, sum (right));
    }

    REQUIRE (writer->writeFromAudioSampleBuffer (buffer, 0, frames));
}

// The amplitude of one frequency in a channel (Goertzel), over the middle half of the file.
[[nodiscard]] double toneLevel (const juce::AudioBuffer<float>& buffer, int channel, double rate, double frequency)
{
    const auto start = buffer.getNumSamples() / 4;
    const auto count = buffer.getNumSamples() / 2;
    const auto coefficient = 2.0 * std::cos (juce::MathConstants<double>::twoPi * frequency / rate);
    auto s1 = 0.0;
    auto s2 = 0.0;

    for (int i = 0; i < count; ++i)
    {
        const auto s0 = buffer.getSample (channel, start + i) + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    return 2.0 * std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - coefficient * s1 * s2)) / count;
}

struct Decoded
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> audio;
};

[[nodiscard]] Decoded decode (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    Decoded decoded { reader->sampleRate, juce::AudioBuffer<float> (2, static_cast<int> (reader->lengthInSamples)) };
    REQUIRE (reader->read (&decoded.audio, 0, decoded.audio.getNumSamples(), 0, true, true));
    return decoded;
}

[[nodiscard]] bool contains (const juce::MemoryBlock& data, const void* bytes, size_t size)
{
    const auto* begin = static_cast<const char*> (data.getData());
    const auto* end = begin + data.getSize();
    const auto* needle = static_cast<const char*> (bytes);
    return std::search (begin, end, needle, needle + size) != end;
}

[[nodiscard]] bool contains (const juce::MemoryBlock& data, const char* text)
{
    return contains (data, text, std::strlen (text));
}

struct TempFolder
{
    TempFolder() { REQUIRE (folder.createDirectory()); }
    TempFolder (const TempFolder&) = delete;
    TempFolder& operator= (const TempFolder&) = delete;
    TempFolder (TempFolder&&) = delete;
    TempFolder& operator= (TempFolder&&) = delete;
    ~TempFolder() { std::ignore = folder.deleteRecursively(); }

    juce::File folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dawg-mp3-" + juce::Uuid().toString());
};
} // namespace

TEST_CASE ("an MP3 is written at a rate it can hold", "[export][mp3]")
{
    CHECK (mp3SampleRate (44100.0) == 44100.0);
    CHECK (mp3SampleRate (48000.0) == 48000.0);
    CHECK (mp3SampleRate (32000.0) == 32000.0);
    CHECK (mp3SampleRate (88200.0) == 44100.0);
    CHECK (mp3SampleRate (176400.0) == 44100.0);
    CHECK (mp3SampleRate (96000.0) == 48000.0);
    CHECK (mp3SampleRate (192000.0) == 48000.0);
    CHECK (mp3SampleRate (64000.0) == 48000.0); // LAME converts an odd rate itself
}

TEST_CASE ("an MP3 holds the render, its tags and a LAME frame", "[export][mp3]")
{
    const TempFolder temp;
    const auto source = temp.folder.getChildFile ("render.wav");
    writeRender (source, 48000.0, 2.0, { { 1000.0, 0.25f } }, { { 3000.0, 0.25f } });

    Mp3Settings settings;
    settings.tags.title = juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f"
                                                  "e");
    settings.tags.artist = "The Testers";
    settings.tags.year = "2026";
    settings.tags.comment = "Made in a test";
    const auto cbr = temp.folder.getChildFile ("cbr.mp3");
    REQUIRE (writeExport (source, cbr, ExportFormat::mp3, 2.0f, settings).isEmpty());

    const auto decoded = decode (cbr);
    CHECK (decoded.sampleRate == 48000.0);
    // Two seconds, plus the encoder's delay and padding.
    CHECK (decoded.audio.getNumSamples() >= 96000);
    CHECK (decoded.audio.getNumSamples() < 96000 + 4800);
    // The gain applied, the channels kept apart.
    CHECK_THAT (toneLevel (decoded.audio, 0, 48000.0, 1000.0), WithinAbs (0.5, 0.02));
    CHECK_THAT (toneLevel (decoded.audio, 1, 48000.0, 3000.0), WithinAbs (0.5, 0.02));
    CHECK (toneLevel (decoded.audio, 0, 48000.0, 3000.0) < 0.01);

    // 320 kbps for two seconds is 80 000 bytes, give or take the tag and the last frame.
    CHECK (cbr.getSize() > 78000);
    CHECK (cbr.getSize() < 90000);

    juce::MemoryBlock bytes;
    REQUIRE (cbr.loadFileAsData (bytes));
    CHECK (contains (bytes, "ID3"));
    CHECK (contains (bytes, "TIT2"));
    CHECK (contains (bytes, "TPE1"));
    CHECK (contains (bytes, "TYER"));
    CHECK (contains (bytes, "COMM"));
    CHECK (contains (bytes, "TSSE"));
    const std::array<unsigned char, 4> umlaut { 0xFC, 0x00, 0xDF, 0x00 }; // "üß" in UTF-16LE
    CHECK (contains (bytes, umlaut.data(), umlaut.size()));
    CHECK (contains (bytes, "Info")); // a constant bitrate's LAME frame
    CHECK (contains (bytes, "LAME3.100"));

    const auto measured = measureFile (cbr);
    REQUIRE (measured.has_value());
    CHECK_THAT (measured->peak, WithinAbs (0.5, 0.05));

    // Variable bitrate: a Xing frame, and smaller than 320 kbps for two plain tones.
    settings.vbrQuality = 2;
    const auto vbr = temp.folder.getChildFile ("vbr.mp3");
    REQUIRE (writeExport (source, vbr, ExportFormat::mp3, 2.0f, settings).isEmpty());
    REQUIRE (vbr.loadFileAsData (bytes));
    CHECK (contains (bytes, "Xing"));
    CHECK (vbr.getSize() < cbr.getSize());

    // Without a title, the file's name is the title.
    settings = {};
    settings.bitrateKbps = 128;
    const auto untitled = temp.folder.getChildFile ("Song name.mp3");
    REQUIRE (writeExport (source, untitled, ExportFormat::mp3, 1.0f, settings).isEmpty());
    REQUIRE (untitled.loadFileAsData (bytes));
    const std::array<unsigned char, 8> song { 'S', 0, 'o', 0, 'n', 0, 'g', 0 };
    CHECK (contains (bytes, song.data(), song.size()));
    CHECK (untitled.getSize() < cbr.getSize() / 2);
}

TEST_CASE ("a render above 48 kHz is filtered and decimated for its MP3", "[export][mp3]")
{
    const TempFolder temp;

    for (const auto& [rate, expected] : std::array<std::pair<double, double>, 3> {
             { { 96000.0, 48000.0 }, { 88200.0, 44100.0 }, { 192000.0, 48000.0 } } })
    {
        const auto source = temp.folder.getChildFile ("render.wav");
        // 30 kHz can't be in the MP3: unfiltered, it would fold down to (rate / factor) − 30 kHz.
        writeRender (source, rate, 1.0, { { 1000.0, 0.25f }, { 30000.0, 0.25f } }, { { 1000.0, 0.25f } });
        const auto target = temp.folder.getChildFile ("decimated.mp3");
        REQUIRE (writeExport (source, target, ExportFormat::mp3, 1.0f, {}).isEmpty());

        const auto decoded = decode (target);
        CHECK (decoded.sampleRate == expected);
        CHECK (decoded.audio.getNumSamples() >= static_cast<int> (expected));
        CHECK (decoded.audio.getNumSamples() < static_cast<int> (expected * 1.1));
        CHECK_THAT (toneLevel (decoded.audio, 0, expected, 1000.0), WithinAbs (0.25, 0.015));
        CHECK (toneLevel (decoded.audio, 0, expected, expected - 30000.0) < 0.0025); // the alias, 40 dB down
    }
}
