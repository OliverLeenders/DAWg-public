#pragma once

#include "Simd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numbers>
#include <vector>

// Small DSP building blocks for the reverb (DESIGN.md, section 1). Everything here that runs per sample is noexcept
// and allocation-free; only allocate() allocates.

namespace dawg::reverb::dsp
{

/**
    Reads at a fractional delay with linear interpolation; exact at whole samples.

    @param at     Returns the sample a whole number of pushes ago.
    @param delay  Samples, at least 0.
*/
template <typename At>
[[nodiscard]] float interpolateLinear (const At& at, float delay) noexcept
{
    const auto whole = static_cast<size_t> (delay);
    const auto fraction = delay - static_cast<float> (whole);
    const auto a = at (whole);
    return a + (at (whole + 1) - a) * fraction;
}

/**
    Reads at a fractional delay with third-order Lagrange interpolation over the four nearest samples [PASP]; exact at
    whole samples, and passive (gain at most 1) because the delay is always in the central interval.

    @param at     Returns the sample a whole number of pushes ago.
    @param delay  Samples, at least 1.
*/
template <typename At>
[[nodiscard]] float interpolateLagrange (const At& at, float delay) noexcept
{
    const auto whole = static_cast<size_t> (delay);
    const auto f = delay - static_cast<float> (whole);
    const auto fm1 = f - 1.0f;
    const auto fm2 = f - 2.0f;
    const auto fp1 = f + 1.0f;
    const auto h0 = -f * fm1 * fm2 * (1.0f / 6.0f);
    const auto h1 = fp1 * fm1 * fm2 * 0.5f;
    const auto h2 = -fp1 * f * fm2 * 0.5f;
    const auto h3 = fp1 * f * fm1 * (1.0f / 6.0f);
    return h0 * at (whole - 1) + h1 * at (whole) + h2 * at (whole + 1) + h3 * at (whole + 2);
}

/**
    Reads at a fractional delay with fifth-order Lagrange interpolation over the six nearest samples [PASP]: a fifth of
    the third order's loss at high frequencies, for delays that move all the time. Exact at whole samples, and passive
    (gain at most 1) because the delay is always in the central interval.

    @param at     Returns the sample a whole number of pushes ago.
    @param delay  Samples, at least 2.
*/
template <typename At>
[[nodiscard]] float interpolateLagrange5 (const At& at, float delay) noexcept
{
    const auto whole = static_cast<size_t> (delay);
    const auto f = delay - static_cast<float> (whole);
    const auto a = f + 2.0f;
    const auto b = f + 1.0f;
    const auto d = f - 1.0f;
    const auto e = f - 2.0f;
    const auto g = f - 3.0f;
    const auto ab = a * b;
    const auto de = d * e;
    const auto cde = f * de;
    const auto abc = ab * f;
    const auto h0 = -b * cde * g * (1.0f / 120.0f);
    const auto h1 = a * cde * g * (1.0f / 24.0f);
    const auto h2 = -ab * de * g * (1.0f / 12.0f);
    const auto h3 = abc * e * g * (1.0f / 12.0f);
    const auto h4 = -abc * d * g * (1.0f / 24.0f);
    const auto h5 = abc * de * (1.0f / 120.0f);
    return h0 * at (whole - 2) + h1 * at (whole - 1) + h2 * at (whole) + h3 * at (whole + 1) + h4 * at (whole + 2)
           + h5 * at (whole + 3);
}

/**
    A circular delay line whose length is a power of two.

    @note  allocate(): message thread. Everything else: audio thread.
*/
class DelayLine final
{
public:
    /**
        Allocates room for at least the given number of past samples, and clears it.

        @param minLength  The longest delay that will be read, in samples, plus any interpolation margin.
    */
    void allocate (size_t minLength)
    {
        size_t size = 1;
        while (size < minLength + 1)
            size *= 2;

        m_data.assign (size, 0.0f);
        m_mask = size - 1;
        m_write = 0;
    }

    /**
        Sets every stored sample to zero.
    */
    void clear() noexcept { std::fill (m_data.begin(), m_data.end(), 0.0f); }

