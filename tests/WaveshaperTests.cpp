#include "EffectTestUtils.h"
#include "TestUtils.h"
#include "dawg/AllocationGuard.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/Commands.h"
#include "dawg/NodeRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <numbers>
#include <random>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// The Waveshaper (docs/design/waveshaper.md).

namespace
{
constexpr int settle = 4800; // before measuring, for the oversampling filters
constexpr int numShapes = 7;
constexpr int drawnShape = 6;

// The cases the tests run: every shape, the drawn curve asymmetric, and as case 7 the drawn curve symmetric.
constexpr int numCases = 8;
constexpr int symmetricDrawn = 7;
constexpr std::array<const char*, numCases> shapeNames { "Soft",         "Tube",         "Fold",  "Sine Fold",
                                                         "Half Rectify", "Full Rectify", "Drawn", "Drawn, symmetric" };

// A case's Shape and Symmetric parameters.
[[nodiscard]] float shapeParam (int shape)
{
    return static_cast<float> (std::min (shape, drawnShape));
}

[[nodiscard]] float symmetricParam (int shape)
{
    return shape == symmetricDrawn ? 1.0f : 0.0f;
}

// The DC blocker's coefficient at 48 kHz without oversampling (S7).
const double dcCoefficient = std::exp (-2.0 * std::numbers::pi * 5.0 / sampleRate);

// The drawn curve's corners as [u, value]: on the table's grid, so the table holds the curve exactly. Not odd, so the
// symmetric mode (its positive half mirrored) differs from it.
constexpr std::array<std::array<double, 2>, 5> drawnCorners {
    { { -1.0, -1.0 }, { -0.5, 0.25 }, { 0.0, 0.0 }, { 0.5, 0.75 }, { 1.0, 0.2 } }
};

[[nodiscard]] Curve drawnCurve()
{
    Curve curve;

    for (const auto& [u, value] : drawnCorners)
        curve.push_back ({ static_cast<Ticks> (std::lround ((u + 1.0) / 2.0 * modulationShapeTicks)), value });

    return curve;
}

// That curve over u, written out again, holding its ends beyond +-1.
[[nodiscard]] double referenceDrawn (double u)
{
    if (u <= -1.0)
        return drawnCorners.front()[1];

    for (size_t i = 1; i < drawnCorners.size(); ++i)
        if (u <= drawnCorners[i][0])
        {
            const auto& [u0, v0] = drawnCorners[i - 1];
            const auto& [u1, v1] = drawnCorners[i];
            return v0 + (v1 - v0) * (u - u0) / (u1 - u0);
        }

    return drawnCorners.back()[1];
}

// The curves of S3, written out again here rather than taken from the engine.
[[nodiscard]] double referenceCurve (int shape, double u)
{
    switch (shape)
    {
        case 0:
            return u / (1.0 + std::abs (u));
        case 1:
            return u >= 0.0 ? 1.0 - std::exp (-u) : 2.0 * (std::exp (u / 2.0) - 1.0);
        case 2:
        {
            // The line up to 1, then down to -1 at 3, up to 1 at 5, and the same mirrored below.
            const auto period = std::fmod (std::fmod (u + 1.0, 4.0) + 4.0, 4.0);
            return period < 2.0 ? period - 1.0 : 3.0 - period;
        }
        case 3:
            return std::sin (u);
        case 4:
            return u > 0.0 ? u / (1.0 + u) : 0.0;
        case 5:
            return std::abs (u) / (1.0 + std::abs (u));
        case 6:
            return referenceDrawn (u);
        default:
            return u < 0.0 ? -referenceDrawn (-u) : referenceDrawn (u);
    }
}

// The mean of a curve between two inputs, by Simpson's rule: what first-order ADAA computes in closed form.
[[nodiscard]] double referenceMean (int shape, double from, double to)
{
    if (std::abs (to - from) < 1.0e-9)
        return referenceCurve (shape, 0.5 * (from + to));

    constexpr int steps = 2000;
    const auto h = (to - from) / steps;
    auto sum = referenceCurve (shape, from) + referenceCurve (shape, to);

    for (int i = 1; i < steps; ++i)
        sum += (i % 2 == 1 ? 4.0 : 2.0) * referenceCurve (shape, from + i * h);

    return sum * h / 3.0 / (to - from);
}

struct Waveshaper : EffectRunner
{
    explicit Waveshaper (const std::map<juce::String, float>& values, const Curve& drawn = {})
        : EffectRunner (nodetype::waveshaper, values)
    {
        draw (drawn);
        node->reset();
    }

