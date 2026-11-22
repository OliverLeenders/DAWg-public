#pragma once

#include <cmath>
#include <numbers>

/**
    Transparent first-order antiderivative anti-aliasing (docs/design/vicanek-adaa.md), after M. Vicanek, "Note on
    Alias Suppression in Digital Distortion" (2023, revised 2024): the rectangular kernel of Parker, Zavalishin and
    Le Bivic (DAFx 2016) delayed by half a sample, and a one-pole filter before and after the shaper that make the
    whole linear-regime response flat.

    Per sample, with v the filtered input and m = (v + v₁) / 2:

        y = ½ mean(v₁, m) + ½ mean(m₁, v₁)

    where mean(a, b) is the curve's mean over [a, b], (F(b) − F(a)) / (b − a) for the curve's antiderivative F. The
    second term is the second half of the previous sample's interval: each sample computes both halves of its own
    interval, uses the first now and keeps the second for the next sample. A fresh state assumes F(0) = 0.

    For f(u) = u this is (v + 6v₁ + v₂) / 8, and with both one-poles the response is a first-order allpass: flat in
    level, 0.71 samples of delay at DC rising to 1.41 at Nyquist. The ADAA stage alone is linear phase, one sample.
*/
namespace dawg::adaa
{

/**
    The compensation filters' pole, a = 1 / (3 + √8), on the inner zero of 1 + 6z⁻¹ + z⁻² (Vicanek, eq. 6).
*/
inline constexpr double poleA = 1.0 / (3.0 + 2.0 * std::numbers::sqrt2);

/**
    The compensation filters' gain, 1 + a, so that their gain at DC is 1.
*/
inline constexpr double poleB = 1.0 + poleA;

/**
    One compensation filter: y[n] = b x[n] − a y[n−1].
*/
struct OnePole
{
    /// The last output.
    double y = 0.0;

    /// Filters one sample.
    [[nodiscard]] double process (double x) noexcept
    {
        y = poleB * x - poleA * y;
        return y;
    }
};

/**
    What the shaped path is in the linear regime, for a node's dry signal: the input filter, (x + 6x₁ + x₂) / 8 and
    the output filter, so that Mix lines dry and wet up exactly where the curve is a straight line.
*/
struct DryPath
{
    /// The filter before the shaper.
    OnePole pre;
    /// The filter after it.
    OnePole post;
    /// The input one sample ago.
    double x1 = 0.0;
    /// The input two samples ago.
    double x2 = 0.0;

    /// Delays one sample through the dry path.
    [[nodiscard]] double process (double x) noexcept
    {
        const auto v = pre.process (x);
        const auto kernel = 0.125 * (v + 6.0 * x1 + x2);
        x2 = x1;
        x1 = v;
        return post.process (kernel);
    }
};

/**
    One channel's state.
*/
struct State
{
    /// The filter before the shaper.
    OnePole pre;
    /// The filter after it.
    OnePole post;

    /** The previous filtered input. */
    double v1 = 0.0;

    /** The previous midpoint, (v₁ + v₂) / 2. */
    double m1 = 0.0;

    /** F(v₁), kept by step() so that a curve that doesn't change needs two antiderivatives per sample. */
    double f1 = 0.0;

    /** The previous sample's second half, ½ mean(m₁, v₁), under the curve it was computed with. */
    double second = 0.0;
};

/**
    Puts a state where a constant input u has left it, with fu the curve's value there (and F(u) for step()), so that
    the constant passes without a transient.
*/
inline void settle (State& state, double u, double fu, double antiderivativeAtU = 0.0) noexcept
{
    state.pre.y = u;
    state.v1 = u;
    state.m1 = u;
    state.f1 = antiderivativeAtU;
    state.second = 0.5 * fu;
    state.post.y = fu;
}

/**
    The filtered input of one sample and its midpoint with the previous one.
*/
struct Input
{
    /// The input value.
    double v = 0.0;
    /// The slope at it.
    double m = 0.0;
};

/**
    Runs the input filter on a shaper input.
*/
[[nodiscard]] inline Input input (State& state, double u) noexcept
{
    const auto v = state.pre.process (u);
    return { v, 0.5 * (v + state.v1) };
}

/**
    The curve's mean over [a, b] from its antiderivative, or the curve at the middle where a and b are too close for
    the quotient (below tolerance).
*/
template <typename Curve, typename Antiderivative>
[[nodiscard]] double mean (double a, double b, const Curve& curve, const Antiderivative& antiderivative,
                           double tolerance) noexcept
{
    const auto difference = b - a;

    if (std::abs (difference) < tolerance)
        return curve (0.5 * (a + b));

    return (antiderivative (b) - antiderivative (a)) / difference;
}

/**
    The shaped value of one sample for a curve given as its mean over an interval, before the output filter. second
    receives this sample's second half, to pass to advance(). With recompute, the previous sample's second half is
    taken again with this curve, so a curve that moves never makes the output jump (V5); otherwise the kept one is
    used.
*/
template <typename Mean>
[[nodiscard]] double shaped (const State& state, const Input& in, const Mean& meanOf, bool recompute,
                             double& second) noexcept
{
    second = 0.5 * meanOf (in.m, in.v);
    const auto previous = recompute ? 0.5 * meanOf (state.m1, state.v1) : state.second;
    return 0.5 * meanOf (state.v1, in.m) + previous;
}

/**
    Moves the history on by one sample, after shaped().
*/
inline void advance (State& state, const Input& in, double second) noexcept
{
    state.m1 = in.m;
    state.v1 = in.v;
    state.second = second;
}

/**
    One sample for a curve that doesn't change, from its antiderivative, with F(v₁) kept between samples: two
    antiderivatives per sample. Runs both filters and returns the output.
*/
template <typename Curve, typename Antiderivative>
[[nodiscard]] double step (State& state, double u, const Curve& curve, const Antiderivative& antiderivative,
                           double tolerance) noexcept
{
    const auto in = input (state, u);
    const auto fm = antiderivative (in.m);
    const auto fv = antiderivative (in.v);
    const auto half = in.m - state.v1; // = v − m

    double first = 0.0;
    double second = 0.0;

    if (std::abs (2.0 * half) < tolerance)
    {
        first = 0.5 * curve (0.5 * (state.v1 + in.m));
        second = 0.5 * curve (0.5 * (in.m + in.v));
    }
    else
    {
        first = 0.5 * (fm - state.f1) / half;
        second = 0.5 * (fv - fm) / half;
    }

    const auto shapedValue = first + state.second;
    advance (state, in, second);
    state.f1 = fv;
    return state.post.process (shapedValue);
}

} // namespace dawg::adaa
