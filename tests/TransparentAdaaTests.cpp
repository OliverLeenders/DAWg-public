#include "EffectTestUtils.h"
#include "TestUtils.h"
#include "dawg/BuiltinNodes.h"
#include "dawg/TransparentAdaa.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <random>

using namespace dawg;
using namespace dawg::test;
using Catch::Matchers::WithinAbs;

// Transparent ADAA (docs/design/vicanek-adaa.md): the shared helper, and the three shapers that use it.

namespace
{
constexpr double tolerance = 1.0e-5; // the nodes' ill-conditioning threshold

[[nodiscard]] double logCosh (double u)
{
    const auto a = std::abs (u);
    return a + std::log1p (std::exp (-2.0 * a)) - std::log (2.0);
}

[[nodiscard]] double tanhMean (double from, double to)
{
    return adaa::mean (from, to, [] (double v) { return std::tanh (v); }, logCosh, tolerance);
}

[[nodiscard]] std::vector<double> randomSignal (int length, double amplitude)
{
    std::mt19937 random (11);
    std::uniform_real_distribution<double> uniform (-amplitude, amplitude);
    std::vector<double> result (static_cast<size_t> (length));

    for (auto& sample : result)
        sample = uniform (random);

    return result;
}

// The helper's two ways to run a curve: step() with F kept between samples, and shaped() with the curve's mean.
[[nodiscard]] std::vector<double> viaStep (const std::vector<double>& inputs)
{
    adaa::State state;
    std::vector<double> result;

    for (const auto u : inputs)
        result.push_back (adaa::step (state, u, [] (double v) { return std::tanh (v); }, logCosh, tolerance));

    return result;
}

[[nodiscard]] std::vector<double> viaMean (const std::vector<double>& inputs, bool recompute)
{
    adaa::State state;
    std::vector<double> result;

    for (const auto u : inputs)
    {
        const auto in = adaa::input (state, u);
        auto second = 0.0;
        const auto value = adaa::shaped (state, in, tanhMean, recompute, second);
        adaa::advance (state, in, second);
        result.push_back (state.post.process (value));
    }

    return result;
}

// A quiet sine, a whole number of cycles per FFT, long enough for the oversampling filters to settle.
[[nodiscard]] std::vector<float> quietSine (int bin)
{
    return sine (binFrequency (bin), 0.001, 4800 + fftSize);
}

// Bins near 1, 5, 10, 15 and 20 kHz at 48 kHz.
constexpr std::array<int, 5> bins { 341, 1707, 3413, 5120, 6827 };
} // namespace

TEST_CASE ("the transparent ADAA helper is Vicanek's scheme", "[adaa][dsp]")
{
    const auto inputs = randomSignal (4000, 4.0);
    const auto reference = referenceTransparent (inputs, tanhMean);
    const auto stepped = viaStep (inputs);
    const auto meaned = viaMean (inputs, false);
    const auto recomputed = viaMean (inputs, true);

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        REQUIRE_THAT (stepped[i], WithinAbs (reference[i], 1.0e-9));
        REQUIRE_THAT (meaned[i], WithinAbs (reference[i], 1.0e-9));
        REQUIRE_THAT (recomputed[i], WithinAbs (reference[i], 1.0e-9));
    }
}