    // Hands the node a drawn curve, as compiling the graph does.
    void draw (const Curve& drawn)
    {
        GraphNode model;
        model.type = nodetype::waveshaper;
        model.modulationShape = drawn;
        node->configure (model);
    }
};

[[nodiscard]] std::vector<float> naiveShape (const std::vector<float>& input, int shape, float driveDb)
{
    const auto g = static_cast<double> (dbToGain (driveDb));
    std::vector<float> result (input.size());

    for (size_t i = 0; i < input.size(); ++i)
        result[i] = static_cast<float> (referenceCurve (shape, g * static_cast<double> (input[i])));

    return result;
}

[[nodiscard]] float peakOf (const std::vector<float>& signal, size_t from = 0)
{
    auto peak = 0.0f;

    for (auto i = from; i < signal.size(); ++i)
        peak = std::max (peak, std::abs (signal[i]));

    return peak;
}

[[nodiscard]] double meanOf (const std::vector<float>& signal, size_t from)
{
    auto sum = 0.0;

    for (auto i = from; i < signal.size(); ++i)
        sum += static_cast<double> (signal[i]);

    return sum / static_cast<double> (signal.size() - from);
}

[[nodiscard]] std::vector<float> noise (int length)
{
    std::mt19937 random (11);
    std::uniform_real_distribution<float> uniform (-1.0f, 1.0f);
    std::vector<float> result (static_cast<size_t> (length));

    for (auto& sample : result)
        sample = uniform (random);

    return result;
}

// Where the largest step is, for the message when a test fails.
[[nodiscard]] size_t largestStepIndex (const std::vector<float>& signal)
{
    size_t at = 0;

    for (size_t i = 1; i < signal.size(); ++i)
        if (std::abs (signal[i] - signal[i - 1]) > std::abs (signal[at] - signal[at == 0 ? 0 : at - 1]))
            at = i;

    return at;
}

[[nodiscard]] float largestStep (const std::vector<float>& signal)
{
    auto step = 0.0f;

    for (size_t i = 1; i < signal.size(); ++i)
        step = std::max (step, std::abs (signal[i] - signal[i - 1]));

    return step;
}

// The aliasing as aliasDb measures it, but relative to the second harmonic for Full Rectify, whose output has no
// fundamental (it doubles the frequency).
[[nodiscard]] double aliasOf (const std::vector<double>& magnitudes, int bin, int shape)
{
    const auto alias = aliasDb (magnitudes, bin);
    return shape == 5 ? alias + levelDb (magnitudes, bin) - levelDb (magnitudes, 2 * bin) : alias;
}

// The drive each shape's aliasing is measured at: where it distorts strongly, folding several times for the folds.
[[nodiscard]] float aliasingDrive (int shape)
{
    return shape == 2 || shape == 3 ? 12.0f : 18.0f;
}
} // namespace

