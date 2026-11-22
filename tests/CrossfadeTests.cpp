// Crossfades between stacked audio clips (docs/design/audio-features.md, AU5-AU8).

#include "TestUtils.h"
#include "dawg/AudioPlayer.h"
#include "dawg/ClipOps.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr Ticks beat = ticksPerBeat;
constexpr int samplesPerBeat = 24000; // 120 BPM at 48 kHz
constexpr int fileFrames = 4 * 48000; // eight beats

[[nodiscard]] juce::var fileArgs (const char* path)
{
    return makeObject (
        { { "path", path }, { "name", path }, { "sampleRate", 48000.0 }, { "channels", 1 }, { "frames", fileFrames } });
}

[[nodiscard]] std::shared_ptr<const AudioSource> constant (float level)
{
    juce::AudioBuffer<float> buffer (1, fileFrames);

    for (int i = 0; i < fileFrames; ++i)
        buffer.setSample (0, i, level);

    return AudioSource::fromBuffer (std::move (buffer), 48000.0);
}

// An audio track; clips are added with add(), each playing a constant 0.5 (files "a" and "b").
struct Rig
{
    Rig()
    {
        project = apply ({}, "addTrack", { { "kind", "audio" } });
        trackId = project.tracks[0]->id;
        compiler.setAudioSourceResolver ([this] (const AudioFile&) { return level; });
    }

    ObjectId add (const char* path, Ticks start, Ticks length)
    {
        project =
            apply (project, "addAudioClip",
                   { { "trackId", trackId }, { "start", start }, { "length", length }, { "file", fileArgs (path) } });
        const auto& clips = project.tracks[0]->clips;
        return std::ranges::max (clips, {}, [] (const ClipPtr& c) { return c->order; })->id;
    }

    void set (ObjectId id, std::initializer_list<std::pair<const char*, juce::var>> args)
    {
        auto object = makeObject (args);
        object.getDynamicObject()->setProperty ("id", id);
        project = applyCommand (project, "setClipProps", object);
    }

    [[nodiscard]] const NoteClip& clip (ObjectId id) const
    {
        for (const auto& c : project.tracks[0]->clips)
            if (c->id == id)
                return *c;

        throw std::runtime_error ("no clip");
    }

    [[nodiscard]] std::vector<float> play (int beats)
    {
        AudioEngine engine;
        engine.setEngine (compiler.compile (project, spec));
        engine.play();
        return render (engine, beats * samplesPerBeat).left();
    }

    std::shared_ptr<const AudioSource> level = constant (0.5f);
    Project project;
    ObjectId trackId = 0;
    GraphCompiler compiler;
};

// The largest deviation from an expected level over samples [from, to).
template <typename Expected>
[[nodiscard]] double deviation (const std::vector<float>& output, int from, int to, Expected&& expected)
{
    double worst = 0.0;

    for (int s = from; s < to; ++s)
        worst = std::max (worst, std::abs (static_cast<double> (output[static_cast<size_t> (s)]) - expected (s)));

    return worst;
}
} // namespace

TEST_CASE ("a clip's fade-in over a clip below crossfades with it", "[crossfade][engine]")
{
    Rig rig;
    rig.project = apply (rig.project, "setAutoCrossfade", { { "ms", 0 } });
    rig.add ("a", 0, 8 * beat);
    const auto top = rig.add ("b", 4 * beat, 4 * beat);
    const auto shape = GENERATE (0, 3); // linear, S-curve: constant gain
    rig.set (top, { { "fadeIn", beat }, { "fadeInShape", shape } });

    const auto output = rig.play (8);
    // From past the first clip's start declick to before the second's end declick: always 0.5.
    CHECK (deviation (output, 200, 8 * samplesPerBeat - 200, [] (int) { return 0.5; }) < 1.0e-4);

    SECTION ("equal power keeps the power, not the sum")
    {
        rig.set (top, { { "fadeInShape", 1 } });
        const auto power = rig.play (8);
        const auto expected = [] (int s)
        {
            const auto x = std::clamp ((s - 4.0 * samplesPerBeat) / samplesPerBeat, 0.0, 1.0);
            return 0.5
                   * (std::sin (x * juce::MathConstants<double>::halfPi)
                      + std::cos (x * juce::MathConstants<double>::halfPi));
        };
        CHECK (deviation (power, 200, 8 * samplesPerBeat - 200, expected) < 1.0e-4);
    }
}

TEST_CASE ("a clip's fade-out over a clip below brings it back under the fade", "[crossfade][engine]")
{
    Rig rig;
    rig.project = apply (rig.project, "setAutoCrossfade", { { "ms", 0 } });
    rig.add ("a", 0, 8 * beat);
    const auto top = rig.add ("b", 2 * beat, 2 * beat);
    rig.set (top, { { "fadeIn", beat / 2 }, { "fadeOut", beat / 2 }, { "fadeInShape", 3 }, { "fadeOutShape", 3 } });

    const auto output = rig.play (8);
    CHECK (deviation (output, 200, 8 * samplesPerBeat - 200, [] (int) { return 0.5; }) < 1.0e-4);
}

