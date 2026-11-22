#include "Reverb.h"

#include "Simd.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <tuple>

namespace dawg::reverb
{

namespace
{
using std::numbers::pi;

// Feedback and Size (DESIGN.md 2.3, 2.4).
constexpr double shortestDecaySeconds = 0.25; // T60 at Feedback 0 %, default size
constexpr double decayRangeRatio = 64.0;      // T60 at 100 % over T60 at 0 %
constexpr double smallestEarlySpanSeconds = 0.005;
constexpr double earlySpanRangeRatio = 20.0; // early span at Size 100 % over 0 %: 5 ms to 100 ms
constexpr double smallestSizeLengthSeconds = 0.025;
constexpr double sizeLengthRangeRatio = 4.0; // FDN size length at Size 100 % over 0 %
constexpr double decaySizeExponent = 0.5;    // T60 grows with the square root of the size length

// Early reflections (DESIGN.md 1.3, 2.1).
constexpr double maxTimingWarp = 2.5;     // tap times u^w, w from 2.5 (early) down to 0.4 (late)
constexpr double maxTimingSlopeDb = 24.0; // level slope across the span at Early Timing 0 % and 100 %
constexpr double earlyEnergy = 0.53;      // energy of each channel's early taps (calibrated with tailLevel, A2)
// Allpass delays over the span; the same for both channels (revision 1.3), so the early part stays coherent.
constexpr std::array<double, 8> earlyAllpassSpan { 0.011, 0.017, 0.029, 0.043, 0.011, 0.017, 0.029, 0.043 };

// Tail (DESIGN.md 1.1, 1.2, 1.6).
// The input allpasses' lengths at the default size; they scale with the size length.
constexpr std::array<double, 8> inputAllpassSeconds { 0.00431, 0.00319, 0.00227, 0.00153,
                                                      0.00473, 0.00347, 0.00253, 0.00171 };
constexpr double tailLevel = 1.29;          // calibration: pink noise at Mix 100 % within 3 dB of the input
constexpr double tailLevelExponent = 0.125; // partial level compensation: energy grows as T60^(1/4)
constexpr float injection = 0.35355339f;    // 1 / sqrt (8): each input feeds 8 lines
constexpr std::array<float, numLines> inputSign { 1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, -1, -1, 1, -1 };
constexpr std::array<float, numLines> outputSign { 1, 1, -1, 1, -1, -1, 1, -1, 1, -1, 1, 1, -1, 1, -1, -1 };
constexpr std::array<float, numLines> innerSign { -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, 1, 1, -1 };
constexpr double innerTapFraction = 0.25; // a second output tap a quarter of the way along each line
// Silence (revision 1.7, SPEC 7.9): after this long with the input exactly 0 and the output below the level, the node
// sleeps until input that isn't 0 arrives, and then restarts as prepare() left it.
constexpr double sleepSeconds = 1.0;
constexpr float quietLevel = 1.0e-6f;          // -120 dBFS
constexpr float innerTapLevel = 0.7f;          // its level relative to the line's end
constexpr double loopModulationRadians = 0.01; // in-loop allpass angle depth at the default size
// Above this T60 the depth falls as 1/T60 (revision 1.5): a long tail is a steady bed, its movement below audibility.
constexpr double steadyDecaySeconds = 3.0;

// Modulation (revision 1.6, DESIGN.md S): up to its default the in-loop allpasses' gentle movement fades in, exactly
// as accepted for 1.5; above it the lines' delays move too, a chorus that grows to its full depth at 100 %.
constexpr double maxChorusSeconds = 0.00015; // the lines' delay excursion at 100 %, up to a T60 of:
constexpr double chorusDecaySeconds = 10.0;  // beyond it the excursion falls as 1/T60 (it accumulates per pass)
constexpr double chorusExponent = 1.5;       // the excursion grows as the chorus amount to this power

// Diffusion (DESIGN.md 2.2).
constexpr double maxInputAllpassGain = 0.7;
constexpr double maxEarlyAllpassGain = 0.55;
constexpr double maxLoopAllpassGain = 0.5;

// Damp highs, Thin lows (DESIGN.md 2.5).
constexpr double dampDecayRatioDecades = 1.5; // HF T60 ratio = 10^(-1.5 damp^2): starts gently (A7, SPEC 1.5)
constexpr double dampInputDb = 12.0;          // each input high shelf at 100 %, times damp^1.5 (A7, SPEC 1.5)
constexpr double dampInputCornerHz = 3000.0;
constexpr double dampLoopCornerHz = 4000.0;

// The tail's own darkening (SPEC 5.2, revisions 1.2 and 1.5; DESIGN.md 1.1): a high-frequency loss per second, like
// air absorption [M79], so the longer the tail, the larger the share of its decay the highs lose; it weakens for long
// tails, so their highs still ring for a fair share of the tail (A15), and it is nearly flat above 8 kHz (A14).
constexpr double airLossDbPerSecond = 20.0;      // above the corner, up to a T60 of about 4.5 s
constexpr double airWeakeningDbPerSecond = 55.0; // beyond: this times airHalfLossSeconds / (T60 + airHalfLossSeconds)
constexpr double airHalfLossSeconds = 2.6;
constexpr double airCornerHz = 6500.0; // a second-order Butterworth lowpass's corner

// Stereo (SPEC 5.10, revision 1.3; DESIGN.md 1.3, 1.9). Early: the side patterns' reflections come from directions
// between the source's side (0) and this far across (in radians; pi / 2 would be the opposite side).
constexpr double maxEarlyPanRadians = 55.0 * pi / 180.0;
// The time difference between the ears for a reflection from the side (azimuth 90 degrees): the far ear hears it this
// much later, about a head's width (SPEC 1.4 allows up to 0.7 ms).
constexpr double earlyInterauralSeconds = 0.00065;
// Tail: the left/right correlation it is given, below and above the crossover; above it, from Width 0 % (narrow) to
// 100 % (as wide as A9 allows for every seed).
constexpr double tailCorrelationLow = 0.92;
constexpr double narrowTailCorrelation = 0.6;
constexpr double wideTailCorrelation = 0.25;
constexpr double tailCrossoverHz = 150.0;
// Beyond the natural width (revision 1.6): 1.5's Width 0-100 % is now 0-75 % (new value = 0.75 times the old), and from
// 75 to 100 % the tail's correlation above the crossover goes on to this; below it stays as it is, so the low end
// stays solid.
constexpr double naturalWidthPercent = 75.0;
constexpr double veryWideTailCorrelation = 0.0;
// The least a side pattern leans to its input's side, in energy: its directions are narrowed if needed.
constexpr double minEarlyLeanDb = 6.0;
constexpr double thinDecayRatioDecades = 0.6; // LF T60 ratio = 10^(-0.6 thin)
constexpr double thinInputDb = 20.0;          // each input low shelf at 100 %
constexpr double thinInputCornerHz = 350.0;
constexpr double thinLoopCornerHz = 200.0;

// Smoothing (DESIGN.md 1.5, 1.7).
constexpr double gainSmoothingSeconds = 0.02;
constexpr double delaySmoothingSeconds = 0.08;
constexpr double maxDelayRate = 0.25; // samples per sample while gliding
constexpr double predelaySmoothingSeconds = 0.05;
constexpr double maxPredelayRate = 0.5;
constexpr double muteRampSeconds = 0.005;
constexpr double earlyFadeSeconds = 0.01; // crossfade between early tap sets
constexpr size_t controlPeriod = 16;      // samples between ramp plans for delays and gains
constexpr float gainMinChange = 1.0e-6f;  // per sample: the last stretch of a gain is linear, then exact
constexpr float delayMinChange = 1.0e-4f; // per sample, in samples: the same for delays

// Buffer sizes: the longest delays the controls can ask for.
constexpr double longestEarlySeconds = smallestEarlySpanSeconds * earlySpanRangeRatio;
constexpr double longestSizeLengthSeconds = smallestSizeLengthSeconds * sizeLengthRangeRatio;
constexpr double longestLineSeconds = 1.35 * longestSizeLengthSeconds + maxChorusSeconds;
constexpr double longestLoopAllpassSeconds = 0.2 * longestSizeLengthSeconds;
constexpr double longestPredelaySeconds = 0.25;
constexpr size_t interpolationMargin = 4;

// The panel's arrivals: taps within 20 dB of the channel's strongest (SPEC.md 6).
constexpr float arrivalThreshold = 0.1f;
constexpr size_t maxArrivals = 32;

// Width from 0 (narrow) to 1 (1.5's widest, natural), and how far beyond it, 0 to 1 (revision 1.6).
[[nodiscard]] double naturalWidth (const ParamValues& values) noexcept
{
    return std::min (1.0, values[param::width] / naturalWidthPercent);
}

[[nodiscard]] double beyondWidth (const ParamValues& values) noexcept
{
    return std::max (0.0, (values[param::width] - naturalWidthPercent) / (100.0 - naturalWidthPercent));
}

// The allpasses' movement, 0 to 1 at the Modulation default and above, and the chorus, 0 there to 1 at 100 %.
[[nodiscard]] double movement (const ParamValues& values) noexcept
{
    return std::min (1.0, values[param::modulation] / static_cast<double> (params[param::modulation].defaultValue));
}

[[nodiscard]] double chorus (const ParamValues& values) noexcept
{
    const auto from = static_cast<double> (params[param::modulation].defaultValue);
    return std::max (0.0, (values[param::modulation] - from) / (100.0 - from));
}

[[nodiscard]] double earlySpanSeconds (double size) noexcept
{
    return smallestEarlySpanSeconds * std::pow (earlySpanRangeRatio, size);
}

[[nodiscard]] double sizeLengthSeconds (double size) noexcept
{
    return smallestSizeLengthSeconds * std::pow (sizeLengthRangeRatio, size);
}

[[nodiscard]] double decaySeconds (double feedback, double size) noexcept
{
    const auto defaultSize = params[param::size].defaultValue / 100.0;
    const auto sizeFactor = std::pow (sizeLengthSeconds (size) / sizeLengthSeconds (defaultSize), decaySizeExponent);
    return shortestDecaySeconds * std::pow (decayRangeRatio, feedback) * sizeFactor;
}

[[nodiscard]] double defaultSizeLengthSeconds() noexcept
{
    return sizeLengthSeconds (params[param::size].defaultValue / 100.0);
}

[[nodiscard]] double defaultDecaySeconds() noexcept
{
    return decaySeconds (params[param::feedback].defaultValue / 100.0, params[param::size].defaultValue / 100.0);
}

// Gain per pass for a delay of the given length so that it decays 60 dB in t60 seconds [JC91].
[[nodiscard]] double decayGain (double seconds, double t60) noexcept
{
    return std::pow (10.0, -3.0 * seconds / t60);
}

[[nodiscard]] float toSamples (double seconds, double sampleRate, double minimum) noexcept
{
    return static_cast<float> (std::max (minimum, std::round (seconds * sampleRate)));
}

[[nodiscard]] size_t bufferLength (double seconds, double sampleRate) noexcept
{
    return static_cast<size_t> (std::ceil (seconds * sampleRate)) + interpolationMargin;
}

// The tail's output gains (DESIGN.md 1.9): for uncorrelated left and right signals of equal energy, mid a * M and
// side b * S with b = k a give a correlation (1 - k^2) / (1 + k^2), and a^2 + b^2 = 2 keeps the energy.
[[nodiscard]] std::array<float, 2> midSideGains (double correlation) noexcept
{
    const auto k = std::sqrt ((1.0 - correlation) / (1.0 + correlation));
    const auto a = std::sqrt (2.0 / (1.0 + k * k));
    return { static_cast<float> (a), static_cast<float> (k * a) };
}

// The widest spread of directions (up to maxEarlyPanRadians) at which a side pattern still leans at least
// minEarlyLeanDb towards its input's side, given its taps' levels: the source keeps its place whatever the seed or
// the settings (SPEC 5.10, A9). Bisection: bounded work.
[[nodiscard]] double earlySpread (const EarlyPattern& early, const std::array<double, tapsPerPattern>& level) noexcept
{
    const auto leanDb = [&early, &level] (double spread)
    {
        double own = 0.0, across = 0.0;
        for (size_t k = 0; k < tapsPerPattern; ++k)
        {
            const auto e = level[k] * level[k];
            const auto direction = k == 0 ? 0.0 : spread * early.pan[k]; // as in computeEarly
            own += e * std::cos (direction) * std::cos (direction);
            across += e * std::sin (direction) * std::sin (direction);
        }
        return 10.0 * std::log10 (own / (across + 1.0e-30));
    };

    if (leanDb (maxEarlyPanRadians) >= minEarlyLeanDb)
        return maxEarlyPanRadians;

    double narrow = 0.0, wide = maxEarlyPanRadians;
    for (int i = 0; i < 24; ++i)
    {
        const auto middle = 0.5 * (narrow + wide);
        (leanDb (middle) >= minEarlyLeanDb ? narrow : wide) = middle;
    }

    return narrow;
}

// Gives both side patterns the same shares of energy on their own side and across (the smaller of their two
// own-side shares, so each still leans at least minEarlyLeanDb), keeping each pattern's total: a centred source's
// early part then has exactly the same energy on both sides (SPEC 5.10).
void balanceSides (Targets::Early& early) noexcept
{
    std::array<double, 2> own {}, across {};

    for (size_t side = 0; side < 2; ++side)
        for (size_t k = 0; k < tapsPerPattern; ++k)
        {
            const auto tap = (side + 1) * tapsPerPattern + k;
            own[side] += static_cast<double> (early.gain[tap]) * early.gain[tap];
            across[side] += static_cast<double> (early.cross[tap]) * early.cross[tap];
        }

    const auto share = std::min (own[0] / (own[0] + across[0] + 1.0e-30), own[1] / (own[1] + across[1] + 1.0e-30));

    for (size_t side = 0; side < 2; ++side)
    {
        const auto total = own[side] + across[side];
        if (total <= 0.0 || across[side] <= 0.0)
            continue;

        const auto ownScale = static_cast<float> (std::sqrt (share * total / own[side]));
        const auto acrossScale = static_cast<float> (std::sqrt ((1.0 - share) * total / across[side]));

        for (size_t k = 0; k < tapsPerPattern; ++k)
        {
            const auto tap = (side + 1) * tapsPerPattern + k;
            early.gain[tap] *= ownScale;
            early.cross[tap] *= acrossScale;
        }
    }
}

void computeEarly (Targets& t, const ParamValues& values, const Pattern& pattern, double sampleRate) noexcept
{
    const auto span = earlySpanSeconds (values[param::size] / 100.0);
    // Early Timing (DESIGN.md 2.1): -1 (bunched early, loud first) to +1 (gathered late, building up).
    const auto timing = values[param::earlyTiming] / 100.0 * 2.0 - 1.0;
    const auto warp = std::pow (maxTimingWarp, -timing);
    const auto slopeDb = timing * maxTimingSlopeDb;
    const auto widthAngle = naturalWidth (values) * pi / 2.0;
    const std::array<double, 3> weight { std::cos (widthAngle), std::sin (widthAngle), std::sin (widthAngle) };

    for (size_t p = 0; p < pattern.early.size(); ++p)
    {
        const auto& early = pattern.early[p];
        std::array<double, tapsPerPattern> position {};
        std::array<double, tapsPerPattern> shaped {};
        double energy = 0.0;

        for (size_t k = 0; k < tapsPerPattern; ++k)
        {
            position[k] = std::pow (static_cast<double> (early.position[k]), warp);
            shaped[k] = early.gain[k] * std::pow (10.0, slopeDb * position[k] / 20.0);
            energy += shaped[k] * shaped[k];
        }

        const auto scale = weight[p] * std::sqrt (earlyEnergy / energy);
        const auto spread = p == 0 ? 0.0 : earlySpread (early, shaped);

        for (size_t k = 0; k < tapsPerPattern; ++k)
        {
            const auto tap = p * tapsPerPattern + k;

            // The shared pattern stays on its input's side; the side patterns' reflections lean towards their
            // input's side, each from its own direction (sine-cosine pan law: the energy stays the same).
            // The first reflection (the floor's, typically) comes from the source's own direction.
            const auto direction = k == 0 ? 0.0 : spread * early.pan[k];
            t.early.gain[tap] = static_cast<float> (shaped[k] * scale * std::cos (direction));
            t.early.cross[tap] = static_cast<float> (shaped[k] * scale * std::sin (direction));

            // The time difference between the ears: a pan of 0 is fully on the input's side (its own ear leads by
            // the most), 45 degrees is in front (no difference), beyond that the other ear leads.
            const auto lead = p == 0 ? 0.0 : earlyInterauralSeconds * std::cos (2.0 * direction);
            const auto arrival = position[k] * span;
            t.early.delay[tap] = toSamples (arrival + std::max (0.0, -lead), sampleRate, 1.0);
            t.early.crossDelay[tap] = toSamples (arrival + std::max (0.0, lead), sampleRate, 1.0);
        }
    }

    balanceSides (t.early);

    for (size_t i = 0; i < earlyAllpassSpan.size(); ++i)
        t.delays.earlyAllpass[i] = toSamples (earlyAllpassSpan[i] * span, sampleRate, 1.0);
}

void computeTail (Targets& t, const ParamValues& values, const Pattern& pattern, double sampleRate,
                  bool darkening) noexcept
{
    const auto size = values[param::size] / 100.0;
    const auto t60 = decaySeconds (values[param::feedback] / 100.0, size);
    const auto lowRatio = std::pow (10.0, -thinDecayRatioDecades * values[param::thin] / 100.0);
    const auto damp = values[param::damp] / 100.0;
    const auto highRatio = std::pow (10.0, -dampDecayRatioDecades * damp * damp);
    const auto sizeLength = sizeLengthSeconds (size);
    const auto sizeRatio = sizeLength / defaultSizeLengthSeconds();
    // The darkening's loss per second: fixed, like air's, for short and medium tails; weaker for long ones.
    const auto airLoss =
        std::min (airLossDbPerSecond, airWeakeningDbPerSecond * airHalfLossSeconds / (t60 + airHalfLossSeconds));
    double meanSeconds = 0.0;

    for (size_t i = 0; i < inputAllpassSeconds.size(); ++i)
        t.delays.inputAllpass[i] = toSamples (inputAllpassSeconds[i] * sizeRatio, sampleRate, 1.0);

    for (size_t i = 0; i < numLines; ++i)
    {
        t.delays.line[i] = toSamples (pattern.lineLength[i] * sizeLength, sampleRate, 2.0);
        t.delays.loopAllpass[i] = toSamples (pattern.allpassLength[i] * sizeLength, sampleRate, 2.0);
        t.delays.innerTap[i] = toSamples (innerTapFraction * t.delays.line[i], 1.0, 2.0);
        // Every delay element loses the same per sample [JC91] (revision 1.5): the line's gain covers the line, the
        // allpass's internal gain its own delay, so every mode decays at the same rate whatever the allpass's group
        // delay at its frequency. The shelves act once per pass, on the mean time a pass takes: the line plus the
        // allpass (whose energy-weighted mean delay is its length).
        const auto seconds = (t.delays.line[i] + t.delays.loopAllpass[i]) / sampleRate;
        meanSeconds += seconds / numLines;
        t.gains.line[i] = static_cast<float> (decayGain (t.delays.line[i] / sampleRate, t60));
        t.gains.loopAllpass[i] = static_cast<float> (decayGain (t.delays.loopAllpass[i] / sampleRate, t60));
        // Shelf gains relative to the mid gain: the extra loss per pass for the shorter T60 at the band's edge.
        t.gains.lineLow[i] = static_cast<float> (decayGain (seconds, t60 * lowRatio / (1.0 - lowRatio + 1.0e-12)));
        t.gains.lineAir[i] = darkening ? static_cast<float> (std::pow (10.0, -airLoss * seconds / 20.0)) : 1.0f;
        t.gains.lineHigh[i] = static_cast<float> (decayGain (seconds, t60 * highRatio / (1.0 - highRatio + 1.0e-12)));
    }

    const auto diffusion = values[param::diffusion] / 100.0;
    // Squared (revision 1.5): below about 50 % the lines mix slowly enough for the tail to keep some grain, so
    // Diffusion sets how much grain the tail has (A14); above it the tail is as smooth as noise either way.
    const auto angle = diffusion * diffusion * pi / 4.0;
    auto& s = t.gains.scalar;
    s[scalar::inputAllpass] = static_cast<float> (maxInputAllpassGain * diffusion);
    // Squared, so the early pattern stays distinct at moderate settings and is fully smeared only near 100 %.
    s[scalar::earlyAllpass] = static_cast<float> (maxEarlyAllpassGain * diffusion * diffusion);

    // Width also sets the tail's (SPEC 1.4, 5.5): the correlation above the crossover goes from narrow to wide.
    // Beyond the natural width (revision 1.6) it goes on to uncorrelated.
    const auto width = naturalWidth (values);
    const auto naturalHigh = narrowTailCorrelation + (wideTailCorrelation - narrowTailCorrelation) * width;
    const auto low = midSideGains (tailCorrelationLow);
    const auto high = midSideGains (naturalHigh + (veryWideTailCorrelation - naturalHigh) * beyondWidth (values));
    s[scalar::midLow] = low[0];
    s[scalar::sideLow] = low[1];
    s[scalar::midHigh] = high[0];
    s[scalar::sideHigh] = high[1];
    s[scalar::rotationCos] = static_cast<float> (std::cos (angle));
    s[scalar::rotationSin] = static_cast<float> (std::sin (angle));
    const auto loopGain = maxLoopAllpassGain * diffusion;
    s[scalar::loopSin] = static_cast<float> (loopGain);
    s[scalar::loopCos] = static_cast<float> (std::sqrt (1.0 - loopGain * loopGain));
    // The same excursion of the allpasses' group delay at every size: the depth shrinks as their length grows.
    s[scalar::loopDepth] = static_cast<float> (loopModulationRadians * diffusion / sizeRatio
                                               * std::min (1.0, steadyDecaySeconds / t60) * movement (values));
    // The chorus (revision 1.6): the lines' delays move by up to this many samples. Each pass adds its pitch movement
    // to a path's, so a long tail needs less per pass for the same chorus.
    const auto amount = chorus (values);
    s[scalar::chorusDepth] = static_cast<float> (maxChorusSeconds * sampleRate * std::pow (amount, chorusExponent)
                                                 * std::min (1.0, chorusDecaySeconds / t60));
    s[scalar::tail] = static_cast<float> (tailLevel * std::sqrt (1.0 - std::pow (10.0, -6.0 * meanSeconds / t60))
                                          * std::pow (t60 / defaultDecaySeconds(), tailLevelExponent));
}

// Adds a tap set to a block: the shared pattern from each channel's input, the left and right patterns from the mid
// input.
// The rotation network: four butterfly stages; in each, lines i and i + stride (bit `stride` of i clear) become
// c a + s b and c b - s a. Four lines at a time (revision 1.7). Within a group of four (strides 1 and 2) the partner is
// a lane swap, and c b - s a is computed as c b + (-s) a, which is the same number exactly.
void rotate (std::array<float, numLines>& y, float c, float s) noexcept
{
    using namespace simd;
    const auto cs = broadcast (c);
    const auto sn = broadcast (s);
    const auto pairSigned = load (std::array<float, 4> { s, -s, s, -s }.data());
    const auto halfSigned = load (std::array<float, 4> { s, s, -s, -s }.data());

    for (size_t i = 0; i < numLines; i += 4)
    {
        const auto v = load (y, i);
        store (y, i, cs * v + pairSigned * swapPairs (v));
    }

    for (size_t i = 0; i < numLines; i += 4)
    {
        const auto v = load (y, i);
        store (y, i, cs * v + halfSigned * swapHalves (v));
    }

    for (const size_t stride : { size_t { 4 }, size_t { 8 } })
        for (size_t base = 0; base < numLines; base += 2 * stride)
            for (size_t k = 0; k < stride; k += 4)
            {
                const auto a = load (y, base + k);
                const auto b = load (y, base + k + stride);
                store (y, base + k, cs * a + sn * b);
                store (y, base + k + stride, cs * b - sn * a);
            }
}

void sumTaps (const std::array<dsp::DelayLine, 2>& input, const Targets::Early& taps, size_t count, float* left,
              float* right) noexcept
{
    std::fill_n (left, count, 0.0f);
    std::fill_n (right, count, 0.0f);

    // Shared pattern: each input to its own side.
    for (size_t k = 0; k < tapsPerPattern; ++k)
    {
        const auto delay = static_cast<size_t> (taps.delay[k]);
        input[0].accumulate (left, count, delay, taps.gain[k]);
        input[1].accumulate (right, count, delay, taps.gain[k]);
    }

    // The left input's own reflections: mostly left, some across; the right input's, mirrored.
    for (size_t k = tapsPerPattern; k < 2 * tapsPerPattern; ++k)
    {
        const auto delay = static_cast<size_t> (taps.delay[k]);
        input[0].accumulate (left, count, delay, taps.gain[k]);
        input[0].accumulate (right, count, static_cast<size_t> (taps.crossDelay[k]), taps.cross[k]);
    }

    for (size_t k = 2 * tapsPerPattern; k < numTaps; ++k)
    {
        const auto delay = static_cast<size_t> (taps.delay[k]);
        input[1].accumulate (right, count, delay, taps.gain[k]);
        input[1].accumulate (left, count, static_cast<size_t> (taps.crossDelay[k]), taps.cross[k]);
    }
}

} // namespace

Targets computeTargets (const ParamValues& values, const Pattern& pattern, double sampleRate, bool darkening) noexcept
{
    Targets t;
    computeEarly (t, values, pattern, sampleRate);
    computeTail (t, values, pattern, sampleRate, darkening);

    t.tone[0] = static_cast<float> (std::pow (10.0, -thinInputDb * values[param::thin] / 100.0 / 20.0));
    t.tone[1] = static_cast<float> (std::pow (10.0, -dampInputDb * std::pow (values[param::damp] / 100.0, 1.5) / 20.0));

    auto& s = t.gains.scalar;
    const auto mixAngle = values[param::mix] / 100.0 * pi / 2.0;
    s[scalar::dry] = values[param::mix] <= 0.0f ? 1.0f : static_cast<float> (std::cos (mixAngle));
    s[scalar::wet] = values[param::mix] <= 0.0f ? 0.0f : static_cast<float> (std::sin (mixAngle));

    t.predelay = toSamples (values[param::predelay] / 1000.0, sampleRate, 0.0);
    t.mute = values[param::mute] > 0.5f ? 0.0f : 1.0f;
    return t;
}

std::array<std::vector<double>, 2> arrivals (const ParamValues& values, double sampleRate)
{
    const auto seed = static_cast<int> (std::lround (values[param::seed]));
    const auto t = computeTargets (values, makePattern (seed), sampleRate);
    std::array<std::vector<double>, 2> result;

    for (size_t channel = 0; channel < 2; ++channel)
    {
        // What each channel hears of a centred source: the shared pattern, its side's own pattern (gain) and the
        // other side's (cross), with each reflection's strength.
        std::vector<std::pair<float, double>> heard;
        const auto own = channel + 1;
        const auto other = 2 - channel;

        for (size_t k = 0; k < tapsPerPattern; ++k)
        {
            const auto mine = own * tapsPerPattern + k;
            const auto theirs = other * tapsPerPattern + k;

            const auto ms = [&t, sampleRate] (float delay) { return (t.predelay + delay) / sampleRate * 1000.0; };
            heard.emplace_back (std::abs (t.early.gain[k]), ms (t.early.delay[k]));
            heard.emplace_back (std::abs (t.early.gain[mine]), ms (t.early.delay[mine]));
            heard.emplace_back (std::abs (t.early.cross[theirs]), ms (t.early.crossDelay[theirs]));
        }

        float strongest = 0.0f;
        for (const auto& [strength, time] : heard)
            strongest = std::max (strongest, strength);

        // At most 32: the strongest, within 20 dB of the strongest; then in time order for the panel.
        std::sort (heard.begin(), heard.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
        auto& times = result[channel];

        for (const auto& [strength, time] : heard)
            if (strongest > 0.0f && strength >= arrivalThreshold * strongest && times.size() < maxArrivals)
                times.push_back (time);

        std::sort (times.begin(), times.end());
    }

    return result;
}

ReverbNode::ReverbNode()
    : EffectNode (reverbDescriptor)
{
}

void ReverbNode::setDarkening (bool on)
{
    m_darkening = on;
}

ParamValues ReverbNode::readParams() const noexcept
{
    ParamValues values {};
    for (size_t i = 0; i < values.size(); ++i)
        values[i] = param (i);
    return values;
}

void ReverbNode::clear() noexcept
{
    clearState();
}

void ReverbNode::prepare (const ProcessSpec& spec)
{
    m_sampleRate = spec.sampleRate;
    const auto rate = m_sampleRate;
    const auto block = static_cast<size_t> (std::max (1, spec.maxBlockSize));

    m_gainCoefficient = dsp::smoothingCoefficient (gainSmoothingSeconds, rate);
    m_rampDelayCoefficient = dsp::smoothingCoefficient (delaySmoothingSeconds, rate / controlPeriod);
    m_rampGainCoefficient = dsp::smoothingCoefficient (gainSmoothingSeconds, rate / controlPeriod);
    m_maxDelayChange = static_cast<float> (maxDelayRate * controlPeriod);
    m_predelayCoefficient = dsp::smoothingCoefficient (predelaySmoothingSeconds, rate);
    m_maxPredelayRate = static_cast<float> (maxPredelayRate);
    m_muteStep = static_cast<float> (1.0 / (muteRampSeconds * rate));
    m_fadeLength = static_cast<size_t> (toSamples (earlyFadeSeconds, rate, 1.0));
    m_thinCoefficient = dsp::OnePoleLowpass::coefficient (thinInputCornerHz, rate);
    m_dampCoefficient = dsp::OnePoleLowpass::coefficient (dampInputCornerHz, rate);
    m_lineLowCoefficient = dsp::OnePoleLowpass::coefficient (thinLoopCornerHz, rate);
    m_lineHighCoefficient = dsp::OnePoleLowpass::coefficient (dampLoopCornerHz, rate);
    // The darkening's lowpass: a trapezoidal state-variable filter, Q = 1/sqrt 2 [TXT].
    const auto airG = std::tan (pi * airCornerHz / rate);
    const auto airA1 = 1.0 / (1.0 + airG * (airG + std::numbers::sqrt2));
    m_lineAirCoefficients = { static_cast<float> (airA1), static_cast<float> (airG * airA1),
                              static_cast<float> (airG * airG * airA1) };
    m_tailMid.clear();
    m_tailMidCoefficient = dsp::OnePoleLowpass::coefficient (tailCrossoverHz, rate);
    m_tailSide.set (tailCrossoverHz, rate);

    // The early taps of a whole block are summed after the block's input is pushed: room for one more block.
    for (auto& line : m_input)
        line.allocate (bufferLength (longestEarlySeconds + earlyInterauralSeconds, rate) + block);

    const auto longestEarlyAllpass = *std::max_element (earlyAllpassSpan.begin(), earlyAllpassSpan.end());
    for (auto& allpass : m_earlyAllpass)
        allpass.allocate (bufferLength (longestEarlyAllpass * longestEarlySeconds, rate));

    const auto longestInputAllpass = *std::max_element (inputAllpassSeconds.begin(), inputAllpassSeconds.end());
    for (auto& allpass : m_inputAllpass)
        allpass.allocate (
            bufferLength (longestInputAllpass * longestSizeLengthSeconds / defaultSizeLengthSeconds(), rate));

    m_lines.allocate (bufferLength (longestLineSeconds, rate));
    m_loopAllpass.allocate (bufferLength (longestLoopAllpassSeconds, rate));

    for (auto& line : m_predelay)
        line.allocate (bufferLength (longestPredelaySeconds, rate));

    for (auto& scratch : m_scratch)
        scratch.assign (block, 0.0f);

    m_sleepAfter = static_cast<size_t> (std::lround (sleepSeconds * rate));
    clearState();
    restart();

    for (auto& peak : m_peak)
        peak.store (0.0f);
}

void ReverbNode::clearState() noexcept
{
    for (size_t ch = 0; ch < 2; ++ch)
    {
        m_thinFirst[ch].clear();
        m_thinSecond[ch].clear();
        m_dampFirst[ch].clear();
        m_dampSecond[ch].clear();
        m_input[ch].clear();
        m_predelay[ch].clear();
    }

    for (auto& allpass : m_earlyAllpass)
        allpass.clear();

    for (auto& allpass : m_inputAllpass)
        allpass.clear();

    m_lines.clear();
    m_loopAllpass.clear();
    m_lineLowState.fill (0.0f);
    m_lineHighState.fill (0.0f);
    m_lineAirFirst.fill (0.0f);
    m_lineAirSecond.fill (0.0f);
    m_tailMid.clear();
    m_tailSide.clear();
}

void ReverbNode::restart() noexcept
{
    // Start at the targets: no glide, fade or fade-in after prepare(), or after a silence (7.9).
    m_quietSamples = 0;
    m_sleeping = false;
    m_values = readParams();
    m_pattern = makePattern (static_cast<int> (std::lround (m_values[param::seed])));
    m_target = computeTargets (m_values, m_pattern, m_sampleRate, m_darkening);
    m_current = m_target;
    m_earlyFrom = m_current.early;
    m_fading = false;
    m_fadePosition = 0;
    m_toneSettled = true;
    m_delaysSettled = true;
    m_gainsSettled = true;
    m_outputMoving = false;
    m_sampleIndex = 0;
    m_delayNext = m_current.delays;
    m_delayStep = {};
    wholeDelays();
    m_gainNext = m_current.gains;
    m_gainStep = {};
    m_phase = m_pattern.modulationPhase;
    m_loopSin.fill (m_current.gains.scalar[scalar::loopSin]); // the first sample is a control tick: these are replaced
    m_loopCos.fill (m_current.gains.scalar[scalar::loopCos]);
    m_chorusPhase = m_pattern.chorusPhase;
    m_chorusOffset.fill (0.0f);
    m_chorusStep.fill (0.0f);
    m_chorusActive = false;

    for (size_t i = 0; i < numLines; ++i)
        m_chorusNext[i] = m_current.gains.scalar[scalar::chorusDepth] * dsp::fastSine (m_chorusPhase[i]);
}

Meters ReverbNode::meters() const
{
    auto times = arrivals (readParams(), m_sampleRate);
    Meters result;
    result["arrivalsLeft"] = std::move (times[0]);
    result["arrivalsRight"] = std::move (times[1]);
    result["level"] = { m_peak[0].exchange (0.0f), m_peak[1].exchange (0.0f) };
    return result;
}

void ReverbNode::updateTargets() noexcept
{
    const auto values = readParams();

    if (values != m_values)
    {
        if (! dsp::same (values[param::seed], m_values[param::seed]))
            m_pattern = makePattern (static_cast<int> (std::lround (values[param::seed])));

        m_values = values;
        m_target = computeTargets (m_values, m_pattern, m_sampleRate, m_darkening);
        m_toneSettled = false;
        m_delaysSettled = false;
        m_gainsSettled = false;
        m_outputMoving = true;
    }

    // New early taps crossfade in from the current ones; a change during a fade waits for it to end.
    if (! m_fading && m_target.early != m_current.early)
    {
        m_earlyFrom = m_current.early;
        m_current.early = m_target.early;
        m_fading = true;
        m_fadePosition = 0;
    }
}

void ReverbNode::advanceInput() noexcept
{
    if (! m_toneSettled)
        m_toneSettled = dsp::smooth (m_current.tone, m_target.tone, m_gainCoefficient, gainMinChange);

    m_current.mute += std::clamp (m_target.mute - m_current.mute, -m_muteStep, m_muteStep);
}

void ReverbNode::advanceOutput() noexcept
{
    // Delays and gains move on ramps planned every controlPeriod samples, counted from prepare() so the plan doesn't
    // depend on the block sizes; in between each value moves by a constant step (DESIGN.md 1.7).
    const auto tick = m_sampleIndex % controlPeriod == 0;

    if (m_outputMoving)
    {
        if (tick)
            planOutput();
        else
            advanceRamps();
    }

    if (tick)
        modulateLoop();

    ++m_sampleIndex;

    std::array<float, 1> predelay { m_current.predelay };
    std::ignore = dsp::glide (predelay, std::array<float, 1> { m_target.predelay }, m_predelayCoefficient,
                              m_maxPredelayRate, delayMinChange);
    m_current.predelay = predelay[0];
}

void ReverbNode::planOutput() noexcept
{
    constexpr auto inverse = 1.0f / static_cast<float> (controlPeriod);
    auto& d = m_current.delays;
    const auto& dt = m_target.delays;
    const auto dc = m_rampDelayCoefficient;
    const auto dm = m_maxDelayChange;
    auto& g = m_current.gains;
    const auto& gt = m_target.gains;
    const auto gc = m_rampGainCoefficient;
    constexpr auto noLimit = 1.0e30f;
    constexpr auto rampDelayMinChange = delayMinChange * controlPeriod;
    constexpr auto rampGainMinChange = gainMinChange * controlPeriod;

    // Non-short-circuiting: every group is planned.
    m_delaysSettled =
        static_cast<int> (dsp::planRamp (d.earlyAllpass, m_delayNext.earlyAllpass, m_delayStep.earlyAllpass,
                                         dt.earlyAllpass, dc, dm, rampDelayMinChange, inverse))
        & static_cast<int> (dsp::planRamp (d.inputAllpass, m_delayNext.inputAllpass, m_delayStep.inputAllpass,
                                           dt.inputAllpass, dc, dm, rampDelayMinChange, inverse))
        & static_cast<int> (
            dsp::planRamp (d.line, m_delayNext.line, m_delayStep.line, dt.line, dc, dm, rampDelayMinChange, inverse))
        & static_cast<int> (dsp::planRamp (d.loopAllpass, m_delayNext.loopAllpass, m_delayStep.loopAllpass,
                                           dt.loopAllpass, dc, dm, rampDelayMinChange, inverse))
        & static_cast<int> (dsp::planRamp (d.innerTap, m_delayNext.innerTap, m_delayStep.innerTap, dt.innerTap, dc, dm,
                                           rampDelayMinChange, inverse));
    m_gainsSettled = static_cast<int> (dsp::planRamp (g.line, m_gainNext.line, m_gainStep.line, gt.line, gc, noLimit,
                                                      rampGainMinChange, inverse))
                     & static_cast<int> (dsp::planRamp (g.lineLow, m_gainNext.lineLow, m_gainStep.lineLow, gt.lineLow,
                                                        gc, noLimit, rampGainMinChange, inverse))
                     & static_cast<int> (dsp::planRamp (g.lineHigh, m_gainNext.lineHigh, m_gainStep.lineHigh,
                                                        gt.lineHigh, gc, noLimit, rampGainMinChange, inverse))
                     & static_cast<int> (dsp::planRamp (g.lineAir, m_gainNext.lineAir, m_gainStep.lineAir, gt.lineAir,
                                                        gc, noLimit, rampGainMinChange, inverse))
                     & static_cast<int> (dsp::planRamp (g.loopAllpass, m_gainNext.loopAllpass, m_gainStep.loopAllpass,
                                                        gt.loopAllpass, gc, noLimit, rampGainMinChange, inverse))
                     & static_cast<int> (dsp::planRamp (g.scalar, m_gainNext.scalar, m_gainStep.scalar, gt.scalar, gc,
                                                        noLimit, rampGainMinChange, inverse));
    m_outputMoving = ! (m_delaysSettled && m_gainsSettled);

    if (m_delaysSettled)
        wholeDelays();
}

void ReverbNode::wholeDelays() noexcept
{
    const auto& d = m_current.delays;
    const auto convert = [] (const auto& from, auto& to, size_t less)
    {
        for (size_t i = 0; i < from.size(); ++i)
            to[i] = static_cast<size_t> (from[i]) - less;
    };
    convert (d.earlyAllpass, m_wholeEarlyAllpass, 0);
    convert (d.inputAllpass, m_wholeInputAllpass, 0);
    convert (d.line, m_wholeLine, 1);
    convert (d.innerTap, m_wholeInnerTap, 1);
    convert (d.loopAllpass, m_wholeLoopAllpass, 1);
}

void ReverbNode::modulateLoop() noexcept
{
    // Each in-loop allpass's angle moves slowly around its centre (DESIGN.md 1.1): rotate (sin, cos) of the centre by
    // a small angle a, with sin a and cos a to third and second order (s^2 + c^2 = 1 - a^4 / 12: never above 1, so
    // still lossless). At 0.1-0.4 Hz an update every controlPeriod samples changes the angle by about 1e-5.
    const auto& s = m_current.gains.scalar;
    const auto centreSin = s[scalar::loopSin];
    const auto centreCos = s[scalar::loopCos];
    const auto depth = s[scalar::loopDepth];
    const auto phaseScale = static_cast<float> (controlPeriod / m_sampleRate);

    for (size_t i = 0; i < numLines; ++i)
    {
        const auto a = depth * dsp::fastSine (m_phase[i]);
        m_phase[i] += m_pattern.modulationRate[i] * phaseScale;
        m_phase[i] -= m_phase[i] >= 1.0f ? 1.0f : 0.0f;
        const auto cosA = 1.0f - 0.5f * a * a;
        const auto sinA = a - a * a * a * (1.0f / 6.0f);
        m_loopSin[i] = centreSin * cosA + centreCos * sinA;
        m_loopCos[i] = centreCos * cosA - centreSin * sinA;
    }

    // The chorus (revision 1.6): each line's delay offset moves linearly between ticks, towards depth times a sine of
    // the line's own rate. At depth 0 every offset is exactly 0 and the lines are read as before.
    const auto chorusDepth = s[scalar::chorusDepth];
    constexpr auto inverse = 1.0f / static_cast<float> (controlPeriod);
    bool active = false;

    for (size_t i = 0; i < numLines; ++i)
    {
        m_chorusOffset[i] = m_chorusNext[i];
        m_chorusPhase[i] += m_pattern.chorusRate[i] * phaseScale;
        m_chorusPhase[i] -= m_chorusPhase[i] >= 1.0f ? 1.0f : 0.0f;
        m_chorusNext[i] = chorusDepth * dsp::fastSine (m_chorusPhase[i]);
        m_chorusStep[i] = (m_chorusNext[i] - m_chorusOffset[i]) * inverse;
        active = active || ! dsp::same (m_chorusOffset[i], 0.0f) || ! dsp::same (m_chorusNext[i], 0.0f);
    }

    m_chorusActive = active;
}

void ReverbNode::advanceRamps() noexcept
{
    auto& d = m_current.delays;
    dsp::advanceRamp (d.earlyAllpass, m_delayStep.earlyAllpass);
    dsp::advanceRamp (d.inputAllpass, m_delayStep.inputAllpass);
    dsp::advanceRamp (d.line, m_delayStep.line);
    dsp::advanceRamp (d.loopAllpass, m_delayStep.loopAllpass);
    dsp::advanceRamp (d.innerTap, m_delayStep.innerTap);

    auto& g = m_current.gains;
    dsp::advanceRamp (g.line, m_gainStep.line);
    dsp::advanceRamp (g.lineLow, m_gainStep.lineLow);
    dsp::advanceRamp (g.lineHigh, m_gainStep.lineHigh);
    dsp::advanceRamp (g.lineAir, m_gainStep.lineAir);
    dsp::advanceRamp (g.loopAllpass, m_gainStep.loopAllpass);
    dsp::advanceRamp (g.scalar, m_gainStep.scalar);
}

std::array<float, 2> ReverbNode::processTone (float left, float right) noexcept
{
    const auto thin = m_current.tone[0];
    const auto damp = m_current.tone[1];
    const auto tone = [&] (size_t ch, float x)
    {
        x = m_thinSecond[ch].process (m_thinFirst[ch].process (x, thin, m_thinCoefficient), thin, m_thinCoefficient);
        return m_dampSecond[ch].process (m_dampFirst[ch].process (x, damp, m_dampCoefficient), damp, m_dampCoefficient);
    };
    const auto wetLeft = tone (0, left);
    const auto wetRight = tone (1, right);

    m_input[0].push (wetLeft);
    m_input[1].push (wetRight);
    return { wetLeft, wetRight };
}

void ReverbNode::sumEarly (size_t count) noexcept
{
    auto& to = m_scratch[4];
    auto& toRight = m_scratch[5];
    sumTaps (m_input, m_current.early, count, to.data(), toRight.data());

    if (! m_fading)
        return;

    auto& from = m_scratch[6];
    auto& fromRight = m_scratch[7];
    sumTaps (m_input, m_earlyFrom, count, from.data(), fromRight.data());
    const auto length = static_cast<float> (m_fadeLength);

    for (size_t n = 0; n < count; ++n)
    {
        const auto f = std::min (1.0f, static_cast<float> (m_fadePosition + n + 1) / length);
        to[n] = from[n] + (to[n] - from[n]) * f;
        toRight[n] = fromRight[n] + (toRight[n] - fromRight[n]) * f;
    }

    m_fadePosition += count;
    m_fading = m_fadePosition < m_fadeLength;
}

std::array<float, 2> ReverbNode::diffuseEarly (float left, float right) noexcept
{
    const auto g = m_current.gains.scalar[scalar::earlyAllpass];
    const auto& d = m_current.delays.earlyAllpass;
    const auto settled = m_delaysSettled; // at rest every delay is whole: read it directly

    const auto& whole = m_wholeEarlyAllpass;

    for (size_t i = 0; i < 4; ++i)
    {
        left = settled ? m_earlyAllpass[i].process (left, whole[i], g) : m_earlyAllpass[i].process (left, d[i], g);
        right = settled ? m_earlyAllpass[i + 4].process (right, whole[i + 4], g)
                        : m_earlyAllpass[i + 4].process (right, d[i + 4], g);
    }

    return { left, right };
}

template <bool thin, bool damp>
void ReverbNode::absorb (std::array<float, numLines>& y) noexcept
{
    // dsp::LowShelf and dsp::HighShelf, written out over four lines at a time (revision 1.7: the same operations, in
    // the same order, as one line at a time).
    using namespace simd;
    const auto& g = m_current.gains;
    const auto aLow = broadcast (m_lineLowCoefficient);
    const auto aHigh = broadcast (m_lineHighCoefficient);
    const auto a1 = broadcast (m_lineAirCoefficients[0]);
    const auto a2 = broadcast (m_lineAirCoefficients[1]);
    const auto a3 = broadcast (m_lineAirCoefficients[2]);
    const auto one = broadcast (1.0f);
    const auto two = broadcast (2.0f);

    for (size_t i = 0; i < numLines; i += 4)
    {
        auto w = load (y, i) * load (g.line, i);

        if constexpr (thin)
        {
            const auto state = load (m_lineLowState, i);
            const auto tLow = (w - state) * aLow;
            const auto low = tLow + state;
            store (m_lineLowState, i, low + tLow);
            w = w + (load (g.lineLow, i) - one) * low;
        }

        if constexpr (damp)
        {
            const auto state = load (m_lineHighState, i);
            const auto tHigh = (w - state) * aHigh;
            const auto lowpass = tHigh + state;
            store (m_lineHighState, i, lowpass + tHigh);
            const auto gain = load (g.lineHigh, i);
            w = gain * w + (one - gain) * lowpass;
        }

        // The darkening: G + (1 - G) LP, LP a second-order Butterworth lowpass (|LP| <= 1, so passive).
        const auto first = load (m_lineAirFirst, i);
        const auto second = load (m_lineAirSecond, i);
        const auto v3 = w - second;
        const auto v1 = a1 * first + a2 * v3;
        const auto v2 = second + a2 * first + a3 * v3;
        store (m_lineAirFirst, i, two * v1 - first);
        store (m_lineAirSecond, i, two * v2 - second);
        const auto gain = load (g.lineAir, i);
        store (y, i, gain * w + (one - gain) * v2);
    }
}

std::array<float, 2> ReverbNode::processTail (float left, float right) noexcept
{
    const auto& s = m_current.gains.scalar;
    const auto diffusion = s[scalar::inputAllpass];
    const auto settled = m_delaysSettled;

    for (size_t i = 0; i < 4; ++i)
    {
        const auto dl = m_current.delays.inputAllpass[i];
        const auto dr = m_current.delays.inputAllpass[i + 4];
        left = settled ? m_inputAllpass[i].process (left, m_wholeInputAllpass[i], diffusion)
                       : m_inputAllpass[i].process (left, dl, diffusion);
        right = settled ? m_inputAllpass[i + 4].process (right, m_wholeInputAllpass[i + 4], diffusion)
                        : m_inputAllpass[i + 4].process (right, dr, diffusion);
    }

    // Read every line and its inner tap, then pass the line through its allpass (DESIGN.md 1.1). A delay of D
    // samples, read before this sample's push, is D - 1 pushes ago. At rest every delay is a whole number of samples,
    // where interpolation gives exactly the stored sample, so it is read directly. The reads go into arrays first and
    // the arithmetic runs over all lines at once, so that it vectorises (revision 1.7); each line's arithmetic is the
    // same as before, operation for operation.
    std::array<float, numLines> x {};
    std::array<float, numLines> inner {};
    std::array<float, numLines> q {};
    const auto& d = m_current.delays;

    if (m_chorusActive)
    {
        std::array<float, numLines> delay {};

        for (size_t i = 0; i < numLines; ++i)
            delay[i] = d.line[i] - 1.0f + m_chorusOffset[i];

        m_lines.readLagrange5 (delay, x);

        for (size_t i = 0; i < numLines; ++i)
            m_chorusOffset[i] += m_chorusStep[i];
    }
    else if (settled)
    {
        m_lines.read (m_wholeLine, x);
    }
    else
    {
        for (size_t i = 0; i < numLines; ++i)
            x[i] = m_lines.lagrange (i, d.line[i] - 1.0f);
    }

    if (settled)
    {
        m_lines.read (m_wholeInnerTap, inner);
        m_loopAllpass.read (m_wholeLoopAllpass, q);
    }
    else
    {
        for (size_t i = 0; i < numLines; ++i)
        {
            inner[i] = m_lines.linear (i, d.innerTap[i] - 1.0f); // outside the loop: linear is enough
            q[i] = m_loopAllpass.linear (i, d.loopAllpass[i] - 1.0f);
        }
    }

    // A normalised (orthogonal) allpass section per line: lossless however its angle moves [G71], [TXT]; its delay
    // loses what any delay of its length loses (Jot's gain), so the section is passive.
    using namespace simd;
    std::array<float, numLines> y;
    std::array<float, numLines> allpassIn;
    std::array<float, numLines> term;
    const auto tapLevel = broadcast (innerTapLevel);

    for (size_t i = 0; i < numLines; i += 4)
    {
        const auto sn = load (m_loopSin, i);
        const auto cs = load (m_loopCos, i);
        const auto xi = load (x, i);
        const auto qi = load (m_current.gains.loopAllpass, i) * load (q, i);
        store (allpassIn, i, cs * xi + sn * qi);
        const auto yi = cs * qi - sn * xi;
        store (y, i, yi);

        // Each side sums its lines' ends (after the allpass) and their inner taps [M79], [JC91]: twice the echoes,
        // and the tail starts as soon as the shortest line's inner tap delivers. The terms here, the sums below in
        // the order they always had.
        store (term, i, load (outputSign, i) * yi + tapLevel * load (innerSign, i) * load (inner, i));
    }

    m_loopAllpass.push (allpassIn);

    float outLeft = 0.0f;
    float outRight = 0.0f;

    for (size_t i = 0; i < numLines / 2; ++i)
    {
        outLeft += term[i];
        outRight += term[i + numLines / 2];
    }

    // Absorption: Jot's gain, then a low shelf (Thin), a high shelf (Damp) and a second-order high shelf (the tail's
    // own darkening) per line [JC91], [M79]. A Thin or Damp shelf whose gains are all exactly 1 for this block is an
    // exact identity, so it is skipped (render()); the output is the same.
    if (m_thinActive)
        m_dampActive ? absorb<true, true> (y) : absorb<true, false> (y);
    else
        m_dampActive ? absorb<false, true> (y) : absorb<false, false> (y);

    // The rotation network: four butterfly stages of plane rotations by the diffusion angle (DESIGN.md 1.1).
    const auto c = s[scalar::rotationCos];
    const auto sn = s[scalar::rotationSin];

    rotate (y, c, sn);

    const auto inject = broadcast (injection);
    const auto leftIn = broadcast (left);
    const auto rightIn = broadcast (right);

    for (size_t i = 0; i < numLines; i += 4)
        store (y, i, load (y, i) + load (inputSign, i) * inject * (i < numLines / 2 ? leftIn : rightIn));

    m_lines.push (y);

    // Output: mid and side, the side scaled per band, so the tail has a natural left/right correlation: about 0.92
    // below 150 Hz and, above, from 0.6 (Width 0 %) to 0.25 (100 %) (DESIGN.md 1.9).
    const auto tail = s[scalar::tail];
    // The side goes through a Linkwitz-Riley crossover (its two bands stay in phase, so the correlation moves
    // smoothly between them); the mid only needs its low end lifted a little to keep the energy, a first-order shelf.
    const auto mid = s[scalar::midHigh]
                     * m_tailMid.process (0.5f * (outLeft + outRight) * tail, s[scalar::midLow] / s[scalar::midHigh],
                                          m_tailMidCoefficient);
    const auto [sideLow, sideHigh] = m_tailSide.process (0.5f * (outLeft - outRight) * tail);
    const auto side = s[scalar::sideLow] * sideLow + s[scalar::sideHigh] * sideHigh;
    return { mid + side, mid - side };
}

void ReverbNode::render (const ProcessContext& context) noexcept
{
    // Silence (revision 1.7, SPEC 7.9). The node sleeps once the input has been exactly 0, and the output below
    // -120 dBFS, for 1 s: its state is cleared and its output is 0 until a sample that isn't 0 arrives. From that
    // sample on it runs as prepare() left it (restart()). Sleeping and waking happen at exact samples, counted one by
    // one, so the output doesn't depend on the block sizes (7.3).
    const auto& in = context.audioInputs[0];
    const auto& out = context.audioOutputs[0];
    const auto count = static_cast<size_t> (std::max (0, context.numSamples));
    size_t done = 0;

    while (done < count)
    {
        if (m_sleeping)
        {
            // Eight samples at a time while they are all 0 (a loop that vectorises), then one by one.
            auto next = done;

            while (next + 8 <= count)
            {
                auto sound = false;

                for (size_t k = 0; k < 8; ++k)
                    sound = sound | ! dsp::same (in[0][next + k], 0.0f)
                            | ! dsp::same (in[1][next + k], 0.0f); // no short circuit

                if (sound)
                    break;

                next += 8;
            }

            while (next < count && dsp::same (in[0][next], 0.0f) && dsp::same (in[1][next], 0.0f))
                ++next;

            std::fill (out[0] + done, out[0] + next, 0.0f);
            std::fill (out[1] + done, out[1] + next, 0.0f);
            done = next;

            if (done < count)
                restart();

            continue;
        }

        const AudioInput rest { in[0] + done, in[1] + done };
        const AudioOutput restOut { out[0] + done, out[1] + done };
        done += renderAwake (rest, restOut, count - done);
    }
}

size_t ReverbNode::renderAwake (const AudioInput& in, const AudioOutput& out, size_t count) noexcept
{
    updateTargets();

    // A shelf group whose current and target gains are all exactly 1 stays an identity for the whole block (targets
    // only change at block starts). Skipped, its state restarts from silence when it is needed again: its gain then
    // ramps away from 1, so the restart is scaled by almost nothing.
    const auto allOne = [] (const std::array<float, numLines>& gains)
    { return std::all_of (gains.begin(), gains.end(), [] (float v) { return dsp::same (v, 1.0f); }); };
    const auto thinActive = ! (allOne (m_current.gains.lineLow) && allOne (m_target.gains.lineLow));
    const auto dampActive = ! (allOne (m_current.gains.lineHigh) && allOne (m_target.gains.lineHigh));

    if (! thinActive)
        m_lineLowState.fill (0.0f);

    if (! dampActive)
        m_lineHighState.fill (0.0f);

    m_thinActive = thinActive;
    m_dampActive = dampActive;

    auto& [dryLeft, dryRight, wetLeft, wetRight, earlyLeft, earlyRight, fromLeft, fromRight] = m_scratch;

    // Three stages over the block. Each smoothed value belongs to exactly one stage and moves once per sample there,
    // so the result is the same as running everything sample by sample (and independent of the block size). The
    // early taps are constant within a block (they crossfade instead of gliding), so they are summed over the whole
    // block at once.
    for (size_t n = 0; n < count; ++n)
    {
        advanceInput();
        dryLeft[n] = in[0][n] * m_current.mute;
        dryRight[n] = in[1][n] * m_current.mute;
        const auto wet = processTone (dryLeft[n], dryRight[n]);
        wetLeft[n] = wet[0];
        wetRight[n] = wet[1];
    }

    sumEarly (count);

    std::array<float, 2> peak {};

    for (size_t n = 0; n < count; ++n)
    {
        advanceOutput();
        const auto early = diffuseEarly (earlyLeft[n], earlyRight[n]);
        const auto tail = processTail (wetLeft[n], wetRight[n]);

        // Pre-delay on the reverb's output, so the whole response shifts exactly (DESIGN.md 1).
        m_predelay[0].push (early[0] + tail[0]);
        m_predelay[1].push (early[1] + tail[1]);
        const auto& s = m_current.gains.scalar;
        const auto left = dryLeft[n] * s[scalar::dry] + m_predelay[0].linear (m_current.predelay) * s[scalar::wet];
        const auto right = dryRight[n] * s[scalar::dry] + m_predelay[1].linear (m_current.predelay) * s[scalar::wet];
        out[0][n] = left;
        out[1][n] = right;
        peak[0] = std::max (peak[0], std::abs (left));
        peak[1] = std::max (peak[1], std::abs (right));
    }

    for (size_t ch = 0; ch < 2; ++ch)
    {
        auto previous = m_peak[ch].load();
        while (peak[ch] > previous && ! m_peak[ch].compare_exchange_weak (previous, peak[ch]))
        {
        }
    }

    // Count the quiet samples. If the node falls asleep within this range, the rest of it is silence: its output is
    // 0 (it was below -120 dBFS) and the caller looks at its input again from there.
    for (size_t n = 0; n < count; ++n)
    {
        const auto quiet = dsp::same (in[0][n], 0.0f) && dsp::same (in[1][n], 0.0f) && std::abs (out[0][n]) < quietLevel
                           && std::abs (out[1][n]) < quietLevel;
        m_quietSamples = quiet ? m_quietSamples + 1 : 0;

        if (m_quietSamples >= m_sleepAfter)
        {
            std::fill (out[0] + n + 1, out[0] + count, 0.0f);
            std::fill (out[1] + n + 1, out[1] + count, 0.0f);
            clearState();
            m_sleeping = true;
            return n + 1;
        }
    }

    return count;
}

} // namespace dawg::reverb