TEST_CASE ("each Waveshaper antiderivative's derivative is its curve", "[waveshaper][dsp]")
{
    waveshaper::DrawnTable asymmetric;
    asymmetric.fill (drawnCurve());
    waveshaper::DrawnTable symmetric;
    symmetric.fill (drawnCurve(), true);

    for (int shape = 0; shape < numCases; ++shape)
    {
        INFO (shapeNames[static_cast<size_t> (shape)]);
        const auto chosen = static_cast<waveshaper::Shape> (shapeParam (shape));
        const auto& table = shape == symmetricDrawn ? symmetric : asymmetric;

        // Points chosen off the branch points, where the central difference holds for a curve with corners.
        for (auto u = -7.013; u < 7.0; u += 0.0517)
        {
            constexpr double h = 1.0e-6;
            const auto slope = (waveshaper::antiderivative (chosen, u + h, &table)
                                - waveshaper::antiderivative (chosen, u - h, &table))
                               / (2.0 * h);
            REQUIRE_THAT (slope, WithinAbs (waveshaper::curve (chosen, u, &table), 1.0e-6));
            REQUIRE_THAT (waveshaper::curve (chosen, u, &table), WithinAbs (referenceCurve (shape, u), 1.0e-12));
        }

        // Continuous at the branch points.
        for (const auto u : { -3.0, -1.0, 0.0, 1.0, 3.0, 5.0 })
        {
            constexpr double h = 1.0e-9;
            REQUIRE_THAT (waveshaper::antiderivative (chosen, u + h, &table),
                          WithinAbs (waveshaper::antiderivative (chosen, u - h, &table), 1.0e-8));
        }
    }
}

TEST_CASE ("the Waveshaper's ADAA output is each curve's transparent ADAA on ramps", "[waveshaper][dsp]")
{
    // A ramp from -4 to 4 (several folds), with and without a bias. The output must be the transparent ADAA of the
    // curve (docs/design/vicanek-adaa.md) with the means integrated numerically here, minus the curve at the bias,
    // through the DC blocker.
    constexpr int length = 3000;
    std::vector<float> ramp (length);

    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float> (-4.0 + 8.0 * static_cast<double> (i) / (length - 1));

    for (int shape = 0; shape < numCases; ++shape)
        for (const auto biasPercent : { 0.0f, 30.0f })
        {
            INFO (shapeNames[static_cast<size_t> (shape)] << ", bias " << biasPercent << " %");
            const auto output = Waveshaper ({ { "shape", shapeParam (shape) },
                                              { "symmetric", symmetricParam (shape) },
                                              { "drive", 0.0f },
                                              { "bias", biasPercent },
                                              { "oversampling", 0.0f } },
                                            drawnCurve())
                                    .run (ramp);
            const auto b = static_cast<double> (biasPercent / 100.0f);
            std::vector<double> inputs;

            for (const auto x : ramp)
                inputs.push_back (static_cast<double> (x) + b);

            const auto shaped = referenceTransparent (
                inputs, [shape] (double from, double to) { return referenceMean (shape, from, to); }, b,
                referenceCurve (shape, b));
            auto dcIn = 0.0;
            auto dcOut = 0.0;

            for (size_t i = 0; i < ramp.size(); ++i)
            {
                const auto wet = shaped[i] - referenceCurve (shape, b);
                const auto expected = wet - dcIn + dcCoefficient * dcOut;
                dcIn = wet;
                dcOut = expected;
                REQUIRE_THAT (static_cast<double> (output[i]), WithinAbs (expected, 2.0e-6));
            }
        }
}