TEST_CASE ("a covered clip shorter than the fade stops at its own end", "[crossfade][engine]")
{
    Rig rig;
    rig.project = apply (rig.project, "setAutoCrossfade", { { "ms", 0 } });
    rig.add ("a", 0, 4 * beat + beat / 4);
    const auto top = rig.add ("b", 4 * beat, 4 * beat);
    rig.set (top, { { "fadeIn", beat } });

    const auto output = rig.play (8);
    // Past the covered clip's end only the fade-in sounds.
    const auto at = [&] (double beats)
    { return static_cast<double> (output[static_cast<size_t> (beats * samplesPerBeat)]); };
    CHECK_THAT (at (4.6), WithinAbs (0.3, 1.0e-3));
    CHECK_THAT (at (4.9), WithinAbs (0.45, 1.0e-3));
    // Before it, the covered clip fades out over what is left of it.
    CHECK_THAT (at (4.125), WithinAbs (0.5 * 0.125 + 0.5 * 0.5, 1.0e-3));
}

TEST_CASE ("musical clips crossfade the same way", "[crossfade][engine][stretch]")
{
    Rig rig;
    rig.project = apply (rig.project, "setAutoCrossfade", { { "ms", 0 } });
    rig.add ("a", 0, 8 * beat);
    const auto top = rig.add ("b", 4 * beat, 4 * beat);

    std::vector<ObjectId> fileIds;
    for (const auto& file : rig.project.audioFiles)
        fileIds.push_back (file.id);

    for (const auto fileId : fileIds)
        rig.project = apply (rig.project, "setFileTiming", { { "fileId", fileId }, { "mode", "musical" } });

    rig.set (top, { { "fadeIn", beat } });
    const auto output = rig.play (8);
    CHECK (deviation (output, 200, 8 * samplesPerBeat - 200, [] (int) { return 0.5; }) < 1.0e-3);
}

TEST_CASE ("crossfadedParts leaves clips alone where nothing covers them", "[crossfade]")
{
    Rig rig;
    rig.add ("a", 0, 2 * beat);
    rig.add ("b", 4 * beat, 2 * beat);
    const auto& clips = rig.project.tracks[0]->clips;
    CHECK (crossfadedParts (clips, rig.project) == clips);
}

TEST_CASE ("clips placed over audio get automatic crossfades", "[crossfade][commands]")
{
    Rig rig;
    const auto lower = rig.add ("a", 0, 8 * beat);
    const auto top = rig.add ("b", 4 * beat, 8 * beat);
    // 10 ms at 120 BPM.
    const auto tenMs = static_cast<Ticks> (std::llround (0.01 * 2.0 * beat));

    CHECK (rig.clip (top).fadeIn == tenMs);
    CHECK (rig.clip (top).fadeOut == 0); // its end is over nothing
    CHECK (rig.clip (lower).fadeIn == 0);
    CHECK (rig.clip (lower).fadeOut == 0);

    SECTION ("a removed crossfade stays removed until the clip is placed again")
    {
        rig.set (top, { { "fadeIn", 0 } });
        CHECK (rig.clip (top).fadeIn == 0);

        rig.project =
            apply (rig.project, "moveClips", { { "ids", juce::Array<juce::var> { top } }, { "delta", beat } });
        CHECK (rig.clip (top).fadeIn == tenMs);
    }

    SECTION ("a clip in empty space gets none, nor does one with the setting off")
    {
        const auto alone = rig.add ("a", 20 * beat, 2 * beat);
        CHECK (rig.clip (alone).fadeIn == 0);

        rig.project = apply (rig.project, "setAutoCrossfade", { { "ms", 0 } });
        const auto over = rig.add ("a", 2 * beat, 1 * beat);
        CHECK (rig.clip (over).fadeIn == 0);
        CHECK (rig.clip (over).fadeOut == 0);
    }

    SECTION ("both edges of a clip inside another")
    {
        const auto inside = rig.add ("a", 1 * beat, 1 * beat);
        CHECK (rig.clip (inside).fadeIn == tenMs);
        CHECK (rig.clip (inside).fadeOut == tenMs);
    }

    SECTION ("crossfadeClips sets the covering edges' length and shape")
    {
        rig.project = apply (rig.project, "crossfadeClips",
                             { { "clipIds", juce::Array<juce::var> { top, lower } }, { "ms", 50 }, { "shape", 3 } });
        CHECK (rig.clip (top).fadeIn == static_cast<Ticks> (std::llround (0.05 * 2.0 * beat)));
        CHECK (rig.clip (top).fadeInShape == FadeShape::sCurve);
        CHECK (rig.clip (top).fadeOutShape == FadeShape::linear);
        CHECK (rig.clip (lower).fadeOut == 0); // the lower clip's edges cover nothing
    }
}

TEST_CASE ("the S-curve fade", "[crossfade]")
{
    CHECK (fadeGain (FadeShape::sCurve, 0.0) == 0.0f);
    CHECK_THAT (fadeGain (FadeShape::sCurve, 0.5), WithinAbs (0.5, 1e-6));
    CHECK_THAT (fadeGain (FadeShape::sCurve, 0.25) + fadeGain (FadeShape::sCurve, 0.75), WithinAbs (1.0, 1e-6));
}