    /**
        Appends a sample; it becomes delay 0.

        @param sample  The new sample.
    */
    void push (float sample) noexcept
    {
        m_write = (m_write + 1) & m_mask;
        m_data[m_write] = sample;
    }

    /**
        Returns the sample pushed the given number of pushes ago (0 is the newest).

        @param delay  Whole samples, less than the allocated length.
    */
    [[nodiscard]] float at (size_t delay) const noexcept { return m_data[(m_write - delay) & m_mask]; }

    /**
        Adds a scaled, delayed copy of the last pushes to a buffer: out[i] += gain * (the sample pushed delay pushes
        before push i of the last count). In contiguous runs, so the loop vectorises.

        @param out    The buffer, count samples.
        @param count  Samples: the number of pushes it covers, at most the allocated length minus delay.
        @param delay  Whole samples.
        @param gain   The scale.
    */
    void accumulate (float* out, size_t count, size_t delay, float gain) const noexcept
    {
        const auto* data = m_data.data();
        const auto size = m_data.size();
        auto start = (m_write - (count - 1 + delay)) & m_mask;
        size_t done = 0;

        while (done < count)
        {
            const auto run = std::min (count - done, size - start);
            const auto* source = data + start;
            auto* target = out + done;

            for (size_t i = 0; i < run; ++i)
                target[i] += gain * source[i];

            done += run;
            start = 0;
        }
    }

    /**
        Reads at a fractional delay with linear interpolation; exact at whole samples.

        @param delay  Samples, at least 0.
    */
    [[nodiscard]] float linear (float delay) const noexcept
    {
        return interpolateLinear ([this] (size_t d) { return at (d); }, delay);
    }

    /**
        Reads at a fractional delay with third-order Lagrange interpolation (interpolateLagrange()).

        @param delay  Samples, at least 1.
    */
    [[nodiscard]] float lagrange (float delay) const noexcept
    {
        return interpolateLagrange ([this] (size_t d) { return at (d); }, delay);
    }

private:
    std::vector<float> m_data { 0.0f };
    size_t m_mask = 0;
    size_t m_write = 0;
};

/**
    count delay lines that are always pushed together: one buffer, one write position. Each line starts one cache
    line further on than a whole number of lengths, so the lines' writes, all at the same position, fall in different
    cache sets; separate power-of-two buffers can evict each other on every sample.

    @note  allocate(): message thread. Everything else: audio thread.
*/
template <size_t count>
class DelayBank final
{
public:
    /**
        Allocates room for at least the given number of past samples per line, and clears it.

        @param minLength  The longest delay that will be read, in samples, plus any interpolation margin.
    */
    void allocate (size_t minLength)
    {
        size_t size = 1;
        while (size < minLength + 1)
            size *= 2;

        m_stride = size + cacheLineFloats;
        m_data.assign (count * m_stride, 0.0f);
        m_mask = size - 1;
        m_write = 0;
    }

    /**
        Sets every stored sample to zero.
    */
    void clear() noexcept { std::fill (m_data.begin(), m_data.end(), 0.0f); }

    /**
        Appends a sample to every line; each becomes its line's delay 0.

        @param samples  The new samples, one per line.
    */
    void push (const std::array<float, count>& samples) noexcept
    {
        m_write = (m_write + 1) & m_mask;
        auto* data = m_data.data() + m_write;

        for (size_t i = 0; i < count; ++i)
            data[i * m_stride] = samples[i];
    }

    /**
        Returns the sample pushed to a line the given number of pushes ago (0 is the newest).

        @param line   The line.
        @param delay  Whole samples, less than the allocated length.
    */
    [[nodiscard]] float at (size_t line, size_t delay) const noexcept
    {
        return m_data[line * m_stride + ((m_write - delay) & m_mask)];
    }

    /**
        Reads a line at a fractional delay with linear interpolation (interpolateLinear()).
    */
    [[nodiscard]] float linear (size_t line, float delay) const noexcept
    {
        return interpolateLinear ([this, line] (size_t d) { return at (line, d); }, delay);
    }