TEST_CASE ("the Waveshaper aliases less than a naive shaper, and less again oversampled", "[waveshaper][dsp]")
{
    constexpr int bin = 2389; // about 7 kHz
    const auto input = sine (binFrequency (bin), 1.0, settle + fftSize);

    for (int shape = 0; shape < numCases; ++shape)
    {
        const auto drive = aliasingDrive (shape);
        const auto naive = aliasOf (spectrum (naiveShape (input, shape, drive)), bin, shape);
        const auto measure = [&] (float oversampling)
        {
            return aliasOf (spectrum (Waveshaper ({ { "shape", shapeParam (shape) },
                                                    { "symmetric", symmetricParam (shape) },
                                                    { "drive", drive },
                                                    { "oversampling", oversampling } },
                                                  drawnCurve())
                                          .run (input)),
                            bin, shape);
        };
        const auto adaa = measure (0.0f);
        const auto twice = measure (1.0f);
        const auto four = measure (2.0f);
        const auto eight = measure (3.0f);

        INFO (shapeNames[static_cast<size_t> (shape)] << ": naive " << naive << " dB, ADAA " << adaa << " dB, 2x "
                                                      << twice << " dB, 4x " << four << " dB, 8x " << eight << " dB");
        // Measured in docs/design/vicanek-adaa.md, section 10. Driven this far into its folds, Sine Fold gains little
        // from transparent ADAA without oversampling (+5.9 dB against the naive +6.9 dB; plain ADAA gave -6.7 dB).
        CHECK (adaa < naive - (shape == 3 ? 0.5 : 3.0));
        CHECK (twice < adaa - 3.0);
        CHECK (four < twice - 3.0);
        CHECK ((eight < four || eight < -85.0)); // or both at the float noise floor
    }
}

TEST_CASE ("the Waveshaper keeps silence silent and leaves no DC", "[waveshaper][dsp]")
{
    for (int shape = 0; shape < numCases; ++shape)
        for (const auto oversampling : { 0.0f, 1.0f, 3.0f })
        {
            INFO (shapeNames[static_cast<size_t> (shape)] << ", oversampling " << oversampling);

            for (const auto sample : Waveshaper ({ { "shape", shapeParam (shape) },
                                                   { "symmetric", symmetricParam (shape) },
                                                   { "drive", 24.0f },
                                                   { "bias", 50.0f },
                                                   { "oversampling", oversampling } },
                                                 drawnCurve())
                                         .run (std::vector<float> (4096, 0.0f)))
                REQUIRE (sample == 0.0f);
        }

    // The rectifiers and the asymmetric curves make DC from signal; the blocker takes it out within a second.
    const auto input = sine (1000.0, 0.5, 2 * static_cast<int> (sampleRate));

    for (const auto& [shape, bias] : { std::pair { 5, 0.0f }, { 4, 0.0f }, { 1, 50.0f }, { 2, 60.0f } })
    {
        INFO (shapeNames[static_cast<size_t> (shape)] << ", bias " << bias << " %");
        const auto output = Waveshaper ({ { "shape", shapeParam (shape) },
                                          { "symmetric", symmetricParam (shape) },
                                          { "drive", 12.0f },
                                          { "bias", bias } })
                                .run (input);
        CHECK (std::abs (meanOf (output, static_cast<size_t> (sampleRate))) < 1.0e-4);
        CHECK (peakOf (output, static_cast<size_t> (sampleRate)) > 0.1f);
    }
}

TEST_CASE ("the Waveshaper's Mix 0 % is the dry path and bypass passes the input", "[waveshaper][dsp]")
{
    const auto loud = sine (1000.0, 0.8, 2048);
    const auto dry =
        Waveshaper ({ { "drive", 24.0f }, { "tone", 6.0f }, { "mix", 0.0f }, { "oversampling", 0.0f } }).run (loud);
    const auto expected = referenceLinear (loud); // the shaped path's linear response (vicanek-adaa.md, V7)

    for (size_t i = 0; i < loud.size(); ++i)
        REQUIRE_THAT (dry[i], WithinAbs (static_cast<float> (expected[i]), 1.0e-6));

    for (const auto oversampling : { 0.0f, 3.0f })
    {
        Waveshaper bypassed ({ { "drive", 24.0f }, { "oversampling", oversampling } });
        GraphNode model;
        model.type = nodetype::waveshaper;
        model.bypassed = true;
        bypassed.node->configure (model);
        CHECK (bypassed.run (loud) == loud);
    }
}

