#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

/**
    The topology-preserving (TPT) state-variable filter of the Filter node (docs/design/phase8-node-graph.md) and the
    Drum Synth's voices (docs/design/drum-synth.md, DR5): coefficients, which can be shared by channels and stages,
    and a state per channel and stage.
*/
namespace dawg
{

/**
    The modes of SvfCoefficients::bilinear(), in the Filter's order.
*/
enum class SvfMode
{
    lowPass,
    highPass,
    bandPass,
    notch,
};

/**
    One stage's coefficients: g and k as a1, a2, a3 and k, and the mix of its high-, band- and low-pass outputs.
*/
struct SvfCoefficients
{
    float a1 = 0.0f;   ///< 1 / (1 + g (g + k)).
    float a2 = 0.0f;   ///< g a1.
    float a3 = 0.0f;   ///< g a2.
    float k = 0.0f;    ///< The damping, 2 R.
    float high = 0.0f; ///< The high-pass output's weight in the result.
    float band = 0.0f; ///< The band-pass output's weight.
    float low = 0.0f;  ///< The low-pass output's weight.

    /**
        The SVF that realises a second-order digital filter b0 + b1 z^-1 + b2 z^-2 over 1 + a1 z^-1 + a2 z^-2 (a matched
        design): g and k from its poles, the output mix from its numerator.
    */
    [[nodiscard]] static SvfCoefficients fromBiquad (double b0, double b1, double b2, double a1, double a2) noexcept
    {
        // The TPT SVF's denominator is (1 + g k + g^2) + 2 (g^2 - 1) z^-1 + (1 - g k + g^2) z^-2.
        const auto sum = std::max (1.0e-12, 1.0 + a1 + a2);
        const auto difference = std::max (1.0e-12, 1.0 - a1 + a2);
        const auto g = std::sqrt (sum / difference);
        const auto damping = 2.0 * (1.0 - a2) / (difference * g);
        const auto d = 4.0 / difference; // 1 + g k + g^2

        // HP, BP and LP have numerators (1 - z^-1)^2, g (1 - z^-2) and g^2 (1 + z^-1)^2 over that denominator.
        SvfCoefficients svf;
        svf.high = static_cast<float> (d * (b0 - b1 + b2) / 4.0);
        svf.band = static_cast<float> (d * (b0 - b2) / (2.0 * g));
        svf.low = static_cast<float> (d * (b0 + b1 + b2) / (4.0 * g * g));
        svf.k = static_cast<float> (damping);
        svf.a1 = static_cast<float> (1.0 / (1.0 + g * (g + damping)));
        svf.a2 = static_cast<float> (g) * svf.a1;
        svf.a3 = static_cast<float> (g) * svf.a2;
        return svf;
    }

    /**
        The plain bilinear SVF, prewarped at the cutoff: a mode's prototype over s^2 + k s + 1. Its outputs are the low-,
        band- and high-pass prototypes themselves, so the band-pass peaks at 1 / k; the notch is low plus high.

        @param mode    The response.
        @param cutoff  The cutoff in Hz, held below 0.45 of the rate.
        @param k       The damping: sqrt 2 for Butterworth, smaller for resonance.
        @param rate    The sample rate the filter runs at.
    */
    [[nodiscard]] static SvfCoefficients bilinear (SvfMode mode, double cutoff, double k, double rate) noexcept
    {
        const auto g = std::tan (std::numbers::pi * std::min (cutoff, 0.45 * rate) / rate);
        SvfCoefficients svf;
        svf.k = static_cast<float> (k);
        svf.a1 = static_cast<float> (1.0 / (1.0 + g * (g + k)));
        svf.a2 = static_cast<float> (g) * svf.a1;
        svf.a3 = static_cast<float> (g) * svf.a2;
        svf.high = mode == SvfMode::highPass || mode == SvfMode::notch ? 1.0f : 0.0f;
        svf.band = mode == SvfMode::bandPass ? 1.0f : 0.0f;
        svf.low = mode == SvfMode::lowPass || mode == SvfMode::notch ? 1.0f : 0.0f;
        return svf;
    }
};

/**
    One channel's state of one SVF stage.
*/
struct SvfState
{
    float ic1 = 0.0f; ///< The first integrator's state.
    float ic2 = 0.0f; ///< The second integrator's state.

    /**
        Filters one sample.

        @param svf  The stage's coefficients.
        @param x    The input.
        @return     The coefficients' mix of the high-, band- and low-pass outputs.
    */
    [[nodiscard]] float process (const SvfCoefficients& svf, float x) noexcept
    {
        const auto v3 = x - ic2;
        const auto v1 = svf.a1 * ic1 + svf.a2 * v3;
        const auto v2 = ic2 + svf.a2 * ic1 + svf.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;

        const auto high = x - svf.k * v1 - v2;
        return svf.high * high + svf.band * v1 + svf.low * v2;
    }

    /**
        Clears the state.
    */
    void reset() noexcept { *this = {}; }
};

} // namespace dawg