    /**
        Reads a line at a fractional delay with third-order Lagrange interpolation (interpolateLagrange()).
    */
    [[nodiscard]] float lagrange (size_t line, float delay) const noexcept
    {
        return interpolateLagrange ([this, line] (size_t d) { return at (line, d); }, delay);
    }

    /**
        Reads a line at a fractional delay with fifth-order Lagrange interpolation (interpolateLagrange5()).
    */
    [[nodiscard]] float lagrange5 (size_t line, float delay) const noexcept
    {
        return interpolateLagrange5 ([this, line] (size_t d) { return at (line, d); }, delay);
    }

    /**
        Reads every line at its own whole delay: out[i] = at (i, delays[i]).

        @param delays  Whole samples, one per line.
        @param out     The samples.
    */
    void read (const std::array<size_t, count>& delays, std::array<float, count>& out) const noexcept
    {
        const auto* data = m_data.data();

        for (size_t i = 0; i < count; ++i)
            out[i] = data[i * m_stride + ((m_write - delays[i]) & m_mask)];
    }

    /**
        Reads every line at its own fractional delay with fifth-order Lagrange interpolation: the same result as
        lagrange5() for each line, with the coefficients computed for all lines at once so that the loops vectorise.

        @param delays  Samples, at least 2 and below 2^31, one per line.
        @param out     The samples.
    */
    void readLagrange5 (const std::array<float, count>& delays, std::array<float, count>& out) const noexcept
    {
        static_assert (count % 4 == 0);
        using namespace simd;
        std::array<int, count> whole; // filled below
        std::array<std::array<float, count>, 6> h;
        const auto one = broadcast (1.0f);
        const auto two = broadcast (2.0f);
        const auto three = broadcast (3.0f);
        const auto k12 = broadcast (1.0f / 12.0f);
        const auto k24 = broadcast (1.0f / 24.0f);
        const auto k120 = broadcast (1.0f / 120.0f);

        // interpolateLagrange5()'s coefficients, four lines at a time. A truncation is the conversion to size_t in
        // this range; -x * y there is negate (x * y) here, the same number exactly.
        for (size_t i = 0; i < count; i += 4)
        {
            const auto delay = load (delays, i);
            const auto f = delay - truncate (delay, whole.data() + i);
            const auto a = f + two;
            const auto b = f + one;
            const auto d = f - one;
            const auto e = f - two;
            const auto g = f - three;
            const auto ab = a * b;
            const auto de = d * e;
            const auto cde = f * de;
            const auto abc = ab * f;
            store (h[0], i, negate (b * cde * g * k120));
            store (h[1], i, a * cde * g * k24);
            store (h[2], i, negate (ab * de * g * k12));
            store (h[3], i, abc * e * g * k12);
            store (h[4], i, negate (abc * d * g * k24));
            store (h[5], i, abc * de * k120);
        }

        std::array<std::array<float, count>, 6> tap; // filled below
        const auto* data = m_data.data();

        for (size_t i = 0; i < count; ++i)
        {
            const auto* line = data + i * m_stride;
            const auto newest = m_write - static_cast<size_t> (whole[i]) + 2; // at (whole - 2)

            for (size_t k = 0; k < 6; ++k)
                tap[k][i] = line[(newest - k) & m_mask];
        }

        for (size_t i = 0; i < count; i += 4)
        {
            auto sum = load (h[0], i) * load (tap[0], i);

            for (size_t k = 1; k < 6; ++k)
                sum = sum + load (h[k], i) * load (tap[k], i);

            store (out, i, sum);
        }
    }

private:
    static constexpr size_t cacheLineFloats = 16; // 64 bytes

    std::vector<float> m_data { 0.0f };
    size_t m_stride = 1;
    size_t m_mask = 0;
    size_t m_write = 0;
};

/**
    A first-order lowpass from the bilinear transform of 1 / (1 + s/w), in topology-preserving form: a zero at
    Nyquist, unity gain at DC, no gain anywhere above 1 [TXT].
*/
class OnePoleLowpass final
{
public:
    /**
        Returns the coefficient for a corner frequency.

        @param cornerHz    The -3 dB frequency.
        @param sampleRate  The sample rate.
    */
    [[nodiscard]] static float coefficient (double cornerHz, double sampleRate) noexcept
    {
        const auto g = std::tan (std::numbers::pi * std::min (cornerHz, 0.49 * sampleRate) / sampleRate);
        return static_cast<float> (g / (1.0 + g));
    }

