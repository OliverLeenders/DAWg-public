#include "EffectTestUtils.h"
#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <map>
#include <random>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// The Clipper (docs/design/clipper.md).

namespace
{
constexpr int settle = 4800; // before measuring, for the oversampling filters

struct Clipper : EffectRunner
{
    explicit Clipper (const std::map<juce::String, float>& values)
        : EffectRunner (nodetype::clipper, values)
    {
    }
};

// The curve of C3, written out again here rather than taken from the node: ceiling c, knee as 0..1.
[[nodiscard]] double referenceCurve (double x, double c, double knee)
{
    const auto w = knee * c;
    const auto a = std::abs (x);
    auto y = c;

    if (a <= c - w)
        y = a;
    else if (a < c + w)
        y = a - (a - c + w) * (a - c + w) / (4.0 * w);

    return std::copysign (y, x);
}

// The mean of the curve between two inputs, by Simpson's rule: what first-order ADAA computes in closed form.
[[nodiscard]] double referenceMean (double from, double to, double c, double knee)
{
    if (std::abs (to - from) < 1.0e-9)
        return referenceCurve (0.5 * (from + to), c, knee);

    constexpr int steps = 2000;
    const auto h = (to - from) / steps;
    auto sum = referenceCurve (from, c, knee) + referenceCurve (to, c, knee);

    for (int i = 1; i < steps; ++i)
        sum += (i % 2 == 1 ? 4.0 : 2.0) * referenceCurve (from + i * h, c, knee);

    return sum * h / 3.0 / (to - from);
}

[[nodiscard]] std::vector<float> naiveClip (const std::vector<float>& input, float driveDb, float ceilingDb,
                                            double knee)
{
    const auto g = static_cast<double> (dbToGain (driveDb));
    const auto c = static_cast<double> (dbToGain (ceilingDb));
    std::vector<float> result (input.size());

    for (size_t i = 0; i < input.size(); ++i)
        result[i] = static_cast<float> (referenceCurve (g * static_cast<double> (input[i]), c, knee));

    return result;
}

[[nodiscard]] float peakOf (const std::vector<float>& signal, size_t from = 0)
{
    auto peak = 0.0f;

    for (auto i = from; i < signal.size(); ++i)
        peak = std::max (peak, std::abs (signal[i]));

    return peak;
}

[[nodiscard]] std::vector<float> noise (int length)
{
    std::mt19937 random (7);
    std::uniform_real_distribution<float> uniform (-1.0f, 1.0f);
    std::vector<float> result (static_cast<size_t> (length));

    for (auto& sample : result)
        sample = uniform (random);

    return result;
}

[[nodiscard]] std::vector<float> square (int period, int length)
{
    std::vector<float> result (static_cast<size_t> (length));

    for (size_t i = 0; i < result.size(); ++i)
        result[i] = (static_cast<int> (i) / (period / 2)) % 2 == 0 ? 0.9f : -0.9f;

    return result;
}
} // namespace

TEST_CASE ("the Clipper's ADAA output is the curve's transparent ADAA on ramps", "[clipper][dsp]")
{
    // A ramp from -3 to 3 through a ceiling of -6 dB, hard and with a knee. The output must be the transparent ADAA of
    // the curve (docs/design/vicanek-adaa.md), with the means integrated numerically here, delayed by the peak guard;
    // the guard turns down wherever the output filter overshoots the ceiling, so those stretches are left out.
    constexpr int length = 3000;
    std::vector<float> ramp (length);

    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float> (-3.0 + 6.0 * static_cast<double> (i) / (length - 1));

    for (const auto knee : { 0.0f, 50.0f, 100.0f })
    {
        INFO ("knee " << knee << " %");
        Clipper clipper ({ { "ceiling", -6.0f }, { "knee", knee }, { "oversampling", 0.0f } });
        const auto latency = static_cast<size_t> (clipper.node->latencySamples());
        const auto output = clipper.run (ramp);
        const auto c = static_cast<double> (static_cast<float> (std::pow (10.0, -6.0 / 20.0)));
        const std::vector<double> inputs (ramp.begin(), ramp.end());
        const auto expected = referenceTransparent (inputs, [c, knee] (double from, double to)
                                                    { return referenceMean (from, to, c, knee / 100.0); });

        // Where the guard acts: within two lookaheads of a sample above the ceiling.
        std::vector<bool> guarded (ramp.size(), false);

        for (size_t i = 0; i < ramp.size(); ++i)
            if (std::abs (expected[i]) > c + 1.0e-9)
                for (auto j = i > 2 * latency ? i - 2 * latency : 0; j < std::min (ramp.size(), i + 2 * latency + 2);
                     ++j)
                    guarded[j] = true;

        auto compared = 0;

        for (size_t i = 0; i + latency < ramp.size(); ++i)
        {
            if (guarded[i])
                continue;

            REQUIRE_THAT (static_cast<double> (output[i + latency]), WithinAbs (expected[i], 1.0e-6));
            ++compared;
        }

        CHECK (compared > length / 2);

        // On the flat parts it is the ceiling.
        CHECK_THAT (output[latency + 200], WithinAbs (-static_cast<float> (c), 1.0e-6));
        CHECK_THAT (output[length - 10], WithinAbs (static_cast<float> (c), 1.0e-6));
    }
}

