#include "TestUtils.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <complex>
#include <optional>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

namespace
{
// An oscillator with some parameters set, rendered in one block, optionally with a constant modulated pitch.
struct Oscillator
{
    explicit Oscillator (std::initializer_list<std::pair<const char*, float>> settings)
        : node (NodeRegistry::builtin().create (nodetype::oscillator))
    {
        REQUIRE (node != nullptr);
        node->prepare ({ sampleRate, 48000 });

        for (const auto& [id, value] : settings)
        {
            const auto index = node->descriptor().indexOfParam (id);
            REQUIRE (index >= 0);
            node->params().set (static_cast<size_t> (index), value);
        }
    }

    [[nodiscard]] StereoBuffer render (int numSamples, std::optional<float> modulatedPitch = std::nullopt) const
    {
        StereoBuffer out (numSamples);
        std::array<AudioOutput, 1> outputs { AudioOutput { out.channels()[0], out.channels()[1] } };
        std::vector<float> pitch (static_cast<size_t> (numSamples), modulatedPitch.value_or (0.0f));
        const std::array modulated { ModulatedParam { 1, pitch.data() } };

        ProcessContext context;
        context.numSamples = numSamples;
        context.audioOutputs = outputs;

        if (modulatedPitch.has_value())
            context.modulatedParams = modulated;

        node->process (context);
        return out;
    }

    std::unique_ptr<Node> node;
};

// The frequency from the zero crossings of one second.
[[nodiscard]] double frequencyOf (const std::vector<float>& samples)
{
    return static_cast<double> (zeroCrossings (samples, 0, samples.size())) / 2.0 * sampleRate
           / static_cast<double> (samples.size());
}

// The energy of a periodic signal outside its harmonics, against the energy on them, in dB. The window is
// `length` samples with the fundamental on bin `bin` (Hann-windowed; a harmonic's two neighbours count as harmonic).
[[nodiscard]] double aliasingDb (const std::vector<float>& samples, size_t length, size_t bin)
{
    constexpr double pi = juce::MathConstants<double>::pi;
    double harmonic = 0.0;
    double alias = 0.0;

    for (size_t k = 0; k <= length / 2; ++k)
    {
        std::complex<double> sum;

        for (size_t n = 0; n < length; ++n)
        {
            const auto window =
                0.5 - 0.5 * std::cos (2.0 * pi * static_cast<double> (n) / static_cast<double> (length));
            // std::polar takes no negative magnitude (libc++ answers NaN): the sample scales a unit phasor instead.
            sum += static_cast<double> (samples[n]) * window
                   * std::polar (1.0, -2.0 * pi * static_cast<double> (k * n) / static_cast<double> (length));
        }

        const auto nearest = std::min (k % bin, bin - k % bin);
        (nearest <= 2 ? harmonic : alias) += std::norm (sum);
    }

    return 10.0 * std::log10 (alias / harmonic);
}

[[nodiscard]] double rms (const std::vector<float>& samples)
{
    double sum = 0.0;

    for (const auto value : samples)
        sum += static_cast<double> (value) * static_cast<double> (value);

    return std::sqrt (sum / static_cast<double> (samples.size()));
}

// The pitch, relative to C4, of a frequency.
[[nodiscard]] float pitchOf (double frequency)
{
    return static_cast<float> (69.0 + 12.0 * std::log2 (frequency / 440.0) - 60.0);
}
} // namespace

TEST_CASE ("the oscillator plays its pitch, tuning and a modulated pitch", "[oscillator][dsp]")
{
    constexpr int second = static_cast<int> (sampleRate);
    const auto middleC = 440.0 * std::exp2 (-9.0 / 12.0);

    Oscillator plain ({ { "waveform", 0.0f } });
    CHECK_THAT (frequencyOf (plain.render (second).left()), WithinAbs (middleC, 1.0));

    Oscillator tuned ({ { "waveform", 0.0f }, { "coarse", 12.0f }, { "fine", 50.0f } });
    CHECK_THAT (frequencyOf (tuned.render (second).left()), WithinAbs (middleC * 2.0 * std::exp2 (0.5 / 12.0), 1.0));

    // The modulated pitch replaces the parameter: 9 semitones above C4 is A4.
    Oscillator modulated ({ { "waveform", 0.0f }, { "pitch", -24.0f } });
    CHECK_THAT (frequencyOf (modulated.render (second, 9.0f).left()), WithinAbs (440.0, 1.0));
}