    /**
        Filters one sample.

        @param x  The input.
        @param a  The coefficient from coefficient().
    */
    [[nodiscard]] float process (float x, float a) noexcept
    {
        const auto v = (x - m_state) * a;
        const auto y = v + m_state;
        m_state = y + v;
        return y;
    }

    /**
        Clears the state.
    */
    void clear() noexcept { m_state = 0.0f; }

private:
    float m_state = 0.0f;
};

/**
    A first-order low shelf: gain g below the corner, 1 above, between g and 1 everywhere. At g = 1 the output is
    the input exactly; at g = 0 it is a first-order highpass; above 1 it is a boost.
*/
class LowShelf final
{
public:
    /**
        Filters one sample.

        @param x     The input.
        @param gain  The low-frequency gain: 0 to 1 for a cut (passive), above 1 for a boost.
        @param a     The lowpass coefficient (OnePoleLowpass::coefficient()).
    */
    [[nodiscard]] float process (float x, float gain, float a) noexcept
    {
        return x + (gain - 1.0f) * m_lowpass.process (x, a);
    }

    /**
        Clears the state.
    */
    void clear() noexcept { m_lowpass.clear(); }

private:
    OnePoleLowpass m_lowpass;
};

/**
    A first-order high shelf: gain 1 below the corner, g above, between g and 1 everywhere. At g = 1 the output is
    the input exactly; at g = 0 it is a first-order lowpass.
*/
class HighShelf final
{
public:
    /**
        Filters one sample.

        @param x     The input.
        @param gain  The high-frequency gain, 0 to 1.
        @param a     The lowpass coefficient (OnePoleLowpass::coefficient()).
    */
    [[nodiscard]] float process (float x, float gain, float a) noexcept
    {
        return gain * x + (1.0f - gain) * m_lowpass.process (x, a);
    }

    /**
        Clears the state.
    */
    void clear() noexcept { m_lowpass.clear(); }

private:
    OnePoleLowpass m_lowpass;
};

/**
    A second-order Butterworth lowpass or highpass (the RBJ cookbook's forms with Q = 1/sqrt 2 [TXT]), transposed
    direct form II.
*/
class Butterworth final
{
public:
    /**
        Sets the filter.

        @param cornerHz    The -3 dB frequency.
        @param sampleRate  The sample rate.
        @param highpass    True for a highpass, false for a lowpass.
    */
    void set (double cornerHz, double sampleRate, bool highpass) noexcept
    {
        const auto w = 2.0 * std::numbers::pi * cornerHz / sampleRate;
        const auto alpha = std::sin (w) / std::sqrt (2.0);
        const auto cw = std::cos (w);
        const auto a0 = 1.0 + alpha;
        const auto b1 = highpass ? -(1.0 + cw) : 1.0 - cw;
        m_b0 = static_cast<float> (b1 / (highpass ? -2.0 : 2.0) / a0);
        m_b1 = static_cast<float> (b1 / a0);
        m_b2 = m_b0;
        m_a1 = static_cast<float> (-2.0 * cw / a0);
        m_a2 = static_cast<float> ((1.0 - alpha) / a0);
        clear();
    }

    /**
        Filters one sample.

        @param x  The input.
    */
    [[nodiscard]] float process (float x) noexcept
    {
        const auto y = m_b0 * x + m_s1;
        m_s1 = m_b1 * x - m_a1 * y + m_s2;
        m_s2 = m_b2 * x - m_a2 * y;
        return y;
    }