TEST_CASE ("the Waveshaper's Tone tilts the wet signal around 800 Hz", "[waveshaper][dsp]")
{
    // A quiet signal through Soft is nearly linear, so Tone's effect is the ratio of the levels with and without it.
    const auto level = [] (double frequency, float tone)
    {
        const auto output =
            Waveshaper ({ { "shape", 0.0f }, { "drive", 0.0f }, { "tone", tone }, { "oversampling", 2.0f } })
                .run (sine (frequency, 0.001, 24000));
        return 20.0 * std::log10 (static_cast<double> (peakOf (output, 12000)));
    };

    const auto low = level (60.0, 12.0f) - level (60.0, 0.0f);
    const auto high = level (12000.0, 12.0f) - level (12000.0, 0.0f);
    INFO ("low " << low << " dB, high " << high << " dB");
    CHECK_THAT (low, WithinAbs (-6.0, 0.6));
    CHECK_THAT (high, WithinAbs (6.0, 0.5));
    CHECK_THAT (level (800.0, 12.0f) - level (800.0, -12.0f), WithinAbs (0.0, 0.5));
}

TEST_CASE ("moving the Waveshaper's controls, shape or drawn curve doesn't click", "[waveshaper][dsp]")
{
    // The input moves by at most 2 pi 100 / 48000 * 0.9 = 0.012 per sample and the curves' slopes are at most 1, so
    // with Drive at 6 dB the output moves by at most about 0.024 per sample when nothing changes; a crossfade adds
    // at most the curves' difference (up to 2) over 960 samples.
    const auto input = sine (100.0, 0.9, 30 * blockSize);

    SECTION ("the controls")
    {
        Waveshaper shaper ({ { "shape", 0.0f }, { "drive", 6.0f }, { "oversampling", 0.0f } });
        std::vector<float> output;

        for (int block = 0; block < 6; ++block)
        {
            if (block == 2)
                shaper.set ({ { "bias", 100.0f }, { "tone", 12.0f }, { "drive", -12.0f } });

            if (block == 4)
                shaper.set ({ { "bias", -100.0f }, { "tone", -12.0f }, { "drive", 6.0f } });

            const std::vector<float> slice (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
            const auto rendered = shaper.run (slice);
            output.insert (output.end(), rendered.begin(), rendered.end());
        }

        INFO ("at block " << largestStepIndex (output) / blockSize << ", sample "
                          << largestStepIndex (output) % blockSize);
        CHECK (largestStep (output) < 0.04f);
    }

    SECTION ("the shape and the drawn curve")
    {
        Waveshaper shaper ({ { "shape", 0.0f }, { "drive", 6.0f }, { "oversampling", 0.0f } });
        std::vector<float> output;

        for (int block = 0; block < 30; ++block)
        {
            // Every shape in turn; a new drawn curve while one fades out (block 15) and while one fades in (block
            // 17); then new drawn curves while Drawn runs.
            const auto inverted = block % 2 == 0;
            const Curve line { { 0, inverted ? 1.0 : -1.0 }, { modulationShapeTicks, inverted ? -1.0 : 1.0 } };

            if (block < 14)
                shaper.set ({ { "shape", static_cast<float> ((block / 2) % numShapes) } });
            else if (block == 14)
                shaper.set ({ { "shape", 2.0f } });
            else if (block == 16)
                shaper.set ({ { "shape", static_cast<float> (drawnShape) } });
            else if (block == 15 || block == 17 || block % 3 == 0)
                shaper.draw (line);

            // A curve that isn't odd (with slopes of at most 1), then switching it from symmetric to asymmetric and
            // back.
            if (block == 25)
                shaper.draw ({ { 0, -0.5 }, { modulationShapeTicks / 2, 0.0 }, { modulationShapeTicks, 1.0 } });

            if (block == 26 || block == 28)
                shaper.set ({ { "symmetric", block == 26 ? 0.0f : 1.0f } });

            const std::vector<float> slice (input.begin() + block * blockSize, input.begin() + (block + 1) * blockSize);
            const auto rendered = shaper.run (slice);
            output.insert (output.end(), rendered.begin(), rendered.end());
        }

        INFO ("at block " << largestStepIndex (output) / blockSize << ", sample "
                          << largestStepIndex (output) % blockSize);
        CHECK (largestStep (output) < 0.04f);
    }
}

TEST_CASE ("the Waveshaper follows a new drawn curve", "[waveshaper][dsp]")
{
    // A step settles at the drawn curve's value minus the curve at 0, before the DC blocker slowly takes it away, so the
    // first few samples show which curve plays.
    Waveshaper shaper ({ { "shape", static_cast<float> (drawnShape) }, { "drive", 0.0f }, { "oversampling", 0.0f } });
    const std::vector<float> step (64, 0.5f);
    CHECK_THAT (static_cast<double> (shaper.run (step)[5]), WithinAbs (0.5, 0.01)); // the identity

    shaper.draw (drawnCurve());
    shaper.node->reset();
    CHECK_THAT (static_cast<double> (shaper.run (step)[5]), WithinAbs (0.75, 0.01));

    // Asymmetric, the negative half is the curve's own, not the positive half mirrored.
    shaper.set ({ { "symmetric", 0.0f } });
    shaper.node->reset();
    CHECK_THAT (static_cast<double> (shaper.run (std::vector<float> (64, -0.5f))[5]), WithinAbs (0.25, 0.01));
    shaper.set ({ { "symmetric", 1.0f } });
    shaper.node->reset();
    CHECK_THAT (static_cast<double> (shaper.run (std::vector<float> (64, -0.5f))[5]), WithinAbs (-0.75, 0.01));
}

TEST_CASE ("the Waveshaper never allocates while processing", "[waveshaper][realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    for (const auto oversampling : { 0.0f, 1.0f, 2.0f, 3.0f })
    {
        Waveshaper shaper (
            { { "shape", static_cast<float> (drawnShape) }, { "drive", 12.0f }, { "oversampling", oversampling } });
        const auto input = noise (16 * blockSize);
        std::vector<float> left (input.size());
        std::vector<float> right (input.size());
        std::vector<float> modulation (static_cast<size_t> (blockSize), 20.0f);
        const std::array<ModulatedParam, 1> modulated { { { 2, modulation.data() } } };
        const std::vector<float> silence (static_cast<size_t> (blockSize));
        std::vector<AudioInput> inputs (shaper.node->descriptor().inputs.size(),
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
            shaper.node->params().set (0, static_cast<float> ((offset / blockSize) % numShapes));

            if (offset % (3 * blockSize) == 0)
                shaper.draw ({ { 0, -0.5 }, { static_cast<Ticks> (offset % modulationShapeTicks), 0.25 } });

            const ScopedNoAllocation guard;
            shaper.node->process (context);
        }

        CHECK (allocationViolationCount() == before);
    }
}

TEST_CASE ("the Waveshaper reports the oversampling's latency", "[waveshaper][dsp]")
{
    for (const auto oversampling : { 0.0f, 1.0f, 2.0f, 3.0f })
    {
        INFO ("oversampling " << oversampling);
        Waveshaper shaper ({ { "shape", 0.0f }, { "drive", 0.0f }, { "oversampling", oversampling } });
        const auto latency = shaper.node->latencySamples();

        if (oversampling < 0.5f)
            CHECK (latency == 0);
        else
            CHECK (latency > 0);

        // A small impulse: the transparent ADAA's allpass puts its peak one sample after the latency at the shaper's
        // rate, which is the latency or one after at the base rate.
        std::vector<float> impulse (2048, 0.0f);
        impulse[100] = 0.001f;
        const auto output = shaper.run (impulse);
        const auto peak = std::ranges::max_element (output, {}, [] (float s) { return std::abs (s); }) - output.begin();
        CHECK (peak >= 100 + latency);
        CHECK (peak <= 101 + latency);
    }
}

TEST_CASE ("the Waveshaper stays finite for extreme input, resets, and keeps its own state", "[waveshaper][dsp]")
{
    std::vector<float> extreme (4096);

    for (size_t i = 0; i < extreme.size(); ++i)
        extreme[i] = (i / 37) % 2 == 0 ? 1000.0f : -1000.0f;

    for (int shape = 0; shape < numCases; ++shape)
        for (const auto oversampling : { 0.0f, 3.0f })
        {
            INFO (shapeNames[static_cast<size_t> (shape)] << ", oversampling " << oversampling);

            for (const auto sample : Waveshaper ({ { "shape", shapeParam (shape) },
                                                   { "symmetric", symmetricParam (shape) },
                                                   { "drive", 36.0f },
                                                   { "bias", 40.0f },
                                                   { "oversampling", oversampling } },
                                                 drawnCurve())
                                         .run (extreme))
                REQUIRE (std::isfinite (sample));

            for (const auto sample :
                 Waveshaper (
                     { { "shape", shapeParam (shape) }, { "symmetric", symmetricParam (shape) }, { "drive", 36.0f } })
                     .run (std::vector<float> (4096, 1.0e-40f)))
                REQUIRE (std::isfinite (sample));
        }

    Waveshaper shaper ({ { "drive", 24.0f }, { "oversampling", 2.0f } });
    std::ignore = shaper.run (sine (3000.0, 0.9, 2048));
    shaper.node->reset();

    for (const auto sample : shaper.run (std::vector<float> (2048, 0.0f)))
        REQUIRE (sample == 0.0f);

    Waveshaper loud ({ { "drive", 24.0f } });
    Waveshaper silent ({ { "drive", 24.0f } });

    for (int round = 0; round < 4; ++round)
    {
        std::ignore = loud.run (sine (440.0, 0.9, 1024));

        for (const auto sample : silent.run (std::vector<float> (1024, 0.0f)))
            REQUIRE (sample == 0.0f);
    }
}

TEST_CASE ("the Waveshaper's panel curve is its static transfer curve", "[waveshaper][panel]")
{
    for (int shape = 0; shape < drawnShape; ++shape)
    {
        INFO (shapeNames[static_cast<size_t> (shape)]);
        const auto curve =
            effectResponse (nodetype::waveshaper,
                            { { "shape", shape }, { "drive", 6.0 }, { "bias", 20.0 }, { "output", -6.0 } }, sampleRate);
        const auto* x = curve["x"].getArray();
        const auto* y = curve["y"].getArray();
        REQUIRE (x != nullptr);
        REQUIRE (y != nullptr);
        REQUIRE (x->size() == 201);
        REQUIRE (y->size() == 201);

        const auto g = std::pow (10.0, 6.0 / 20.0);
        const auto gain = std::pow (10.0, -6.0 / 20.0);

        for (int i = 0; i < x->size(); ++i)
        {
            const auto in = static_cast<double> ((*x)[i]);
            REQUIRE_THAT (
                static_cast<double> ((*y)[i]),
                WithinAbs (gain * (referenceCurve (shape, g * in + 0.2) - referenceCurve (shape, 0.2)), 1.0e-4));
        }
    }
}

TEST_CASE ("the Waveshaper is an effect whose oversampling changes latency", "[waveshaper][automation]")
{
    const auto* descriptor = NodeRegistry::builtin().find (nodetype::waveshaper);
    REQUIRE (descriptor != nullptr);
    CHECK (descriptor->category == NodeCategory::effect);
    CHECK (nodetype::allowedInVoiceGraph (nodetype::waveshaper));
    const auto& oversampling = descriptor->params[static_cast<size_t> (descriptor->indexOfParam ("oversampling"))];
    CHECK (oversampling.changesLatency);
    CHECK (oversampling.choices.size() == 4);
    CHECK_FALSE (oversampling.acceptsAutomation());
    const auto& shape = descriptor->params[static_cast<size_t> (descriptor->indexOfParam ("shape"))];
    CHECK (shape.acceptsAutomation());
    CHECK_FALSE (shape.acceptsModulation());
    CHECK (shape.choices.size() == numShapes);
    CHECK (shape.defaultValue == static_cast<float> (drawnShape)); // Drawn by default
    const auto& symmetric = descriptor->params[static_cast<size_t> (descriptor->indexOfParam ("symmetric"))];
    CHECK (symmetric.kind == ParamKind::toggle);
    CHECK (symmetric.defaultValue == 1.0f); // symmetric by default
    CHECK (descriptor->params[static_cast<size_t> (descriptor->indexOfParam ("drive"))].acceptsModulation());
}

TEST_CASE ("setModulationShape draws a Waveshaper's curve and anchors its ends", "[waveshaper][commands]")
{
    auto project = apply ({}, "addTrack", { { "kind", "audio" } });
    const auto trackId = project.tracks[0]->id;
    project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::waveshaper } });
    const auto shaper = project.tracks[0]->graph.nodes.back().id;
    project = apply (project, "addNode", { { "trackId", trackId }, { "type", nodetype::clipper } });
    const auto clipper = project.tracks[0]->graph.nodes.back().id;

    const auto points = juce::Array<juce::var> { juce::Array<juce::var> { modulationShapeTicks / 4, -0.5, 0 },
                                                 juce::Array<juce::var> { modulationShapeTicks / 2, 0.75, 0 } };
    project =
        apply (project, "setModulationShape", { { "trackId", trackId }, { "nodeId", shaper }, { "points", points } });

    const auto& nodes = project.tracks[0]->graph.nodes;
    const auto& curve =
        std::ranges::find_if (nodes, [shaper] (const GraphNode& n) { return n.id == shaper; })->modulationShape;
    REQUIRE (curve.size() == 4);
    CHECK (curve.front().time == 0);
    CHECK (curve.front().value == -0.5);
    CHECK (curve.back().time == modulationShapeTicks);
    CHECK (curve.back().value == 0.75); // its own end value, not looped back as an LFO's

    CHECK_THROWS_AS (
        apply (project, "setModulationShape", { { "trackId", trackId }, { "nodeId", clipper }, { "points", points } }),
        CommandError);
}

