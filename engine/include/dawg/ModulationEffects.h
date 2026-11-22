#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

/**
    The pieces the Chorus, Flanger and Phaser share (docs/design/modulation-effects.md, X1): a stereo LFO with a
    phase offset between its channels, a delay line with an interpolated read, and a first-order allpass. Header-only,
    so the tests can drive them directly.
*/
namespace dawg::modfx
{

/**
    The LFO's shapes, in the order of the nodes' Shape parameter (X3).
*/
enum class LfoShape
{
    sine,     ///< sin 2πφ
    triangle, ///< a linear sweep, 0 at φ = 0 rising to 1 at φ = ¼
};

/**
    A shape's value at a phase.

    @param shape  The shape.
    @param phase  The phase in cycles; any value, only its fraction counts.
    @return       −1..1, 0 at phase 0 and rising.
*/
[[nodiscard]] inline double lfoValue (LfoShape shape, double phase) noexcept
{
    const auto fraction = phase - std::floor (phase);

    if (shape == LfoShape::triangle)
    {
        const auto shifted = fraction + 0.25 - std::floor (fraction + 0.25);
        return 1.0 - 4.0 * std::abs (shifted - 0.5);
    }

    return std::sin (2.0 * std::numbers::pi * fraction);
}

/**
    The difference of two phases, wrapped to −½..½ cycles.
*/
[[nodiscard]] inline double phaseDifference (double to, double from) noexcept
{
    const auto d = to - from;
    return d - std::floor (d + 0.5);
}

/**
    The LFO the three effects share (X2, X3, X4): one phase for the left channel, the right channel at the left's
    phase plus Spread, and a crossfade between shapes. Advanced once per sample.
*/
class StereoLfo
{
public:
    /**
        How long a shape change crossfades, and how fast a synced LFO slews to the song position after a jump.
    */
    static constexpr double fadeSeconds = 0.02;
    /// How long a change of rate or depth takes to settle, in seconds.
    static constexpr double slewSeconds = 0.05;

    /**
        Sets the rate-dependent coefficients.

        @param sampleRate  The sample rate.
    */
    void prepare (double sampleRate) noexcept
    {
        m_fadeStep = 1.0 / std::max (1.0, fadeSeconds * sampleRate);
        m_slew = 1.0 - std::exp (-1.0 / std::max (1.0, slewSeconds * sampleRate));
        reset();
    }

    /**
        Phase 0, no fade under way; the next song position it is given is taken at once.
    */
    void reset() noexcept
    {
        m_phase = 0.0;
        m_fade = 1.0;
        m_previous = m_shape;
        m_snap = true;
    }

    /**
        Changes the shape, crossfading from the old one (X12). The shape a fresh LFO starts with is set without one.

        @param shape  The new shape.
    */
    void setShape (LfoShape shape) noexcept
    {
        if (shape == m_shape)
            return;

        // A change during a fade starts a new one from the shape that is mostly heard.
        m_previous = m_fade < 0.5 ? m_previous : m_shape;
        m_shape = shape;
        m_fade = 0.0;
    }

    /**
        Sets the shape at once, without a crossfade (before playing).
    */
    void setShapeNow (LfoShape shape) noexcept
    {
        m_shape = shape;
        m_previous = shape;
        m_fade = 1.0;
    }

    /**
        Advances one sample.

        @param increment  Cycles per sample.
        @param target     The song position's phase at the next sample, for a synced LFO while playing; negative for
                          none, so the LFO runs on freely. The phase slews towards it, or takes it at once when
                          the LFO has had no other sample since a reset.
    */
    void advance (double increment, double target) noexcept
    {
        // Once it has run free, a song position that comes later is slewed to, never jumped to.
        if (target < 0.0)
            m_snap = false;

        if (target >= 0.0 && m_snap)
        {
            m_phase = target - std::floor (target);
            m_snap = false;
        }
        else
        {
            auto next = m_phase + increment;

            if (target >= 0.0)
                next += phaseDifference (target, next) * m_slew;

            m_phase = next - std::floor (next);
        }

        if (m_fade < 1.0)
            m_fade = std::min (1.0, m_fade + m_fadeStep);
    }