    /**
        Clears the state.
    */
    void clear() noexcept
    {
        m_s1 = 0.0f;
        m_s2 = 0.0f;
    }

private:
    float m_b0 = 1.0f, m_b1 = 0.0f, m_b2 = 0.0f, m_a1 = 0.0f, m_a2 = 0.0f;
    float m_s1 = 0.0f, m_s2 = 0.0f;
};

/**
    A fourth-order Linkwitz-Riley crossover: low = Butterworth lowpass twice, high = Butterworth highpass twice. The
    two outputs are in phase at every frequency and their magnitudes add up to 1 [TXT], so a weighted sum
    a * low + b * high moves smoothly from a to b, without a dip at the crossover.
*/
class Crossover final
{
public:
    /**
        Sets the crossover frequency and clears the state.

        @param cornerHz    The crossover frequency.
        @param sampleRate  The sample rate.
    */
    void set (double cornerHz, double sampleRate) noexcept
    {
        m_low[0].set (cornerHz, sampleRate, false);
        m_low[1].set (cornerHz, sampleRate, false);
        m_high[0].set (cornerHz, sampleRate, true);
        m_high[1].set (cornerHz, sampleRate, true);
    }

    /**
        Clears the state.
    */
    void clear() noexcept
    {
        for (auto* filter : { &m_low[0], &m_low[1], &m_high[0], &m_high[1] })
            filter->clear();
    }

    /**
        Splits one sample into its low and high parts.

        @param x  The input.
        @return   { low, high }.
    */
    [[nodiscard]] std::array<float, 2> process (float x) noexcept
    {
        return { m_low[1].process (m_low[0].process (x)), m_high[1].process (m_high[0].process (x)) };
    }

private:
    std::array<Butterworth, 2> m_low {};
    std::array<Butterworth, 2> m_high {};
};

/**
    A Schroeder allpass, H(z) = (-g + z^-D) / (1 - g z^-D) [S62], with a fractional, linearly interpolated delay
    so that it can glide.
*/
class Allpass final
{
public:
    /**
        Allocates the delay.

        @param maxDelay  The longest delay in samples.
    */
    void allocate (size_t maxDelay) { m_line.allocate (maxDelay + 2); }

    /**
        Clears the state.
    */
    void clear() noexcept { m_line.clear(); }

    /**
        Filters one sample.

        @param x      The input.
        @param delay  The delay in samples, at least 1.
        @param gain   The gain g, below 1 in magnitude.
    */
    [[nodiscard]] float process (float x, float delay, float gain) noexcept
    {
        return output (x, m_line.linear (delay - 1.0f), gain); // read before the push: delay - 1 pushes ago
    }

    /**
        Filters one sample with a whole-sample delay (the same as the fractional version at a whole delay, cheaper).

        @param x      The input.
        @param delay  The delay in samples, at least 1.
        @param gain   The gain g, below 1 in magnitude.
    */
    [[nodiscard]] float process (float x, size_t delay, float gain) noexcept
    {
        return output (x, m_line.at (delay - 1), gain);
    }

private:
    [[nodiscard]] float output (float x, float delayed, float gain) noexcept
    {
        const auto v = x + gain * delayed;
        m_line.push (v);
        return delayed - gain * v;
    }