TEST_CASE ("the Clipper aliases less than a naive clip, and less again oversampled", "[clipper][dsp]")
{
    constexpr int bin = 2389; // about 7 kHz
    const auto input = sine (binFrequency (bin), 1.0, settle + fftSize);

    for (const auto knee : { 0.0f, 50.0f })
    {
        const auto naive = aliasDb (spectrum (naiveClip (input, 18.0f, 0.0f, knee / 100.0)), bin);
        const auto measure = [&] (float oversampling)
        {
            return aliasDb (
                spectrum (
                    Clipper ({ { "drive", 18.0f }, { "knee", knee }, { "oversampling", oversampling } }).run (input)),
                bin);
        };
        const auto adaa = measure (0.0f);
        const auto twice = measure (1.0f);
        const auto four = measure (2.0f);

        INFO ("knee " << knee << " %: naive " << naive << " dB, ADAA " << adaa << " dB, 2x " << twice << " dB, 4x "
                      << four << " dB");
        // Measured in docs/design/clipper.md, section 8.
        CHECK (adaa < naive - 6.0);
        CHECK (twice < adaa - 6.0);
        CHECK (four < twice - 3.0);
    }
}

TEST_CASE ("the downsampling filter overshoots a clipped signal", "[clipper][dsp]")
{
    // Why the Clipper clamps after downsampling (docs/design/clipper.md, C5): a sine hard-clipped at the 2x rate comes
    // out of the half-band filter above the clip level.
    juce::dsp::Oversampling<float> oversampler (1, 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true,
                                                true);
    oversampler.initProcessing (blockSize);
    const auto input = sine (binFrequency (341), 1.0, 8 * blockSize);
    std::vector<float> output (input.size());
    constexpr float clip = 0.25f;

    for (size_t offset = 0; offset < input.size(); offset += blockSize)
    {
        const float* in = input.data() + offset;
        auto up = oversampler.processSamplesUp (juce::dsp::AudioBlock<const float> (&in, 1, blockSize));

        for (size_t i = 0; i < up.getNumSamples(); ++i)
            up.setSample (0, static_cast<int> (i), std::clamp (up.getSample (0, static_cast<int> (i)), -clip, clip));

        float* out = output.data() + offset;
        juce::dsp::AudioBlock<float> down (&out, 1, blockSize);
        oversampler.processSamplesDown (down);
    }

    CHECK (peakOf (output, 4 * blockSize) > clip * 1.02f);
}

TEST_CASE ("no Clipper output sample exceeds the ceiling", "[clipper][dsp]")
{
    const std::vector<std::pair<const char*, std::vector<float>>> signals {
        { "sine", sine (binFrequency (2389), 0.9, 8192) },
        { "noise", noise (8192) },
        { "square", square (37, 8192) },
    };

    for (const auto& [name, signal] : signals)
        for (const auto ceiling : { 0.0f, -6.0f, -18.0f })
            for (const auto oversampling : { 0.0f, 1.0f, 2.0f })
                for (const auto mix : { 100.0f, 50.0f })
                    for (const auto knee : { 0.0f, 60.0f })
                    {
                        INFO (name << ", ceiling " << ceiling << " dB, oversampling " << oversampling << ", mix " << mix
                                   << " %, knee " << knee << " %");
                        const auto output = Clipper ({ { "drive", 24.0f },
                                                       { "ceiling", ceiling },
                                                       { "knee", knee },
                                                       { "mix", mix },
                                                       { "oversampling", oversampling } })
                                                .run (signal);
                        const auto c = static_cast<float> (std::pow (10.0, static_cast<double> (ceiling) / 20.0));
                        CHECK (peakOf (output) <= c);
                        CHECK (peakOf (output) > 0.9f * c); // it does reach the ceiling
                    }

    // Output scales the ceiling.
    const auto output = Clipper ({ { "drive", 24.0f }, { "ceiling", -6.0f }, { "output", -6.0f } }).run (noise (8192));
    CHECK (peakOf (output) <= static_cast<float> (std::pow (10.0, -12.0 / 20.0)) * 1.000001f);
}