TEST_CASE ("the oscillator's waveforms are band-limited", "[oscillator][dsp]")
{
    // The fundamental on a whole bin of a 4800-sample window, so the harmonics fall on whole bins and their aliases
    // between them: 430 Hz and 1730 Hz. The limits measure all alias energy against all harmonic energy. A naive saw
    // has -13 dB at 1730 Hz; polyBLEP measured -35 dB (saw) to -73 dB (triangle) at 430 Hz and -28 dB to -55 dB at
    // 1730 Hz.
    constexpr size_t length = 4800;
    const std::vector<std::pair<const char*, std::vector<std::pair<const char*, float>>>> cases {
        { "triangle", { { "waveform", 1.0f } } },
        { "saw", { { "waveform", 2.0f } } },
        { "square", { { "waveform", 3.0f } } },
        { "pulse 25 %", { { "waveform", 3.0f }, { "pulseWidth", 25.0f } } },
    };

    for (const auto& [bin, limit] : { std::pair<size_t, double> { 43, -32.0 }, { 173, -25.0 } })
    {
        const auto pitch = pitchOf (sampleRate * static_cast<double> (bin) / static_cast<double> (length));

        for (const auto& [name, settings] : cases)
        {
            Oscillator oscillator ({});

            for (const auto& [id, value] : settings)
                oscillator.node->params().set (static_cast<size_t> (oscillator.node->descriptor().indexOfParam (id)),
                                               value);

            const auto out = oscillator.render (static_cast<int> (length), pitch);
            const auto db = aliasingDb (out.left(), length, bin);
            INFO (name << " at bin " << bin << ": aliasing " << db << " dB");
            CHECK (db < limit);
        }
    }
}

TEST_CASE ("unison copies are detuned and spread across the stereo field", "[oscillator][dsp]")
{
    constexpr int second = static_cast<int> (sampleRate);
    const auto middleC = 440.0 * std::exp2 (-9.0 / 12.0);

    // Two copies, 100 cents apart, panned hard left and right: each side plays one.
    Oscillator wide ({ { "waveform", 0.0f }, { "unison", 2.0f }, { "detune", 100.0f }, { "spread", 100.0f } });
    const auto out = wide.render (second);
    CHECK_THAT (frequencyOf (out.left()), WithinAbs (middleC * std::exp2 (-0.5 / 12.0), 1.0));
    CHECK_THAT (frequencyOf (out.right()), WithinAbs (middleC * std::exp2 (0.5 / 12.0), 1.0));

    // Without spread both sides are the same mix, at the level of one copy (equal power).
    Oscillator centred ({ { "waveform", 0.0f }, { "unison", 4.0f }, { "spread", 0.0f }, { "level", 0.0f } });
    const auto mono = centred.render (second);
    CHECK (mono.left() == mono.right());
    CHECK_THAT (rms (mono.left()), WithinAbs (std::sqrt (0.5), 0.15));
}

TEST_CASE ("the oscillator's noise is white and within range", "[oscillator][dsp]")
{
    Oscillator noise ({ { "waveform", 4.0f }, { "level", 0.0f } });
    const auto out = noise.render (48000).left();
    const auto [low, high] = std::ranges::minmax (out);
    double mean = 0.0;

    for (const auto value : out)
        mean += static_cast<double> (value);

    mean /= static_cast<double> (out.size());
    CHECK (low >= -1.0f);
    CHECK (high <= 1.0f);
    CHECK_THAT (mean, WithinAbs (0.0, 0.02));
    CHECK_THAT (rms (out), WithinAbs (1.0 / std::sqrt (3.0), 0.02)); // uniform in -1..1
}

TEST_CASE ("random phase starts each note's unison copies at new phases", "[oscillator]")
{
    // Starts are the first block and each reset (a new note in a voice slot).
    const auto starts = [] (float randomPhase)
    {
        const Oscillator osc ({ { "waveform", 0.0f }, { "unison", 4.0f }, { "randomPhase", randomPhase } });
        std::vector<std::vector<float>> result;

        for (int note = 0; note < 3; ++note)
        {
            result.push_back (osc.render (64).left());
            (*osc.node).reset();
        }

        return result;
    };

    // Without it every note starts alike, so unison attacks repeat their phasing.
    const auto fixed = starts (0.0f);
    CHECK (fixed[0] == fixed[1]);
    CHECK (fixed[1] == fixed[2]);

    // With it, every note starts differently.
    const auto random = starts (100.0f);
    CHECK (random[0] != random[1]);
    CHECK (random[1] != random[2]);

    // Two oscillators (two voice slots) don't share a sequence either.
    const Oscillator first ({ { "waveform", 0.0f }, { "randomPhase", 100.0f } });
    const Oscillator second ({ { "waveform", 0.0f }, { "randomPhase", 100.0f } });
    CHECK (first.render (64).left() != second.render (64).left());
}