    /**
        The left channel's phase, 0..1.
    */
    [[nodiscard]] double phase() const noexcept { return m_phase; }

    /**
        The LFO's value at an offset from the left phase.

        @param offset  Cycles after the left channel's phase: Spread / 360 for the right, plus a voice's offset.
        @return        −1..1.
    */
    [[nodiscard]] double value (double offset) const noexcept
    {
        const auto at = m_phase + offset;
        const auto current = lfoValue (m_shape, at);

        if (m_fade >= 1.0)
            return current;

        return lfoValue (m_previous, at) + (current - lfoValue (m_previous, at)) * m_fade;
    }

private:
    double m_phase = 0.0;
    double m_fade = 1.0;
    double m_fadeStep = 1.0;
    double m_slew = 1.0;
    LfoShape m_shape = LfoShape::sine;
    LfoShape m_previous = LfoShape::sine;
    bool m_snap = true;
};

/**
    One channel's delay line with a 4-point Catmull-Rom read behind the write position (X6), as the Delay reads.
    Read the taps, then write the sample.
*/
class DelayLine
{
public:
    /**
        The shortest delay a read may ask for: the kernel reads one sample past the position.
    */
    static constexpr double minDelay = 2.0;

    /**
        A written sample's limit: far above full scale, it only keeps a feedback loop from building up overshoot.
    */
    static constexpr float writeLimit = 4.0f;

    /**
        Sizes the line. Allocates: call from prepare().

        @param maxDelay  The longest delay in samples a read will ask for.
    */
    void prepare (double maxDelay)
    {
        m_size = static_cast<int> (std::ceil (maxDelay)) + margin;
        m_line.assign (static_cast<size_t> (m_size), 0.0f);
        m_write = 0;
    }

    /**
        Clears the line.
    */
    void reset() noexcept
    {
        std::ranges::fill (m_line, 0.0f);
        m_write = 0;
    }

    /**
        Reads behind the position the next write goes to.

        @param delay  Samples, clamped to minDelay..the line's length.
        @return       The interpolated sample.
    */
    [[nodiscard]] float read (double delay) const noexcept
    {
        delay = std::clamp (delay, minDelay, static_cast<double> (m_size - margin));
        const auto position = static_cast<double> (m_write) - delay + static_cast<double> (m_size);
        const auto whole = static_cast<int> (position);
        const auto t = static_cast<float> (position - whole);
        const auto at = [&] (int offset) { return m_line[static_cast<size_t> ((whole + offset) % m_size)]; };
        const auto a = at (m_size - 1);
        const auto b = at (0);
        const auto c = at (1);
        const auto d = at (2);
        return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
    }

    /**
        Writes the next sample, limited to ±writeLimit, and moves on.
    */
    void write (float sample) noexcept
    {
        m_line[static_cast<size_t> (m_write)] = std::clamp (sample, -writeLimit, writeLimit);
        m_write = m_write + 1 == m_size ? 0 : m_write + 1;
    }

private:
    static constexpr int margin = 4;

    std::vector<float> m_line;
    int m_size = 0;
    int m_write = 0;
};

/**
    The first-order allpass coefficient for a frequency (X10): the allpass's phase is −90° there.

    @param frequency   Hz.
    @param sampleRate  The sample rate.
    @return            a = (tan(πf/fs) − 1) / (tan(πf/fs) + 1).
*/
[[nodiscard]] inline float allpassCoefficient (double frequency, double sampleRate) noexcept
{
    const auto t = std::tan (std::numbers::pi * frequency / sampleRate);
    return static_cast<float> ((t - 1.0) / (t + 1.0));
}

/**
    A first-order allpass, y = a·x + x₁ − a·y₁.
*/
struct AllpassStage
{
    /// The last input.
    float x1 = 0.0f;
    /// The last output.
    float y1 = 0.0f;

    /// Filters one sample with coefficient a.
    [[nodiscard]] float process (float x, float a) noexcept
    {
        const auto y = a * (x - y1) + x1;
        x1 = x;
        y1 = y;
        return y;
    }
};

} // namespace dawg::modfx