TEST_CASE ("the Clipper keeps silence silent and treats DC as signal", "[clipper][dsp]")
{
    for (const auto oversampling : { 0.0f, 1.0f, 2.0f })
    {
        INFO ("oversampling " << oversampling);

        for (const auto sample : Clipper ({ { "drive", 24.0f }, { "knee", 50.0f }, { "oversampling", oversampling } })
                                     .run (std::vector<float> (4096, 0.0f)))
            REQUIRE (sample == 0.0f);

        // A constant input comes out as the curve of the driven constant, once the filters have settled (the
        // oversampling filters' gain at DC is 0.9998).
        for (const auto level : { 0.2f, 0.5f, -0.5f })
            for (const auto knee : { 0.0f, 50.0f })
            {
                INFO ("level " << level << ", knee " << knee << " %");
                const auto output =
                    Clipper (
                        { { "drive", 6.0f }, { "ceiling", -3.0f }, { "knee", knee }, { "oversampling", oversampling } })
                        .run (std::vector<float> (4096, level));
                const auto expected = referenceCurve (static_cast<double> (dbToGain (6.0f) * level),
                                                      static_cast<double> (dbToGain (-3.0f)), knee / 100.0);
                CHECK_THAT (static_cast<double> (output.back()), WithinRel (expected, 5.0e-4));
            }
    }
}

TEST_CASE ("the Clipper's Mix 0 % is the dry path and bypass passes the input", "[clipper][dsp]")
{
    // The dry path is the shaped path's linear response (vicanek-adaa.md, V7), behind the peak guard's delay.
    const auto loud = sine (1000.0, 0.8, 2048);
    Clipper clipper ({ { "drive", 24.0f }, { "mix", 0.0f }, { "oversampling", 0.0f } });
    const auto latency = static_cast<size_t> (clipper.node->latencySamples());
    const auto dry = clipper.run (loud);
    const auto expected = referenceLinear (loud);

    for (size_t i = 0; i + latency < loud.size(); ++i)
        REQUIRE_THAT (dry[i + latency], WithinAbs (static_cast<float> (expected[i]), 1.0e-6));

    for (const auto oversampling : { 0.0f, 2.0f })
    {
        Clipper bypassed ({ { "drive", 24.0f }, { "ceiling", -12.0f }, { "oversampling", oversampling } });
        GraphNode model;
        model.type = nodetype::clipper;
        model.bypassed = true;
        bypassed.node->configure (model);
        CHECK (bypassed.run (loud) == loud);
    }
}

TEST_CASE ("small signals pass the Clipper with Drive's gain", "[clipper][dsp]")
{
    const auto quiet = sine (1000.0, 0.001, 8192);

    for (const auto driveDb : { -12.0f, 0.0f, 24.0f })
        for (const auto knee : { 0.0f, 50.0f })
        {
            INFO ("drive " << driveDb << " dB, knee " << knee << " %");
            const auto output = Clipper ({ { "drive", driveDb }, { "knee", knee } }).run (quiet);
            CHECK_THAT (peakOf (output, output.size() / 2), WithinRel (0.001f * dbToGain (driveDb), 0.01f));
        }
}

