#include "TestUtils.h"
#include "dawg/Sequence.h"
#include "dawg/Vibrato.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
// 120 BPM: one beat is 0.5 s.
constexpr double secondsPerTick = 0.5 / static_cast<double> (ticksPerBeat);

[[nodiscard]] Vibrato vibrato (double depth, VibratoShape shape = VibratoShape::sine, double rateHz = 2.0)
{
    Vibrato v;
    v.depth = depth;
    v.shape = shape;
    v.rateHz = rateHz;
    return v;
}

[[nodiscard]] double ticksAt (double seconds)
{
    return seconds / secondsPerTick;
}

[[nodiscard]] juce::var array (std::initializer_list<juce::var> items)
{
    return juce::Array<juce::var> (items);
}
} // namespace

TEST_CASE ("vibrato is silent at zero intensity, the default", "[vibrato]")
{
    CHECK (laneInfos()[static_cast<size_t> (Lane::vibrato)].defaultValue <= 0.0);
    CHECK_THAT (vibratoOffset ({}, ticksAt (0.125), secondsPerTick, 0.0), WithinAbs (0.0, 1.0e-12));
    CHECK_THAT (Vibrato {}.depth, WithinAbs (1.0, 1.0e-12)); // one semitone at full intensity
}

TEST_CASE ("sine vibrato peaks a quarter cycle in", "[vibrato]")
{
    const auto v = vibrato (1.0); // 2 Hz: a quarter cycle is 0.125 s
    CHECK_THAT (vibratoOffset (v, ticksAt (0.125), secondsPerTick, 1.0), WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (vibratoOffset (v, ticksAt (0.375), secondsPerTick, 1.0), WithinAbs (-1.0, 1.0e-9));
    CHECK_THAT (vibratoOffset (v, ticksAt (0.125), secondsPerTick, 0.5), WithinAbs (0.5, 1.0e-9)); // depth lane
}

TEST_CASE ("vibrato shapes start at zero and swing both ways", "[vibrato]")
{
    for (const auto shape : { VibratoShape::triangle, VibratoShape::sawUp, VibratoShape::sawDown })
        CHECK_THAT (vibratoOffset (vibrato (1.0, shape), 0.0, secondsPerTick, 1.0), WithinAbs (0.0, 1.0e-9));

    CHECK_THAT (vibratoOffset (vibrato (1.0, VibratoShape::triangle), ticksAt (0.125), secondsPerTick, 1.0),
                WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (vibratoOffset (vibrato (1.0, VibratoShape::square), ticksAt (0.3), secondsPerTick, 1.0),
                WithinAbs (-1.0, 1.0e-9));
}

TEST_CASE ("vibrato waits for its delay and fades in", "[vibrato]")
{
    auto v = vibrato (1.0, VibratoShape::square);
    v.delay = ticksPerBeat;  // 0.5 s
    v.fadeIn = ticksPerBeat; // full depth after another 0.5 s

    CHECK_THAT (vibratoOffset (v, ticksAt (0.4), secondsPerTick, 1.0), WithinAbs (0.0, 1.0e-9));
    CHECK_THAT (vibratoOffset (v, ticksAt (0.5 + 0.1), secondsPerTick, 1.0), WithinAbs (0.2, 1.0e-9));
    CHECK_THAT (vibratoOffset (v, ticksAt (1.1), secondsPerTick, 1.0), WithinAbs (1.0, 1.0e-9));
}

TEST_CASE ("tempo-synced vibrato follows beats, not seconds", "[vibrato]")
{
    auto v = vibrato (1.0, VibratoShape::square);
    v.syncBeats = 0.5; // a cycle every eighth note

    CHECK_THAT (vibratoOffset (v, 0.2 * ticksPerBeat, 1.0 /* ignored */, 1.0), WithinAbs (1.0, 1.0e-9));
    CHECK_THAT (vibratoOffset (v, 0.3 * ticksPerBeat, 1.0, 1.0), WithinAbs (-1.0, 1.0e-9));
}

TEST_CASE ("vibrato is set through note commands and merges partial updates", "[vibrato][commands]")
{
    auto [project, trackId, clipId] = projectWithClip();
    auto note = noteArgs (0, ticksPerBeat, 60);
    note.getDynamicObject()->setProperty ("vibrato",
                                          makeObject ({ { "depth", 0.5 }, { "rateHz", 6.0 }, { "shape", 1 } }));
    project = apply (project, "addNotes", { { "clipId", clipId }, { "notes", array ({ note }) } });

    const auto id = project.tracks[0]->clips[0]->notes[0]->id;
    project = apply (
        project, "updateNotes",
        { { "clipId", clipId },
          { "notes", array ({ makeObject ({ { "id", id }, { "vibrato", makeObject ({ { "depth", 9.0 } }) } }) }) } });

    const auto& v = project.tracks[0]->clips[0]->notes[0]->vibrato;
    CHECK_THAT (v.depth, WithinAbs (limits::maxVibratoDepth, 1.0e-9)); // clamped
    CHECK_THAT (v.rateHz, WithinAbs (6.0, 1.0e-9));                    // kept from before
    CHECK (v.shape == VibratoShape::triangle);

    auto bad = noteArgs (0, ticksPerBeat, 60);
    bad.getDynamicObject()->setProperty ("vibrato", makeObject ({ { "shape", 42 } }));
    CHECK_THROWS_AS (apply (project, "addNotes", { { "clipId", clipId }, { "notes", array ({ bad }) } }), CommandError);
}

TEST_CASE ("the synth plays vibrato", "[vibrato][engine]")
{
    // Square vibrato of one semitone at 5 Hz: +1 st for 0.1 s, then -1 st for 0.1 s.
    auto setup = projectWithClip();
    auto note = noteArgs (0, 4 * ticksPerBeat, 69);
    note.getDynamicObject()->setProperty ("vibrato",
                                          makeObject ({ { "depth", 1.0 }, { "rateHz", 5.0 }, { "shape", 2 } }));
    note.getDynamicObject()->setProperty ("expression", makeObject ({ { "vibrato", array ({ array ({ 0, 1.0 }) }) } }));
    const auto project =
        apply (setup.project, "addNotes", { { "clipId", setup.clipId }, { "notes", array ({ note }) } });

    GraphCompiler compiler;
    AudioEngine engine;
    engine.setEngine (compiler.compile (project, spec));
    engine.play();

    const auto buffer = render (engine, static_cast<int> (0.2 * sampleRate));
    const auto frequency = [&] (double from, double to)
    {
        const auto a = static_cast<size_t> (from * sampleRate);
        const auto b = static_cast<size_t> (to * sampleRate);
        return zeroCrossings (buffer.left(), a, b) / (2.0 * (to - from));
    };

    CHECK_THAT (frequency (0.02, 0.09), WithinAbs (440.0 * std::exp2 (1.0 / 12.0), 12.0));  // ~466 Hz
    CHECK_THAT (frequency (0.12, 0.19), WithinAbs (440.0 * std::exp2 (-1.0 / 12.0), 12.0)); // ~415 Hz
}
