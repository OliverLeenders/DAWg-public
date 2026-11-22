// Audio to MIDI (docs/design/audio-features.md, AU4).

#include "TestUtils.h"
#include "dawg/AudioToMidi.h"
#include "dawg/ClipOps.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <random>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double rate = 48000.0;

[[nodiscard]] double hzOf (double pitch)
{
    return 440.0 * std::pow (2.0, (pitch - 69.0) / 12.0);
}

// Notes as a sine sequence: (pitch, seconds) each, with 20 ms of silence between them; pitch 0 is silence.
[[nodiscard]] juce::AudioBuffer<float> melody (std::initializer_list<std::pair<double, double>> notes,
                                               double vibratoCents = 0.0)
{
    auto total = 0.0;
    for (const auto& [pitch, seconds] : notes)
        total += seconds + 0.02;

    juce::AudioBuffer<float> audio (1, static_cast<int> (total * rate) + 1);
    audio.clear();
    auto at = 0;
    auto phase = 0.0;

    for (const auto& [pitch, seconds] : notes)
    {
        const auto count = static_cast<int> (seconds * rate);

        for (int i = 0; i < count && pitch > 0.0; ++i)
        {
            const auto t = i / rate;
            const auto cents = vibratoCents * std::sin (juce::MathConstants<double>::twoPi * 5.0 * t);
            phase += juce::MathConstants<double>::twoPi * hzOf (pitch + cents / 100.0) / rate;
            // 5 ms ramps at both ends, as a played note has.
            const auto ramp = std::min ({ 1.0, i / (0.005 * rate), (count - i) / (0.005 * rate) });
            audio.setSample (0, at + i, static_cast<float> (0.5 * ramp * std::sin (phase)));
        }

        at += count + static_cast<int> (0.02 * rate);
    }

    return audio;
}

// Drum-like hits at the given times: a low sine thump, a 1 kHz tone with a little noise, and white noise.
[[nodiscard]] juce::AudioBuffer<float> hits (std::initializer_list<std::pair<double, int>> list, double seconds)
{
    juce::AudioBuffer<float> audio (1, static_cast<int> (seconds * rate));
    audio.clear();
    std::mt19937 random (7);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);

    for (const auto& [time, kind] : list)
    {
        const auto start = static_cast<int> (time * rate);

        for (int i = 0; i < static_cast<int> (0.1 * rate) && start + i < audio.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            const auto envelope = std::exp (-t * 40.0);
            auto value = static_cast<double> (noise (random)); // a hat: noise

            if (kind == 0)
                value = std::sin (juce::MathConstants<double>::twoPi * 60.0 * t);
            else if (kind == 1)
                value = std::sin (juce::MathConstants<double>::twoPi * 1000.0 * t) + 0.05 * noise (random);
            audio.setSample (0, start + i, static_cast<float> (0.8 * envelope * value));
        }
    }

    return audio;
}
} // namespace

TEST_CASE ("pitch detection follows a sine melody", "[tomidi]")
{
    const auto audio = melody ({ { 60.0, 0.3 }, { 64.0, 0.3 }, { 67.0, 0.3 } });
    const auto frames = detectPitch (audio, rate);
    REQUIRE_FALSE (frames.empty());

    // In the middle of each note, the pitch is right within a few cents.
    const auto pitchAt = [&] (double seconds)
    {
        const auto frame =
            std::ranges::min_element (frames, {}, [&] (const PitchFrame& f) { return std::abs (f.seconds - seconds); });
        return frame->pitch;
    };
    CHECK_THAT (pitchAt (0.15), WithinAbs (60.0, 0.05));
    CHECK_THAT (pitchAt (0.47), WithinAbs (64.0, 0.05));
    CHECK_THAT (pitchAt (0.79), WithinAbs (67.0, 0.05));

    const auto notes = pitchNotes (frames);
    REQUIRE (notes.size() == 3);
    const std::array starts { 0.0, 0.32, 0.64 };
    const std::array keys { 60.0, 64.0, 67.0 };

    for (size_t i = 0; i < 3; ++i)
    {
        CHECK (notes[i].pitch == keys[i]);
        CHECK_THAT (notes[i].start, WithinAbs (starts[i], 0.015));
        CHECK_THAT (notes[i].end, WithinAbs (starts[i] + 0.3, 0.02));
        CHECK_THAT (notes[i].velocity, WithinAbs (velocityOfPeak (20.0 * std::log10 (0.5 / std::sqrt (2.0))), 0.05));
    }
}