TEST_CASE ("moving the Clipper's ceiling and knee doesn't click", "[clipper][dsp]")
{
    const auto input = sine (100.0, 0.9, 6 * blockSize);
    Clipper clipper ({ { "oversampling", 0.0f } });
    std::vector<float> output;

    for (int block = 0; block < 6; ++block)
    {
        if (block == 2)
            clipper.set ({ { "ceiling", -24.0f }, { "knee", 100.0f } });

        if (block == 4)
            clipper.set ({ { "ceiling", 0.0f }, { "knee", 0.0f } });

        const std::vector<float> slice (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
        const auto rendered = clipper.run (slice);
        output.insert (output.end(), rendered.begin(), rendered.end());
    }

    // The input moves by at most 2 pi 100 / 48000 * 0.9 = 0.012 per sample, the curve's slope is at most 1, and the
    // ceiling's ramp moves it by at most 0.001 per sample.
    auto step = 0.0f;

    for (size_t i = 1; i < output.size(); ++i)
        step = std::max (step, std::abs (output[i] - output[i - 1]));

    CHECK (step < 0.015f);
}

TEST_CASE ("the Clipper never allocates while processing", "[clipper][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    for (const auto oversampling : { 0.0f, 1.0f, 2.0f })
    {
        Clipper clipper ({ { "drive", 12.0f }, { "knee", 30.0f }, { "oversampling", oversampling } });
        const auto input = noise (16 * blockSize);
        std::vector<float> left (input.size());
        std::vector<float> right (input.size());
        std::vector<float> modulation (static_cast<size_t> (blockSize), -9.0f);
        const std::array<ModulatedParam, 1> modulated { { { 1, modulation.data() } } };
        const std::vector<float> silence (static_cast<size_t> (blockSize));
        std::vector<AudioInput> inputs (clipper.node->descriptor().inputs.size(),
                                        AudioInput { silence.data(), silence.data() });
        const auto before = allocationViolationCount();

        for (size_t offset = 0; offset < input.size(); offset += blockSize)
        {
            inputs[0] = { input.data() + offset, input.data() + offset };
            const std::array<AudioOutput, 1> outputs { AudioOutput { left.data() + offset, right.data() + offset } };
            ProcessContext context;
            context.numSamples = blockSize;
            context.audioInputs = inputs;
            context.audioOutputs = outputs;
            context.modulatedParams = modulated;
            clipper.node->params().set (2, static_cast<float> (offset % 100));

            const ScopedNoAllocation guard;
            clipper.node->process (context);
        }

        CHECK (allocationViolationCount() == before);
    }
}

TEST_CASE ("the Clipper reports the oversampling's latency", "[clipper][dsp]")
{
    for (const auto oversampling : { 0.0f, 1.0f, 2.0f })
    {
        INFO ("oversampling " << oversampling);
        Clipper clipper ({ { "oversampling", oversampling } });
        const auto latency = clipper.node->latencySamples();

        // The peak guard's lookahead at every choice (vicanek-adaa.md, V9), plus the oversampling's.
        if (oversampling < 0.5f)
            CHECK (latency == static_cast<int> (std::lround (0.001 * sampleRate)));
        else
            CHECK (latency > static_cast<int> (std::lround (0.001 * sampleRate)));

        // A small impulse: the transparent ADAA's allpass puts its peak one sample after the latency at the shaper's
        // rate, which is the latency or one after at the base rate.
        std::vector<float> impulse (2048, 0.0f);
        impulse[100] = 0.001f;
        const auto output = clipper.run (impulse);
        const auto peak = std::ranges::max_element (output, {}, [] (float s) { return std::abs (s); }) - output.begin();
        CHECK (peak >= 100 + latency);
        CHECK (peak <= 101 + latency);
    }
}

TEST_CASE ("the Clipper's meter reads how far the input went above the ceiling", "[clipper][dsp]")
{
    for (const auto oversampling : { 0.0f, 1.0f })
    {
        INFO ("oversampling " << oversampling);
        Clipper loud ({ { "drive", 6.0f }, { "ceiling", -3.0f }, { "oversampling", oversampling } });
        std::ignore = loud.run (sine (1000.0, 1.0, 4096));
        CHECK_THAT (static_cast<double> (loud.node->meters()["over"]), WithinAbs (9.0, 0.2));

        // It falls at 20 dB per second once clipping stops.
        std::ignore = loud.run (std::vector<float> (24000, 0.0f));
        CHECK_THAT (static_cast<double> (loud.node->meters()["over"]), WithinAbs (0.0, 0.3));

        Clipper quiet ({ { "oversampling", oversampling } });
        std::ignore = quiet.run (sine (1000.0, 0.1, 4096));
        CHECK (static_cast<double> (quiet.node->meters()["over"]) == 0.0);
    }
}

TEST_CASE ("the Clipper stays finite for extreme input, resets, and keeps its own state", "[clipper][dsp]")
{
    std::vector<float> extreme (4096);

    for (size_t i = 0; i < extreme.size(); ++i)
        extreme[i] = (i / 37) % 2 == 0 ? 1000.0f : -1000.0f;

    for (const auto oversampling : { 0.0f, 1.0f, 2.0f })
    {
        INFO ("oversampling " << oversampling);

        for (const auto sample :
             Clipper ({ { "drive", 24.0f }, { "knee", 40.0f }, { "oversampling", oversampling } }).run (extreme))
            REQUIRE ((std::isfinite (sample) && std::abs (sample) <= 1.0f));
    }

    for (const auto sample : Clipper ({ { "drive", 24.0f } }).run (std::vector<float> (4096, 1.0e-40f)))
        REQUIRE (std::isfinite (sample));

    Clipper clipper ({ { "drive", 24.0f }, { "oversampling", 2.0f } });
    std::ignore = clipper.run (sine (3000.0, 0.9, 2048));
    clipper.node->reset();

    for (const auto sample : clipper.run (std::vector<float> (2048, 0.0f)))
        REQUIRE (sample == 0.0f);

    Clipper loud ({ { "drive", 24.0f } });
    Clipper silent ({ { "drive", 24.0f } });

    for (int round = 0; round < 4; ++round)
    {
        std::ignore = loud.run (sine (440.0, 0.9, 1024));

        for (const auto sample : silent.run (std::vector<float> (1024, 0.0f)))
            REQUIRE (sample == 0.0f);
    }
}

TEST_CASE ("the Clipper's panel curve is its static transfer curve", "[clipper][panel]")
{
    const auto curve =
        effectResponse (nodetype::clipper, { { "drive", 6.0 }, { "ceiling", -6.0 }, { "knee", 50.0 } }, sampleRate);
    const auto* x = curve["x"].getArray();
    const auto* y = curve["y"].getArray();
    REQUIRE (x != nullptr);
    REQUIRE (y != nullptr);
    REQUIRE (x->size() == y->size());
    CHECK_THAT (static_cast<double> ((*x)[0]), WithinAbs (-1.0, 1.0e-9));
    CHECK_THAT (static_cast<double> ((*x)[x->size() - 1]), WithinAbs (1.0, 1.0e-9));

    const auto c = std::pow (10.0, -6.0 / 20.0);
    const auto g = std::pow (10.0, 6.0 / 20.0);

    for (int i = 0; i < x->size(); ++i)
        REQUIRE_THAT (static_cast<double> ((*y)[i]),
                      WithinAbs (referenceCurve (g * static_cast<double> ((*x)[i]), c, 0.5), 1.0e-4));

    // Mix 0 % is the identity, clamped at the ceiling, scaled by Output.
    const auto dry =
        effectResponse (nodetype::clipper, { { "mix", 0.0 }, { "ceiling", -6.0 }, { "output", -6.0 } }, sampleRate);
    CHECK_THAT (static_cast<double> (dry["y"][0]), WithinAbs (-c * c, 1.0e-4));
    CHECK_THAT (static_cast<double> (dry["y"][110]), WithinAbs (0.1 * c, 1.0e-4));
}

TEST_CASE ("the Clipper's oversampling changes latency and can't be automated", "[clipper][automation]")
{
    const auto* descriptor = NodeRegistry::builtin().find (nodetype::clipper);
    REQUIRE (descriptor != nullptr);
    CHECK (descriptor->category == NodeCategory::effect);
    CHECK (nodetype::allowedInVoiceGraph (nodetype::clipper));
    const auto& oversampling = descriptor->params[static_cast<size_t> (descriptor->indexOfParam ("oversampling"))];
    CHECK (oversampling.changesLatency);
    CHECK_FALSE (oversampling.acceptsAutomation());
    CHECK (descriptor->params[1].acceptsAutomation());
}

TEST_CASE ("the Clipper's meter reads the input level for the panel's dot", "[clipper][panel]")
{
    // Before Drive, which the curve is drawn against.
    Clipper clipper ({ { "drive", 12.0f }, { "ceiling", -6.0f } });
    std::ignore = clipper.run (sine (1000.0, 0.25, 4096));
    CHECK_THAT (static_cast<double> (clipper.node->meters()["input"]), WithinAbs (0.25, 1.0e-3));

    std::ignore = clipper.run (std::vector<float> (24000, 0.0f));
    CHECK (static_cast<double> (clipper.node->meters()["input"]) < 0.001);
}