TEST_CASE ("the Waveshaper's meter reads the input level and where it sits on the curve", "[waveshaper][panel]")
{
    SECTION ("a built-in shape: the input level before Drive")
    {
        Waveshaper shaper ({ { "shape", 0.0f }, { "drive", 12.0f } });
        std::ignore = shaper.run (sine (1000.0, 0.5, 4096));
        const auto meters = shaper.node->meters();
        CHECK_THAT (static_cast<double> (meters["input"]), WithinAbs (0.5, 1.0e-3));
        CHECK_THAT (static_cast<double> (meters["shaperInput"]),
                    WithinAbs (0.5 * static_cast<double> (dbToGain (12.0f)), 1.0e-3));
    }

    SECTION ("the drawn curve: the shaper's input u = g x + b and the curve there")
    {
        Waveshaper shaper (
            { { "shape", shapeParam (drawnShape) }, { "symmetric", 0.0f }, { "drive", 0.0f }, { "bias", 20.0f } },
            drawnCurve());
        std::ignore = shaper.run (sine (1000.0, 0.5, 4096));
        const auto meters = shaper.node->meters();
        CHECK_THAT (static_cast<double> (meters["input"]), WithinAbs (0.5, 1.0e-3));
        CHECK_THAT (static_cast<double> (meters["shaperInput"]), WithinAbs (0.7, 1.0e-3));
        CHECK_THAT (static_cast<double> (meters["shaped"]), WithinAbs (referenceDrawn (0.7), 2.0e-3));

        std::ignore = shaper.run (std::vector<float> (24000, 0.0f));
        CHECK (static_cast<double> (shaper.node->meters()["input"]) < 0.001);
    }
}