    DelayLine m_line;
};

/**
    Whether two values are exactly equal: for values that settle on their targets by copying them, where any
    difference matters.
*/
[[nodiscard]] inline bool same (float a, float b) noexcept
{
    return std::equal_to<float> {}(a, b);
}

/**
    Returns the one-pole smoothing coefficient for a time constant: each sample moves this fraction of the way.

    @param seconds     The time constant.
    @param sampleRate  The sample rate.
*/
[[nodiscard]] inline float smoothingCoefficient (double seconds, double sampleRate) noexcept
{
    return static_cast<float> (1.0 - std::exp (-1.0 / (seconds * sampleRate)));
}

/**
    Returns a value one step closer to its target: a rate-limited one-pole step, but never less than minChange, and
    exactly the target once within minChange. A one-pole alone never arrives in floating point: once its step is
    below half the value's resolution, adding it changes nothing. Here the last stretch is linear, and if rounding
    would still leave the value where it is, it moves by one unit of the last place, so every value lands exactly on
    its target in a bounded number of steps.

    @param current      The value.
    @param target       Its target.
    @param coefficient  The one-pole coefficient for one step.
    @param maxChange    The largest change per step.
    @param minChange    The smallest change per step, and the distance within which the value lands on the target.
    @return             The next value.
*/
[[nodiscard]] inline float approach (float current, float target, float coefficient, float maxChange,
                                     float minChange) noexcept
{
    const auto distance = target - current;

    if (std::abs (distance) <= minChange)
        return target;

    const auto move = distance * coefficient;
    const auto next =
        current
        + std::clamp (std::abs (move) < minChange ? std::copysign (minChange, distance) : move, -maxChange, maxChange);
    return same (next, current) ? std::nextafter (current, target) : next;
}

/**
    Moves values one sample towards their targets with a one-pole (approach()).

    @param values       The running values.
    @param targets      The targets.
    @param coefficient  From smoothingCoefficient().
    @param minChange    The smallest change per sample.
    @return             True when every value is exactly at its target.
*/
template <typename Array>
[[nodiscard]] bool smooth (Array& values, const Array& targets, float coefficient, float minChange) noexcept
{
    bool settled = true;

    for (size_t i = 0; i < values.size(); ++i)
    {
        if (same (values[i], targets[i]))
            continue;

        values[i] = approach (values[i], targets[i], coefficient, 1.0e30f, minChange);
        settled = false;
    }

    return settled;
}

/**
    Moves delays one sample towards their targets with a rate-limited one-pole (a glide; approach()).

    @param values       The running delays, in samples.
    @param targets      The targets.
    @param coefficient  From smoothingCoefficient().
    @param maxRate      The largest change per sample, in samples (the largest pitch deviation while gliding).
    @param minChange    The smallest change per sample.
    @return             True when every value is exactly at its target.
*/
template <typename Array>
[[nodiscard]] bool glide (Array& values, const Array& targets, float coefficient, float maxRate,
                          float minChange) noexcept
{
    bool settled = true;

    for (size_t i = 0; i < values.size(); ++i)
    {
        if (same (values[i], targets[i]))
            continue;

        values[i] = approach (values[i], targets[i], coefficient, maxRate, minChange);
        settled = false;
    }

    return settled;
}

/**
    Plans one control period of a ramp: each value first lands exactly on the point the previous plan aimed at, then
    aims at the point approach() gives for one period and gets the per-sample step that reaches it at the end of the
    period.

    @param current        The running values; set to the previous plan's end points.
    @param next           The end points: in, the previous plan's; out, this plan's.
    @param step           Out: the change per sample for this period.
    @param target         The targets.
    @param coefficient    The one-pole coefficient for a whole period.
    @param maxChange      The largest change per period.
    @param minChange      The smallest change per period.
    @param inverseLength  1 / samples per period.
    @return               True when every value is exactly at its target (and stays there: every step is 0).
*/
template <typename Array>
[[nodiscard]] bool planRamp (Array& current, Array& next, Array& step, const Array& target, float coefficient,
                             float maxChange, float minChange, float inverseLength) noexcept
{
    bool atRest = true;

    for (size_t i = 0; i < current.size(); ++i)
    {
        current[i] = next[i];

        if (same (current[i], target[i]))
        {
            step[i] = 0.0f;
            continue;
        }

        atRest = false;
        next[i] = approach (current[i], target[i], coefficient, maxChange, minChange);
        step[i] = (next[i] - current[i]) * inverseLength;
    }

    return atRest;
}

/**
    Moves values one sample along their planned ramps.

    @param current  The running values.
    @param step     The change per sample (from planRamp()).
*/
template <typename Array>
void advanceRamp (Array& current, const Array& step) noexcept
{
    for (size_t i = 0; i < current.size(); ++i)
        current[i] += step[i];
}

/**
    A sine approximation for modulation oscillators: a parabola with one refinement step, within about 0.1 % of
    sin (2 pi phase) [TXT].

    @param phase  Cycles, 0 to 1.
*/
[[nodiscard]] inline float fastSine (float phase) noexcept
{
    const auto q = phase < 0.5f ? phase : phase - 1.0f;
    const auto y = 8.0f * q - 16.0f * q * std::abs (q);
    return 0.225f * (y * std::abs (y) - y) + y;
}

} // namespace dawg::reverb::dsp