TEST_CASE ("vibrato stays one note with a pitch curve of its depth", "[tomidi]")
{
    const auto vibratoFrames = detectPitch (melody ({ { 62.0, 1.0 } }, 50.0), rate);
    const auto notes = pitchNotes (vibratoFrames);
    REQUIRE (notes.size() == 1);
    CHECK (notes[0].pitch == 62.0);

    auto lowest = 0.0;
    auto highest = 0.0;
    for (const auto& [time, semitones] : notes[0].bend)
        if (time > 0.05 && time < 0.95)
        {
            lowest = std::min (lowest, semitones);
            highest = std::max (highest, semitones);
        }

    CHECK_THAT (highest, WithinAbs (0.5, 0.08));
    CHECK_THAT (lowest, WithinAbs (-0.5, 0.08));
    // Thinned: far fewer points than frames.
    CHECK (notes[0].bend.size() < 150);
}

TEST_CASE ("a jump of a tone without a gap starts a new note", "[tomidi]")
{
    // Two pitches joined without silence: one 0.4 s sine sweeping straight from C4 to D4.
    juce::AudioBuffer<float> audio (1, static_cast<int> (0.8 * rate));
    auto phase = 0.0;

    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto pitch = i < audio.getNumSamples() / 2 ? 60.0 : 62.0;
        phase += juce::MathConstants<double>::twoPi * hzOf (pitch) / rate;
        audio.setSample (0, i, static_cast<float> (0.5 * std::sin (phase)));
    }

    const auto notes = pitchNotes (detectPitch (audio, rate));
    REQUIRE (notes.size() == 2);
    CHECK (notes[0].pitch == 60.0);
    CHECK (notes[1].pitch == 62.0);
    CHECK_THAT (notes[1].start, WithinAbs (0.4, 0.02));
}

TEST_CASE ("drum hits become notes, split by their sound when asked", "[tomidi]")
{
    const auto audio = hits ({ { 0.1, 0 }, { 0.5, 1 }, { 0.9, 2 }, { 1.3, 0 } }, 1.6);

    const auto plain = drumNotes (audio, rate, 1.0, false);
    REQUIRE (plain.size() == 4);
    for (const auto& note : plain)
        CHECK (note.pitch == 36.0);

    const std::array times { 0.1, 0.5, 0.9, 1.3 };
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK_THAT (plain[i].start, WithinAbs (times[i], 0.002));
        CHECK_THAT (plain[i].end - plain[i].start, WithinAbs (0.125, 1e-9));
    }

    const auto split = drumNotes (audio, rate, 1.0, true);
    REQUIRE (split.size() == 4);
    CHECK (split[0].pitch == 36.0);
    CHECK (split[1].pitch == 38.0);
    CHECK (split[2].pitch == 42.0);
    CHECK (split[3].pitch == 36.0);
}

TEST_CASE ("velocity follows the peak level", "[tomidi]")
{
    CHECK (velocityOfPeak (0.0) == 1.0);
    CHECK (velocityOfPeak (-40.0) == 1.0 / 127.0);
    CHECK (velocityOfPeak (-80.0) == 1.0 / 127.0);
    CHECK_THAT (velocityOfPeak (-20.0), WithinAbs (64.0 / 127.0, 1e-9));
}

TEST_CASE ("found notes land where the clip plays them, on a new track", "[tomidi][commands]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addAudioClip",
                     { { "trackId", trackId },
                       { "start", 4 * ticksPerBeat },
                       { "offset", ticksPerBeat },
                       { "file", makeObject ({ { "path", "Audio/a.wav" },
                                               { "name", "a" },
                                               { "sampleRate", rate },
                                               { "channels", 1 },
                                               { "frames", 192000 } }) } });
    const auto& clip = *project.tracks[0]->clips[0];
    const ClipFrames frames (clip, *project.findAudioFile (clip.fileId), project.tempoMap);

    DetectedNote note;
    note.start = 0.25; // half a beat after the clip's start, at 120 BPM
    note.end = 0.75;
    note.pitch = 64.0;
    note.velocity = 0.5;
    note.bend = { { 0.0, 0.0 }, { 0.25, 0.3 } };
    const auto notes = notesVar ({ note }, frames, clip.start, frames.range().first, rate, true);
    REQUIRE (notes.size() == 1);
    CHECK (static_cast<juce::int64> (notes[0]["start"]) == ticksPerBeat / 2);
    CHECK (static_cast<juce::int64> (notes[0]["length"]) == ticksPerBeat);
    CHECK (static_cast<juce::int64> (notes[0]["expression"]["pitch"][1][0]) == ticksPerBeat / 2);

    const auto result = apply (project, "addNoteTrack",
                               { { "after", trackId },
                                 { "name", "a notes" },
                                 { "start", clip.start },
                                 { "length", clip.length },
                                 { "notes", notes } });
    REQUIRE (result.tracks.size() == 2);
    const auto& added = *result.tracks[1];
    CHECK (added.kind == TrackKind::instrument);
    CHECK (added.name == "a notes");
    REQUIRE (added.clips.size() == 1);
    CHECK (added.clips[0]->start == clip.start);
    REQUIRE (added.clips[0]->notes.size() == 1);
    CHECK (added.clips[0]->notes[0]->pitch == 64.0);
}