TEST_CASE ("the transparent ADAA is a flat allpass where the curve is a line", "[adaa][dsp]")
{
    // The impulse response for f(u) = u, through the helper and through the dry path.
    constexpr int length = 64;
    std::vector<double> impulse (length, 0.0);
    impulse[0] = 1.0;

    adaa::State state;
    adaa::DryPath dry;
    std::vector<double> response;
    const auto line = [] (double v) { return v; };
    const auto lineIntegral = [] (double v) { return 0.5 * v * v; };

    for (const auto x : impulse)
    {
        const auto shaped = adaa::step (state, x, line, lineIntegral, tolerance);
        REQUIRE_THAT (dry.process (x), WithinAbs (shaped, 1.0e-12));
        response.push_back (shaped);
    }

    // Flat in level at every frequency, and (z^-1 + a) / (1 + a z^-1): a at 0, 1 - a^2 at 1.
    for (int k = 0; k <= 32; ++k)
    {
        std::complex<double> sum {};

        for (int n = 0; n < length; ++n)
            sum += response[static_cast<size_t> (n)]
                   * std::polar (1.0, -juce::MathConstants<double>::twoPi * k * n / (2.0 * length));

        REQUIRE_THAT (std::abs (sum), WithinAbs (1.0, 1.0e-9));
    }

    CHECK_THAT (response[0], WithinAbs (adaa::poleA, 1.0e-12));
    CHECK_THAT (response[1], WithinAbs (1.0 - adaa::poleA * adaa::poleA, 1.0e-12));

    // The group delay at DC: (1 - a) / (1 + a), about 0.71 samples.
    auto moment = 0.0;
    auto total = 0.0;

    for (int n = 0; n < length; ++n)
    {
        moment += n * response[static_cast<size_t> (n)];
        total += response[static_cast<size_t> (n)];
    }

    CHECK_THAT (moment / total, WithinAbs ((1.0 - adaa::poleA) / (1.0 + adaa::poleA), 1.0e-9));
}

TEST_CASE ("the transparent ADAA has no step where it falls back to the curve", "[adaa][dsp]")
{
    // A ramp so slow that the differences cross the ill-conditioning threshold back and forth: the output must stay
    // on the reference computed with exact means.
    std::vector<double> inputs;
    auto u = -0.5;

    for (int i = 0; i < 4000; ++i)
    {
        u += (i % 7 < 3 ? 0.4 : 1.6) * tolerance;
        inputs.push_back (u);
    }

    const auto exactMean = [] (double from, double to)
    {
        if (from == to)
            return std::tanh (from);

        // Simpson's rule: exact enough for intervals this short.
        const auto mid = 0.5 * (from + to);
        return (std::tanh (from) + 4.0 * std::tanh (mid) + std::tanh (to)) / 6.0;
    };
    const auto reference = referenceTransparent (inputs, exactMean);
    const auto stepped = viaStep (inputs);

    for (size_t i = 50; i < inputs.size(); ++i)
        REQUIRE_THAT (stepped[i], WithinAbs (reference[i], 1.0e-9));
}

TEST_CASE ("the shapers are flat for quiet signals at every oversampling, dry and mixed", "[adaa][dsp]")
{
    const std::vector<std::pair<const char*, std::map<juce::String, float>>> nodes {
        { nodetype::saturator, { { "drive", 0.0f } } },
        { nodetype::clipper, { { "drive", 0.0f }, { "ceiling", 0.0f } } },
        { nodetype::waveshaper, { { "shape", 0.0f }, { "drive", 0.0f } } },
    };

    for (const auto& [type, values] : nodes)
        for (const auto oversampling : { 0.0f, 1.0f, 2.0f })
            for (const auto mix : { 100.0f, 50.0f })
            {
                auto settings = values;
                settings["oversampling"] = oversampling;
                settings["mix"] = mix;
                const auto reference = quietSine (bins[0]);
                const auto atReference = gainAtDb (EffectRunner (type, settings).run (reference), reference, bins[0]);

                for (const auto bin : bins)
                {
                    INFO (type << ", oversampling " << oversampling << ", mix " << mix << " %, bin " << bin);
                    const auto input = quietSine (bin);
                    const auto gain = gainAtDb (EffectRunner (type, settings).run (input), input, bin);

                    // Off is the ADAA alone; oversampled, the half-band filters' own passband comes on top.
                    CHECK_THAT (gain - atReference, WithinAbs (0.0, oversampling < 0.5f ? 0.05 : 0.2));
                }
            }
}
