#pragma once

/**
    PolyBLEP and polyBLAMP residuals for band-limited oscillators: the Oscillator's saw, pulse and triangle
    (docs/design/phase12-voice-scope.md, V3) and the Drum Synth's squares (docs/design/drum-synth.md, DR5).
*/
namespace dawg::polyblep
{

/**
    The polyBLEP residual of an upward step of 2 at phase 0. Add it at a falling edge's phase, subtract it at a rising
    one's.

    @param t   The phase, 0..1.
    @param dt  The phase advance per sample, 0..0.5.
    @return    The residual, non-zero only within dt of phase 0 or 1.
*/
[[nodiscard]] inline double blep (double t, double dt) noexcept
{
    if (t < dt)
    {
        const auto x = t / dt;
        return x + x - x * x - 1.0;
    }

    if (t > 1.0 - dt)
    {
        const auto x = (t - 1.0) / dt;
        return x * x + x + x + 1.0;
    }

    return 0.0;
}

/**
    The polyBLAMP residual of a corner at phase 0 whose slope rises by one per sample: the integral of the step's.

    @param t   The phase, 0..1.
    @param dt  The phase advance per sample, 0..0.5.
    @return    The residual, non-zero only within dt of phase 0 or 1.
*/
[[nodiscard]] inline double blamp (double t, double dt) noexcept
{
    if (t < dt)
    {
        const auto x = 1.0 - t / dt;
        return x * x * x / 6.0;
    }

    if (t > 1.0 - dt)
    {
        const auto x = (t - 1.0) / dt + 1.0;
        return x * x * x / 6.0;
    }

    return 0.0;
}

} // namespace dawg::polyblep
