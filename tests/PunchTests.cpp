// Punch recording and loop takes (docs/design/audio-features.md, AU10-AU13).

#include "TestUtils.h"
#include "dawg/ProjectFile.h"
#include "dawg/Recording.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr int samplesPerBeat = 24000; // 120 BPM at 48 kHz

[[nodiscard]] PunchRange punch (Ticks start, Ticks end)
{
    return { true, start, end };
}

[[nodiscard]] LoopRange loop (Ticks start, Ticks end)
{
    return { true, start, end };
}
} // namespace

TEST_CASE ("a recording without loop or punch is one clip of the whole file", "[record][punch]")
{
    CHECK (recordedTakes (8 * beat, 4 * beat, 0, std::nullopt, std::nullopt)
           == std::vector<TakeSpan> { { 8 * beat, 0, 0 } });
    // Shifted earlier by the latency.
    CHECK (recordedTakes (8 * beat, 4 * beat, 100, std::nullopt, std::nullopt)
           == std::vector<TakeSpan> { { 8 * beat - 100, 0, 0 } });
    // Not before the song's start: the file's first ticks are cut instead.
    CHECK (recordedTakes (50, 4 * beat, 100, std::nullopt, std::nullopt) == std::vector<TakeSpan> { { 0, 50, 0 } });
    CHECK (recordedTakes (0, 0, 0, std::nullopt, std::nullopt).empty());
}

TEST_CASE ("loop recording makes one clip per full pass", "[record][punch]")
{
    // From the loop start: two full passes and a partial one.
    CHECK (recordedTakes (4 * beat, 9 * beat, 10, loop (4 * beat, 8 * beat), std::nullopt)
           == std::vector<TakeSpan> { { 4 * beat, 10, 4 * beat }, { 4 * beat, 4 * beat + 10, 4 * beat } });
    // From inside the loop: the first partial pass is skipped.
    CHECK (recordedTakes (6 * beat, 7 * beat, 0, loop (4 * beat, 8 * beat), std::nullopt)
           == std::vector<TakeSpan> { { 4 * beat, 2 * beat, 4 * beat } });
    // No full pass: the whole file.
    CHECK (recordedTakes (6 * beat, 3 * beat, 0, loop (4 * beat, 8 * beat), std::nullopt)
           == std::vector<TakeSpan> { { 6 * beat, 0, 0 } });
}

TEST_CASE ("a punch keeps the punch range of the recording", "[record][punch]")
{
    // Recorded from a bar before the punch-in to past the punch-out: the file holds the pre-roll as a handle.
    CHECK (recordedTakes (4 * beat, 12 * beat, 0, std::nullopt, punch (8 * beat, 12 * beat))
           == std::vector<TakeSpan> { { 8 * beat, 4 * beat, 4 * beat } });
    // With latency, the content is taken that much later in the file.
    CHECK (recordedTakes (4 * beat, 12 * beat, 30, std::nullopt, punch (8 * beat, 12 * beat))
           == std::vector<TakeSpan> { { 8 * beat, 4 * beat + 30, 4 * beat } });
    // Stopped before the punch-out: cut where the recording ends.
    CHECK (recordedTakes (4 * beat, 6 * beat, 0, std::nullopt, punch (8 * beat, 12 * beat))
           == std::vector<TakeSpan> { { 8 * beat, 4 * beat, 2 * beat } });
    // Stopped before the punch-in: nothing.
    CHECK (recordedTakes (4 * beat, 3 * beat, 0, std::nullopt, punch (8 * beat, 12 * beat)).empty());
    // Started after the punch-in: from where the recording starts.
    CHECK (recordedTakes (9 * beat, 6 * beat, 0, std::nullopt, punch (8 * beat, 12 * beat))
           == std::vector<TakeSpan> { { 9 * beat, 0, 3 * beat } });
}

TEST_CASE ("a punch inside a loop makes one take per pass over the punch range", "[record][punch]")
{
    const auto range = punch (5 * beat, 6 * beat);
    const auto loopRange = loop (4 * beat, 8 * beat);

    // Pre-roll from a bar before the loop: straight into the first pass, then every four beats.
    CHECK (recordedTakes (0, 16 * beat, 0, loopRange, range)
           == std::vector<TakeSpan> {
               { 5 * beat, 5 * beat, beat }, { 5 * beat, 9 * beat, beat }, { 5 * beat, 13 * beat, beat } });
    // Started after the punch-in: the first take comes after the wrap.
    CHECK (recordedTakes (7 * beat, 7 * beat, 0, loopRange, range)
           == std::vector<TakeSpan> { { 5 * beat, 2 * beat, beat }, { 5 * beat, 6 * beat, beat } });
    // A punch partly outside the loop is clipped to it; one wholly outside makes nothing.
    CHECK (punchInLoop (punch (2 * beat, 6 * beat), loopRange)->start == 4 * beat);
    CHECK_FALSE (punchInLoop (punch (9 * beat, 10 * beat), loopRange).has_value());
    CHECK (recordedTakes (0, 16 * beat, 0, loopRange, punch (9 * beat, 10 * beat)).empty());
}

TEST_CASE ("the punch range is a command and round-trips", "[record][punch][file]")
{
    auto project = apply ({}, "setPunch", { { "enabled", true }, { "start", 2 * beat }, { "end", 6 * beat } });
    CHECK (project.punch.enabled);
    CHECK (project.punch.start == 2 * beat);
    CHECK (project.punch.end == 6 * beat);
    CHECK_THROWS_AS (apply (project, "setPunch", { { "end", 2 * beat } }), CommandError);

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-punch-" + juce::Uuid().toString() + ".dawg");
    REQUIRE (projectfile::write (project, { "id", "v" }, file, false).isEmpty());
    const auto loaded = projectfile::read (file).project;
    std::ignore = file.deleteFile();
    CHECK (loaded.punch.enabled);
    CHECK (loaded.punch.start == 2 * beat);
    CHECK (loaded.punch.end == 6 * beat);
}

TEST_CASE ("recording stops writing at the record window's end", "[record][punch][audio]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("dawg-punch-" + juce::Uuid().toString() + ".wav");
    juce::TimeSliceThread diskThread ("test recorder");
    diskThread.startThread();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (
        stream,
        juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), diskThread, 1 << 16);

    const RecordTarget target { project.tracks[0]->id, 0, false, threaded.get() };
    engine.startRecording ({ &target, 1 }, 0.25, 1.5); // from a quarter beat to one and a half
    engine.play();

    std::vector<float> input (static_cast<size_t> (2 * samplesPerBeat), 0.5f);
    const std::array<const float*, 1> inputs { input.data() };
    StereoBuffer out (2 * samplesPerBeat);
    engine.renderOffline (out.channels(), 2, 2 * samplesPerBeat, sampleRate, inputs.data(), 1);
    engine.stopRecording();

    CHECK_THAT (engine.recordStartBeats(), WithinAbs (0.25, 1e-3));
    CHECK (engine.recordedFrames (0) == samplesPerBeat + samplesPerBeat / 4);

    threaded.reset();
    diskThread.stopThread (1000);
    std::ignore = file.deleteFile();
}
