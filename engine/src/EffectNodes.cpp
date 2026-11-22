#include "dawg/BuiltinNodes.h"
#include "dawg/CompiledEngine.h"
#include "dawg/Convolver.h"
#include "dawg/Modulation.h"
#include "dawg/ModulationEffects.h"
#include "dawg/NodeRegistry.h"
#include "dawg/Realtime.h"
#include "dawg/Svf.h"
#include "dawg/TransparentAdaa.h"
#include "dawg/VarUtils.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <string_view>

// Built-in effects (docs/design/phase8-node-graph.md, G1): EQ, filter, saturator, clipper, compressor, utility, mix,
// and the modulation effects (docs/design/modulation-effects.md). Small,
// allocation-free DSP; coefficients are recomputed on the audio thread when parameters move.

namespace dawg
{

namespace
{
constexpr std::array<PortInfo, 1> stereoPort { { { PortType::audio, "Audio" } } };

// A port that is mono when everything feeding the node is (docs/design/mono-ports.md, MP1).
constexpr std::array<PortInfo, 1> adaptivePort { { { PortType::audio, "Audio", adaptiveChannels } } };
constexpr auto adaptiveModPorts = withModulationInputs (adaptivePort);

constexpr std::array<PortInfo, 4> mixInputs { {
    { PortType::audio, "In 1", adaptiveChannels },
    { PortType::audio, "In 2", adaptiveChannels },
    { PortType::audio, "In 3", adaptiveChannels },
    { PortType::audio, "In 4", adaptiveChannels },
} };

constexpr double pi = juce::MathConstants<double>::pi;
constexpr double smoothingSeconds = 0.02;

[[nodiscard]] float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db / 20.0f);
}

[[nodiscard]] float gainToDb (float gain) noexcept
{
    return 20.0f * std::log10 (std::max (gain, 1.0e-9f));
}

// Exact comparison for change detection, without tripping -Wfloat-equal.
[[nodiscard]] bool differs (float a, float b) noexcept
{
    return std::less<> {}(a, b) || std::less<> {}(b, a);
}

// The channels a node writes on output 0: both when the port is stereo (a mono-in, stereo-out effect fed mono reads
// its one channel on both sides), else the step's width (docs/design/mono-ports.md, MP6).
[[nodiscard]] size_t outputChannels (const Node& node, const ProcessContext& context) noexcept
{
    return node.descriptor().outputs[0].channels == adaptiveChannels ? static_cast<size_t> (context.channels) : 2;
}

// Copies input 0 to output 0 (bypass).
void passThrough (const Node& node, const ProcessContext& context) noexcept
{
    const auto& in = context.audioInputs[0];
    const auto& out = context.audioOutputs[0];

    for (size_t ch = 0; ch < outputChannels (node, context); ++ch)
        if (in[ch] != out[ch])
            juce::FloatVectorOperations::copy (out[ch], in[ch], context.numSamples);
}

// Base for the built-in effects: bypass passes input 0 through unchanged.
class EffectNode : public Node
{
public:
    using Node::Node;

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    void process (const ProcessContext& context) noexcept final
    {
        const juce::ScopedNoDenormals noDenormals;

        const auto bypassed = m_bypassed.load();

        // A delay line left from before the bypass would play stale audio.
        if (m_wasBypassed && ! bypassed && latencySamples() > 0)
            reset();

        m_wasBypassed = bypassed;

        if (bypassed)
            passThrough (*this, context);
        else
            render (context);
    }

protected:
    virtual void render (const ProcessContext& context) noexcept = 0;

    [[nodiscard]] float param (size_t index) const noexcept { return params().get (index); }

private:
    std::atomic<bool> m_bypassed { false };
    bool m_wasBypassed = false;
};

// ---------------------------------------------------------------------------------------------------------
// Biquad, direct form I in double precision, two channels. Direct form I keeps the signal's own history rather than
// state that depends on the coefficients, so a cut made as the reciprocal of a boost undoes it sample for sample
// even while both change (docs/design/parameter-linking.md, section 10). The EQ's bands use matched designs (M. Vicanek, "Matched Second
// Order Digital Filters", 2016): the analogue prototype's poles by impulse invariance, and zeros fitted so the
// magnitude equals the prototype's at DC, at Nyquist and at the band frequency. Unlike the bilinear (RBJ cookbook)
// designs, high bands keep their analogue shape instead of narrowing and leaning towards Nyquist.

// Second-order digital coefficients b0 + b1 z^-1 + b2 z^-2 over 1 + a1 z^-1 + a2 z^-2.
struct Coefficients
{
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// The matched design of an analogue prototype H(s) = (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0), with s normalised to
// the frequency (n and d are { n0, n1, n2 } and { d0, d1, d2 }).
[[nodiscard]] Coefficients matchedCoefficients (double sampleRate, double frequency, std::array<double, 3> n,
                                                std::array<double, 3> d, double highestMatch = 0.95 * pi) noexcept
{
    // The band frequency in radians per sample, just below Nyquist.
    const auto w0 = 2.0 * pi * std::min (frequency, 0.49 * sampleRate) / sampleRate;

    // Poles: the denominator's natural frequency and damping, mapped by impulse invariance.
    const auto natural = std::sqrt (d[0] / d[2]);
    const auto zeta = d[1] / d[2] / (2.0 * natural);
    const auto wp = w0 * natural;
    const auto decay = std::exp (-zeta * wp);
    const auto a1 = zeta <= 1.0 ? -2.0 * decay * std::cos (wp * std::sqrt (1.0 - zeta * zeta))
                                : -2.0 * decay * std::cosh (wp * std::sqrt (zeta * zeta - 1.0));
    const auto a2 = decay * decay;

    // |H(e^jw)|^2 of c0 + c1 z^-1 + c2 z^-2 is C0 phi0 + C1 phi1 + C2 phi2, with phi1 = sin^2 (w / 2),
    // phi0 = 1 - phi1, phi2 = 4 phi0 phi1, C0 = (c0 + c1 + c2)^2, C1 = (c0 - c1 + c2)^2 and C2 = -4 c0 c2.
    const auto phis = [] (double w)
    {
        const auto phi1 = std::pow (std::sin (w / 2.0), 2.0);
        return std::array { 1.0 - phi1, phi1, 4.0 * (1.0 - phi1) * phi1 };
    };
    const std::array denominator { std::pow (1.0 + a1 + a2, 2.0), std::pow (1.0 - a1 + a2, 2.0), -4.0 * a2 };
    const auto analogue = [&] (double w)
    {
        const std::complex<double> s (0.0, w / w0);
        return std::norm ((n[2] * s * s + n[1] * s + n[0]) / (d[2] * s * s + d[1] * s + d[0]));
    };
    const auto weigh = [] (const std::array<double, 3>& c, const std::array<double, 3>& phi)
    { return c[0] * phi[0] + c[1] * phi[1] + c[2] * phi[2]; };

    // The numerator's magnitude matches at DC, Nyquist and the band frequency, kept below highestMatch (and clear of
    // Nyquist, where phi2 vanishes).
    const auto wm = std::min (w0, highestMatch);
    const auto phiM = phis (wm);
    const auto b0Sq = analogue (0.0) * denominator[0];
    const auto b1Sq = analogue (pi) * denominator[1];
    const auto b2Term = (analogue (wm) * weigh (denominator, phiM) - b0Sq * phiM[0] - b1Sq * phiM[1]) / phiM[2];

    const auto r0 = std::sqrt (std::max (0.0, b0Sq));
    const auto r1 = std::sqrt (std::max (0.0, b1Sq));
    const auto w = 0.5 * (r0 + r1);
    const auto b0 = 0.5 * (w + std::sqrt (std::max (0.0, w * w + b2Term)));
    const auto b1 = 0.5 * (r0 - r1);
    const auto b2 = b0 > 0.0 ? -b2Term / (4.0 * b0) : 0.0;
    return { b0, b1, b2, a1, a2 };
}

class Biquad
{
public:
    void reset() noexcept
    {
        m_x1 = {};
        m_x2 = {};
        m_y1 = {};
        m_y2 = {};
    }

    /**
        A peaking band: gain at the frequency, bandwidth set by q. With mirrorCuts, a cut is the exact reciprocal of
        the boost by as much (docs/design/parameter-linking.md, section 10), rather than the matched design of the
        analogue cut, so a boost and a cut of the same band cancel.
    */
    void setPeak (double sampleRate, double frequency, double q, double gainDb, bool mirrorCuts = false) noexcept
    {
        const auto mirrored = mirrorCuts && gainDb < 0.0;
        const auto a = std::pow (10.0, (mirrored ? -gainDb : gainDb) / 40.0);
        use (matchedCoefficients (sampleRate, frequency, { 1.0, a / q, 1.0 }, { 1.0, 1.0 / (a * q), 1.0 }), mirrored);
    }

    /**
        A low or high shelf: gain below (low) or above (high) the frequency, half of it there. q sets how sharply it
        turns: 1/sqrt 2 is the plain shelf (slope 1), higher overshoots on both sides of the frequency.
    */
    void setShelf (double sampleRate, double frequency, double gainDb, double q, bool high,
                   bool mirrorCuts = false) noexcept
    {
        const auto mirrored = mirrorCuts && gainDb < 0.0;
        const auto a = std::pow (10.0, (mirrored ? -gainDb : gainDb) / 40.0);
        const auto root = std::sqrt (a) / q;

        // Low: A (s^2 + root s + A) / (A s^2 + root s + 1); high: A (A s^2 + root s + 1) / (s^2 + root s + A).
        if (high)
            use (matchedCoefficients (sampleRate, frequency, { a, a * root, a * a }, { a, root, 1.0 }), mirrored);
        else
            use (matchedCoefficients (sampleRate, frequency, { a * a, a * root, a }, { 1.0, root, a }), mirrored);
    }

    /**
        A tilt: half the gain (in dB) above the frequency and as much cut below it, or the other way round, flat
        there; q sets how sharply it turns, as for a shelf.
    */
    void setTilt (double sampleRate, double frequency, double gainDb, double q, bool mirrorCuts = false) noexcept
    {
        const auto mirrored = mirrorCuts && gainDb < 0.0;
        const auto a = std::pow (10.0, (mirrored ? -gainDb : gainDb) / 40.0);
        const auto root = std::sqrt (a) / q;

        // The high shelf over A: 1 / A below, A above.
        use (matchedCoefficients (sampleRate, frequency, { 1.0, root, a }, { a, root, 1.0 }), mirrored);
    }

    /**
        A notch: silence at the frequency, its width set by q.
    */
    void setNotch (double sampleRate, double frequency, double q) noexcept
    {
        setMatched (sampleRate, frequency, { 1.0, 0.0, 1.0 }, { 1.0, 1.0 / q, 1.0 });
    }

    /**
        A 12 dB/octave low-pass or high-pass with resonance q (0.7071 is Butterworth).
    */
    void setPass (double sampleRate, double frequency, double q, bool high) noexcept
    {
        if (high)
            setMatched (sampleRate, frequency, { 0.0, 0.0, 1.0 }, { 1.0, 1.0 / q, 1.0 });
        else
            setMatched (sampleRate, frequency, { 1.0, 0.0, 0.0 }, { 1.0, 1.0 / q, 1.0 });
    }

    /**
        A 6 dB/octave low-pass or high-pass: the pole by impulse invariance, and the zero fitted so the magnitude
        equals the prototype's (1 / (s + 1) or s / (s + 1)) at DC and at Nyquist.
    */
    void setFirstOrderPass (double sampleRate, double frequency, bool high) noexcept
    {
        const auto w0 = 2.0 * pi * std::min (frequency, 0.49 * sampleRate) / sampleRate;
        const auto pole = std::exp (-w0);
        const auto nyquist = pi / w0; // Nyquist in units of the frequency
        const auto atNyquist = (high ? nyquist : 1.0) / std::sqrt (1.0 + nyquist * nyquist);
        const auto atDc = high ? 0.0 : 1.0;

        // (b0 + b1 z^-1) / (1 - pole z^-1) is (b0 + b1) / (1 - pole) at DC and (b0 - b1) / (1 + pole) at Nyquist.
        const auto sum = atDc * (1.0 - pole);
        const auto difference = atNyquist * (1.0 + pole);
        set (0.5 * (sum + difference), 0.5 * (sum - difference), 0.0, 1.0, -pole, 0.0);
    }

    /**
        Sets the coefficients from an analogue prototype H(s) = (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0), with s
        normalised to the frequency; n and d are { n0, n1, n2 } and { d0, d1, d2 }.
    */
    void setMatched (double sampleRate, double frequency, std::array<double, 3> n, std::array<double, 3> d) noexcept
    {
        const auto c = matchedCoefficients (sampleRate, frequency, n, d);
        set (c.b0, c.b1, c.b2, 1.0, c.a1, c.a2);
    }

    /**
        The magnitude response at an angular frequency (radians per sample), from the coefficients in use.
    */
    [[nodiscard]] double magnitude (double w) const noexcept
    {
        const auto z1 = std::polar (1.0, -w);
        const auto z2 = z1 * z1;
        const auto numerator = m_b0 + m_b1 * z1 + m_b2 * z2;
        const auto denominator = 1.0 + m_a1 * z1 + m_a2 * z2;
        return std::abs (numerator / denominator);
    }

    /**
        Moves the state to mid and side (from left and right), or back. The filter is linear, so its state transforms
        like the signal: a section running on both channels carries on without a click when the domain changes.
    */
    void convertState (bool toMidSide) noexcept
    {
        const auto scale = toMidSide ? 0.5 : 1.0;

        for (auto* z : { &m_x1, &m_x2, &m_y1, &m_y2 })
            *z = { scale * ((*z)[0] + (*z)[1]), scale * ((*z)[0] - (*z)[1]) };
    }

    // In double precision: a low band's poles sit close to z = 1, where float coefficients and state lose enough
    // that a cut no longer undoes its boost.
    [[nodiscard]] float process (float x, size_t ch) noexcept
    {
        const double in = x;
        const auto y = m_b0 * in + m_b1 * m_x1[ch] + m_b2 * m_x2[ch] - m_a1 * m_y1[ch] - m_a2 * m_y2[ch];
        m_x2[ch] = m_x1[ch];
        m_x1[ch] = in;
        m_y2[ch] = m_y1[ch];
        m_y1[ch] = y;
        return static_cast<float> (y);
    }

private:
    // Uses a design, or its reciprocal: B / A turned into A / B. The matched design's numerator has its zeros inside
    // the unit circle (minimum phase), so the reciprocal's poles are too and it is stable.
    void use (const Coefficients& c, bool reciprocal) noexcept
    {
        if (reciprocal)
            set (1.0, c.a1, c.a2, c.b0, c.b1, c.b2);
        else
            set (c.b0, c.b1, c.b2, 1.0, c.a1, c.a2);
    }

    void set (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        m_b0 = b0 / a0;
        m_b1 = b1 / a0;
        m_b2 = b2 / a0;
        m_a1 = a1 / a0;
        m_a2 = a2 / a0;
    }

    double m_b0 = 1.0;
    double m_b1 = 0.0;
    double m_b2 = 0.0;
    double m_a1 = 0.0;
    double m_a2 = 0.0;
    std::array<double, 2> m_x1 {};
    std::array<double, 2> m_x2 {};
    std::array<double, 2> m_y1 {};
    std::array<double, 2> m_y2 {};
};

// ---------------------------------------------------------------------------------------------------------
// EQ: eight bands, each a bell, a shelf, a high- or low-pass, a notch or a tilt (docs/design/channel-strip.md, C1
// and C9). Parameters keep the ids of the bands' first types (HP, low shelf, mids 1 to 4, high shelf, LP), which are
// also each band's default type.

constexpr std::array<const char*, 8> eqSlopes { "6 dB/oct",  "12 dB/oct", "18 dB/oct", "24 dB/oct",
                                                "30 dB/oct", "36 dB/oct", "42 dB/oct", "48 dB/oct" };

constexpr std::array<const char*, 3> eqModes { "Stereo", "Mid", "Side" };

// How the EQ makes a cut (docs/design/parameter-linking.md, section 10): the matched design of the analogue cut, or the
// exact reciprocal of the boost by as much, so a boost and a cut cancel, as a linked de-emphasis needs.
constexpr std::array<const char*, 2> eqCutDesigns { "Matched", "Mirror boosts" };

constexpr std::array<const char*, 7> eqTypes { "Bell",     "Low shelf", "High shelf", "High-pass",
                                               "Low-pass", "Notch",     "Tilt" };

constexpr std::array<ParamInfo, 65> eqParams { {
    { "hpOn", "Band 1 on", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "hpFreq", "Band 1 freq", 20.0f, 20000.0f, 30.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "hpSlope", "Band 1 slope", 0.0f, 7.0f, 1.0f, ParamKind::choice, "", 0.0f, eqSlopes },
    { "lowOn", "Band 2 on", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "lowFreq", "Band 2 freq", 20.0f, 20000.0f, 120.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "lowGain", "Band 2 gain", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mid1On", "Band 3 on", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "mid1Freq", "Band 3 freq", 20.0f, 20000.0f, 250.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "mid1Gain", "Band 3 gain", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mid1Q", "Band 3 Q", 0.1f, 10.0f, 0.7f, ParamKind::continuous, "", 1.0f },
    { "mid2On", "Band 4 on", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "mid2Freq", "Band 4 freq", 20.0f, 20000.0f, 800.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "mid2Gain", "Band 4 gain", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mid2Q", "Band 4 Q", 0.1f, 10.0f, 0.7f, ParamKind::continuous, "", 1.0f },
    { "mid3On", "Band 5 on", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "mid3Freq", "Band 5 freq", 20.0f, 20000.0f, 2200.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "mid3Gain", "Band 5 gain", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mid3Q", "Band 5 Q", 0.1f, 10.0f, 0.7f, ParamKind::continuous, "", 1.0f },
    { "mid4On", "Band 6 on", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "mid4Freq", "Band 6 freq", 20.0f, 20000.0f, 5500.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "mid4Gain", "Band 6 gain", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mid4Q", "Band 6 Q", 0.1f, 10.0f, 0.7f, ParamKind::continuous, "", 1.0f },
    { "highOn", "Band 7 on", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "highFreq", "Band 7 freq", 20.0f, 20000.0f, 10000.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "highGain", "Band 7 gain", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "lpOn", "Band 8 on", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "lpFreq", "Band 8 freq", 20.0f, 20000.0f, 18000.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "lpSlope", "Band 8 slope", 0.0f, 7.0f, 1.0f, ParamKind::choice, "", 0.0f, eqSlopes },
    { "hpUsed", "Band 1 in use", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "lowUsed", "Band 2 in use", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "mid1Used", "Band 3 in use", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "mid2Used", "Band 4 in use", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "mid3Used", "Band 5 in use", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "mid4Used", "Band 6 in use", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "highUsed", "Band 7 in use", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "lpUsed", "Band 8 in use", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "lowQ", "Band 2 Q", 0.1f, 10.0f, 0.7071f, ParamKind::continuous, "", 1.0f },
    { "highQ", "Band 7 Q", 0.1f, 10.0f, 0.7071f, ParamKind::continuous, "", 1.0f },
    { "hpMode", "Band 1 channels", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, eqModes },
    { "lowMode", "Band 2 channels", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, eqModes },
    { "mid1Mode", "Band 3 channels", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, eqModes },
    { "mid2Mode", "Band 4 channels", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, eqModes },
    { "mid3Mode", "Band 5 channels", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, eqModes },
    { "mid4Mode", "Band 6 channels", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, eqModes },
    { "highMode", "Band 7 channels", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, eqModes },
    { "lpMode", "Band 8 channels", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, eqModes },
    { "hpType", "Band 1 type", 0.0f, 6.0f, 3.0f, ParamKind::choice, "", 0.0f, eqTypes },
    { "lowType", "Band 2 type", 0.0f, 6.0f, 1.0f, ParamKind::choice, "", 0.0f, eqTypes },
    { "mid1Type", "Band 3 type", 0.0f, 6.0f, 0.0f, ParamKind::choice, "", 0.0f, eqTypes },
    { "mid2Type", "Band 4 type", 0.0f, 6.0f, 0.0f, ParamKind::choice, "", 0.0f, eqTypes },
    { "mid3Type", "Band 5 type", 0.0f, 6.0f, 0.0f, ParamKind::choice, "", 0.0f, eqTypes },
    { "mid4Type", "Band 6 type", 0.0f, 6.0f, 0.0f, ParamKind::choice, "", 0.0f, eqTypes },
    { "highType", "Band 7 type", 0.0f, 6.0f, 2.0f, ParamKind::choice, "", 0.0f, eqTypes },
    { "lpType", "Band 8 type", 0.0f, 6.0f, 4.0f, ParamKind::choice, "", 0.0f, eqTypes },
    { "hpGain", "Band 1 gain", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "hpQ", "Band 1 Q", 0.1f, 10.0f, 0.7071f, ParamKind::continuous, "", 1.0f },
    { "lpGain", "Band 8 gain", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "lpQ", "Band 8 Q", 0.1f, 10.0f, 0.7071f, ParamKind::continuous, "", 1.0f },
    { "lowSlope", "Band 2 slope", 0.0f, 7.0f, 1.0f, ParamKind::choice, "", 0.0f, eqSlopes },
    { "mid1Slope", "Band 3 slope", 0.0f, 7.0f, 1.0f, ParamKind::choice, "", 0.0f, eqSlopes },
    { "mid2Slope", "Band 4 slope", 0.0f, 7.0f, 1.0f, ParamKind::choice, "", 0.0f, eqSlopes },
    { "mid3Slope", "Band 5 slope", 0.0f, 7.0f, 1.0f, ParamKind::choice, "", 0.0f, eqSlopes },
    { "mid4Slope", "Band 6 slope", 0.0f, 7.0f, 1.0f, ParamKind::choice, "", 0.0f, eqSlopes },
    { "highSlope", "Band 7 slope", 0.0f, 7.0f, 1.0f, ParamKind::choice, "", 0.0f, eqSlopes },
    { "cuts", "Cuts", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, eqCutDesigns },
} };

// A band's type, by the value of its type parameter (eqTypes).
enum class EqType : uint8_t
{
    bell,
    lowShelf,
    highShelf,
    highPass,
    lowPass,
    notch,
    tilt,
};

// The index of an EQ parameter in eqParams.
[[nodiscard]] constexpr size_t eqIndex (std::string_view id) noexcept
{
    for (size_t i = 0; i < eqParams.size(); ++i)
        if (std::string_view (eqParams[i].id) == id)
            return i;

    return eqParams.size();
}

// A band's parameters, as indices in eqParams.
struct EqBandParams
{
    size_t on, used, type, mode, freq, gain, q, slope;
};

[[nodiscard]] constexpr EqBandParams eqBandParams (std::string_view on, std::string_view used, std::string_view type,
                                                   std::string_view mode, std::string_view freq, std::string_view gain,
                                                   std::string_view q, std::string_view slope) noexcept
{
    return { eqIndex (on),   eqIndex (used), eqIndex (type), eqIndex (mode),
             eqIndex (freq), eqIndex (gain), eqIndex (q),    eqIndex (slope) };
}

constexpr std::array<EqBandParams, 8> eqBands { {
    eqBandParams ("hpOn", "hpUsed", "hpType", "hpMode", "hpFreq", "hpGain", "hpQ", "hpSlope"),
    eqBandParams ("lowOn", "lowUsed", "lowType", "lowMode", "lowFreq", "lowGain", "lowQ", "lowSlope"),
    eqBandParams ("mid1On", "mid1Used", "mid1Type", "mid1Mode", "mid1Freq", "mid1Gain", "mid1Q", "mid1Slope"),
    eqBandParams ("mid2On", "mid2Used", "mid2Type", "mid2Mode", "mid2Freq", "mid2Gain", "mid2Q", "mid2Slope"),
    eqBandParams ("mid3On", "mid3Used", "mid3Type", "mid3Mode", "mid3Freq", "mid3Gain", "mid3Q", "mid3Slope"),
    eqBandParams ("mid4On", "mid4Used", "mid4Type", "mid4Mode", "mid4Freq", "mid4Gain", "mid4Q", "mid4Slope"),
    eqBandParams ("highOn", "highUsed", "highType", "highMode", "highFreq", "highGain", "highQ", "highSlope"),
    eqBandParams ("lpOn", "lpUsed", "lpType", "lpMode", "lpFreq", "lpGain", "lpQ", "lpSlope"),
} };

static_assert (std::ranges::all_of (eqBands,
                                    [] (const EqBandParams& b)
                                    {
                                        return std::ranges::all_of (
                                            std::array { b.on, b.used, b.type, b.mode, b.freq, b.gain, b.q, b.slope },
                                            [] (size_t i) { return i < eqParams.size(); });
                                    }),
               "every EQ band has all its parameters");

const NodeDescriptor eqDescriptor { nodetype::eq, "EQ", adaptiveModPorts,    adaptivePort,
                                    eqParams,     true, NodeCategory::effect };

class EqNode final : public EffectNode
{
public:
    /**
        Sections per band: enough for an HP or LP at 48 dB/octave (eighth order, four second-order sections).
    */
    static constexpr size_t passSections = 4;

    /**
        What a section filters: both channels, or only the mid or the side (docs/design/channel-strip.md, C8).
    */
    enum class Channels : uint8_t
    {
        both,
        mid,
        side,
    };

    /**
        The filter sections: passSections for each band, of which a bell, shelf, notch or tilt uses the first.
    */
    struct Sections
    {
        std::array<Biquad, passSections * eqBands.size()> filters;
        std::array<bool, passSections * eqBands.size()> active {};
        std::array<Channels, passSections * eqBands.size()> channels {};

        /**
            Whether a running section filters only the mid or the side, so the EQ works on mid and side.
        */
        [[nodiscard]] bool midSide() const noexcept
        {
            for (size_t i = 0; i < active.size(); ++i)
                if (active[i] && channels[i] != Channels::both)
                    return true;

            return false;
        }
    };

    /**
        The eight bands' sections in `Sections::filters`, in the order of eqBands.
    */
    static constexpr std::array<std::array<int, passSections>, 8> bandSections { {
        { 0, 1, 2, 3 },
        { 4, 5, 6, 7 },
        { 8, 9, 10, 11 },
        { 12, 13, 14, 15 },
        { 16, 17, 18, 19 },
        { 20, 21, 22, 23 },
        { 24, 25, 26, 27 },
        { 28, 29, 30, 31 },
    } };

    EqNode()
        : EffectNode (eqDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        m_applied.fill (-1.0f);
        m_midSide = false;
        reset();
    }

    void reset() noexcept override
    {
        for (auto& filter : m_sections.filters)
            filter.reset();
    }

private:
    void render (const ProcessContext& context) noexcept override
    {
        updateCoefficients();

        if (context.channels == 1)
        {
            renderMono (context);
            return;
        }

        // While a band filters only the mid or the side, all of them run on mid and side: a band on both channels
        // filters the same there as on left and right.
        const auto midSide = m_sections.midSide();

        if (midSide != m_midSide)
        {
            for (auto& filter : m_sections.filters)
                filter.convertState (midSide);

            m_midSide = midSide;
        }

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];

        for (int s = 0; s < context.numSamples; ++s)
        {
            auto a = in[0][s];
            auto b = in[1][s];

            if (midSide)
            {
                const auto mid = 0.5f * (a + b);
                b = 0.5f * (a - b);
                a = mid;
            }

            for (size_t i = 0; i < m_sections.filters.size(); ++i)
            {
                if (! m_sections.active[i])
                    continue;

                auto& filter = m_sections.filters[i];
                const auto channels = m_sections.channels[i];

                if (channels != Channels::side)
                    a = filter.process (a, 0);

                if (channels != Channels::mid)
                    b = filter.process (b, 1);
            }

            if (midSide)
            {
                const auto left = a + b;
                b = a - b;
                a = left;
            }

            out[0][s] = a;
            out[1][s] = b;
        }
    }

    // A mono signal is all mid and no side: the bands on both channels and on the mid filter it, the side's do
    // nothing, as they would on a stereo signal with equal channels (docs/design/mono-ports.md, section 6).
    void renderMono (const ProcessContext& context) noexcept
    {
        const auto* in = context.audioInputs[0][0];
        auto* out = context.audioOutputs[0][0];

        for (int s = 0; s < context.numSamples; ++s)
        {
            auto x = in[s];

            for (size_t i = 0; i < m_sections.filters.size(); ++i)
                if (m_sections.active[i] && m_sections.channels[i] != Channels::side)
                    x = m_sections.filters[i].process (x, 0);

            out[s] = x;
        }
    }

    void updateCoefficients() noexcept
    {
        std::array<float, eqParams.size()> current {};

        for (size_t i = 0; i < current.size(); ++i)
            current[i] = param (i);

        if (std::ranges::equal (current, m_applied, [] (float a, float b) { return ! differs (a, b); }))
            return;

        // A section switched back on, or moved to other channels or another type, starts from silence, not from a
        // state left from long ago or from another filter.
        const auto before = m_sections.active;
        const auto channelsBefore = m_sections.channels;
        configureSections (m_sections, m_sampleRate, current);

        for (size_t i = 0; i < before.size(); ++i)
        {
            const auto band = eqBands[i / passSections];
            const auto retyped = differs (current[band.type], m_applied[band.type]);
            const auto resloped = differs (current[band.slope], m_applied[band.slope]);

            if (m_sections.active[i]
                && (! before[i] || m_sections.channels[i] != channelsBefore[i] || retyped || resloped))
                m_sections.filters[i].reset();
        }

        m_applied = current;
    }

public:
    /**
        A band's type from the parameter values.
    */
    [[nodiscard]] static EqType typeOf (const std::array<float, eqParams.size()>& values, size_t band) noexcept
    {
        return static_cast<EqType> (std::clamp (static_cast<int> (std::lround (values[eqBands[band].type])), 0, 6));
    }

    /**
        Sets the sections' coefficients from the parameter values, and which run: bands not in use or bypassed, flat
        bells, shelves and tilts, and the HP's and LP's sections beyond their slope are skipped. Also used for the
        panel's response curve.
    */
    static void configureSections (Sections& sections, double sampleRate,
                                   const std::array<float, eqParams.size()>& values) noexcept
    {
        for (size_t band = 0; band < eqBands.size(); ++band)
            configureBand (sections, sampleRate, values, band);
    }

private:
    static void configureBand (Sections& sections, double sampleRate, const std::array<float, eqParams.size()>& values,
                               size_t band) noexcept
    {
        // A band runs while it is in use (added in the panel) and on (not bypassed).
        const auto& p = eqBands[band];
        const auto running = values[p.on] > 0.5f && values[p.used] > 0.5f;
        const auto first = band * passSections;
        const double frequency = values[p.freq];
        const double gain = values[p.gain];
        const double q = values[p.q];
        const auto audible = std::abs (gain) >= 0.01;
        constexpr auto cuts = eqIndex ("cuts");
        const auto mirrorCuts = values[cuts] > 0.5f;
        auto& f = sections.filters;
        auto& active = sections.active;

        for (size_t i = first; i < first + passSections; ++i)
            active[i] = false;

        switch (typeOf (values, band))
        {
            case EqType::bell:
                f[first].setPeak (sampleRate, frequency, q, gain, mirrorCuts);
                active[first] = running && audible;
                break;
            case EqType::lowShelf:
            case EqType::highShelf:
                f[first].setShelf (sampleRate, frequency, gain, q, typeOf (values, band) == EqType::highShelf,
                                   mirrorCuts);
                active[first] = running && audible;
                break;
            case EqType::tilt:
                f[first].setTilt (sampleRate, frequency, gain, q, mirrorCuts);
                active[first] = running && audible;
                break;
            case EqType::notch:
                f[first].setNotch (sampleRate, frequency, q);
                active[first] = running;
                break;
            case EqType::highPass:
            case EqType::lowPass:
                configurePass (sections, sampleRate, first, frequency, values[p.slope],
                               typeOf (values, band) == EqType::highPass, running);
                break;
        }

        const auto mode = std::clamp (static_cast<int> (std::lround (values[p.mode])), 0, 2);

        for (size_t i = first; i < first + passSections; ++i)
            sections.channels[i] = static_cast<Channels> (mode);
    }

    // Butterworth of order 1 to 8 (6 dB/octave each, from the slope's index): a first-order section when the order
    // is odd, and a second-order section for each pair of poles, with Q 1 / (2 cos angle).
    static void configurePass (Sections& sections, double sampleRate, size_t first, double frequency, float slope,
                               bool high, bool running) noexcept
    {
        const auto order = std::clamp (static_cast<int> (std::lround (slope)), 0, 7) + 1;
        auto section = first;

        if (order % 2 == 1)
        {
            sections.filters[section].setFirstOrderPass (sampleRate, frequency, high);
            sections.active[section++] = running;
        }

        for (int pair = 1; pair <= order / 2; ++pair)
        {
            const auto angle = order % 2 == 1 ? pair * pi / order : (2 * pair - 1) * pi / (2 * order);
            sections.filters[section].setPass (sampleRate, frequency, 1.0 / (2.0 * std::cos (angle)), high);
            sections.active[section++] = running;
        }
    }

    double m_sampleRate = 48000.0;
    Sections m_sections;
    std::array<float, eqParams.size()> m_applied {};
    bool m_midSide = false;
};

// ---------------------------------------------------------------------------------------------------------
// Filter: state-variable filter (topology-preserving transform) with drive, matched to its analogue prototype.
//
// The TPT SVF stays stable while its cutoff is modulated, but as a bilinear design its response is squeezed towards
// Nyquist. Oversampled (2x, the default, or 4x) that squeeze lies above the audible range: each stage is the plain
// bilinear SVF, prewarped at the cutoff, so resonance and slope follow the analogue prototype up to 20 kHz, and Drive
// saturates at the high rate too. The halfband filters are polyphase IIRs: 4 samples of latency at 2x and 6 at 4x
// (the Saturator's linear-phase FIRs take 49 and 61). Inside a feedback loop the Filter runs without oversampling
// whatever its setting, as the halfbands' latency and uneven phase would detune a tuned loop. Without oversampling the coefficients come from the matched design (see Biquad): g and
// k put the SVF's poles on the matched poles, and the output is a fixed mix of its high-, band- and low-pass outputs that
// makes the matched numerator (the three span every second-order numerator). That keeps the filter free of latency, for
// loops, at the cost of accuracy near Nyquist. The structure, and its behaviour under modulation, stay the same.

constexpr std::array<const char*, 4> filterModes { "Low-pass", "High-pass", "Band-pass", "Notch" };

constexpr std::array<const char*, 4> filterSlopes { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" };

// The Filter's, the Saturator's and the Clipper's oversampling.
constexpr std::array<const char*, 3> oversamplingChoices { "Off", "2x", "4x" };

constexpr std::array<ParamInfo, 6> filterParams { {
    { "mode", "Mode", 0.0f, 3.0f, 0.0f, ParamKind::choice, "", 0.0f, filterModes },
    { "cutoff", "Cutoff", 20.0f, 20000.0f, 1000.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "resonance", "Resonance", 0.0f, 100.0f, 20.0f, ParamKind::continuous, "%" },
    { "drive", "Drive", 0.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "slope", "Slope", 0.0f, 3.0f, 0.0f, ParamKind::choice, "", 0.0f, filterSlopes },
    // Changes the latency, so it isn't automatable (as the Saturator's).
    { "oversampling", "Oversampling", 0.0f, 2.0f, 1.0f, ParamKind::choice, "", 0.0f, oversamplingChoices,
      ModulationPolarity::bipolar, false, true },
} };

const NodeDescriptor filterDescriptor { nodetype::filter, "Filter", adaptiveModPorts,    adaptivePort,
                                        filterParams,     true,     NodeCategory::effect };

class FilterNode final : public EffectNode
{
public:
    static constexpr size_t maxStages = 4;

    FilterNode()
        : EffectNode (filterDescriptor)
    {
        for (size_t i = 0; i < m_oversamplers.size(); ++i)
            m_oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
                2, i + 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        m_cutoff.reset (spec.sampleRate, smoothingSeconds);
        m_modulatedCutoff.reset (spec.sampleRate, modulatedCutoffSeconds);

        for (auto& oversampler : m_oversamplers)
            oversampler->initProcessing (static_cast<size_t> (std::max (spec.maxBlockSize, 1)));

        m_choice.store (effectiveChoice());
        reset();
    }

    void configure (const GraphNode& node) override
    {
        EffectNode::configure (node);
        m_choice.store (effectiveChoice());
    }

    // In a feedback loop the filter runs without oversampling: the halfbands' latency and uneven phase would put a
    // tuned loop (a plucked string) out of tune.
    void setInLoop (bool inLoop) noexcept override
    {
        m_inLoop.store (inLoop);
        m_choice.store (effectiveChoice());
    }

    // The oversampling chosen when the graph was last compiled; the audio thread switches to it at its next block.
    [[nodiscard]] int latencySamples() const noexcept override
    {
        const auto chosen = m_choice.load();
        return chosen == 0 ? 0
                           : static_cast<int> (
                                 std::lround (m_oversamplers[static_cast<size_t> (chosen - 1)]->getLatencyInSamples()));
    }

    void reset() noexcept override
    {
        m_active = m_choice.load();

        for (auto& oversampler : m_oversamplers)
            (*oversampler).reset(); // the oversampler's state, not the pointer

        m_cutoff.setCurrentAndTargetValue (param (1));
        m_modulatedCutoff.setCurrentAndTargetValue (param (1));
        m_wasModulated = false;
        m_appliedSlope = -1;
        m_svfKey = {};
        m_activeCutoff.store (param (1), std::memory_order_relaxed);
        m_activeResonance.store (param (2), std::memory_order_relaxed);
        m_activeDrive.store (param (3), std::memory_order_relaxed);
        m_activeSlope.store (param (4), std::memory_order_relaxed);
        m_activeMode.store (param (0), std::memory_order_relaxed);
        m_hasModulationInput.store (false, std::memory_order_relaxed);
        for (auto& stage : m_states)
            stage = {};
    }

    /**
        One stage's damping k. The stages' k multiply to the single stage's, so every slope has the same gain at the cutoff: -6 dB at
        resonance 0, and the same peak above it. They keep a Butterworth filter's proportions, scaled up from its own product of sqrt 2.
        Each factor of the response's denominator is then at least Butterworth's, so at resonance 0 no slope rises above 0 dB, as equal
        stages at 36 and 48 dB/octave did. The most damped stage runs first.
    */
    [[nodiscard]] static double stageDamping (double resonance, int stages, int stage) noexcept
    {
        static constexpr std::array<std::array<double, maxStages>, maxStages> butterworth { {
            { 1.414213562, 0.0, 0.0, 0.0 },
            { 1.847759065, 0.765366865, 0.0, 0.0 },
            { 1.931851653, 1.414213562, 0.517638090, 0.0 },
            { 1.961570561, 1.662939224, 1.111140466, 0.390180644 },
        } };
        const auto count = std::clamp (stages, 1, static_cast<int> (maxStages));
        const auto kSingle = std::max (0.04, 2.0 - 1.96 * resonance / 100.0);
        const auto scale = std::pow (kSingle / juce::MathConstants<double>::sqrt2, 1.0 / static_cast<double> (count));
        return butterworth[static_cast<size_t> (count - 1)][static_cast<size_t> (std::clamp (stage, 0, count - 1))]
               * scale;
    }

    /**
        The highest frequency (radians per sample) a stage's numerator is matched at: a quarter of the sample rate.
        Matched at the cutoff right up to Nyquist, as an EQ band is, each stage's impulse-invariant poles peak more than
        its analogue's between DC and the match, and a cascade adds the stages' errors up: at 20 kHz the 24 dB/oct
        low-pass rose 1.6 dB above its passband at the default resonance, and the 48 dB/oct one 4 dB. Matched lower
        down, no slope rises above its analogue curve at low resonance; near Nyquist, a high resonance's peak comes
        out somewhat lower instead. Cutoffs up to here are matched at the cutoff as before.
    */
    static constexpr double highestMatch = 0.5 * juce::MathConstants<double>::pi;

    /**
        The matched design of one stage of a mode's analogue prototype, 1, s^2, s or s^2 + 1 over s^2 + k s + 1.
    */
    [[nodiscard]] static Coefficients coefficients (int mode, double cutoff, double resonance, double sampleRate,
                                                    int stages = 1, int stage = 0) noexcept
    {
        constexpr std::array<std::array<double, 3>, 4> numerators {
            { { 1.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 }, { 0.0, 1.0, 0.0 }, { 1.0, 0.0, 1.0 } }
        };
        const auto k = stageDamping (resonance, stages, stage);
        return matchedCoefficients (sampleRate, cutoff, numerators[static_cast<size_t> (std::clamp (mode, 0, 3))],
                                    { 1.0, k, 1.0 }, highestMatch);
    }

private:
    static constexpr int coefficientInterval = 16;

    // A modulated cutoff follows its per-sample value (ModulatedParam) with only this much smoothing, so an LFO's
    // or envelope's jump stays a sharp attack. Manual and automated changes arrive once per block and keep the
    // longer smoothing, which hides the steps between blocks.
    static constexpr double modulatedCutoffSeconds = 0.001;

    [[nodiscard]] int choice() const noexcept { return std::clamp (static_cast<int> (std::lround (param (5))), 0, 2); }

    [[nodiscard]] int effectiveChoice() const noexcept { return m_inLoop.load() ? 0 : choice(); }

    void render (const ProcessContext& context) noexcept override
    {
        const auto mode = static_cast<int> (std::lround (param (0)));
        const auto resonance = static_cast<double> (param (2));
        const auto driveDb = param (3);
        const auto drive = dbToGain (driveDb);
        const auto saturate = driveDb > 0.01f;
        const auto slope = std::clamp (static_cast<int> (std::lround (param (4))), 0, static_cast<int> (maxStages - 1));
        const auto numStages = slope + 1;

        // A new oversampling starts from silence, as a new slope does.
        if (const auto chosen = m_choice.load(); chosen != m_active)
        {
            m_active = chosen;
            m_appliedSlope = -1;

            if (chosen > 0)
                (*m_oversamplers[static_cast<size_t> (chosen - 1)]).reset();
        }

        if (slope != m_appliedSlope)
        {
            for (auto& stage : m_states)
                stage = {};
            m_appliedSlope = slope;
        }

        const auto* modulated = context.modulatedValues (1);
        followModulation (modulated != nullptr);
        auto& cutoffSmoother = modulated != nullptr ? m_modulatedCutoff : m_cutoff;

        if (modulated == nullptr)
            m_cutoff.setTargetValue (param (1));

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto channels = static_cast<size_t> (context.channels);
        const auto count = static_cast<size_t> (context.numSamples);
        const auto shift = m_active;
        const auto rate = m_sampleRate * static_cast<double> (1 << shift);
        auto& svfs = m_svfs;

        // Oversampled, the stages run in place on the oversampler's buffer, a base sample's worth at a time. A mono
        // step oversamples its one channel (docs/design/mono-ports.md, MP6).
        std::array<const float*, 2> source { in[0], in[1] };
        std::array<float*, 2> destination { out[0], out[1] };
        juce::dsp::Oversampling<float>* oversampler = nullptr;

        if (shift > 0)
        {
            oversampler = m_oversamplers[static_cast<size_t> (shift - 1)].get();
            auto up =
                oversampler->processSamplesUp (juce::dsp::AudioBlock<const float> (source.data(), channels, count));

            for (size_t ch = 0; ch < channels; ++ch)
            {
                source[ch] = up.getChannelPointer (ch);
                destination[ch] = up.getChannelPointer (ch);
            }
        }

        for (int s = 0; s < context.numSamples; ++s)
        {
            if (s % coefficientInterval == 0)
            {
                const auto length = std::min (coefficientInterval, context.numSamples - s);

                if (modulated != nullptr)
                    m_modulatedCutoff.setTargetValue (modulated[s + (length / 2)]);

                const auto cutoff = cutoffSmoother.skip (length);

                // A held cutoff (and a loop's 16-sample pieces of it) keeps its coefficients: they are worked out
                // again only when what they come from changes.
                const SvfKey key { std::bit_cast<std::uint32_t> (cutoff), std::bit_cast<std::uint64_t> (resonance),
                                   mode, numStages, shift };

                if (key != m_svfKey)
                {
                    for (int st = 0; st < numStages; ++st)
                    {
                        if (shift > 0)
                        {
                            svfs[static_cast<size_t> (st)] = SvfCoefficients::bilinear (
                                static_cast<SvfMode> (std::clamp (mode, 0, 3)), static_cast<double> (cutoff),
                                stageDamping (resonance, numStages, st), rate);
                        }
                        else
                        {
                            const auto c = coefficients (mode, static_cast<double> (cutoff), resonance, m_sampleRate,
                                                         numStages, st);
                            svfs[static_cast<size_t> (st)] = SvfCoefficients::fromBiquad (c.b0, c.b1, c.b2, c.a1, c.a2);
                        }
                    }

                    m_svfKey = key;
                }
            }

            for (size_t ch = 0; ch < channels; ++ch)
                for (int i = s << shift; i < (s + 1) << shift; ++i)
                {
                    auto x = source[ch][i];

                    if (saturate)
                        x = std::tanh (x * drive);

                    for (size_t st = 0; st < static_cast<size_t> (numStages); ++st)
                        x = m_states[st][ch].process (svfs[st], x);

                    destination[ch][i] = x;
                }
        }

        if (oversampler != nullptr)
        {
            const std::array<float*, 2> outputs { out[0], out[1] };
            juce::dsp::AudioBlock<float> down (outputs.data(), channels, count);
            oversampler->processSamplesDown (down);
        }

        m_activeCutoff.store (cutoffSmoother.getCurrentValue(), std::memory_order_relaxed);
        m_activeResonance.store (static_cast<float> (resonance), std::memory_order_relaxed);
        m_activeDrive.store (driveDb, std::memory_order_relaxed);
        m_activeSlope.store (static_cast<float> (slope), std::memory_order_relaxed);
        m_activeMode.store (static_cast<float> (mode), std::memory_order_relaxed);
        m_hasModulationInput.store (modulated != nullptr, std::memory_order_relaxed);
    }

public:
    /**
        The filter's magnitude response in dB at a frequency: the matched design's, which the SVF realises exactly.
        Drive (a saturation) is left out.
    */
    [[nodiscard]] static double responseDb (int mode, float cutoff, float resonance, double sampleRate,
                                            double frequency, int stages = 1, int oversampling = 0) noexcept
    {
        const auto count = std::max (1, stages);
        const auto factor = static_cast<double> (1 << std::clamp (oversampling, 0, 2));
        const auto rate = sampleRate * factor;
        const auto z1 = std::polar (1.0, -2.0 * pi * std::min (frequency, 0.4999 * sampleRate) / rate);
        double db = 0.0;

        for (int stage = 0; stage < count; ++stage)
        {
            Coefficients c {};

            if (oversampling > 0)
            {
                // The bilinear SVF's response: its outputs' sum over the prototype's denominator.
                const auto k = stageDamping (static_cast<double> (resonance), count, stage);
                const auto g = std::tan (pi * std::min (static_cast<double> (cutoff), 0.45 * rate) / rate);
                const auto d = 1.0 + g * (g + k);
                const auto low = g * g / d;
                const auto band = g / d;
                const auto high = 1.0 / d;
                const auto a1 = 2.0 * (g * g - 1.0) / d;
                const auto a2 = (1.0 - g * k + g * g) / d;
                const auto h = mode == 1 ? high : 0.0;
                const auto l = mode == 0 ? low : 0.0;
                const auto b = mode == 2 ? band : 0.0;
                const auto n = mode == 3 ? 1.0 : 0.0;
                // HP (1 - z^-1)^2, BP g (1 - z^-2) / g, LP g^2 (1 + z^-1)^2 / g^2, notch HP + LP, each over d.
                c = { h + l + b + n * (high + low), -2.0 * h + 2.0 * l + n * (-2.0 * high + 2.0 * low),
                      h + l - b + n * (high + low), a1, a2 };
            }
            else
            {
                c = coefficients (mode, static_cast<double> (cutoff), static_cast<double> (resonance), sampleRate,
                                  count, stage);
            }

            const auto h = (c.b0 + c.b1 * z1 + c.b2 * z1 * z1) / (1.0 + c.a1 * z1 + c.a2 * z1 * z1);
            db += 20.0 * std::log10 (std::max (std::abs (h), 1.0e-9));
        }

        return db;
    }

    /**
        The filter's latest modulated parameters.
    */
    [[nodiscard]] juce::var meters() const override;

    /**
        meters() plus the response curve for those parameters, for the device panel.
    */
    [[nodiscard]] juce::var panelMeters() const override;

private:
    // Hands the cutoff over between the two smoothers when modulation starts or stops, without a jump.
    void followModulation (bool modulated) noexcept
    {
        if (modulated == m_wasModulated)
            return;

        if (modulated)
            m_modulatedCutoff.setCurrentAndTargetValue (m_cutoff.getCurrentValue());
        else
            m_cutoff.setCurrentAndTargetValue (m_modulatedCutoff.getCurrentValue());

        m_wasModulated = modulated;
    }

    double m_sampleRate = 48000.0;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> m_cutoff { 1000.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> m_modulatedCutoff { 1000.0f };
    bool m_wasModulated = false;
    int m_appliedSlope = -1;

    // What the stages' coefficients in m_svfs were worked out from; stages 0 (none) until the first block.
    struct SvfKey
    {
        std::uint32_t cutoff = 0;
        std::uint64_t resonance = 0;
        int mode = 0;
        int stages = 0;
        int shift = 0;

        [[nodiscard]] bool operator== (const SvfKey&) const = default;
    };

    std::array<SvfCoefficients, maxStages> m_svfs {};
    SvfKey m_svfKey;
    std::array<std::array<SvfState, 2>, maxStages> m_states {}; // per stage, then channel
    std::atomic<float> m_activeCutoff { 1000.0f };
    std::atomic<float> m_activeResonance { 20.0f };
    std::atomic<float> m_activeDrive { 0.0f };
    std::atomic<float> m_activeSlope { 0.0f };
    std::atomic<float> m_activeMode { 0.0f };
    std::atomic<bool> m_hasModulationInput { false };
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 2> m_oversamplers; // 2x and 4x
    std::atomic<int> m_choice { 1 };                                               // as last compiled
    std::atomic<bool> m_inLoop { false };                                          // see setInLoop()
    int m_active = 1;                                                              // audio thread
};

// ---------------------------------------------------------------------------------------------------------
// Delay and Limiter (docs/design/sampler-midi-delay-limiter.md).

// A built-in effect's value for the block: its modulated value where one arrives (per voice, nothing else carries it),
// else the parameter. Ramps inside the node smooth the block steps.
[[nodiscard]] float blockValue (const ProcessContext& context, size_t index, const ParamInfo& info,
                                float value) noexcept
{
    const auto* modulated = context.modulatedValues (index);
    return modulated != nullptr && context.numSamples > 0
               ? std::clamp (modulated[context.numSamples / 2], info.minValue, info.maxValue)
               : value;
}

constexpr float dottedEighth = 12.0f; // "1/8.", in lfoCycleChoices
constexpr float toggleThreshold = 0.5f;

constexpr std::array<ParamInfo, 11> delayParams { {
    { "sync", "Tempo sync", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "link", "Link", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
    { "pingPong", "Ping-pong", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "noteL", "Left note", 0.0f, static_cast<float> (lfoCycleChoices.size() - 1), dottedEighth, ParamKind::choice, "",
      0.0f, lfoCycleChoices },
    { "noteR", "Right note", 0.0f, static_cast<float> (lfoCycleChoices.size() - 1), dottedEighth, ParamKind::choice, "",
      0.0f, lfoCycleChoices },
    { "timeL", "Left time", 1.0f, 4000.0f, 375.0f, ParamKind::continuous, "ms", 250.0f },
    { "timeR", "Right time", 1.0f, 4000.0f, 375.0f, ParamKind::continuous, "ms", 250.0f },
    { "feedback", "Feedback", 0.0f, 100.0f, 35.0f, ParamKind::continuous, "%" },
    { "lowCut", "Low cut", 20.0f, 2000.0f, 20.0f, ParamKind::continuous, "Hz", 200.0f },
    { "highCut", "High cut", 500.0f, 20000.0f, 12000.0f, ParamKind::continuous, "Hz", 4000.0f },
    { "mix", "Wet", 0.0f, 100.0f, 30.0f, ParamKind::continuous, "%" },
} };

const NodeDescriptor delayDescriptor { nodetype::delay, "Delay", adaptiveModPorts,    stereoPort,
                                       delayParams,     true,    NodeCategory::effect };

constexpr std::array<const char*, 3> feedbackLengthChoices { "Time", "Sync", "Note" };

// A flanger moves a loop by a few milliseconds, which a depth over the whole 2 seconds couldn't set.
constexpr float feedbackTimeModulationMs = 50.0f;

constexpr std::array<ParamInfo, 8> feedbackParams { {
    { "mode", "Length", 0.0f, 2.0f, 0.0f, ParamKind::choice, "", 0.0f, feedbackLengthChoices },
    { "time",
      "Time",
      0.1f,
      2000.0f,
      100.0f,
      ParamKind::continuous,
      "ms",
      100.0f,
      {},
      ModulationPolarity::bipolar,
      true,
      false,
      feedbackTimeModulationMs },
    { "note", "Sync", 0.0f, static_cast<float> (lfoCycleChoices.size() - 1), dottedEighth, ParamKind::choice, "", 0.0f,
      lfoCycleChoices },
    { "pitch", "Pitch", -120.0f, 120.0f, 0.0f, ParamKind::continuous, "st" },
    { "tune", "Tune", -100.0f, 100.0f, 0.0f, ParamKind::continuous, "ct" },
    { "amount", "Amount", -150.0f, 150.0f, 50.0f, ParamKind::continuous, "%" },
    { "above", "Above 100 %", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "trim", "Trim", 0.0f, 16.0f, 0.0f, ParamKind::continuous, "smp" },
} };

const NodeDescriptor feedbackDescriptor { nodetype::feedback, "Feedback", adaptiveModPorts,    adaptivePort,
                                          feedbackParams,     true,       NodeCategory::effect };

// The delay line a loop closes through (docs/design/feedback-routing.md, R1, R2). In a loop it runs as two steps, one reading before
// the loop's other nodes and one writing after them; with no loop, as one step.
//
// A loop is built to grow, so what comes out is always limited and has no DC on it, and nothing invalid stays in the line (R6). A
// bypassed one breaks the loop rather than closing it with no delay (R8).
class FeedbackNode final : public Node
{
public:
    FeedbackNode()
        : Node (feedbackDescriptor)
    {
    }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        const auto longest =
            static_cast<size_t> (std::ceil (maxSeconds * spec.sampleRate)) + static_cast<size_t> (spec.maxBlockSize);
        size_t size = 1;

        while (size < longest)
            size *= 2;

        for (auto& line : m_lines)
            line.assign (size, 0.0f);

        m_mask = size - 1;
        m_glide = 1.0 - std::exp (-1.0 / (glideSeconds * spec.sampleRate));
        m_amount.reset (spec.sampleRate, smoothingSeconds);
        m_dcCoefficient =
            static_cast<float> (std::exp (-juce::MathConstants<double>::twoPi * dcBlockerHz / spec.sampleRate));
        reset();
    }

    // A voice does this at every new note, in a line of up to a megabyte. So the line isn't cleared: only what's been written since counts.
    void reset() noexcept override
    {
        m_written = 0;
        m_dcIn = {};
        m_dcOut = {};
        m_fresh = true;
        m_invalid = false;
    }

    void requestClear() noexcept override { m_clear.store (true); }

    void setLoopLatency (int samples) noexcept override { m_loopLatency.store (samples); }

    [[nodiscard]] juce::var meters() const override
    {
        const auto shortest = static_cast<double> (shortestDelay) + m_loopLatency.load();
        return makeObject (
            { { "limiting", m_limiting.exchange (false) }, { "shortestMs", shortest * 1000.0 / m_sampleRate } });
    }

    void process (const ProcessContext& context) noexcept override
    {
        const juce::ScopedNoDenormals noDenormals;
        const auto numSamples = static_cast<size_t> (context.numSamples);
        const auto writes = context.loopPart != LoopPart::read;
        const auto reads = context.loopPart != LoopPart::write;
        const auto bypassed = m_bypassed.load();

        if (bypassed || m_clear.exchange (false))
            reset();

        if (writes && ! bypassed)
            write (context, numSamples);

        if (! reads)
            return;

        if (bypassed)
        {
            for (size_t ch = 0; ch < channelsOf (context); ++ch)
                std::fill_n (context.audioOutputs[0][ch], numSamples, 0.0f);

            return;
        }

        read (context, numSamples, writes);
    }

private:
    // Mono loops run one line (docs/design/mono-ports.md, MP6).
    [[nodiscard]] size_t channelsOf (const ProcessContext& context) const noexcept
    {
        return std::min (m_lines.size(), static_cast<size_t> (context.channels));
    }

    void write (const ProcessContext& context, size_t numSamples) noexcept
    {
        for (size_t ch = 0; ch < channelsOf (context); ++ch)
        {
            const auto* in = context.audioInputs[0][ch];

            for (size_t i = 0; i < numSamples; ++i)
            {
                if (! std::isfinite (in[i]))
                {
                    reset();
                    m_invalid = true;
                    return;
                }

                m_lines[ch][(m_write + i) & m_mask] = in[i];
            }
        }

        m_write = (m_write + numSamples) & m_mask;
        m_written = std::min (m_written + numSamples, m_mask + 1);
    }

    [[nodiscard]] float valueAt (const ProcessContext& context, size_t index, size_t sample) const noexcept
    {
        const auto* modulated = context.modulatedValues (index);
        return modulated != nullptr ? modulated[sample] : param (index);
    }

    // How long the whole trip should be, in samples, and so what this node's own delay has to be.
    [[nodiscard]] double targetDelay (const ProcessContext& context, size_t sample) const noexcept
    {
        const auto mode = static_cast<int> (std::lround (param (modeParam)));
        double length = 0.0;

        if (mode == syncMode)
        {
            const auto bpm = context.transport != nullptr
                                 ? context.transport->timeMap.tempoMap().bpmAt (context.transport->startTick)
                                 : defaultBpm;
            const auto note = std::clamp (static_cast<int> (std::lround (param (noteParam))), 0,
                                          static_cast<int> (lfoCycleBeats.size() - 1));
            length = lfoCycleBeats[static_cast<size_t> (note)] * 60.0 / bpm * m_sampleRate;
        }
        else if (mode == noteMode)
        {
            const auto semitones =
                static_cast<double> (valueAt (context, pitchParam, sample)) + param (tuneParam) / 100.0;
            length = m_sampleRate / (middleCHz * std::pow (2.0, semitones / 12.0));
        }
        else
        {
            length = static_cast<double> (valueAt (context, timeParam, sample)) * 0.001 * m_sampleRate;
        }

        // A filter delays by a little, and the same at every note, so Tune can't take it out of a string, but Trim can.
        return std::clamp (length - param (trimParam) - m_loopLatency.load (std::memory_order_relaxed),
                           static_cast<double> (shortestDelay), maxSeconds * m_sampleRate);
    }

    // What the line held `back` samples ago, 1 being the newest. What's older than the last reset is silence.
    [[nodiscard]] float lineAt (size_t channel, size_t back) const noexcept
    {
        return back <= m_written ? m_lines[channel][(m_write - back) & m_mask] : 0.0f;
    }

    void read (const ProcessContext& context, size_t numSamples, bool wroteThisPiece) noexcept
    {
        // Parameters arrive after prepare(), so the first read is where the amount and length start, not a ramp to them.
        const auto limit = param (aboveParam) > 0.5f ? 1.5f : 1.0f;

        const auto amountAt = [&] (size_t sample)
        { return std::clamp (valueAt (context, amountParam, sample) / 100.0f, -limit, limit); };

        if (m_fresh)
        {
            m_amount.setCurrentAndTargetValue (amountAt (0));
            m_delay = m_target = targetDelay (context, 0);
            m_sinceTarget = 0;
        }

        m_fresh = false;

        const auto wrote = wroteThisPiece ? static_cast<double> (numSamples) : 0.0;
        auto limited = false;

        for (size_t i = 0; i < numSamples; ++i)
        {
            // Often enough for a bend, which the glide then smooths, and it needs a power of two. Counted through blocks, so that it
            // doesn't depend on how they divide up.
            if (m_sinceTarget == 0)
                m_target = targetDelay (context, i);

            m_sinceTarget = (m_sinceTarget + 1) % targetEvery;
            m_delay += (m_target - m_delay) * m_glide;
            m_amount.setTargetValue (amountAt (i));
            const auto amount = m_amount.getNextValue();

            const auto back = wrote - static_cast<double> (i) + m_delay;
            const auto whole = static_cast<size_t> (back);
            const auto f = static_cast<float> (back - static_cast<double> (whole));
            const auto fm1 = f - 1.0f;
            const auto fm2 = f - 2.0f;
            const auto fp1 = f + 1.0f;
            const auto h0 = -f * fm1 * fm2 * (1.0f / 6.0f);
            const auto h1 = fp1 * fm1 * fm2 * 0.5f;
            const auto h2 = -fp1 * f * fm2 * 0.5f;
            const auto h3 = fp1 * f * fm1 * (1.0f / 6.0f);

            for (size_t ch = 0; ch < channelsOf (context); ++ch)
            {
                const auto y = h0 * lineAt (ch, whole - 1) + h1 * lineAt (ch, whole) + h2 * lineAt (ch, whole + 1)
                               + h3 * lineAt (ch, whole + 2);
                const auto x = y * amount;
                const auto blocked = x - m_dcIn[ch] + m_dcCoefficient * m_dcOut[ch];
                m_dcIn[ch] = x;
                m_dcOut[ch] = blocked;
                limited = limited || std::abs (blocked) > 1.0f;
                context.audioOutputs[0][ch][i] = softLimit (blocked);
            }
        }

        if (limited)
            m_limiting.store (true, std::memory_order_relaxed);

        // Something on the loop went invalid. The line is clean again, but that node may not be, and may not reach the output, so
        // pass one invalid sample on: the track's check then silences the block, resets its nodes and tells the user (S1).
        if (m_invalid)
        {
            context.audioOutputs[0][0][0] = std::numeric_limits<float>::quiet_NaN();
            m_invalid = false;
        }
    }

    // Exact up to full scale, then bends towards +6 dBFS, and never gets there.
    [[nodiscard]] static float softLimit (float x) noexcept
    {
        const auto size = std::abs (x);
        return size <= 1.0f ? x : std::copysign (1.0f + std::tanh (size - 1.0f), x);
    }

    [[nodiscard]] float param (size_t index) const noexcept { return params().get (index); }

    static constexpr size_t modeParam = 0;
    static constexpr size_t timeParam = 1;
    static constexpr size_t noteParam = 2;
    static constexpr size_t pitchParam = 3;
    static constexpr size_t tuneParam = 4;
    static constexpr size_t amountParam = 5;
    static constexpr size_t aboveParam = 6;
    static constexpr size_t trimParam = 7;
    static constexpr int syncMode = 1;
    static constexpr int noteMode = 2;
    static constexpr int targetEvery = loopChunkSamples;
    static constexpr double middleCHz = 261.6255653;
    static constexpr double defaultBpm = 120.0;
    static constexpr double maxSeconds = 2.0;
    static constexpr double glideSeconds = 0.01;
    static constexpr double smoothingSeconds = 0.02;
    static constexpr double dcBlockerHz = 5.0;
    static constexpr size_t shortestDelay = loopChunkSamples + 2; // room to read between samples

    std::array<std::vector<float>, 2> m_lines;
    size_t m_mask = 0;
    size_t m_write = 0;
    size_t m_written = 0;
    double m_sampleRate = 48000.0;
    double m_delay = 0.0;
    double m_target = 0.0;
    double m_glide = 1.0;
    int m_sinceTarget = 0;
    std::atomic<int> m_loopLatency { 0 };
    juce::LinearSmoothedValue<float> m_amount;
    float m_dcCoefficient = 1.0f;
    std::array<float, 2> m_dcIn {};
    std::array<float, 2> m_dcOut {};
    bool m_fresh = true;
    bool m_invalid = false;
    std::atomic<bool> m_bypassed { false };
    std::atomic<bool> m_clear { false };
    mutable std::atomic<bool> m_limiting { false };
};

class DelayNode final : public EffectNode
{
public:
    DelayNode()
        : EffectNode (delayDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        m_size = static_cast<int> (std::ceil (maxSeconds * spec.sampleRate)) + interpolationMargin;

        for (auto& channel : m_channels)
            channel.line.assign (static_cast<size_t> (m_size), 0.0f);

        m_glide = 1.0 - std::exp (-1.0 / (glideSeconds * spec.sampleRate));
        m_feedback.reset (spec.sampleRate, smoothingSeconds);
        m_mix.reset (spec.sampleRate, smoothingSeconds);
        reset();
    }

    void reset() noexcept override
    {
        for (auto& channel : m_channels)
        {
            std::ranges::fill (channel.line, 0.0f);
            channel.low = 0.0f;
            channel.lowCut = 0.0f;
        }

        m_write = 0;
        m_snap = true;
        m_feedback.setCurrentAndTargetValue (param (7) / 100.0f);
        m_mix.setCurrentAndTargetValue (param (10) / 100.0f);
    }

private:
    static constexpr double maxSeconds = 8.0;
    static constexpr double glideSeconds = 0.1;
    static constexpr int interpolationMargin = 4;
    static constexpr int minDelaySamples = 2; // the interpolation reads a sample on either side
    static constexpr float writeLimit = 4.0f; // far above full scale: only stops overshoot from building up

    struct Channel
    {
        std::vector<float> line;
        double delay = 0.0;
        float low = 0.0f;    // the high cut's state
        float lowCut = 0.0f; // the low cut's state
    };

    [[nodiscard]] float value (const ProcessContext& context, size_t index) const noexcept
    {
        return blockValue (context, index, delayParams[index], param (index));
    }

    // The delay of one side in samples, for this block's tempo.
    [[nodiscard]] double targetDelay (const ProcessContext& context, size_t noteIndex, size_t timeIndex) const noexcept
    {
        const auto seconds = [&]
        {
            if (param (0) < toggleThreshold)
                return static_cast<double> (value (context, timeIndex)) / 1000.0;

            const auto bpm = context.transport != nullptr
                                 ? context.transport->timeMap.tempoMap().bpmAt (context.transport->startTick)
                                 : defaultBpm;
            const auto note = static_cast<size_t> (std::clamp (static_cast<int> (std::lround (param (noteIndex))), 0,
                                                               static_cast<int> (lfoCycleBeats.size() - 1)));
            return lfoCycleBeats[note] * 60.0 / bpm;
        }();

        return std::clamp (seconds * m_sampleRate, static_cast<double> (minDelaySamples),
                           static_cast<double> (m_size - interpolationMargin));
    }

    // Four-point (Catmull-Rom) interpolation behind the write position.
    [[nodiscard]] float read (const Channel& channel, double delay) const noexcept
    {
        const auto position = static_cast<double> (m_write) - delay + static_cast<double> (m_size);
        const auto whole = static_cast<int> (position);
        const auto t = static_cast<float> (position - whole);
        const auto at = [&] (int offset) { return channel.line[static_cast<size_t> ((whole + offset) % m_size)]; };
        const auto a = at (-1 + m_size);
        const auto b = at (0);
        const auto c = at (1);
        const auto d = at (2);
        return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
    }

    void render (const ProcessContext& context) noexcept override
    {
        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];

        if (m_size == 0)
        {
            passThrough (*this, context);
            return;
        }

        const auto left = targetDelay (context, 3, 5);
        const auto right = param (1) >= toggleThreshold ? left : targetDelay (context, 4, 6);

        if (m_snap)
        {
            m_channels[0].delay = left;
            m_channels[1].delay = right;
            m_snap = false;
        }

        const auto pingPong = param (2) >= toggleThreshold;
        const auto highCut =
            static_cast<float> (1.0 - std::exp (-2.0 * pi * static_cast<double> (value (context, 9)) / m_sampleRate));
        const auto lowCut =
            static_cast<float> (1.0 - std::exp (-2.0 * pi * static_cast<double> (value (context, 8)) / m_sampleRate));
        m_feedback.setTargetValue (value (context, 7) / 100.0f);
        m_mix.setTargetValue (value (context, 10) / 100.0f);

        for (int s = 0; s < context.numSamples; ++s)
        {
            const auto feedback = m_feedback.getNextValue();
            const auto mix = m_mix.getNextValue();
            const std::array targets { left, right };
            std::array<float, 2> wet {};

            for (size_t ch = 0; ch < 2; ++ch)
            {
                auto& channel = m_channels[ch];
                channel.delay += (targets[ch] - channel.delay) * m_glide;
                const auto tap = read (channel, channel.delay);
                channel.low += (tap - channel.low) * highCut;
                channel.lowCut += (channel.low - channel.lowCut) * lowCut;
                wet[ch] = channel.low - channel.lowCut;
            }

            // Ping-pong: the mono input enters left, and each side feeds the other.
            const std::array dry { in[0][s], in[1][s] };
            const std::array written { pingPong ? 0.5f * (dry[0] + dry[1]) + feedback * wet[1]
                                                : dry[0] + feedback * wet[0],
                                       pingPong ? feedback * wet[0] : dry[1] + feedback * wet[1] };

            for (size_t ch = 0; ch < 2; ++ch)
            {
                m_channels[ch].line[static_cast<size_t> (m_write)] = std::clamp (written[ch], -writeLimit, writeLimit);
                // The echoes' level: the dry sound always passes at full (an insert's Wet, not a crossfade).
                out[ch][s] = dry[ch] + wet[ch] * mix;
            }

            m_write = (m_write + 1) % m_size;
        }
    }

    static constexpr double defaultBpm = 120.0;

    double m_sampleRate = 48000.0;
    int m_size = 0;
    int m_write = 0;
    double m_glide = 1.0;
    bool m_snap = true;
    std::array<Channel, 2> m_channels;
    juce::SmoothedValue<float> m_feedback { 0.35f };
    juce::SmoothedValue<float> m_mix { 0.3f };
};

// A lookahead gain stage: a stereo delay of window - 1 samples, and the moving average over the window of the sliding
// minimum over the window of the gain each sample needs. By the time a sample leaves the delay, every value in the
// average is at or below what that sample needed, so the gain never lets it through above what it asked for, and the
// gain moves smoothly, over two windows. The Limiter's core, also the Clipper's peak guard (docs/design/clipper.md, C5).
class LookaheadGain
{
public:
    // Sizes the stage for a window of at least 2 samples; allocates.
    void prepare (int window)
    {
        m_window = std::max (2, window);
        const auto size = static_cast<size_t> (m_window);
        m_gains.assign (size, 1.0f);
        m_averaged.assign (size, 1.0f);
        m_minimumQueue.assign (size, 0);

        for (auto& line : m_lines)
            line.assign (size, 0.0f);

        reset();
    }

    void reset() noexcept
    {
        std::ranges::fill (m_gains, 1.0f);
        std::ranges::fill (m_averaged, 1.0f);

        for (auto& line : m_lines)
            std::ranges::fill (line, 0.0f);

        m_position = 0;
        m_queueHead = 0;
        m_queueCount = 0;
        m_sample = 0;
        m_sum = static_cast<double> (m_window);
        m_sinceSum = 0;
    }

    // 0 until prepared.
    [[nodiscard]] int window() const noexcept { return m_window; }

    // The delay: window - 1 samples.
    [[nodiscard]] int latencySamples() const noexcept { return std::max (0, m_window - 1); }

    // Takes a stereo sample and the gain it needs (at most 1). Returns the sample window - 1 samples older in
    // delayed, and the gain for it.
    [[nodiscard]] float push (std::array<float, 2> input, float needed, std::array<float, 2>& delayed) noexcept
    {
        const auto minimum = slidingMinimum (needed);
        const auto slot = static_cast<size_t> (m_position);
        m_sum += static_cast<double> (minimum) - static_cast<double> (m_averaged[slot]);
        m_averaged[slot] = minimum;

        if (++m_sinceSum >= sumRefreshSamples)
        {
            m_sum = 0.0;

            for (const auto gain : m_averaged)
                m_sum += static_cast<double> (gain);

            m_sinceSum = 0;
        }

        const auto gain = std::min (1.0f, static_cast<float> (m_sum / static_cast<double> (m_window)));
        const auto oldest = static_cast<size_t> ((m_position + 1) % m_window);

        for (size_t ch = 0; ch < 2; ++ch)
        {
            m_lines[ch][slot] = input[ch];
            delayed[ch] = m_lines[ch][oldest];
        }

        m_position = (m_position + 1) % m_window;
        return gain;
    }

private:
    static constexpr int sumRefreshSamples = 65536; // how often the moving average is added up afresh, against drift

    // The lowest of the last m_window values: a queue of sample numbers whose values rise, so the front is the minimum.
    [[nodiscard]] float slidingMinimum (float gain) noexcept
    {
        const auto window = static_cast<juce::int64> (m_window);
        const auto capacity = m_minimumQueue.size();
        const auto valueOf = [&] (juce::int64 sample) { return m_gains[static_cast<size_t> (sample % window)]; };
        const auto entry = [&] (size_t i) -> juce::int64& { return m_minimumQueue[(m_queueHead + i) % capacity]; };

        if (m_queueCount > 0 && entry (0) <= m_sample - window)
        {
            m_queueHead = (m_queueHead + 1) % capacity;
            --m_queueCount;
        }

        while (m_queueCount > 0 && valueOf (entry (m_queueCount - 1)) >= gain)
            --m_queueCount;

        m_gains[static_cast<size_t> (m_sample % window)] = gain;
        entry (m_queueCount) = m_sample;
        ++m_queueCount;
        ++m_sample;
        return valueOf (entry (0));
    }

    int m_window = 0;
    int m_position = 0;
    std::vector<float> m_gains;    // the needed gains over the window
    std::vector<float> m_averaged; // the sliding minimum over the window
    std::vector<juce::int64> m_minimumQueue;
    juce::int64 m_sample = 0;
    size_t m_queueHead = 0;
    size_t m_queueCount = 0;
    std::array<std::vector<float>, 2> m_lines;
    double m_sum = 0.0;
    int m_sinceSum = 0;
};

constexpr std::array<ParamInfo, 3> limiterParams { {
    { "gain", "Gain", 0.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "ceiling", "Ceiling", -24.0f, 0.0f, -0.3f, ParamKind::continuous, "dB" },
    { "release", "Release", 1.0f, 1000.0f, 100.0f, ParamKind::continuous, "ms", 100.0f },
} };

const NodeDescriptor limiterDescriptor { nodetype::limiter, "Limiter", adaptiveModPorts,    adaptivePort,
                                         limiterParams,     true,      NodeCategory::effect };

class LimiterNode final : public EffectNode
{
public:
    LimiterNode()
        : EffectNode (limiterDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        m_lookahead.prepare (static_cast<int> (std::lround (lookAheadSeconds * spec.sampleRate)) + 1);
        m_gain.reset (spec.sampleRate, smoothingSeconds);
        m_ceiling.reset (spec.sampleRate, smoothingSeconds);
        reset();
    }

    void reset() noexcept override
    {
        m_lookahead.reset();
        m_envelope = 1.0f;
        m_gain.setCurrentAndTargetValue (dbToGain (param (0)));
        m_ceiling.setCurrentAndTargetValue (dbToGain (param (1)));
        m_reduction.store (0.0f);
    }

    [[nodiscard]] int latencySamples() const noexcept override { return m_lookahead.latencySamples(); }

    [[nodiscard]] juce::var meters() const override { return makeObject ({ { "reduction", m_reduction.load() } }); }

private:
    static constexpr double lookAheadSeconds = 0.005;

    [[nodiscard]] float value (const ProcessContext& context, size_t index) const noexcept
    {
        return blockValue (context, index, limiterParams[index], param (index));
    }

    void render (const ProcessContext& context) noexcept override
    {
        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];

        if (m_lookahead.window() == 0)
        {
            passThrough (*this, context);
            return;
        }

        const auto release = static_cast<float> (
            1.0 - std::exp (-1.0 / (static_cast<double> (value (context, 2)) * 0.001 * m_sampleRate)));
        m_gain.setTargetValue (dbToGain (value (context, 0)));
        m_ceiling.setTargetValue (dbToGain (value (context, 1)));
        auto lowest = 1.0f;

        for (int s = 0; s < context.numSamples; ++s)
        {
            const auto inputGain = m_gain.getNextValue();
            const auto ceiling = m_ceiling.getNextValue();
            const std::array input { in[0][s] * inputGain, in[1][s] * inputGain };
            const auto level = std::max (std::abs (input[0]), std::abs (input[1]));
            const auto needed = level > ceiling ? ceiling / level : 1.0f;

            // Down at once, back up with the release.
            m_envelope = needed < m_envelope ? needed : m_envelope + (1.0f - m_envelope) * release;
            m_envelope = std::min (m_envelope, 1.0f);

            std::array<float, 2> delayed {};
            const auto gain = m_lookahead.push (input, m_envelope, delayed);
            lowest = std::min (lowest, gain);

            // Mono: both input channels are the one channel, and only it is written (docs/design/mono-ports.md, MP6).
            for (size_t ch = 0; ch < static_cast<size_t> (context.channels); ++ch)
                out[ch][s] = std::clamp (delayed[ch] * gain, -ceiling, ceiling);
        }

        m_reduction.store (-gainToDb (lowest));
    }

    double m_sampleRate = 48000.0;
    LookaheadGain m_lookahead;
    float m_envelope = 1.0f;
    juce::SmoothedValue<float> m_gain { 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> m_ceiling { 1.0f };
    std::atomic<float> m_reduction { 0.0f };
};

// ---------------------------------------------------------------------------------------------------------
// Saturator: tanh waveshaping with a bias for even harmonics (docs/design/saturator.md), anti-aliased by transparent
// first-order ADAA (docs/design/vicanek-adaa.md), optionally on top of 2x or 4x oversampling.
//
// Per sample, with g the drive's gain and b the bias's offset: u = g (x + b) goes through the ADAA tanh, and the wet
// signal is (that - tanh(g b)) / g, so silence stays silent and small signals keep unity gain (W3). A DC blocker takes
// the level-dependent DC the bias leaves (W6). The dry signal for Mix goes through the shaped path's linear response
// (V7), so dry and wet line up. Mix happens at the oversampled rate, Output after downsampling.

// Fills values with a parameter's per-sample value: ramped towards target when set or automated, following modulation
// closely when modulated, like the Utility's parameters (docs/design/saturator.md, W8).
void followParam (juce::SmoothedValue<float>& ramp, float* values, const ParamInfo& info, float target,
                  const float* modulated, int numSamples, float modulationCoefficient) noexcept
{
    if (modulated == nullptr)
    {
        ramp.setTargetValue (target);

        for (int s = 0; s < numSamples; ++s)
            values[s] = ramp.getNextValue();

        return;
    }

    auto value = ramp.getCurrentValue();

    for (int s = 0; s < numSamples; ++s)
    {
        value += (std::clamp (modulated[s], info.minValue, info.maxValue) - value) * modulationCoefficient;
        values[s] = value;
    }

    ramp.setCurrentAndTargetValue (value);
}

// The input level a shaper's panel marks on its curve, like the Compressor's level dot: the input's peak over each
// block, before Drive (the curves are drawn against it), held and falling with a 50 ms time constant so the dot
// moves smoothly between the panel's frames. Written by the audio thread once a block, read by the panel.
class InputLevel
{
public:
    void prepare (double sampleRate) noexcept
    {
        m_sampleRate = sampleRate;
        reset();
    }

    void reset() noexcept
    {
        m_level = 0.0f;
        m_shown.store (0.0f, std::memory_order_relaxed);
    }

    // A block's input, on its first `channels` channels; returns the level, linear.
    float add (const AudioInput& input, int channels, int numSamples) noexcept
    {
        auto peak = 0.0f;

        for (int ch = 0; ch < std::min (channels, 2); ++ch)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (input[static_cast<size_t> (ch)], numSamples);
            peak = std::max ({ peak, -range.getStart(), range.getEnd() });
        }

        const auto fall = static_cast<float> (std::exp (-numSamples / (fallSeconds * m_sampleRate)));
        m_level = std::isfinite (peak) ? std::max (peak, m_level * fall) : 0.0f;
        m_shown.store (m_level, std::memory_order_relaxed);
        return m_level;
    }

    [[nodiscard]] float load() const noexcept { return m_shown.load (std::memory_order_relaxed); }

private:
    static constexpr double fallSeconds = 0.05;

    double m_sampleRate = 48000.0;
    float m_level = 0.0f;
    std::atomic<float> m_shown { 0.0f };
};

constexpr std::array<ParamInfo, 5> saturatorParams { {
    { "drive", "Drive", 0.0f, 40.0f, 12.0f, ParamKind::continuous, "dB" },
    { "bias", "Bias", 0.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "output", "Output", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, ParamKind::continuous, "%" },
    { "oversampling", "Oversampling", 0.0f, 2.0f, 1.0f, ParamKind::choice, "", 0.0f, oversamplingChoices,
      ModulationPolarity::bipolar, false, true },
} };

const NodeDescriptor saturatorDescriptor { nodetype::saturator, "Saturator", adaptiveModPorts,    adaptivePort,
                                           saturatorParams,     true,        NodeCategory::effect };

class SaturatorNode final : public EffectNode
{
public:
    SaturatorNode()
        : EffectNode (saturatorDescriptor)
    {
        for (size_t i = 0; i < m_oversamplers.size(); ++i)
            m_oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
                2, i + 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    }

    // The drive's gain: 0 dB is the plugin's drive 1 (docs/design/saturator.md, W3).
    [[nodiscard]] static double driveGain (double db) noexcept { return 0.1 * std::pow (10.0, db / 20.0); }

    // The bias's offset, added to the input before the drive: 0 to 0.01005, as tuned in the plugin (W3).
    [[nodiscard]] static double biasOffset (double percent) noexcept { return std::exp (percent / 10000.0) - 1.0; }

    // The static curve the panel draws, without anti-aliasing, DC blocker or Output.
    [[nodiscard]] static double shape (double x, double drive, double bias, double mix) noexcept
    {
        const auto g = driveGain (drive);
        const auto b = biasOffset (bias);
        const auto wet = (std::tanh (g * (x + b)) - std::tanh (g * b)) / g;
        return mix / 100.0 * wet + (1.0 - mix / 100.0) * x;
    }

    void prepare (const ProcessSpec& spec) override
    {
        const auto maxBlockSize = static_cast<size_t> (std::max (spec.maxBlockSize, 1));

        for (auto& oversampler : m_oversamplers)
            oversampler->initProcessing (maxBlockSize);

        for (auto& ramp : m_ramps)
            ramp.reset (spec.sampleRate, smoothingSeconds);

        for (auto& values : m_values)
            values.assign (maxBlockSize, 0.0f);

        for (size_t i = 0; i < m_dcCoefficients.size(); ++i)
            m_dcCoefficients[i] =
                std::exp (-2.0 * pi * dcBlockerHz / (spec.sampleRate * factorOf (static_cast<int> (i))));

        m_modulationCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        m_input.prepare (spec.sampleRate);
        m_choice.store (choice());
        reset();
    }

    void reset() noexcept override
    {
        for (size_t i = 0; i < m_ramps.size(); ++i)
            m_ramps[i].setCurrentAndTargetValue (param (i));

        m_active = m_choice.load();
        m_channels = {};
        m_input.reset();

        for (auto& oversampler : m_oversamplers)
            (*oversampler).reset(); // the oversampler's state, not the pointer
    }

    void configure (const GraphNode& node) override
    {
        EffectNode::configure (node);
        m_choice.store (choice());
    }

    // The oversampling chosen when the graph was last compiled; the audio thread switches to it at its next block.
    [[nodiscard]] int latencySamples() const noexcept override
    {
        const auto chosen = m_choice.load();
        return chosen == 0 ? 0
                           : static_cast<int> (
                                 std::lround (m_oversamplers[static_cast<size_t> (chosen - 1)]->getLatencyInSamples()));
    }

    // The input level, linear, for the dot on the panel's curve.
    [[nodiscard]] juce::var meters() const override { return makeObject ({ { "input", m_input.load() } }); }

private:
    static constexpr double dcBlockerHz = 5.0;

    // Below this difference between two inputs, the ADAA quotient is ill-conditioned and the midpoint's tanh is used.
    static constexpr double illConditioned = 1.0e-5;

    enum Value : size_t
    {
        drive,
        bias,
        output,
        mix,
        dcOffset, // tanh(g b) / g, filled from drive and bias
    };

    struct Channel
    {
        adaa::State shaper; // the transparent ADAA's history and filters
        adaa::DryPath dry;  // the dry signal's matching linear chain
        double dcIn = 0.0;  // the DC blocker's previous input
        double dcOut = 0.0; // and output
    };

    [[nodiscard]] int choice() const noexcept { return std::clamp (static_cast<int> (std::lround (param (4))), 0, 2); }

    [[nodiscard]] static int factorOf (int chosen) noexcept { return 1 << chosen; }

    // tanh's antiderivative log cosh u, in a form that doesn't overflow for large |u|.
    [[nodiscard]] static double logCosh (double u) noexcept
    {
        const auto a = std::abs (u);
        return a + std::log1p (std::exp (-2.0 * a)) - std::numbers::ln2;
    }

    void follow (size_t index, const float* modulated, int numSamples) noexcept
    {
        followParam (m_ramps[index], m_values[index].data(), saturatorParams[index], param (index), modulated,
                     numSamples, m_modulationCoefficient);
    }

    void render (const ProcessContext& context) noexcept override
    {
        if (const auto chosen = m_choice.load(); chosen != m_active)
        {
            m_active = chosen;
            m_channels = {};
            (*m_oversamplers[static_cast<size_t> (std::max (chosen, 1) - 1)]).reset();
        }

        const auto numSamples = context.numSamples;

        for (size_t i = 0; i <= Value::mix; ++i)
            follow (i, context.modulatedValues (i), numSamples);

        // From the parameters' units to what the shaper uses: g, b, the mix as 0..1, the output gain, the DC term. The
        // drive, bias and output mostly hold still for a block, so their exponentials are worked out again only when
        // the value they come from changes (to the bit).
        const auto same = [] (float a, float b)
        { return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b); };
        float lastDrive = 0.0f;
        float lastBias = 0.0f;
        float lastOutput = 0.0f;
        std::array<float, 3> shaper {}; // g, b and the DC term for lastDrive and lastBias
        float outputGain = 0.0f;

        for (size_t s = 0; s < static_cast<size_t> (numSamples); ++s)
        {
            const auto driveValue = m_values[drive][s];
            const auto biasValue = m_values[bias][s];
            const auto outputValue = m_values[output][s];

            if (s == 0 || ! same (driveValue, lastDrive) || ! same (biasValue, lastBias))
            {
                const auto g = driveGain (driveValue);
                const auto b = biasOffset (biasValue);
                shaper = { static_cast<float> (g), static_cast<float> (b), static_cast<float> (std::tanh (g * b) / g) };
                lastDrive = driveValue;
                lastBias = biasValue;
            }

            if (s == 0 || ! same (outputValue, lastOutput))
            {
                outputGain = dbToGain (outputValue);
                lastOutput = outputValue;
            }

            m_values[drive][s] = shaper[0];
            m_values[bias][s] = shaper[1];
            m_values[dcOffset][s] = shaper[2];
            m_values[output][s] = outputGain;
            m_values[mix][s] /= 100.0f;
        }

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto count = static_cast<size_t> (numSamples);
        const auto channels = static_cast<size_t> (context.channels);
        std::ignore = m_input.add (in, context.channels, numSamples);

        if (m_active == 0)
        {
            saturate ({ in[0], in[1] }, { out[0], out[1] }, channels, numSamples, 0);
        }
        else
        {
            // A mono step oversamples its one channel (docs/design/mono-ports.md, MP6).
            auto& oversampler = *m_oversamplers[static_cast<size_t> (m_active - 1)];
            const std::array<const float*, 2> inputs { in[0], in[1] };
            auto up =
                oversampler.processSamplesUp (juce::dsp::AudioBlock<const float> (inputs.data(), channels, count));
            saturate ({ up.getChannelPointer (0), up.getChannelPointer (1) },
                      { up.getChannelPointer (0), up.getChannelPointer (1) }, channels, numSamples, m_active);

            const std::array<float*, 2> outputs { out[0], out[1] };
            juce::dsp::AudioBlock<float> down (outputs.data(), channels, count);
            oversampler.processSamplesDown (down);
        }

        for (size_t ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::multiply (out[ch], m_values[output].data(), numSamples);
    }

    // Shapes numSamples base-rate samples' worth of audio, on the first `channels` channels, at the chosen
    // oversampling, holding each base sample's parameter values across its oversampled samples. Source and
    // destination may be the same.
    void saturate (std::array<const float*, 2> source, std::array<float*, 2> destination, size_t channels,
                   int numSamples, int chosen) noexcept
    {
        const auto shift = chosen;
        const auto dcCoefficient = m_dcCoefficients[static_cast<size_t> (chosen)];
        const auto* gains = m_values[drive].data();
        const auto* offsets = m_values[bias].data();
        const auto* mixes = m_values[mix].data();
        const auto* dcOffsets = m_values[dcOffset].data();

        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto state = m_channels[ch];

            for (int i = 0; i < (numSamples << shift); ++i)
            {
                const auto s = i >> shift;
                const auto g = static_cast<double> (gains[s]);
                const auto x = static_cast<double> (source[ch][i]);

                const auto u = g * (x + static_cast<double> (offsets[s]));
                const auto shaped =
                    adaa::step (state.shaper, u, [] (double v) { return std::tanh (v); }, logCosh, illConditioned);

                const auto wet = shaped / g - static_cast<double> (dcOffsets[s]);
                const auto blocked = wet - state.dcIn + dcCoefficient * state.dcOut;
                state.dcIn = wet;
                state.dcOut = blocked;

                const auto dry = state.dry.process (x);

                const auto amount = static_cast<double> (mixes[s]);
                destination[ch][i] = static_cast<float> (amount * blocked + (1.0 - amount) * dry);
            }

            m_channels[ch] = state;
        }
    }

    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 2> m_oversamplers; // 2x and 4x
    std::array<juce::SmoothedValue<float>, 4> m_ramps;
    std::array<std::vector<float>, 5> m_values;
    std::array<double, 3> m_dcCoefficients {};
    std::array<Channel, 2> m_channels {};
    float m_modulationCoefficient = 1.0f;
    InputLevel m_input;
    std::atomic<int> m_choice { 1 };
    int m_active = 1;
};

// ---------------------------------------------------------------------------------------------------------
// Clipper: a hard clip with a quadratic knee at a ceiling (docs/design/clipper.md), anti-aliased by transparent
// first-order ADAA like the Saturator (docs/design/vicanek-adaa.md), optionally on top of 2x or 4x oversampling.
//
// Per sample, with g the drive's gain, c the ceiling and w the knee's half-width: u = g x goes through the ADAA curve
// (C3, C4). The dry signal goes through the shaped path's linear response as in the Saturator (V7), and Mix happens
// at the shaper's rate. The ADAA's output filter and the downsampling filter overshoot the ceiling a little, so the
// mixed signal at the base rate goes through the peak guard at every oversampling choice, then a clamp to +-c
// (C5, V9). Output comes last.

constexpr std::array<ParamInfo, 6> clipperParams { {
    { "drive", "Drive", -12.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "ceiling", "Ceiling", -24.0f, 0.0f, 0.0f, ParamKind::continuous, "dB" },
    { "knee", "Knee", 0.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "output", "Output", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, ParamKind::continuous, "%" },
    { "oversampling", "Oversampling", 0.0f, 2.0f, 1.0f, ParamKind::choice, "", 0.0f, oversamplingChoices,
      ModulationPolarity::bipolar, false, true },
} };

const NodeDescriptor clipperDescriptor { nodetype::clipper, "Clipper", adaptiveModPorts,    adaptivePort,
                                         clipperParams,     true,      NodeCategory::effect };

class ClipperNode final : public EffectNode
{
public:
    ClipperNode()
        : EffectNode (clipperDescriptor)
    {
        for (size_t i = 0; i < m_oversamplers.size(); ++i)
            m_oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
                2, i + 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    }

    // The curve (C3) for |x| = a, with ceiling c and knee half-width w: the line up to c - w, a quadratic knee that
    // reaches c with slope 0 at c + w, then c.
    [[nodiscard]] static double curve (double a, double c, double w) noexcept
    {
        const auto t = c - w;

        if (a <= t)
            return a;

        if (w > 0.0 && a < c + w)
            return a - (a - t) * (a - t) / (4.0 * w);

        return c;
    }

    // The curve's antiderivative for |x| = a (C4); F(x) = antiderivative(|x|) is even, as the curve is odd.
    [[nodiscard]] static double antiderivative (double a, double c, double w) noexcept
    {
        const auto t = c - w;

        if (a <= t)
            return 0.5 * a * a;

        if (w > 0.0 && a < c + w)
        {
            const auto d = a - t;
            return 0.5 * a * a - d * d * d / (12.0 * w);
        }

        return 0.5 * (c + w) * (c + w) - 2.0 * w * w / 3.0 + c * (a - c - w);
    }

    // A level in dB as a linear gain, in double: the drive's gain and the ceiling's level.
    [[nodiscard]] static double linear (double db) noexcept { return std::pow (10.0, db / 20.0); }

    // The knee's half-width for a knee in % and a ceiling c (C3).
    [[nodiscard]] static double kneeWidth (double percent, double c) noexcept { return percent / 100.0 * c; }

    // The static curve the panel draws, without anti-aliasing, with Mix and the final clamp but not Output.
    [[nodiscard]] static double shape (double x, double drive, double ceiling, double knee, double mix) noexcept
    {
        const auto c = linear (ceiling);
        const auto u = linear (drive) * x;
        const auto wet = std::copysign (curve (std::abs (u), c, kneeWidth (knee, c)), u);
        return std::clamp (mix / 100.0 * wet + (1.0 - mix / 100.0) * x, -c, c);
    }

    void prepare (const ProcessSpec& spec) override
    {
        const auto maxBlockSize = static_cast<size_t> (std::max (spec.maxBlockSize, 1));

        for (auto& oversampler : m_oversamplers)
            oversampler->initProcessing (maxBlockSize);

        for (auto& ramp : m_ramps)
            ramp.reset (spec.sampleRate, smoothingSeconds);

        for (auto& values : m_values)
            values.assign (maxBlockSize, 0.0f);

        m_sampleRate = spec.sampleRate;
        m_modulationCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        m_guard.prepare (static_cast<int> (std::lround (guardSeconds * spec.sampleRate)) + 1);
        m_input.prepare (spec.sampleRate);
        m_choice.store (choice());
        reset();
    }

    void reset() noexcept override
    {
        for (size_t i = 0; i < m_ramps.size(); ++i)
            m_ramps[i].setCurrentAndTargetValue (param (i));

        m_active = m_choice.load();
        m_channels = {};
        m_guard.reset();
        m_overDb = 0.0f;
        m_over.store (0.0f);
        m_input.reset();

        for (auto& oversampler : m_oversamplers)
            (*oversampler).reset(); // the oversampler's state, not the pointer
    }

    void configure (const GraphNode& node) override
    {
        EffectNode::configure (node);
        m_choice.store (choice());
    }

    // The oversampling chosen when the graph was last compiled, and the peak guard behind it, which runs at every
    // choice (V9); the audio thread switches to it at its next block.
    [[nodiscard]] int latencySamples() const noexcept override
    {
        const auto chosen = m_choice.load();
        const auto oversampling =
            chosen == 0 ? 0
                        : static_cast<int> (
                              std::lround (m_oversamplers[static_cast<size_t> (chosen - 1)]->getLatencyInSamples()));
        return oversampling + m_guard.latencySamples();
    }

    // How far the driven input's peak went above the ceiling, in dB, held and released (C9), and the input level,
    // linear, for the dot on the panel's curve.
    [[nodiscard]] juce::var meters() const override
    {
        return makeObject ({ { "over", m_over.load() }, { "input", m_input.load() } });
    }

private:
    // Below this difference between two inputs, the ADAA quotient is ill-conditioned and the midpoint's curve is used.
    static constexpr double illConditioned = 1.0e-5;

    // How fast the clipping meter falls, in dB per second (C9).
    static constexpr double overReleaseDbPerSecond = 20.0;

    // The peak guard's lookahead after downsampling (C5).
    static constexpr double guardSeconds = 0.001;

    enum Value : size_t
    {
        drive,   // as the drive's gain g, once converted
        ceiling, // as the ceiling's level c
        knee,    // as the knee's half-width w
        output,  // as a gain
        mix,     // as 0..1
    };

    struct Channel
    {
        adaa::State shaper; // the transparent ADAA's history and filters
        adaa::DryPath dry;  // the dry signal's matching linear chain
    };

    [[nodiscard]] int choice() const noexcept { return std::clamp (static_cast<int> (std::lround (param (5))), 0, 2); }

    void render (const ProcessContext& context) noexcept override
    {
        if (const auto chosen = m_choice.load(); chosen != m_active)
        {
            m_active = chosen;
            m_channels = {};
            m_guard.reset();
            (*m_oversamplers[static_cast<size_t> (std::max (chosen, 1) - 1)]).reset();
        }

        const auto numSamples = context.numSamples;

        for (size_t i = 0; i <= Value::mix; ++i)
            followParam (m_ramps[i], m_values[i].data(), clipperParams[i], param (i), context.modulatedValues (i),
                         numSamples, m_modulationCoefficient);

        // From the parameters' units to what the curve uses.
        for (size_t s = 0; s < static_cast<size_t> (numSamples); ++s)
        {
            const auto c = static_cast<float> (linear (m_values[ceiling][s]));
            m_values[drive][s] = static_cast<float> (linear (m_values[drive][s]));
            m_values[ceiling][s] = c;
            m_values[knee][s] = static_cast<float> (kneeWidth (m_values[knee][s], static_cast<double> (c)));
            m_values[output][s] = dbToGain (m_values[output][s]);
            m_values[mix][s] /= 100.0f;
        }

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto count = static_cast<size_t> (numSamples);
        const auto channels = static_cast<size_t> (context.channels);
        auto peak = 0.0; // the driven input's peak relative to the ceiling
        std::ignore = m_input.add (in, context.channels, numSamples);

        if (m_active == 0)
        {
            peak = clip ({ in[0], in[1] }, { out[0], out[1] }, channels, numSamples, 0);
            guard ({ out[0], out[1] }, channels, numSamples); // the output filter's overshoot (V9)
        }
        else
        {
            // A mono step oversamples its one channel (docs/design/mono-ports.md, MP6).
            auto& oversampler = *m_oversamplers[static_cast<size_t> (m_active - 1)];
            const std::array<const float*, 2> inputs { in[0], in[1] };
            auto up =
                oversampler.processSamplesUp (juce::dsp::AudioBlock<const float> (inputs.data(), channels, count));
            peak = clip ({ up.getChannelPointer (0), up.getChannelPointer (1) },
                         { up.getChannelPointer (0), up.getChannelPointer (1) }, channels, numSamples, m_active);

            const std::array<float*, 2> outputs { out[0], out[1] };
            juce::dsp::AudioBlock<float> down (outputs.data(), channels, count);
            oversampler.processSamplesDown (down);
            guard ({ out[0], out[1] }, channels, numSamples);
        }

        // The final clamp (C5), then Output.
        const auto* ceilings = m_values[ceiling].data();
        const auto* gains = m_values[output].data();

        for (size_t ch = 0; ch < channels; ++ch)
            for (size_t s = 0; s < count; ++s)
                out[ch][s] = std::clamp (out[ch][s], -ceilings[s], ceilings[s]) * gains[s];

        const auto released =
            m_overDb - static_cast<float> (overReleaseDbPerSecond * static_cast<double> (numSamples) / m_sampleRate);
        const auto blockOver = peak > 1.0 ? static_cast<float> (20.0 * std::log10 (peak)) : 0.0f;
        m_overDb = std::max ({ blockOver, released, 0.0f });
        m_over.store (m_overDb);
    }

    // The peak guard (C5): the downsampling filter rings on clipped edges, so the downsampled signal is turned down
    // smoothly, with a short lookahead and linked across the channels, wherever it rises above the ceiling. A gain
    // that moves slowly adds sidebands near the harmonics but folds nothing back, unlike a clamp.
    // A mono step guards its one channel; the second is scratch.
    void guard (std::array<float*, 2> signal, size_t channels, int numSamples) noexcept
    {
        const auto* ceilings = m_values[ceiling].data();
        auto* second = channels == 2 ? signal[1] : signal[0];

        for (size_t s = 0; s < static_cast<size_t> (numSamples); ++s)
        {
            const auto level = std::max (std::abs (signal[0][s]), std::abs (second[s]));
            const auto needed = level > ceilings[s] ? ceilings[s] / level : 1.0f;
            std::array<float, 2> delayed {};
            const auto gain = m_guard.push ({ signal[0][s], second[s] }, needed, delayed);
            signal[0][s] = delayed[0] * gain;

            if (channels == 2)
                signal[1][s] = delayed[1] * gain;
        }
    }

    // Clips numSamples base-rate samples' worth of audio, on the first `channels` channels, at the chosen
    // oversampling, holding each base sample's parameter values across its oversampled samples, and mixes in the dry
    // path. Source and destination may be the same. Returns the driven input's peak relative to the ceiling.
    double clip (std::array<const float*, 2> source, std::array<float*, 2> destination, size_t channels, int numSamples,
                 int chosen) noexcept
    {
        const auto shift = chosen;
        const auto* gains = m_values[drive].data();
        const auto* ceilings = m_values[ceiling].data();
        const auto* knees = m_values[knee].data();
        const auto* mixes = m_values[mix].data();
        auto peak = 0.0;

        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto state = m_channels[ch];

            for (int i = 0; i < (numSamples << shift); ++i)
            {
                const auto s = static_cast<size_t> (i >> shift);
                const auto c = static_cast<double> (ceilings[s]);
                const auto w = static_cast<double> (knees[s]);
                const auto x = static_cast<double> (source[ch][i]);
                const auto u = static_cast<double> (gains[s]) * x;

                // The history is taken again with this sample's curve, so a moving ceiling or knee never makes the
                // output jump (C4, V5). The output filter may overshoot the ceiling a little; the peak guard after
                // the shaper catches it (C5, V9).
                const auto meanOf = [c, w] (double a, double b)
                {
                    return adaa::mean (
                        a, b, [c, w] (double v) { return std::copysign (curve (std::abs (v), c, w), v); },
                        [c, w] (double v) { return antiderivative (std::abs (v), c, w); }, illConditioned);
                };
                const auto in = adaa::input (state.shaper, u);
                auto second = 0.0;
                const auto shaped = adaa::shaped (state.shaper, in, meanOf, true, second);
                adaa::advance (state.shaper, in, second);
                const auto wet = state.shaper.post.process (shaped);
                peak = std::max (peak, std::abs (u) / c);

                const auto dry = state.dry.process (x);

                const auto amount = static_cast<double> (mixes[s]);
                destination[ch][i] = static_cast<float> (amount * wet + (1.0 - amount) * dry);
            }

            m_channels[ch] = state;
        }

        return peak;
    }

    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 2> m_oversamplers; // 2x and 4x
    std::array<juce::SmoothedValue<float>, 5> m_ramps;
    std::array<std::vector<float>, 5> m_values;
    std::array<Channel, 2> m_channels {};
    LookaheadGain m_guard;
    double m_sampleRate = 48000.0;
    float m_modulationCoefficient = 1.0f;
    float m_overDb = 0.0f;
    std::atomic<float> m_over { 0.0f };
    InputLevel m_input;
    std::atomic<int> m_choice { 1 };
    int m_active = 1;
};

// ---------------------------------------------------------------------------------------------------------
// Waveshaper: a choice of curves (docs/design/waveshaper.md), from a soft curve through folds and rectifiers to one
// drawn on the panel, anti-aliased by transparent first-order ADAA (docs/design/vicanek-adaa.md) with a closed-form
// antiderivative per curve, optionally on top of 2x, 4x or 8x oversampling.
//
// Per sample, with g the drive's gain and b the bias: u = g x + b goes through the ADAA curve (S3, S4), and the wet
// signal is that minus f(b), so silence stays silent (S7). A DC blocker takes the DC the asymmetric curves leave, a
// tilt after the shaper sets the tone (S8), and the dry signal for Mix goes through the shaped path's linear response
// as in the Saturator (V7). A change of curve, chosen or drawn, crossfades over 20 ms (S10).

constexpr std::array<const char*, 4> waveshaperOversamplingChoices { "Off", "2x", "4x", "8x" };
constexpr std::array<const char*, 7> waveshaperShapes { "Soft",         "Tube",         "Fold", "Sine Fold",
                                                        "Half Rectify", "Full Rectify", "Drawn" };

constexpr std::array<ParamInfo, 8> waveshaperParams { {
    { "shape", "Shape", 0.0f, 6.0f, 6.0f, ParamKind::choice, "", 0.0f, waveshaperShapes },
    { "drive", "Drive", -12.0f, 36.0f, 6.0f, ParamKind::continuous, "dB" },
    { "bias", "Bias", -100.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "tone", "Tone", -12.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
    { "output", "Output", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, ParamKind::continuous, "%" },
    { "oversampling", "Oversampling", 0.0f, 3.0f, 1.0f, ParamKind::choice, "", 0.0f, waveshaperOversamplingChoices,
      ModulationPolarity::bipolar, false, true },
    { "symmetric", "Symmetric", 0.0f, 1.0f, 1.0f, ParamKind::toggle },
} };

const NodeDescriptor waveshaperDescriptor { nodetype::waveshaper, "Waveshaper", adaptiveModPorts,    adaptivePort,
                                            waveshaperParams,     true,         NodeCategory::effect };

} // namespace

namespace waveshaper
{
double curve (Shape shape, double u, const DrawnTable* table) noexcept
{
    switch (shape)
    {
        case Shape::soft:
            return u / (1.0 + std::abs (u));
        case Shape::tube:
            return u >= 0.0 ? -std::expm1 (-u) : 2.0 * std::expm1 (0.5 * u);
        case Shape::fold:
        {
            auto p = u + 1.0;
            p -= 4.0 * std::floor (p / 4.0);
            return 1.0 - std::abs (p - 2.0);
        }
        case Shape::sineFold:
            return std::sin (u);
        case Shape::halfRectify:
            return u > 0.0 ? u / (1.0 + u) : 0.0;
        case Shape::fullRectify:
            return std::abs (u) / (1.0 + std::abs (u));
        case Shape::drawn:
            break;
    }

    if (table == nullptr)
        return std::clamp (u, -1.0, 1.0);

    const auto position = (u + 1.0) * DrawnTable::segmentsPerUnit;

    if (position <= 0.0)
        return table->y.front();

    if (position >= static_cast<double> (DrawnTable::segments))
        return table->y.back();

    const auto k = static_cast<size_t> (position);
    const auto t = position - static_cast<double> (k);
    return table->y[k] + t * (table->y[k + 1] - table->y[k]);
}

double antiderivative (Shape shape, double u, const DrawnTable* table) noexcept
{
    switch (shape)
    {
        case Shape::soft:
            return std::abs (u) - std::log1p (std::abs (u));
        case Shape::tube:
            return u >= 0.0 ? u + std::expm1 (-u) : 4.0 * std::expm1 (0.5 * u) - 2.0 * u;
        case Shape::fold:
        {
            // A triangle has zero mean over its period, so the antiderivative is periodic and stays bounded.
            auto p = u + 1.0;
            p -= 4.0 * std::floor (p / 4.0);
            const auto g = p < 2.0 ? 0.5 * p * p - p : 3.0 * (p - 2.0) - 0.5 * (p * p - 4.0);
            return g + 0.5;
        }
        case Shape::sineFold:
            return 1.0 - std::cos (u);
        case Shape::halfRectify:
            return u > 0.0 ? u - std::log1p (u) : 0.0;
        case Shape::fullRectify:
            return std::copysign (std::abs (u) - std::log1p (std::abs (u)), u);
        case Shape::drawn:
            break;
    }

    if (table == nullptr)
    {
        const auto a = std::abs (u);
        return a <= 1.0 ? 0.5 * u * u : a - 0.5;
    }

    constexpr auto h = 1.0 / DrawnTable::segmentsPerUnit;
    const auto position = (u + 1.0) * DrawnTable::segmentsPerUnit;

    if (position <= 0.0)
        return table->integral.front() + table->y.front() * (u + 1.0);

    if (position >= static_cast<double> (DrawnTable::segments))
        return table->integral.back() + table->y.back() * (u - 1.0);

    const auto k = static_cast<size_t> (position);
    const auto t = position - static_cast<double> (k);
    return table->integral[k] + h * (table->y[k] * t + 0.5 * (table->y[k + 1] - table->y[k]) * t * t);
}

double antialiased (Shape shape, double u, double previous, const DrawnTable* table) noexcept
{
    const auto difference = u - previous;
    const auto mean = 0.5 * (u + previous);

    // (cos u1 - cos u) / (u - u1) = sin m sin h / h: exact and well-conditioned everywhere (S4).
    if (shape == Shape::sineFold)
    {
        const auto half = 0.5 * difference;
        const auto sinc = std::abs (half) < 1.0e-8 ? 1.0 : std::sin (half) / half;
        return std::sin (mean) * sinc;
    }

    if (std::abs (difference) < illConditioned)
        return curve (shape, mean, table);

    return (antiderivative (shape, u, table) - antiderivative (shape, previous, table)) / difference;
}

void DrawnTable::fill (const Curve& points, bool symmetric)
{
    // The curve at u, from -1 to 1 over the curve's times.
    const auto at = [&points] (double u)
    {
        return points.empty()
                   ? u
                   : std::clamp (
                         evaluateCurve (points, 0.5 * (u + 1.0) * static_cast<double> (modulationShapeTicks), 0.0),
                         -1.0, 1.0);
    };

    for (size_t i = 0; i <= segments; ++i)
    {
        const auto u = -1.0 + static_cast<double> (i) / segmentsPerUnit;
        y[i] = symmetric && u < 0.0 ? -at (-u) : at (u);
    }

    integral[0] = 0.0;

    for (size_t i = 0; i < segments; ++i)
        integral[i + 1] = integral[i] + 0.5 * (y[i] + y[i + 1]) / segmentsPerUnit;
}
} // namespace waveshaper

namespace
{
class WaveshaperNode final : public EffectNode
{
public:
    using Shape = waveshaper::Shape;
    using DrawnTable = waveshaper::DrawnTable;

    WaveshaperNode()
        : EffectNode (waveshaperDescriptor)
    {
        for (size_t i = 0; i < m_oversamplers.size(); ++i)
            m_oversamplers[i] = std::make_unique<juce::dsp::Oversampling<float>> (
                2, i + 1, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);

        // The identity until a drawn curve is published.
        for (auto& table : m_drawn)
            table.fill ({});
    }

    // The static curve the panel draws, without anti-aliasing, DC blocker, tone or Output; the drawn curve as the
    // identity (the panel computes that one itself, from the points it has).
    [[nodiscard]] static double shape (double x, int shapeIndex, double drive, double bias, double mix) noexcept
    {
        const auto chosen = shapeOf (static_cast<float> (shapeIndex));
        const auto g = std::pow (10.0, drive / 20.0);
        const auto b = bias / 100.0;
        const auto wet = waveshaper::curve (chosen, g * x + b, nullptr) - waveshaper::curve (chosen, b, nullptr);
        return mix / 100.0 * wet + (1.0 - mix / 100.0) * x;
    }

    void prepare (const ProcessSpec& spec) override
    {
        const auto maxBlockSize = static_cast<size_t> (std::max (spec.maxBlockSize, 1));

        for (auto& oversampler : m_oversamplers)
            oversampler->initProcessing (maxBlockSize);

        for (auto& ramp : m_ramps)
            ramp.reset (spec.sampleRate, smoothingSeconds);

        for (auto& values : m_values)
            values.assign (maxBlockSize, 0.0f);

        for (size_t i = 0; i < m_dcCoefficients.size(); ++i)
        {
            const auto rate = spec.sampleRate * factorOf (static_cast<int> (i));
            m_dcCoefficients[i] = std::exp (-2.0 * pi * dcBlockerHz / rate);
            m_toneCoefficients[i] = 1.0 - std::exp (-2.0 * pi * tiltHz / rate);
        }

        m_fadeLength = std::max (1, static_cast<int> (std::lround (smoothingSeconds * spec.sampleRate)));
        m_modulationCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        m_input.prepare (spec.sampleRate);
        m_choice.store (choice());
        reset();
    }

    void reset() noexcept override
    {
        for (size_t i = 1; i < m_ramps.size(); ++i)
            m_ramps[i].setCurrentAndTargetValue (param (i));

        m_active = m_choice.load();
        clearChannels();
        m_input.reset();
        m_shaperInput.store (0.0f);
        m_shaped.store (0.0f);

        // The newest drawn curve, taken over without a fade.
        m_symmetric = symmetric();

        if (const auto* tables = m_tables.acquire(); tables != nullptr)
        {
            m_seen = tables;
            m_drawn[0] = m_symmetric ? tables->mirrored : tables->full;
        }

        m_to = { shapeOf (param (0)), 0 };
        m_from = m_to;
        m_fade = 0;

        for (auto& oversampler : m_oversamplers)
            (*oversampler).reset(); // the oversampler's state, not the pointer
    }

    void configure (const GraphNode& node) override
    {
        EffectNode::configure (node);
        m_choice.store (choice());

        // A new table only when the curve changed, so recompiling for other reasons doesn't fade (S6, S10).
        if (! m_published || ! sameCurve (node.modulationShape, m_publishedCurve))
        {
            auto tables = std::make_shared<waveshaper::DrawnTables>();
            tables->full.fill (node.modulationShape);
            tables->mirrored.fill (node.modulationShape, true);
            m_tables.publish (std::move (tables));
            m_publishedCurve = node.modulationShape;
            m_published = true;
        }
    }

    // The oversampling chosen when the graph was last compiled; the audio thread switches to it at its next block.
    [[nodiscard]] int latencySamples() const noexcept override
    {
        const auto chosen = m_choice.load();
        return chosen == 0 ? 0
                           : static_cast<int> (
                                 std::lround (m_oversamplers[static_cast<size_t> (chosen - 1)]->getLatencyInSamples()));
    }

    // For the dot on the panel's curve: the input level, linear; the shaper's input u it gives at the current Drive
    // and Bias; and the running curve at u, which the drawn shape's plot is drawn over.
    [[nodiscard]] juce::var meters() const override
    {
        return makeObject (
            { { "input", m_input.load() }, { "shaperInput", m_shaperInput.load() }, { "shaped", m_shaped.load() } });
    }

private:
    static constexpr double dcBlockerHz = 5.0;

    // The tone tilt's turnover frequency (S8).
    static constexpr double tiltHz = 800.0;

    enum Value : size_t
    {
        drive = 1, // as the drive's gain g, once converted
        bias,      // as the offset b
        tone,      // as the high part's gain; the low part's is its inverse
        output,    // as a gain
        mix,       // as 0..1
        count,
    };

    // A curve the shaper can run: a shape and, for the drawn one, which of the two tables.
    struct Running
    {
        Shape shape = Shape::fold;
        size_t table = 0;
    };

    struct Channel
    {
        adaa::State shaper; // the transparent ADAA's history and filters
        adaa::DryPath dry;  // the dry signal's matching linear chain
        double dcIn = 0.0;  // the DC blocker's previous input
        double dcOut = 0.0; // and output
        double low = 0.0;   // the tone tilt's low part
        bool fresh = true;  // the history is set at the next sample, from the bias and the curve then running
    };

    [[nodiscard]] static Shape shapeOf (float value) noexcept
    {
        return static_cast<Shape> (std::clamp (static_cast<int> (std::lround (value)), 0, 6));
    }

    [[nodiscard]] int choice() const noexcept { return std::clamp (static_cast<int> (std::lround (param (6))), 0, 3); }

    [[nodiscard]] bool symmetric() const noexcept { return param (7) >= toggleThreshold; }

    [[nodiscard]] static int factorOf (int chosen) noexcept { return 1 << chosen; }

    // Clears the history. At the next sample it starts where a constant bias has left it, with the curve running
    // then, so silence stays silent from the first sample.
    void clearChannels() noexcept { m_channels = {}; }

    [[nodiscard]] static bool sameCurve (const Curve& a, const Curve& b) noexcept
    {
        return std::ranges::equal (a, b,
                                   [] (const CurvePoint& p, const CurvePoint& q)
                                   {
                                       return juce::exactlyEqual (p.time, q.time)
                                              && juce::exactlyEqual (p.value, q.value) && p.shape == q.shape
                                              && juce::exactlyEqual (p.tension, q.tension)
                                              && juce::exactlyEqual (p.handleTime, q.handleTime)
                                              && juce::exactlyEqual (p.handleValue, q.handleValue);
                                   });
    }

    [[nodiscard]] const DrawnTable* tableOf (const Running& running) const noexcept
    {
        return running.shape == Shape::drawn ? &m_drawn[running.table] : nullptr;
    }

    // Starts a fade from the running curve to a new one (S10).
    void fadeTo (Running next) noexcept
    {
        m_from = m_to;
        m_to = next;
        m_fade = m_fadeLength;
    }

    // Takes over a new shape choice, a new drawn curve or a change of symmetry at the start of a block. A change while
    // a fade runs waits for it to end (at most 20 ms), so the fade never drops a curve that is still audible.
    void followCurve() noexcept
    {
        if (m_fade > 0)
            return;

        const auto* tables = m_tables.acquire();

        if (tables != nullptr && (tables != m_seen || symmetric() != m_symmetric))
        {
            m_seen = tables;
            m_symmetric = symmetric();

            // The new table goes into the slot the running drawn curve doesn't use.
            const size_t slot = m_to.shape == Shape::drawn ? 1 - m_to.table : m_to.table;
            m_drawn[slot] = m_symmetric ? tables->mirrored : tables->full;

            if (m_to.shape == Shape::drawn)
                fadeTo ({ Shape::drawn, slot });
            else
                m_to.table = slot;
        }

        if (const auto chosen = shapeOf (param (0)); m_fade == 0 && chosen != m_to.shape)
            fadeTo ({ chosen, m_to.table });
    }

    void render (const ProcessContext& context) noexcept override
    {
        if (const auto chosen = m_choice.load(); chosen != m_active)
        {
            m_active = chosen;
            clearChannels();
            (*m_oversamplers[static_cast<size_t> (std::max (chosen, 1) - 1)]).reset();
        }

        followCurve();

        const auto numSamples = context.numSamples;

        for (size_t i = drive; i < count; ++i)
            followParam (m_ramps[i], m_values[i].data(), waveshaperParams[i], param (i), context.modulatedValues (i),
                         numSamples, m_modulationCoefficient);

        // From the parameters' units to what the shaper uses.
        for (size_t s = 0; s < static_cast<size_t> (numSamples); ++s)
        {
            m_values[drive][s] = dbToGain (m_values[drive][s]);
            m_values[bias][s] /= 100.0f;
            m_values[tone][s] = dbToGain (0.5f * m_values[tone][s]);
            m_values[output][s] = dbToGain (m_values[output][s]);
            m_values[mix][s] /= 100.0f;
        }

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto samples = static_cast<size_t> (numSamples);
        const auto channels = static_cast<size_t> (context.channels);

        if (numSamples > 0)
        {
            const auto last = samples - 1;
            const auto u =
                m_values[drive][last] * m_input.add (in, context.channels, numSamples) + m_values[bias][last];
            m_shaperInput.store (u);
            m_shaped.store (
                static_cast<float> (waveshaper::curve (m_to.shape, static_cast<double> (u), tableOf (m_to))));
        }

        if (m_active == 0)
        {
            run ({ in[0], in[1] }, { out[0], out[1] }, channels, numSamples, 0);
        }
        else
        {
            // A mono step oversamples its one channel (docs/design/mono-ports.md, MP6).
            auto& oversampler = *m_oversamplers[static_cast<size_t> (m_active - 1)];
            const std::array<const float*, 2> inputs { in[0], in[1] };
            auto up =
                oversampler.processSamplesUp (juce::dsp::AudioBlock<const float> (inputs.data(), channels, samples));
            run ({ up.getChannelPointer (0), up.getChannelPointer (1) },
                 { up.getChannelPointer (0), up.getChannelPointer (1) }, channels, numSamples, m_active);

            const std::array<float*, 2> outputs { out[0], out[1] };
            juce::dsp::AudioBlock<float> down (outputs.data(), channels, samples);
            oversampler.processSamplesDown (down);
        }

        for (size_t ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::multiply (out[ch], m_values[output].data(), numSamples);

        m_fade = std::max (0, m_fade - numSamples);
    }

    // Shapes numSamples base-rate samples' worth of audio, on the first `channels` channels, at the chosen
    // oversampling, holding each base sample's parameter values across its oversampled samples. Source and
    // destination may be the same.
    void run (std::array<const float*, 2> source, std::array<float*, 2> destination, size_t channels, int numSamples,
              int chosen) noexcept
    {
        const auto shift = chosen;
        const auto dcCoefficient = m_dcCoefficients[static_cast<size_t> (chosen)];
        const auto toneCoefficient = m_toneCoefficients[static_cast<size_t> (chosen)];
        const auto* gains = m_values[drive].data();
        const auto* offsets = m_values[bias].data();
        const auto* tones = m_values[tone].data();
        const auto* mixes = m_values[mix].data();
        const auto* toTable = tableOf (m_to);
        const auto* fromTable = tableOf (m_from);

        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto state = m_channels[ch];

            for (int i = 0; i < (numSamples << shift); ++i)
            {
                const auto s = static_cast<size_t> (i >> shift);
                const auto b = static_cast<double> (offsets[s]);
                const auto x = static_cast<double> (source[ch][i]);
                const auto u = static_cast<double> (gains[s]) * x + b;

                if (state.fresh)
                {
                    adaa::settle (state.shaper, b, waveshaper::curve (m_to.shape, b, toTable));
                    state.fresh = false;
                }

                const auto in = adaa::input (state.shaper, u);
                const auto toMean = [this, toTable] (double from, double to)
                { return waveshaper::antialiased (m_to.shape, to, from, toTable); };
                const auto remaining = m_fade - static_cast<int> (s);
                const auto fading = remaining > 0;

                // During a fade the history is taken again with each curve, as the kept half belongs to neither
                // for certain (V5).
                auto second = 0.0;
                auto shaped = adaa::shaped (state.shaper, in, toMean, fading, second);
                auto offset = waveshaper::curve (m_to.shape, b, toTable);

                // During a fade, the outgoing curve from the same inputs, crossfaded linearly (S10).
                if (fading)
                {
                    const auto fromMean = [this, fromTable] (double from, double to)
                    { return waveshaper::antialiased (m_from.shape, to, from, fromTable); };
                    auto unused = 0.0;
                    const auto outgoing = adaa::shaped (state.shaper, in, fromMean, true, unused);
                    const auto weight = static_cast<double> (remaining) / static_cast<double> (m_fadeLength);
                    shaped += weight * (outgoing - shaped);
                    offset += weight * (waveshaper::curve (m_from.shape, b, fromTable) - offset);
                }

                adaa::advance (state.shaper, in, second);
                const auto wet = state.shaper.post.process (shaped) - offset;

                const auto blocked = wet - state.dcIn + dcCoefficient * state.dcOut;
                state.dcIn = wet;
                state.dcOut = blocked;

                // The tilt (S8): the low part down and the rest up by the same factor.
                state.low += toneCoefficient * (blocked - state.low);
                const auto high = static_cast<double> (tones[s]);
                const auto toned = state.low / high + (blocked - state.low) * high;

                const auto dry = state.dry.process (x);

                const auto amount = static_cast<double> (mixes[s]);
                destination[ch][i] = static_cast<float> (amount * toned + (1.0 - amount) * dry);
            }

            m_channels[ch] = state;
        }
    }

    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 3> m_oversamplers; // 2x, 4x and 8x
    std::array<juce::SmoothedValue<float>, count> m_ramps;                         // the shape's is unused
    std::array<std::vector<float>, count> m_values;
    std::array<double, 4> m_dcCoefficients {};
    std::array<double, 4> m_toneCoefficients {};
    std::array<Channel, 2> m_channels {};
    float m_modulationCoefficient = 1.0f;
    std::atomic<int> m_choice { 1 };
    int m_active = 1;

    // The drawn curve: published by configure() on the message thread, copied into one of two tables by the audio
    // thread, so a fade can run between the old and the new without holding a snapshot (S6).
    SnapshotExchange<waveshaper::DrawnTables> m_tables;
    Curve m_publishedCurve;
    bool m_published = false;
    const waveshaper::DrawnTables* m_seen = nullptr;
    bool m_symmetric = true; // which of the two tables the drawn curve plays
    std::array<DrawnTable, 2> m_drawn {};

    // The curve running, and during a fade the one fading out; m_fade counts the fade's remaining base samples.
    Running m_to;
    Running m_from;
    int m_fade = 0;
    int m_fadeLength = 1;
    InputLevel m_input;
    std::atomic<float> m_shaperInput { 0.0f };
    std::atomic<float> m_shaped { 0.0f };
};

// ---------------------------------------------------------------------------------------------------------
// The dynamics processors' shared parts (docs/design/gate-expander.md, N2): the key path (detector choice, high-pass,
// low-pass, Listen) and the meters, used by the Compressor, the Gate and the Expander.

constexpr std::array<const char*, 2> keyDetectors { "Internal", "Sidechain" };

// The key path's parameters, in this order after each node's own (sidechain.md, K6; gate-expander.md, N4).
constexpr ParamInfo keyDetectorParam { "detector",        "Detector", 0.0f, 1.0f,        0.0f,
                                       ParamKind::choice, "",         0.0f, keyDetectors };
constexpr ParamInfo keyHpParam { "keyHp", "Sidechain HP on", 0.0f, 1.0f, 0.0f, ParamKind::toggle };
constexpr ParamInfo keyHpFreqParam { "keyHpFreq", "Sidechain HP",        20.0f, 5000.0f,
                                     100.0f,      ParamKind::continuous, "Hz",  200.0f };
constexpr ParamInfo keyLpParam { "keyLp", "Sidechain LP on", 0.0f, 1.0f, 0.0f, ParamKind::toggle };
constexpr ParamInfo keyLpFreqParam { "keyLpFreq", "Sidechain LP",        100.0f, 20000.0f,
                                     8000.0f,     ParamKind::continuous, "Hz",   2000.0f };
constexpr ParamInfo listenParam { "listen", "Listen", 0.0f, 1.0f, 0.0f, ParamKind::toggle };

// What the detector hears: the key (or the input), through an optional 12 dB/octave high-pass and low-pass.
class KeyFilter
{
public:
    void prepare (double sampleRate) noexcept
    {
        m_sampleRate = sampleRate;
        m_hpFreq = -1.0f;
        m_lpFreq = -1.0f;
        reset();
    }

    void reset() noexcept
    {
        m_hp.reset();
        m_lp.reset();
    }

    // Recomputes a filter whose frequency moved.
    void update (bool hp, float hpFreq, bool lp, float lpFreq) noexcept
    {
        m_hpOn = hp;
        m_lpOn = lp;

        if (hp && differs (hpFreq, m_hpFreq))
        {
            m_hpFreq = hpFreq;
            m_hp.setPass (m_sampleRate, static_cast<double> (hpFreq), 0.7071068, true);
        }

        if (lp && differs (lpFreq, m_lpFreq))
        {
            m_lpFreq = lpFreq;
            m_lp.setPass (m_sampleRate, static_cast<double> (lpFreq), 0.7071068, false);
        }
    }

    void process (float& left, float& right) noexcept
    {
        if (m_hpOn)
        {
            left = m_hp.process (left, 0);
            right = m_hp.process (right, 1);
        }

        if (m_lpOn)
        {
            left = m_lp.process (left, 0);
            right = m_lp.process (right, 1);
        }
    }

private:
    double m_sampleRate = 48000.0;
    Biquad m_hp;
    Biquad m_lp;
    float m_hpFreq = -1.0f;
    float m_lpFreq = -1.0f;
    bool m_hpOn = false;
    bool m_lpOn = false;
};

// The device panel's history (docs/design/phase11-faust-panels.md, P2): the loudest input, output and detector level
// and the most gain reduction of each 10 ms slot, written by the audio thread and read by the panel.
class DynamicsMeters
{
public:
    void prepare (double sampleRate) noexcept
    {
        m_sampleRate = sampleRate;
        m_slotSamples = std::max (1, static_cast<int> (std::lround (sampleRate * slotSeconds)));
        reset();
    }

    void reset() noexcept { m_slot = {}; }

    // One sample's input and output peak, detector level (linear) and gain change in dB.
    void add (float input, float output, float key, float reductionDb) noexcept
    {
        m_slot.input = std::max (m_slot.input, input);
        m_slot.output = std::max (m_slot.output, output);
        m_slot.key = std::max (m_slot.key, key);
        m_slot.reduction = std::min (m_slot.reduction, reductionDb);

        if (++m_slot.samples >= m_slotSamples)
        {
            const auto index = m_written.load (std::memory_order_relaxed);
            auto& entry = m_history[index % historySize];
            entry[0].store (gainToDb (m_slot.input), std::memory_order_relaxed);
            entry[1].store (gainToDb (m_slot.output), std::memory_order_relaxed);
            entry[2].store (m_slot.reduction, std::memory_order_relaxed);
            entry[3].store (gainToDb (m_slot.key), std::memory_order_relaxed);
            m_written.store (index + 1, std::memory_order_release);
            m_slot = {};
        }
    }

    /**
        Returns the measurements since the last call: { history, reduction, input, output, keyLateMs }. history holds
        one [input, output, reduction, key] quadruple in dB per 10 ms slot, oldest first (key is the detector's level:
        the input's, or the key's when keyed); reduction (0 or negative), input and output are the most gain reduction
        and the input and output peaks over those slots, for the meter and the level dot. keyLateMs says how late the
        key from other tracks arrives (sidechain.md, K5).

        @note  Message thread, one reader (the watched node's panel).
    */
    [[nodiscard]] juce::var read (int keyLateSamples) const
    {
        const auto written = m_written.load (std::memory_order_acquire);
        auto from = std::max (m_read, written > historySize ? written - historySize : std::uint32_t { 0 });
        juce::Array<juce::var> history;

        for (; from != written; ++from)
        {
            const auto& entry = m_history[from % historySize];
            const std::array values { entry[0].load (std::memory_order_relaxed),
                                      entry[1].load (std::memory_order_relaxed),
                                      entry[2].load (std::memory_order_relaxed),
                                      entry[3].load (std::memory_order_relaxed) };

            for (const auto value : values)
                history.add (value);

            m_last = { std::max (m_last[0], values[0]), std::max (m_last[1], values[1]),
                       std::min (m_last[2], values[2]) };
        }

        m_read = written;

        // With new slots, the frame's peaks; without (no audio), the last ones stay.
        auto peaks = m_shown;

        if (! history.isEmpty())
        {
            peaks = m_last;
            m_shown = m_last;
        }

        m_last = { -180.0f, -180.0f, 0.0f };
        return makeObject ({ { "history", history },
                             { "input", peaks[0] },
                             { "output", peaks[1] },
                             { "reduction", peaks[2] },
                             { "keyLateMs", 1000.0 * keyLateSamples / m_sampleRate } });
    }

private:
    static constexpr double slotSeconds = 0.01;
    static constexpr std::uint32_t historySize = 256; // 2.56 s of slots between reads

    struct Slot
    {
        float input = 0.0f;
        float output = 0.0f;
        float key = 0.0f;
        float reduction = 0.0f;
        int samples = 0;
    };

    double m_sampleRate = 48000.0;
    int m_slotSamples = 480;
    Slot m_slot;
    std::array<std::array<std::atomic<float>, 4>, historySize> m_history {};
    std::atomic<std::uint32_t> m_written { 0 };
    mutable std::uint32_t m_read = 0;
    mutable std::array<float, 3> m_last { -180.0f, -180.0f, 0.0f };
    mutable std::array<float, 3> m_shown { -180.0f, -180.0f, 0.0f };
};

// The dynamics effects' ports: an audio input and a key input (docs/design/sidechain.md, K1). The audio follows its
// source's width; the key stays stereo, so a mono key is read on both sides and a stereo key into a mono effect still
// drives it from both channels (docs/design/mono-ports.md).
constexpr std::array<PortInfo, 2> adaptiveKeyedPorts { {
    { PortType::audio, "Audio", adaptiveChannels },
    { PortType::audio, "Key" },
} };
constexpr auto adaptiveKeyedModPorts = withModulationInputs (adaptiveKeyedPorts);

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Bit Crusher (docs/design/bit-crusher.md): drive and clip, an optional anti-alias low-pass, a sample-and-hold at a
// rate in Hz or a note, a quantiser with fractional depths, an optional smoothing low-pass, a DC blocker, Mix and
// Output. Nothing anti-aliases the quantiser or the hold: their aliasing is the effect (section 1).

namespace bitcrusher
{
double quantise (double x, double bits, bool midRise) noexcept
{
    if (bits >= static_cast<double> (maxBits))
        return x;

    const auto m = std::exp2 (std::max (bits, 1.0) - 1.0);

    // Mid-rise: (k + 1/2) / m for the k that keep the level within -1..1 (B4).
    if (midRise)
    {
        const auto top = std::floor (m - 0.5);
        return (std::clamp (std::floor (x * m), -top - 1.0, top) + 0.5) / m;
    }

    // Mid-tread: k / m, symmetric, from -floor(m) / m to floor(m) / m.
    const auto top = std::floor (m);
    return std::clamp (std::round (x * m), -top, top) / m;
}

double noteRate (double semitones) noexcept
{
    return noteRateC4 * std::exp2 (semitones / 12.0);
}
} // namespace bitcrusher

namespace
{
constexpr std::array<const char*, 2> quantiserChoices { "Mid-tread", "Mid-rise" };
constexpr std::array<const char*, 2> rateModeChoices { "Hz", "Note" };
constexpr std::array<const char*, 2> ditherChoices { "Off", "TPDF" };
constexpr std::array<const char*, 2> clipChoices { "Hard", "Soft" };

constexpr std::array<ParamInfo, 14> bitcrusherParams { {
    { "bits", "Bits", 1.0f, bitcrusher::maxBits, 8.0f, ParamKind::continuous, "bits" },
    { "quantiser", "Quantiser", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, quantiserChoices },
    { "rateMode", "Rate mode", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, rateModeChoices },
    { "rate", "Rate", 20.0f, 192000.0f, 11000.0f, ParamKind::continuous, "Hz", 2000.0f },
    { "note", "Note", -120.0f, 120.0f, 24.0f, ParamKind::continuous, "st" },
    { "jitter", "Jitter", 0.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "antiAlias", "AA filter", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "smooth", "Smooth", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "dither", "Dither", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, ditherChoices },
    { "drive", "Drive", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "clip", "Clip", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, clipChoices },
    { "bias", "Bias", -100.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, ParamKind::continuous, "%" },
    { "output", "Output", -24.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
} };

const NodeDescriptor bitcrusherDescriptor { nodetype::bitcrusher, "Bit Crusher", adaptiveModPorts,    adaptivePort,
                                            bitcrusherParams,     true,          NodeCategory::effect };

class BitCrusherNode final : public EffectNode
{
public:
    BitCrusherNode()
        : EffectNode (bitcrusherDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        const auto maxBlockSize = static_cast<size_t> (std::max (spec.maxBlockSize, 1));

        for (auto& ramp : m_ramps)
            ramp.reset (spec.sampleRate, smoothingSeconds);

        m_rateRamp.reset (spec.sampleRate, smoothingSeconds);

        for (auto& values : m_values)
            values.assign (maxBlockSize, 0.0f);

        m_increments.assign (maxBlockSize, 0.0);
        m_dcCoefficient = std::exp (-2.0 * pi * dcBlockerHz / spec.sampleRate);
        m_modulationCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        reset();
    }

    void reset() noexcept override
    {
        for (size_t i = 0; i < smoothedParams.size(); ++i)
            m_ramps[i].setCurrentAndTargetValue (param (smoothedParams[i]));

        const auto noteMode = param (rateModeParam) > 0.5f;
        m_rateRamp.setCurrentAndTargetValue (logRateOf (noteMode, param (noteMode ? noteParam : rateParam)));

        // At 1 the first sample is taken at once, and every hold after it has its full length (B14).
        m_phase = 1.0;
        m_channels = {};
        m_samples = 0;
        m_filterIncrement = -1.0;
        m_antiAliasActive = false;
        m_smoothActive = false;

        for (auto* filter : { &m_antiAlias, &m_smooth })
            for (auto& section : *filter)
                section.reset();

        m_random.setSeed (randomSeed);
    }

private:
    enum Param : size_t
    {
        bitsParam,
        quantiserParam,
        rateModeParam,
        rateParam,
        noteParam,
        jitterParam,
        antiAliasParam,
        smoothParam,
        ditherParam,
        driveParam,
        clipParam,
        biasParam,
        mixParam,
        outputParam,
    };

    // The parameters that ramp in their own units (B13), in the order of m_ramps and m_values.
    static constexpr std::array<size_t, 6> smoothedParams { bitsParam, jitterParam, driveParam,
                                                            biasParam, mixParam,    outputParam };

    enum Value : size_t
    {
        bits,
        jitter,
        drive,
        bias,
        mix,
        output,
    };

    struct Channel
    {
        double held = 0.0;  // the hold's quantised value
        double dcIn = 0.0;  // the DC blocker's previous input
        double dcOut = 0.0; // and output
    };

    static constexpr double dcBlockerHz = 5.0;

    // The filters' cutoff relative to the rate, and how often (in samples) they follow a moving rate (B7).
    static constexpr double filterCutoff = 0.45;
    static constexpr int filterUpdateInterval = 32;

    // Q of the two sections of a 4th-order Butterworth low-pass.
    static constexpr std::array<double, 2> butterworthQ { 0.5411961001461970, 1.3065629648763766 };

    static constexpr int64_t randomSeed = 0x5eed;

    // How far below 1 the phase may be and still take a sample: a whole ratio's increment, such as 0.1, isn't exact in
    // binary, and ten of them add up to just under 1. Without the tolerance, such a hold would now and then last one
    // sample longer (B6).
    static constexpr double phaseTolerance = 1.0e-9;

    // The rate's logarithm (base 2, in Hz), what the rate ramps in (B13).
    [[nodiscard]] static double logRateOf (bool noteMode, float value) noexcept
    {
        return noteMode ? std::log2 (bitcrusher::noteRate (static_cast<double> (value)))
                        : std::log2 (static_cast<double> (value));
    }

    // Fills m_increments with the hold's phase increment per sample, rate / sample rate, from the rate ramped in its
    // logarithm. Once the ramp has settled, the increment is the parameter's own value over the sample rate, so whole
    // ratios are exact (B6).
    void followRate (const ProcessContext& context) noexcept
    {
        const auto noteMode = param (rateModeParam) > 0.5f;
        const auto index = noteMode ? noteParam : rateParam;
        const auto& info = bitcrusherParams[index];
        const auto* modulated = context.modulatedValues (index);
        const auto numSamples = static_cast<size_t> (context.numSamples);

        if (modulated == nullptr)
        {
            const auto value = param (index);
            const auto exact =
                (noteMode ? bitcrusher::noteRate (static_cast<double> (value)) : static_cast<double> (value))
                / m_sampleRate;
            m_rateRamp.setTargetValue (logRateOf (noteMode, value));

            for (size_t s = 0; s < numSamples; ++s)
            {
                const auto logRate = m_rateRamp.getNextValue();
                m_increments[s] = m_rateRamp.isSmoothing() ? std::exp2 (logRate) / m_sampleRate : exact;
            }

            return;
        }

        auto logRate = m_rateRamp.getCurrentValue();
        const auto coefficient = static_cast<double> (m_modulationCoefficient);

        for (size_t s = 0; s < numSamples; ++s)
        {
            const auto target = logRateOf (noteMode, std::clamp (modulated[s], info.minValue, info.maxValue));
            logRate += (target - logRate) * coefficient;
            m_increments[s] = std::exp2 (logRate) / m_sampleRate;
        }

        m_rateRamp.setCurrentAndTargetValue (logRate);
    }

    // Points the filters at the rate of this increment, if it moved since they were last set.
    void updateFilters (double increment) noexcept
    {
        if (! differs (static_cast<float> (increment), static_cast<float> (m_filterIncrement)))
            return;

        m_filterIncrement = increment;
        const auto cutoff = filterCutoff * increment * m_sampleRate;

        for (size_t i = 0; i < butterworthQ.size(); ++i)
        {
            m_antiAlias[i].setPass (m_sampleRate, cutoff, butterworthQ[i], false);
            m_smooth[i].setPass (m_sampleRate, cutoff, butterworthQ[i], false);
        }
    }

    [[nodiscard]] static float filter (std::array<Biquad, 2>& sections, float x, size_t ch) noexcept
    {
        return sections[1].process (sections[0].process (x, ch), ch);
    }

    // The filters follow the rate every few samples, and start from silence whenever they come on.
    void switchFilters (bool antiAliasActive, bool smoothActive, double increment) noexcept
    {
        if ((antiAliasActive || smoothActive)
            && ((m_samples % filterUpdateInterval) == 0 || antiAliasActive != m_antiAliasActive
                || smoothActive != m_smoothActive))
            updateFilters (increment);

        if (antiAliasActive && ! m_antiAliasActive)
            for (auto& section : m_antiAlias)
                section.reset();

        if (smoothActive && ! m_smoothActive)
            for (auto& section : m_smooth)
                section.reset();

        m_antiAliasActive = antiAliasActive;
        m_smoothActive = smoothActive;
    }

    void render (const ProcessContext& context) noexcept override
    {
        const auto numSamples = context.numSamples;

        for (size_t i = 0; i < smoothedParams.size(); ++i)
            followParam (m_ramps[i], m_values[i].data(), bitcrusherParams[smoothedParams[i]], param (smoothedParams[i]),
                         context.modulatedValues (smoothedParams[i]), numSamples, m_modulationCoefficient);

        followRate (context);

        const auto midRise = param (quantiserParam) > 0.5f;
        const auto dither = param (ditherParam) > 0.5f;
        const auto softClip = param (clipParam) > 0.5f;
        const auto antiAliasOn = param (antiAliasParam) > 0.5f;
        const auto smoothOn = param (smoothParam) > 0.5f;
        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];

        for (size_t s = 0; s < static_cast<size_t> (numSamples); ++s, ++m_samples)
        {
            const auto increment = m_increments[s];
            const auto full = increment >= 1.0;

            // The filters follow the rate every few samples, counted from the reset so that blocks don't matter, and
            // start from silence whenever they come on (B7, B13).
            const auto antiAliasActive = antiAliasOn && ! full;
            const auto smoothActive = smoothOn && ! full;

            switchFilters (antiAliasActive, smoothActive, increment);

            // The hold takes a sample when its phase reaches 1 (B5, B6); jitter moves the next one by up to half a
            // period either way (B8). At or above the sample rate it takes every sample.
            const auto take = m_phase >= 1.0 - phaseTolerance;

            if (take)
            {
                const auto spread = static_cast<double> (m_values[jitter][s]) / 100.0;
                const auto shift = spread > 0.0 ? spread * (m_random.nextDouble() - 0.5) : 0.0;
                m_phase = std::min (m_phase - 1.0 - shift, 1.0);
            }

            m_phase += increment;

            const auto depth = static_cast<double> (m_values[bits][s]);
            const auto quantising = depth < static_cast<double> (bitcrusher::maxBits);
            const auto levels = std::exp2 (depth - 1.0);
            const auto driveGain = dbToGain (m_values[drive][s]);
            const auto biasOffset = static_cast<double> (m_values[bias][s]) / 100.0 * 0.5 / levels;
            const auto amount = m_values[mix][s] / 100.0f;
            const auto outputGain = dbToGain (m_values[output][s]);

            for (size_t ch = 0; ch < static_cast<size_t> (context.channels); ++ch)
            {
                const auto dry = in[ch][s];
                auto x = dry * driveGain;

                if (softClip)
                    x = std::tanh (x);

                if (antiAliasActive)
                    x = filter (m_antiAlias, x, ch);

                auto& state = m_channels[ch];

                if (take)
                {
                    auto v = static_cast<double> (x);

                    if (quantising)
                    {
                        v += biasOffset;

                        if (dither)
                            v += (m_random.nextDouble() - m_random.nextDouble()) / levels;
                    }

                    state.held = bitcrusher::quantise (v, depth, midRise);
                }

                auto wet = static_cast<float> (state.held);

                if (smoothActive)
                    wet = filter (m_smooth, wet, ch);

                // The blocker always runs, so it carries on without a jump when the quantiser comes back on, but only
                // a quantising node uses it: with the quantiser off nothing adds DC, and the input passes unchanged.
                const auto blocked = static_cast<double> (wet) - state.dcIn + m_dcCoefficient * state.dcOut;
                state.dcIn = static_cast<double> (wet);
                state.dcOut = blocked;

                if (quantising)
                    wet = static_cast<float> (blocked);

                out[ch][s] = (amount * wet + (1.0f - amount) * dry) * outputGain;
            }
        }
    }

    double m_sampleRate = 48000.0;
    std::array<juce::SmoothedValue<float>, smoothedParams.size()> m_ramps;
    juce::SmoothedValue<double> m_rateRamp;
    std::array<std::vector<float>, smoothedParams.size()> m_values;
    std::vector<double> m_increments;
    std::array<Biquad, 2> m_antiAlias;
    std::array<Biquad, 2> m_smooth;
    std::array<Channel, 2> m_channels {};
    juce::Random m_random;
    double m_phase = 1.0;
    double m_filterIncrement = -1.0;
    double m_dcCoefficient = 1.0;
    int64_t m_samples = 0;
    float m_modulationCoefficient = 1.0f;
    bool m_antiAliasActive = false;
    bool m_smoothActive = false;
};

// ---------------------------------------------------------------------------------------------------------
// Compressor: stereo-linked peak detector, soft knee, attack and release on the gain reduction.

// The key path's parameters come after the original six, whose indices the render code uses (sidechain.md, K6).
constexpr std::array<ParamInfo, 12> compressorParams { {
    { "threshold", "Threshold", -60.0f, 0.0f, -18.0f, ParamKind::continuous, "dB" },
    { "ratio", "Ratio", 1.0f, 20.0f, 4.0f, ParamKind::continuous, ":1", 4.0f },
    { "attack", "Attack", 0.1f, 100.0f, 10.0f, ParamKind::continuous, "ms", 10.0f },
    { "release", "Release", 10.0f, 1000.0f, 100.0f, ParamKind::continuous, "ms", 150.0f },
    { "makeup", "Makeup", 0.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mix", "Mix", 0.0f, 100.0f, 100.0f, ParamKind::continuous, "%" },
    keyDetectorParam,
    keyHpParam,
    keyHpFreqParam,
    keyLpParam,
    keyLpFreqParam,
    listenParam,
} };

const NodeDescriptor compressorDescriptor { nodetype::compressor, "Compressor", adaptiveKeyedModPorts, adaptivePort,
                                            compressorParams,     true,         NodeCategory::effect };

class CompressorNode final : public EffectNode
{
public:
    CompressorNode()
        : EffectNode (compressorDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        m_keyFilter.prepare (spec.sampleRate);
        m_meters.prepare (spec.sampleRate);
        reset();
    }

    void reset() noexcept override
    {
        m_reductionDb = 0.0f;
        m_meters.reset();
        m_keyFilter.reset();
    }

    /**
        The static gain curve: gain change in dB (0 or negative) for an input level in dB, with a 6 dB soft knee.
    */
    [[nodiscard]] static float gainChangeDb (float levelDb, float thresholdDb, float ratio) noexcept
    {
        constexpr float knee = 6.0f;
        const auto over = levelDb - thresholdDb;
        const auto slope = 1.0f / ratio - 1.0f;

        if (2.0f * over < -knee)
            return 0.0f;

        if (2.0f * std::abs (over) <= knee)
            return slope * (over + knee / 2.0f) * (over + knee / 2.0f) / (2.0f * knee);

        return slope * over;
    }

    void setKeyLateSamples (int samples) override { m_keyLateSamples.store (samples); }

    [[nodiscard]] juce::var meters() const override { return m_meters.read (m_keyLateSamples.load()); }

private:
    void render (const ProcessContext& context) noexcept override
    {
        const auto threshold = param (0);
        const auto ratio = param (1);
        const auto coefficient = [this] (float ms)
        { return static_cast<float> (std::exp (-1.0 / (static_cast<double> (ms) * 0.001 * m_sampleRate))); };
        const auto attack = coefficient (param (2));
        const auto release = coefficient (param (3));
        const auto makeup = param (4);
        const auto wet = param (5) / 100.0f;

        // The detector (docs/design/sidechain.md, K6): the input or the key, through the optional key filters; Listen
        // plays what it hears instead of the compressed audio.
        const auto keyed = param (6) > 0.5f && context.audioInputs.size() > 1;
        m_keyFilter.update (param (7) > 0.5f, param (8), param (9) > 0.5f, param (10));
        const auto listen = param (11) > 0.5f;

        const auto& in = context.audioInputs[0];
        const auto& detector = keyed ? context.audioInputs[1] : context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto mono = context.channels == 1;

        for (int s = 0; s < context.numSamples; ++s)
        {
            const auto left = in[0][s];
            const auto right = in[1][s];
            auto keyLeft = detector[0][s];
            auto keyRight = detector[1][s];
            m_keyFilter.process (keyLeft, keyRight);

            const auto level = std::max (std::abs (left), std::abs (right));
            const auto keyLevel = std::max (std::abs (keyLeft), std::abs (keyRight));
            const auto target = gainChangeDb (gainToDb (keyLevel), threshold, ratio);
            const auto c = target < m_reductionDb ? attack : release;
            m_reductionDb = c * m_reductionDb + (1.0f - c) * target;

            const auto gain = dbToGain (m_reductionDb + makeup);
            const auto mix = wet * gain + (1.0f - wet);

            // Mono: both input channels are the one channel; Listen hears the key folded to it.
            if (mono)
            {
                out[0][s] = listen ? 0.5f * (keyLeft + keyRight) : left * mix;
            }
            else
            {
                out[0][s] = listen ? keyLeft : left * mix;
                out[1][s] = listen ? keyRight : right * mix;
            }

            m_meters.add (level, level * mix, keyLevel, m_reductionDb);
        }
    }

    double m_sampleRate = 48000.0;
    float m_reductionDb = 0.0f;
    KeyFilter m_keyFilter;
    DynamicsMeters m_meters;
    std::atomic<int> m_keyLateSamples { 0 };
};

// ---------------------------------------------------------------------------------------------------------
// Gate and Expander (docs/design/gate-expander.md): downward dynamics on one core. The base class has the key path,
// the stereo-linked detector, the lookahead delay, hold and the meters; the two nodes turn the detector's level into a
// gain.

constexpr std::array<const char*, 6> lookaheadChoices { "Off", "0.5 ms", "1 ms", "2 ms", "5 ms", "10 ms" };
constexpr std::array<double, 6> lookaheadMs { 0.0, 0.5, 1.0, 2.0, 5.0, 10.0 };
constexpr std::array<const char*, 2> detectionChoices { "Peak", "RMS" };

// The shared parameters after each node's own first two, in this order (N1).
constexpr ParamInfo dynamicsRangeParam (float defaultDb) noexcept
{
    return { "range", "Range", -80.0f, 0.0f, defaultDb, ParamKind::continuous, "dB" };
}

constexpr ParamInfo dynamicsAttackParam (float defaultMs) noexcept
{
    return { "attack", "Attack", 0.01f, 100.0f, defaultMs, ParamKind::continuous, "ms", 1.0f };
}

constexpr ParamInfo dynamicsHoldParam (float defaultMs) noexcept
{
    return { "hold", "Hold", 0.0f, 500.0f, defaultMs, ParamKind::continuous, "ms", 50.0f };
}

constexpr ParamInfo dynamicsReleaseParam (float defaultMs) noexcept
{
    return { "release", "Release", 1.0f, 2000.0f, defaultMs, ParamKind::continuous, "ms", 100.0f };
}

constexpr ParamInfo lookaheadParam { "lookahead",
                                     "Lookahead",
                                     0.0f,
                                     5.0f,
                                     0.0f,
                                     ParamKind::choice,
                                     "",
                                     0.0f,
                                     lookaheadChoices,
                                     ModulationPolarity::bipolar,
                                     false,
                                     true };

constexpr ParamInfo detectionParam (float defaultChoice) noexcept
{
    return { "detection", "Detection", 0.0f, 1.0f, defaultChoice, ParamKind::choice, "", 0.0f, detectionChoices };
}

constexpr std::array<ParamInfo, 14> gateParams { {
    { "threshold", "Threshold", -80.0f, 0.0f, -40.0f, ParamKind::continuous, "dB" },
    { "hysteresis", "Hysteresis", 0.0f, 24.0f, 6.0f, ParamKind::continuous, "dB" },
    dynamicsRangeParam (-80.0f),
    dynamicsAttackParam (0.5f),
    dynamicsHoldParam (20.0f),
    dynamicsReleaseParam (100.0f),
    lookaheadParam,
    detectionParam (0.0f),
    keyDetectorParam,
    keyHpParam,
    keyHpFreqParam,
    keyLpParam,
    keyLpFreqParam,
    listenParam,
} };

constexpr std::array<ParamInfo, 14> expanderParams { {
    { "threshold", "Threshold", -80.0f, 0.0f, -40.0f, ParamKind::continuous, "dB" },
    { "ratio", "Ratio", 1.0f, 20.0f, 2.0f, ParamKind::continuous, ":1", 4.0f },
    dynamicsRangeParam (-40.0f),
    dynamicsAttackParam (1.0f),
    dynamicsHoldParam (10.0f),
    dynamicsReleaseParam (150.0f),
    lookaheadParam,
    detectionParam (1.0f),
    keyDetectorParam,
    keyHpParam,
    keyHpFreqParam,
    keyLpParam,
    keyLpFreqParam,
    listenParam,
} };

const NodeDescriptor gateDescriptor { nodetype::gate, "Gate", adaptiveKeyedModPorts, adaptivePort,
                                      gateParams,     true,   NodeCategory::effect };

const NodeDescriptor expanderDescriptor { nodetype::expander, "Expander", adaptiveKeyedModPorts, adaptivePort,
                                          expanderParams,     true,       NodeCategory::effect };

// The bottom of the range: the Gate's gain there is 0, fully closed (N9).
constexpr float closedDb = -80.0f;

class DownwardDynamicsNode : public EffectNode
{
public:
    using EffectNode::EffectNode;

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        m_detectorCoefficient = static_cast<float> (std::exp (-1.0 / (detectorSeconds * spec.sampleRate)));
        const auto longest = static_cast<size_t> (std::lround (lookaheadMs.back() * 0.001 * spec.sampleRate)) + 1;

        for (auto& line : m_lines)
            line.assign (longest, 0.0f);

        m_keyFilter.prepare (spec.sampleRate);
        m_meters.prepare (spec.sampleRate);
        m_choice.store (lookaheadChoice());
        reset();
    }

    void configure (const GraphNode& node) override
    {
        EffectNode::configure (node);
        m_choice.store (lookaheadChoice());
    }

    void reset() noexcept override
    {
        for (auto& line : m_lines)
            std::ranges::fill (line, 0.0f);

        m_position = 0;
        m_lookahead = lookaheadSamples (m_choice.load());
        m_peak = 0.0f;
        m_squares = {};
        m_holdLeft = 0;
        m_keyFilter.reset();
        m_meters.reset();
        resetGain();
    }

    // The lookahead chosen when the graph was last compiled (N5); the audio thread switches to it at its next block.
    [[nodiscard]] int latencySamples() const noexcept override { return lookaheadSamples (m_choice.load()); }

    void setKeyLateSamples (int samples) override { m_keyLateSamples.store (samples); }

    [[nodiscard]] juce::var meters() const override
    {
        auto result = m_meters.read (m_keyLateSamples.load());
        addMeters (*result.getDynamicObject());
        return result;
    }

protected:
    enum Index : size_t
    {
        threshold,
        shape, // the Gate's hysteresis, the Expander's ratio
        range,
        attack,
        hold,
        release,
        lookahead,
        detection,
        detector,
        keyHp,
        keyHpFreq,
        keyLp,
        keyLpFreq,
        listen,
    };

    // Per block: the parameters in the form the gain computer uses.
    struct Settings
    {
        float threshold = 0.0f;
        float shape = 0.0f;
        float rangeDb = 0.0f;
        double attackSamples = 1.0;
        int holdSamples = 0;
        double releaseSamples = 1.0;
        float attackCoefficient = 0.0f; // one-poles with the attack's and release's time constants
        float releaseCoefficient = 0.0f;
    };

    // The gain for one sample from the detector's level in dB; updates the hold count through holdLeft.
    [[nodiscard]] virtual float nextGain (float levelDb, const Settings& settings, int& holdLeft) noexcept = 0;

    // The gain state as a fresh node has it: closed, or at the curve's resting gain.
    virtual void resetGain() noexcept = 0;

    virtual void addMeters (juce::DynamicObject& meters) const { std::ignore = meters; }

    [[nodiscard]] float paramValue (Index index) const noexcept { return param (index); }

private:
    static constexpr double detectorSeconds = 0.01;

    [[nodiscard]] int lookaheadChoice() const noexcept
    {
        return std::clamp (static_cast<int> (std::lround (param (Index::lookahead))), 0,
                           static_cast<int> (lookaheadMs.size()) - 1);
    }

    [[nodiscard]] int lookaheadSamples (int choice) const noexcept
    {
        return static_cast<int> (std::lround (lookaheadMs[static_cast<size_t> (choice)] * 0.001 * m_sampleRate));
    }

    void render (const ProcessContext& context) noexcept override
    {
        const auto choice = m_choice.load();
        const auto samples = std::min (lookaheadSamples (choice), static_cast<int> (m_lines[0].size()) - 1);

        if (samples != m_lookahead)
        {
            for (auto& line : m_lines)
                std::ranges::fill (line, 0.0f);

            m_lookahead = samples;
        }

        const auto perMs = 0.001 * m_sampleRate;
        Settings settings;
        settings.threshold = param (Index::threshold);
        settings.shape = param (Index::shape);
        settings.rangeDb = std::min (0.0f, param (Index::range));
        settings.attackSamples = std::max (1.0, static_cast<double> (param (Index::attack)) * perMs);
        settings.holdSamples = static_cast<int> (std::lround (static_cast<double> (param (Index::hold)) * perMs));
        settings.releaseSamples = std::max (1.0, static_cast<double> (param (Index::release)) * perMs);
        settings.attackCoefficient = static_cast<float> (std::exp (-1.0 / settings.attackSamples));
        settings.releaseCoefficient = static_cast<float> (std::exp (-1.0 / settings.releaseSamples));

        const auto rms = param (Index::detection) > 0.5f;
        const auto keyed = param (Index::detector) > 0.5f && context.audioInputs.size() > 1;
        m_keyFilter.update (param (Index::keyHp) > 0.5f, param (Index::keyHpFreq), param (Index::keyLp) > 0.5f,
                            param (Index::keyLpFreq));
        const auto listening = param (Index::listen) > 0.5f;

        const auto& in = context.audioInputs[0];
        const auto& key = keyed ? context.audioInputs[1] : context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto size = static_cast<int> (m_lines[0].size());
        const auto c = m_detectorCoefficient;
        const auto mono = context.channels == 1;

        for (int s = 0; s < context.numSamples; ++s)
        {
            auto keyLeft = key[0][s];
            auto keyRight = key[1][s];
            m_keyFilter.process (keyLeft, keyRight);

            // The stereo-linked detector (N3): peak with an instant attack and a 10 ms decay, or the mean square
            // over 10 ms, the louder channel's.
            float level = 0.0f;

            if (rms)
            {
                m_squares[0] = c * m_squares[0] + (1.0f - c) * keyLeft * keyLeft;
                m_squares[1] = c * m_squares[1] + (1.0f - c) * keyRight * keyRight;
                level = std::sqrt (std::max (m_squares[0], m_squares[1]));
            }
            else
            {
                const auto rectified = std::max (std::abs (keyLeft), std::abs (keyRight));
                m_peak = rectified > m_peak ? rectified : c * m_peak;
                level = m_peak;
            }

            const auto gain = nextGain (gainToDb (level), settings, m_holdLeft);

            // The audio through the lookahead delay (N5).
            auto left = in[0][s];
            auto right = in[1][s];

            if (m_lookahead > 0)
            {
                const auto read = (m_position + size - m_lookahead) % size;
                const auto write = static_cast<size_t> (m_position);
                m_lines[0][write] = left;
                m_lines[1][write] = right;
                left = m_lines[0][static_cast<size_t> (read)];
                right = m_lines[1][static_cast<size_t> (read)];
                m_position = (m_position + 1) % size;
            }

            // Mono: both input channels are the one channel; Listen hears the key folded to it.
            if (mono)
            {
                out[0][s] = listening ? 0.5f * (keyLeft + keyRight) : left * gain;
            }
            else
            {
                out[0][s] = listening ? keyLeft : left * gain;
                out[1][s] = listening ? keyRight : right * gain;
            }

            const auto input = std::max (std::abs (left), std::abs (right));
            m_meters.add (input, input * gain, level, std::max (closedDb, gainToDb (gain)));
        }
    }

    double m_sampleRate = 48000.0;
    float m_detectorCoefficient = 0.0f;
    float m_peak = 0.0f;
    std::array<float, 2> m_squares {};
    int m_holdLeft = 0;
    std::array<std::vector<float>, 2> m_lines;
    int m_position = 0;
    int m_lookahead = 0;
    std::atomic<int> m_choice { 0 };
    KeyFilter m_keyFilter;
    DynamicsMeters m_meters;
    std::atomic<int> m_keyLateSamples { 0 };
};

// The Gate (N6–N9): opens above the threshold, closes below threshold - hysteresis after the hold; the gain ramps
// linearly in amplitude while opening and linearly in dB while closing, so Attack and Release are the times to open
// and close fully.
class GateNode final : public DownwardDynamicsNode
{
public:
    GateNode()
        : DownwardDynamicsNode (gateDescriptor)
    {
    }

private:
    [[nodiscard]] float nextGain (float levelDb, const Settings& settings, int& holdLeft) noexcept override
    {
        const auto closeDb = settings.threshold - std::max (0.0f, settings.shape);

        if (! m_open && levelDb >= settings.threshold)
            m_open = true;

        if (m_open)
        {
            if (levelDb >= closeDb)
                holdLeft = settings.holdSamples;
            else if (holdLeft > 0)
                --holdLeft;
            else
                m_open = false;
        }

        const auto closed = settings.rangeDb <= closedDb;
        const auto floor = closed ? 0.0f : dbToGain (settings.rangeDb);
        const auto rise = static_cast<float> (1.0 / settings.attackSamples);

        if (m_open)
        {
            m_gain = std::min (1.0f, m_gain + rise * (1.0f - floor));
        }
        else if (m_gain > floor)
        {
            // Down by the range in the release time, in equal dB steps; fully closed falls the same way to -80 dB,
            // then to silence.
            const auto fall =
                dbToGain (std::max (settings.rangeDb, closedDb) / static_cast<float> (settings.releaseSamples));
            m_gain *= fall;

            if (m_gain <= floor || (closed && m_gain <= dbToGain (closedDb)))
                m_gain = floor;
        }
        else if (m_gain < floor)
        {
            // A range raised while closed: up to the new floor at the attack's rate.
            m_gain = std::min (floor, m_gain + rise * (1.0f - floor));
        }

        m_shownOpen.store (m_open, std::memory_order_relaxed);
        return m_gain;
    }

    void resetGain() noexcept override
    {
        m_open = false;
        const auto rangeDb = std::min (0.0f, paramValue (Index::range));
        m_gain = rangeDb <= closedDb ? 0.0f : dbToGain (rangeDb);
    }

    void addMeters (juce::DynamicObject& meters) const override
    {
        meters.setProperty ("open", m_shownOpen.load (std::memory_order_relaxed));
    }

    bool m_open = false;
    float m_gain = 0.0f;
    std::atomic<bool> m_shownOpen { false };
};

// The Expander (N8, N10): the static curve's gain change, followed by a one-pole in dB (the attack's time constant
// while the gain rises, the release's while it falls, after the hold).
class ExpanderNode final : public DownwardDynamicsNode
{
public:
    ExpanderNode()
        : DownwardDynamicsNode (expanderDescriptor)
    {
    }

    [[nodiscard]] static float gainChangeDb (float levelDb, float thresholdDb, float ratio, float rangeDb) noexcept
    {
        constexpr float knee = 6.0f;
        const auto under = levelDb - thresholdDb;
        const auto slope = std::max (0.0f, ratio - 1.0f);
        auto change = 0.0f;

        if (2.0f * under >= knee)
            change = 0.0f;
        else if (2.0f * std::abs (under) < knee)
            change = -slope * (under - knee / 2.0f) * (under - knee / 2.0f) / (2.0f * knee);
        else
            change = slope * under;

        return std::max (std::min (0.0f, rangeDb), change);
    }

private:
    [[nodiscard]] float nextGain (float levelDb, const Settings& settings, int& holdLeft) noexcept override
    {
        const auto target = gainChangeDb (levelDb, settings.threshold, settings.shape, settings.rangeDb);

        if (target >= m_reductionDb)
        {
            holdLeft = settings.holdSamples;
            m_reductionDb = target + (m_reductionDb - target) * settings.attackCoefficient;
        }
        else if (holdLeft > 0)
        {
            --holdLeft;
        }
        else
        {
            m_reductionDb = target + (m_reductionDb - target) * settings.releaseCoefficient;
        }

        return dbToGain (m_reductionDb);
    }

    void resetGain() noexcept override
    {
        m_reductionDb =
            gainChangeDb (-180.0f, paramValue (Index::threshold), paramValue (Index::shape), paramValue (Index::range));
    }

    float m_reductionDb = 0.0f;
};

// ---------------------------------------------------------------------------------------------------------
// Utility: phase, width, mono, pan and gain.

constexpr std::array<ParamInfo, 6> utilityParams { {
    { "gain", "Gain", -60.0f, 24.0f, 0.0f, ParamKind::continuous, "dB" },
    { "pan", "Pan", -100.0f, 100.0f, 0.0f, ParamKind::continuous, "" },
    { "width", "Width", 0.0f, 200.0f, 100.0f, ParamKind::continuous, "%" },
    { "invertLeft", "Invert left", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "invertRight", "Invert right", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "mono", "Mono", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
} };

const NodeDescriptor utilityDescriptor { nodetype::utility, "Utility", adaptiveModPorts,     stereoPort,
                                         utilityParams,     true,      NodeCategory::utility };

class UtilityNode final : public EffectNode
{
public:
    UtilityNode()
        : EffectNode (utilityDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        for (auto* value : { &m_left, &m_right, &m_width })
            value->reset (spec.sampleRate, smoothingSeconds);

        m_modulationCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        reset();
    }

    void reset() noexcept override
    {
        const auto targets = gains();
        m_left.setCurrentAndTargetValue (targets[0]);
        m_right.setCurrentAndTargetValue (targets[1]);
        m_width.setCurrentAndTargetValue (width());
    }

private:
    [[nodiscard]] std::array<float, 2> gains() const noexcept { return gains (param (0), param (1)); }

    // The channel gains for a gain in dB and a pan of -100..100, with the inversions applied.
    [[nodiscard]] std::array<float, 2> gains (float db, float panning) const noexcept
    {
        const auto gain = dbToGain (db);
        const auto pan = panGains (panning / 100.0f);
        return { gain * pan[0] * (param (3) > 0.5f ? -1.0f : 1.0f), gain * pan[1] * (param (4) > 0.5f ? -1.0f : 1.0f) };
    }

    [[nodiscard]] float width() const noexcept { return width (param (2)); }

    [[nodiscard]] float width (float percent) const noexcept { return param (5) > 0.5f ? 0.0f : percent / 100.0f; }

    void render (const ProcessContext& context) noexcept override
    {
        const std::array modulated { context.modulatedValues (0), context.modulatedValues (1),
                                     context.modulatedValues (2) };

        if (std::ranges::any_of (modulated, [] (const float* values) { return values != nullptr; }))
        {
            renderModulated (context, modulated);
            return;
        }

        const auto targets = gains();
        m_left.setTargetValue (targets[0]);
        m_right.setTargetValue (targets[1]);
        m_width.setTargetValue (width());

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];

        for (int s = 0; s < context.numSamples; ++s)
        {
            const auto mid = 0.5f * (in[0][s] + in[1][s]);
            const auto side = 0.5f * (in[0][s] - in[1][s]) * m_width.getNextValue();
            out[0][s] = (mid + side) * m_left.getNextValue();
            out[1][s] = (mid - side) * m_right.getNextValue();
        }
    }

    // Follows the per-sample gain, pan and width with a short smoothing, then hands the values back to the smoothers
    // that ramp manual and automated changes.
    void renderModulated (const ProcessContext& context, const std::array<const float*, 3>& modulated) noexcept
    {
        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto value = [&] (size_t index, int s)
        { return modulated[index] != nullptr ? modulated[index][s] : param (index); };
        auto left = m_left.getCurrentValue();
        auto right = m_right.getCurrentValue();
        auto currentWidth = m_width.getCurrentValue();

        for (int s = 0; s < context.numSamples; ++s)
        {
            const auto targets = gains (value (0, s), value (1, s));
            left += (targets[0] - left) * m_modulationCoefficient;
            right += (targets[1] - right) * m_modulationCoefficient;
            currentWidth += (width (value (2, s)) - currentWidth) * m_modulationCoefficient;

            const auto mid = 0.5f * (in[0][s] + in[1][s]);
            const auto side = 0.5f * (in[0][s] - in[1][s]) * currentWidth;
            out[0][s] = (mid + side) * left;
            out[1][s] = (mid - side) * right;
        }

        m_left.setCurrentAndTargetValue (left);
        m_right.setCurrentAndTargetValue (right);
        m_width.setCurrentAndTargetValue (currentWidth);
    }

    juce::SmoothedValue<float> m_left { 1.0f };
    juce::SmoothedValue<float> m_right { 1.0f };
    juce::SmoothedValue<float> m_width { 1.0f };
    float m_modulationCoefficient = 1.0f;
};

// ---------------------------------------------------------------------------------------------------------
// Mix: four stereo inputs, each with a gain.

constexpr std::array<ParamInfo, 4> mixParams { {
    { "gain1", "In 1", -96.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
    { "gain2", "In 2", -96.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
    { "gain3", "In 3", -96.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
    { "gain4", "In 4", -96.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
} };

constexpr auto mixModInputs = withModulationInputs (mixInputs);

const NodeDescriptor mixDescriptor { nodetype::mix, "Mix", mixModInputs,         adaptivePort,
                                     mixParams,     true,  NodeCategory::utility };

class MixNode final : public EffectNode
{
public:
    MixNode()
        : EffectNode (mixDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        for (size_t i = 0; i < m_gains.size(); ++i)
        {
            m_gains[i].reset (spec.sampleRate, smoothingSeconds);
            m_gains[i].setCurrentAndTargetValue (dbToGain (param (i)));
        }
    }

private:
    void render (const ProcessContext& context) noexcept override
    {
        const auto& out = context.audioOutputs[0];
        const auto channels = static_cast<size_t> (context.channels);

        for (size_t ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::clear (out[ch], context.numSamples);

        for (size_t i = 0; i < m_gains.size(); ++i)
        {
            auto& gain = m_gains[i];
            gain.setTargetValue (dbToGain (param (i)));
            const auto& in = context.audioInputs[i];

            for (int s = 0; s < context.numSamples; ++s)
            {
                const auto g = gain.getNextValue();

                for (size_t ch = 0; ch < channels; ++ch)
                    out[ch][s] += in[ch][s] * g;
            }
        }
    }

    std::array<juce::SmoothedValue<float>, 4> m_gains;
};

// ---------------------------------------------------------------------------------------------------------
// Convolution Reverb (docs/design/convolution-reverb.md): the input, delayed by Pre-delay, convolved with an IR from
// the audio pool (or the generated default) by the zero-latency convolver (Convolver.h), then a low cut, a high cut,
// Width and Gain on the wet signal, mixed with the dry at equal power. The shaping controls rebuild the IR on the
// builder thread (I4, I8); the others work in real time (I9).

// A parameter the IR is built from: neither modulatable nor automatable (I14).
constexpr ParamInfo shapingParam (const char* id, const char* name, float minValue, float maxValue, float defaultValue,
                                  ParamKind kind, const char* unit, float midValue = 0.0f) noexcept
{
    return { id,    name,  minValue, maxValue, defaultValue, kind, unit, midValue, {}, ModulationPolarity::bipolar,
             false, false, 0.0f,     false };
}

constexpr std::array<ParamInfo, 12> convolutionParams { {
    { "predelay",
      "Pre-delay",
      0.0f,
      250.0f,
      0.0f,
      ParamKind::continuous,
      "ms",
      50.0f,
      {},
      ModulationPolarity::bipolar,
      false },
    shapingParam ("start", "Start", 0.0f, 1000.0f, 0.0f, ParamKind::continuous, "ms", 100.0f),
    shapingParam ("length", "Length", 1.0f, 100.0f, 100.0f, ParamKind::continuous, "%"),
    shapingParam ("decay", "Decay", 10.0f, 100.0f, 100.0f, ParamKind::continuous, "%"),
    shapingParam ("reverse", "Reverse", 0.0f, 1.0f, 0.0f, ParamKind::toggle, ""),
    shapingParam ("stretch", "Stretch", 50.0f, 200.0f, 100.0f, ParamKind::continuous, "%", 100.0f),
    shapingParam ("normalise", "Normalise", 0.0f, 1.0f, 1.0f, ParamKind::toggle, ""),
    { "lowCut", "Low cut", 20.0f, 1000.0f, 20.0f, ParamKind::continuous, "Hz", 140.0f },
    { "highCut", "High cut", 1000.0f, 20000.0f, 20000.0f, ParamKind::continuous, "Hz", 4500.0f },
    { "width", "Width", 0.0f, 200.0f, 100.0f, ParamKind::continuous, "%" },
    { "gain", "Gain", -24.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
    { "mix", "Mix", 0.0f, 100.0f, 25.0f, ParamKind::continuous, "%" },
} };

const NodeDescriptor convolutionDescriptor {
    nodetype::convolutionReverb, "Convolution Reverb", adaptiveModPorts, stereoPort, convolutionParams, true,
    NodeCategory::effect
};

class ConvolutionReverbNode final : public EffectNode
{
public:
    enum Param : size_t
    {
        predelay,
        start,
        length,
        decay,
        reverse,
        stretch,
        normalise,
        lowCut,
        highCut,
        width,
        gain,
        mix,
    };

    // The parameters that work in real time (I9), from lowCut on.
    static constexpr size_t firstRealtime = lowCut;
    static constexpr size_t realtimeCount = convolutionParams.size() - firstRealtime;

    ConvolutionReverbNode()
        : EffectNode (convolutionDescriptor)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        const auto rateChanged = ! juce::approximatelyEqual (spec.sampleRate, m_sampleRate);
        m_sampleRate = spec.sampleRate;
        const auto maxBlockSize = static_cast<size_t> (std::max (spec.maxBlockSize, 1));

        m_convolver.prepare();

        // A program built for another rate is of no use; the same rate keeps it (an export pass starts here).
        if (rateChanged)
            m_convolver.reset (m_inbox->exchange);
        else
            m_convolver.clear (m_inbox->exchange);

        for (auto& buffer : m_delayed)
            buffer.assign (maxBlockSize, 0.0f);

        for (auto& buffer : m_wet)
            buffer.assign (maxBlockSize, 0.0f);

        for (auto& values : m_values)
            values.assign (maxBlockSize, 0.0f);

        for (auto& ramp : m_ramps)
            ramp.reset (spec.sampleRate, smoothingSeconds);

        const auto longest =
            static_cast<int> (std::ceil (convolutionParams[predelay].maxValue / 1000.0 * spec.sampleRate));
        const auto lineSize = static_cast<size_t> (juce::nextPowerOfTwo (longest + 2));

        for (auto& line : m_line)
            line.assign (lineSize, 0.0f);

        m_lineMask = static_cast<int> (lineSize) - 1;
        m_delayFadeLength = std::max (1, static_cast<int> (std::lround (0.02 * spec.sampleRate)));
        m_modulationCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        clearState();

        if (rateChanged)
            requestBuild();
    }

    void reset() noexcept override
    {
        m_convolver.clear (m_inbox->exchange);
        clearState();
    }

    void configure (const GraphNode& node) override
    {
        EffectNode::configure (node);

        // A new file arrives through setAudioFile(), right after; building before it would build the old one.
        if (! m_configured)
            m_awaitingFile = node.fileId != 0;
        else if (node.fileId != m_fileId)
            m_awaitingFile = true;

        m_fileId = node.fileId;

        m_configured = true;
        requestBuild();
    }

    void setAudioFile (std::shared_ptr<const AudioSource> source) override
    {
        m_source = std::move (source);
        m_awaitingFile = false;
        requestBuild();
    }

    [[nodiscard]] juce::var panelMeters() const override
    {
        const auto panel = m_inbox->panel();
        juce::Array<juce::var> envelope;

        for (const auto value : panel.analysis.envelope)
            envelope.add (value);

        return makeObject ({ { "envelope", envelope },
                             { "seconds", panel.analysis.seconds },
                             { "decay", panel.analysis.decaySeconds },
                             { "pending", m_inbox->pending() || ! panel.ready },
                             { "missing", panel.missing },
                             { "truncated", panel.truncated } });
    }

    [[nodiscard]] bool running() const noexcept { return m_convolver.running(); }

protected:
    void render (const ProcessContext& context) noexcept override
    {
        const auto numSamples = context.numSamples;
        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];

        for (size_t i = 0; i < realtimeCount; ++i)
            followParam (m_ramps[i], m_values[i].data(), convolutionParams[firstRealtime + i],
                         param (firstRealtime + i), context.modulatedValues (firstRealtime + i), numSamples,
                         m_modulationCoefficient);

        delayInput (in, numSamples);
        m_convolver.process (m_inbox->exchange, { m_delayed[0].data(), m_delayed[1].data() },
                             { m_wet[0].data(), m_wet[1].data() }, numSamples, m_sampleRate);

        const auto* lowCuts = m_values[lowCut - firstRealtime].data();
        const auto* highCuts = m_values[highCut - firstRealtime].data();
        const auto* widths = m_values[width - firstRealtime].data();
        const auto* gains = m_values[gain - firstRealtime].data();
        const auto* mixes = m_values[mix - firstRealtime].data();

        for (int s = 0; s < numSamples; ++s)
        {
            // The filters follow their frequencies every 16 samples.
            if (s % 16 == 0)
            {
                if (differs (lowCuts[s], m_lowCutHz))
                {
                    m_lowCutHz = lowCuts[s];
                    m_lowCut.setPass (m_sampleRate, m_lowCutHz, juce::MathConstants<double>::sqrt2 / 2.0, true);
                }

                if (differs (highCuts[s], m_highCutHz))
                {
                    m_highCutHz = highCuts[s];
                    m_highCut.setPass (m_sampleRate, m_highCutHz, juce::MathConstants<double>::sqrt2 / 2.0, false);
                }
            }

            if (differs (gains[s], m_gainDb))
            {
                m_gainDb = gains[s];
                m_gainLinear = dbToGain (m_gainDb);
            }

            if (differs (mixes[s], m_mixPercent))
            {
                m_mixPercent = mixes[s];
                const auto angle = static_cast<float> (m_mixPercent / 100.0f * pi / 2.0);
                m_dryGain = std::cos (angle);
                m_wetGain = std::sin (angle);
            }

            // At the ends of their ranges the filters are out of the path; they keep running, so moving one in
            // starts from its settled state.
            std::array<float, 2> wet { m_wet[0][static_cast<size_t> (s)], m_wet[1][static_cast<size_t> (s)] };

            for (size_t ch = 0; ch < 2; ++ch)
            {
                const auto low = m_lowCut.process (wet[ch], ch);
                wet[ch] = m_lowCutHz > convolutionParams[lowCut].minValue ? low : wet[ch];
                const auto high = m_highCut.process (wet[ch], ch);
                wet[ch] = m_highCutHz < convolutionParams[highCut].maxValue ? high : wet[ch];
            }

            auto left = wet[0];
            auto right = wet[1];
            const auto mid = 0.5f * (left + right);
            const auto side = 0.5f * (left - right) * widths[s] / 100.0f;
            left = (mid + side) * m_gainLinear;
            right = (mid - side) * m_gainLinear;

            const auto dryLeft = in[0][s];
            const auto dryRight = in[1][s];
            out[0][s] = m_dryGain * dryLeft + m_wetGain * left;
            out[1][s] = m_dryGain * dryRight + m_wetGain * right;
        }
    }

private:
    // The input through the pre-delay line (I7): a change of Pre-delay crossfades from the old tap to the new.
    void delayInput (const std::array<const float*, 2>& in, int numSamples) noexcept
    {
        const auto target = static_cast<int> (std::lround (
            std::clamp (param (predelay), 0.0f, convolutionParams[predelay].maxValue) / 1000.0 * m_sampleRate));

        for (int s = 0; s < numSamples; ++s)
        {
            if (m_delayFade >= m_delayFadeLength && target != m_delayTo)
            {
                m_delayFrom = m_delayTo;
                m_delayTo = target;
                m_delayFade = 0;
            }

            for (size_t ch = 0; ch < 2; ++ch)
            {
                auto& line = m_line[ch];
                line[static_cast<size_t> (m_write)] = in[ch][s];
                const auto to = line[static_cast<size_t> ((m_write - m_delayTo) & m_lineMask)];

                if (m_delayFade < m_delayFadeLength)
                {
                    const auto from = line[static_cast<size_t> ((m_write - m_delayFrom) & m_lineMask)];
                    const auto g = static_cast<float> (m_delayFade) / static_cast<float> (m_delayFadeLength);
                    m_delayed[ch][static_cast<size_t> (s)] = from + g * (to - from);
                }
                else
                {
                    m_delayed[ch][static_cast<size_t> (s)] = to;
                }
            }

            m_write = (m_write + 1) & m_lineMask;

            if (m_delayFade < m_delayFadeLength)
                ++m_delayFade;
        }
    }

    void clearState() noexcept
    {
        for (auto& line : m_line)
            std::fill (line.begin(), line.end(), 0.0f);

        m_write = 0;
        m_delayTo = m_delayFrom = static_cast<int> (std::lround (
            std::clamp (param (predelay), 0.0f, convolutionParams[predelay].maxValue) / 1000.0 * m_sampleRate));
        m_delayFade = m_delayFadeLength;

        for (size_t i = 0; i < m_ramps.size(); ++i)
            m_ramps[i].setCurrentAndTargetValue (param (firstRealtime + i));

        m_lowCut.reset();
        m_highCut.reset();
        m_lowCutHz = m_highCutHz = -1.0f;
        m_gainDb = m_mixPercent = -1000.0f;
    }

    [[nodiscard]] convolution::Shape shape() const noexcept
    {
        return { param (start),   param (length),          param (decay),
                 param (stretch), param (reverse) >= 0.5f, param (normalise) >= 0.5f };
    }

    // Asks the builder for a program when what it is built from changed (I4).
    void requestBuild()
    {
        if (! m_configured || m_awaitingFile || m_sampleRate <= 0.0)
            return;

        convolution::Request request;
        request.useDefault = m_fileId == 0;
        request.source = request.useDefault ? nullptr : m_source;
        request.sampleRate = m_sampleRate;
        request.shape = shape();

        if (m_requested.has_value() && m_requested->useDefault == request.useDefault
            && m_requested->source == request.source
            && juce::approximatelyEqual (m_requested->sampleRate, request.sampleRate)
            && m_requested->shape == request.shape)
            return;

        m_requested = request;
        convolution::Builder::shared().request (m_inbox, std::move (request));
    }

    std::shared_ptr<convolution::Inbox> m_inbox = std::make_shared<convolution::Inbox>();
    convolution::Convolver m_convolver;
    double m_sampleRate = 0.0;

    // Message thread.
    ObjectId m_fileId = 0;
    std::shared_ptr<const AudioSource> m_source;
    bool m_configured = false;
    bool m_awaitingFile = false;
    std::optional<convolution::Request> m_requested;

    // Audio thread.
    std::array<std::vector<float>, 2> m_delayed;
    std::array<std::vector<float>, 2> m_wet;
    std::array<std::vector<float>, 2> m_line;
    int m_lineMask = 0;
    int m_write = 0;
    int m_delayFrom = 0;
    int m_delayTo = 0;
    int m_delayFade = 0;
    int m_delayFadeLength = 1;
    std::array<juce::SmoothedValue<float>, realtimeCount> m_ramps;
    std::array<std::vector<float>, realtimeCount> m_values;
    float m_modulationCoefficient = 1.0f;
    Biquad m_lowCut;
    Biquad m_highCut;
    float m_lowCutHz = -1.0f;
    float m_highCutHz = -1.0f;
    float m_gainDb = -1000.0f;
    float m_gainLinear = 1.0f;
    float m_mixPercent = -1000.0f;
    float m_dryGain = 1.0f;
    float m_wetGain = 0.0f;
};

// ---------------------------------------------------------------------------------------------------------
// Chorus, Flanger and Phaser (docs/design/modulation-effects.md): true stereo, each channel through its own delay line
// or allpass chain, the right channel's LFO Spread behind the left's. They share the LFO's parameters (the first five,
// in the same order), Mix (the tenth) and the core in dawg/ModulationEffects.h.

constexpr std::array<const char*, 2> lfoShapeChoices { "Sine", "Triangle" };
constexpr float wholeCycle = 19.0f;  // "1/1" in lfoCycleChoices
constexpr float doubleCycle = 22.0f; // "2/1"

[[nodiscard]] constexpr ParamInfo modRateParam (float defaultValue)
{
    return { "rate", "Rate", 0.01f, 10.0f, defaultValue, ParamKind::continuous, "Hz", 1.0f };
}

[[nodiscard]] constexpr ParamInfo modCycleParam (float defaultValue)
{
    return { "cycle",        "Cycle",           0.0f, static_cast<float> (lfoCycleChoices.size() - 1),
             defaultValue,   ParamKind::choice, "",   0.0f,
             lfoCycleChoices };
}

[[nodiscard]] constexpr ParamInfo modShapeParam (float defaultValue)
{
    return { "shape", "Shape", 0.0f, 1.0f, defaultValue, ParamKind::choice, "", 0.0f, lfoShapeChoices };
}

constexpr ParamInfo modSyncParam { "sync", "Tempo sync", 0.0f, 1.0f, 0.0f, ParamKind::toggle };
constexpr ParamInfo modSpreadParam { "spread", "Spread", 0.0f, 180.0f, 90.0f, ParamKind::continuous, "\xc2\xb0" };
constexpr ParamInfo modMixParam { "mix", "Mix", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" };

constexpr std::array<ParamInfo, 10> chorusParams { {
    modSyncParam,
    modRateParam (0.8f),
    modCycleParam (wholeCycle),
    modShapeParam (0.0f),
    modSpreadParam,
    { "delay", "Delay", 5.0f, 40.0f, 15.0f, ParamKind::continuous, "ms" },
    { "depth", "Depth", 0.0f, 100.0f, 15.0f, ParamKind::continuous, "%" },
    { "voices", "Voices", 1.0f, 4.0f, 2.0f, ParamKind::integer },
    { "feedback", "Feedback", 0.0f, 80.0f, 0.0f, ParamKind::continuous, "%" },
    modMixParam,
} };

constexpr std::array<ParamInfo, 10> flangerParams { {
    modSyncParam,
    modRateParam (0.2f),
    modCycleParam (doubleCycle),
    modShapeParam (1.0f),
    modSpreadParam,
    { "delay", "Delay", 0.1f, 10.0f, 2.0f, ParamKind::continuous, "ms", 2.0f },
    { "depth", "Depth", 0.0f, 100.0f, 70.0f, ParamKind::continuous, "%" },
    { "feedback", "Feedback", -95.0f, 95.0f, 50.0f, ParamKind::continuous, "%" },
    { "throughZero",
      "Through-zero",
      0.0f,
      1.0f,
      0.0f,
      ParamKind::toggle,
      "",
      0.0f,
      {},
      ModulationPolarity::bipolar,
      false,
      true },
    modMixParam,
} };

constexpr std::array<const char*, 6> phaserStageChoices { "2", "4", "6", "8", "10", "12" };

constexpr std::array<ParamInfo, 10> phaserParams { {
    modSyncParam,
    modRateParam (0.4f),
    modCycleParam (wholeCycle),
    modShapeParam (0.0f),
    modSpreadParam,
    { "stages", "Stages", 0.0f, 5.0f, 1.0f, ParamKind::choice, "", 0.0f, phaserStageChoices },
    { "centre", "Centre", 50.0f, 8000.0f, 700.0f, ParamKind::continuous, "Hz", 700.0f },
    { "depth", "Depth", 0.0f, 100.0f, 60.0f, ParamKind::continuous, "%" },
    { "feedback", "Feedback", -95.0f, 95.0f, 40.0f, ParamKind::continuous, "%" },
    modMixParam,
} };

const NodeDescriptor chorusDescriptor { nodetype::chorus, "Chorus", adaptiveModPorts,    stereoPort,
                                        chorusParams,     true,     NodeCategory::effect };
const NodeDescriptor flangerDescriptor { nodetype::flanger, "Flanger", adaptiveModPorts,    stereoPort,
                                         flangerParams,     true,      NodeCategory::effect };
const NodeDescriptor phaserDescriptor { nodetype::phaser, "Phaser", adaptiveModPorts,    stereoPort,
                                        phaserParams,     true,     NodeCategory::effect };

// What the three share: the LFO and its parameters, the per-sample values of every continuous parameter (ramped over
// 20 ms, or following modulation, X12), and the LFO's phase for the panel (X14).
class ModulationEffectNode : public EffectNode
{
public:
    ModulationEffectNode (const NodeDescriptor& descriptor, std::span<const ParamInfo, 10> infos)
        : EffectNode (descriptor),
          m_infos (infos)
    {
    }

    [[nodiscard]] juce::var meters() const override { return makeObject ({ { "phase", m_phaseMeter.load() } }); }

protected:
    static constexpr size_t syncIndex = 0;
    static constexpr size_t rateIndex = 1;
    static constexpr size_t cycleIndex = 2;
    static constexpr size_t shapeIndex = 3;
    static constexpr size_t spreadIndex = 4;
    static constexpr size_t mixIndex = 9;
    static constexpr double defaultBpm = 120.0;

    void prepareShared (const ProcessSpec& spec)
    {
        const auto maxBlockSize = static_cast<size_t> (std::max (spec.maxBlockSize, 1));
        m_sampleRate = spec.sampleRate;
        m_modulationCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        m_lfo.prepare (spec.sampleRate);
        m_fadeStep = static_cast<float> (1.0 / std::max (1.0, smoothingSeconds * spec.sampleRate));

        for (auto& ramp : m_ramps)
            ramp.reset (spec.sampleRate, smoothingSeconds);

        for (auto& values : m_values)
            values.assign (maxBlockSize, 0.0f);

        m_increments.assign (maxBlockSize, 0.0);
        m_targets.assign (maxBlockSize, -1.0);
    }

    void resetShared() noexcept
    {
        for (size_t i = 0; i < m_ramps.size(); ++i)
            m_ramps[i].setCurrentAndTargetValue (param (i));

        m_lfo.reset();
        m_lfo.setShapeNow (shape());
        m_phaseMeter.store (0.0f);
    }

    // Fills the parameters' values and the LFO's increments and song-position targets for this block.
    void beginBlock (const ProcessContext& context) noexcept
    {
        const auto numSamples = context.numSamples;

        for (size_t i = 0; i < m_infos.size(); ++i)
            if (m_infos[i].kind == ParamKind::continuous)
                followParam (m_ramps[i], m_values[i].data(), m_infos[i], param (i), context.modulatedValues (i),
                             numSamples, m_modulationCoefficient);

        m_lfo.setShape (shape());
        const auto* transport = context.transport;

        if (param (syncIndex) < toggleThreshold)
        {
            for (size_t s = 0; s < static_cast<size_t> (numSamples); ++s)
            {
                m_increments[s] = static_cast<double> (m_values[rateIndex][s]) / m_sampleRate;
                m_targets[s] = -1.0;
            }

            return;
        }

        // Synced (X4): the rate from the tempo at the block's start; while playing, the song position as the target.
        const auto bpm = transport != nullptr ? transport->timeMap.tempoMap().bpmAt (transport->startTick) : defaultBpm;
        const auto note = static_cast<size_t> (std::clamp (static_cast<int> (std::lround (param (cycleIndex))), 0,
                                                           static_cast<int> (lfoCycleBeats.size() - 1)));
        const auto cycleBeats = lfoCycleBeats[note];
        const auto increment = bpm / 60.0 / cycleBeats / m_sampleRate;
        const auto following =
            transport != nullptr && transport->playing && transport->endTick > transport->startTick && numSamples > 0;
        const auto ticksPerSample = following ? (transport->endTick - transport->startTick) / numSamples : 0.0;

        for (size_t s = 0; s < static_cast<size_t> (numSamples); ++s)
        {
            m_increments[s] = increment;

            if (following)
            {
                const auto beats = (transport->startTick + ticksPerSample * static_cast<double> (s))
                                   / static_cast<double> (ticksPerBeat);
                const auto cycles = beats / cycleBeats;
                m_targets[s] = cycles - std::floor (cycles);
            }
            else
            {
                m_targets[s] = -1.0;
            }
        }
    }

    void endBlock() noexcept { m_phaseMeter.store (static_cast<float> (m_lfo.phase())); }

    // Advances the LFO to sample s and returns the right channel's offset in cycles.
    [[nodiscard]] double advance (size_t s) noexcept
    {
        m_lfo.advance (m_increments[s], m_targets[s]);
        return static_cast<double> (m_values[spreadIndex][s]) / 360.0;
    }

    [[nodiscard]] modfx::LfoShape shape() const noexcept
    {
        return param (shapeIndex) >= 0.5f ? modfx::LfoShape::triangle : modfx::LfoShape::sine;
    }

    [[nodiscard]] const float* values (size_t index) const noexcept { return m_values[index].data(); }

    double m_sampleRate = 48000.0;
    float m_fadeStep = 1.0f;
    modfx::StereoLfo m_lfo;

private:
    std::span<const ParamInfo, 10> m_infos;
    std::array<juce::SmoothedValue<float>, 10> m_ramps;
    std::array<std::vector<float>, 10> m_values;
    std::vector<double> m_increments;
    std::vector<double> m_targets;
    float m_modulationCoefficient = 1.0f;
    std::atomic<float> m_phaseMeter { 0.0f };
};

// Chorus (X5): up to four taps per channel, voice k of V at the LFO's phase plus k / V (plus Spread / 360 / V on
// the right); the voices' sum is scaled by
// 1 / sqrt V for the output and by 1 / V for the feedback, so the loop's gain stays below Feedback. A change of Voices
// crossfades from the old count's taps to the new count's over 20 ms.
class ChorusNode final : public ModulationEffectNode
{
public:
    ChorusNode()
        : ModulationEffectNode (chorusDescriptor, chorusParams)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        prepareShared (spec);

        for (auto& line : m_lines)
            line.prepare (2.0 * maxDelayMs / 1000.0 * spec.sampleRate + modfx::DelayLine::minDelay);

        reset();
    }

    void reset() noexcept override
    {
        resetShared();

        for (auto& line : m_lines)
            line.reset();

        m_voices = voices();
        m_previousVoices = m_voices;
        m_voiceFade = 1.0f;
    }

private:
    static constexpr float maxDelayMs = 40.0f;

    [[nodiscard]] int voices() const noexcept { return std::clamp (static_cast<int> (std::lround (param (7))), 1, 4); }

    // The sum of a channel's V taps.
    [[nodiscard]] float taps (const modfx::DelayLine& line, int count, double offset, double delay,
                              double depth) const noexcept
    {
        auto sum = 0.0f;

        // The right channel's voices sit Spread / 360 of the way to the next left voice, so 180 degrees puts them
        // halfway between the left ones, whatever the count.
        const auto voiceOffset = offset / count;

        for (int k = 0; k < count; ++k)
        {
            const auto m = m_lfo.value (voiceOffset + static_cast<double> (k) / count);
            sum += line.read (delay * (1.0 + depth * m));
        }

        return sum;
    }

    void render (const ProcessContext& context) noexcept override
    {
        beginBlock (context);

        if (const auto wanted = voices(); wanted != m_voices)
        {
            m_previousVoices = m_voiceFade < 0.5f ? m_previousVoices : m_voices;
            m_voices = wanted;
            m_voiceFade = 0.0f;
        }

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto* delays = values (5);
        const auto* depths = values (6);
        const auto* feedbacks = values (8);
        const auto* mixes = values (mixIndex);

        for (size_t s = 0; s < static_cast<size_t> (context.numSamples); ++s)
        {
            const auto spread = advance (s);
            const auto delay = static_cast<double> (delays[s]) / 1000.0 * m_sampleRate;
            const auto depth = static_cast<double> (depths[s]) / 100.0;
            const auto feedback = feedbacks[s] / 100.0f;
            const auto mix = mixes[s] / 100.0f;

            for (size_t ch = 0; ch < 2; ++ch)
            {
                auto& line = m_lines[ch];
                const auto offset = ch == 0 ? 0.0 : spread;
                const auto count = static_cast<float> (m_voices);
                const auto sum = taps (line, m_voices, offset, delay, depth);
                auto wet = sum / std::sqrt (count);
                auto back = sum / count;

                if (m_voiceFade < 1.0f)
                {
                    const auto before = taps (line, m_previousVoices, offset, delay, depth);
                    const auto previousCount = static_cast<float> (m_previousVoices);
                    wet = before / std::sqrt (previousCount) + (wet - before / std::sqrt (previousCount)) * m_voiceFade;
                    back = before / previousCount + (back - before / previousCount) * m_voiceFade;
                }

                const auto dry = in[ch][s];
                line.write (dry + feedback * back);
                out[ch][s] = dry + (wet - dry) * mix;
            }

            if (m_voiceFade < 1.0f)
                m_voiceFade = std::min (1.0f, m_voiceFade + m_fadeStep);
        }

        endBlock();
    }

    std::array<modfx::DelayLine, 2> m_lines;
    int m_voices = 2;
    int m_previousVoices = 2;
    float m_voiceFade = 1.0f;
};

// Flanger (X7, X8): one tap per channel at Delay (1 + Depth m), fed back; through zero, the dry signal is the line read
// at a fixed 5 ms reference and the tap sweeps around it, and the node reports that reference as latency.
class FlangerNode final : public ModulationEffectNode
{
public:
    FlangerNode()
        : ModulationEffectNode (flangerDescriptor, flangerParams)
    {
    }

    void configure (const GraphNode& node) override
    {
        ModulationEffectNode::configure (node);
        m_throughZero.store (param (8) >= toggleThreshold);
    }

    void prepare (const ProcessSpec& spec) override
    {
        prepareShared (spec);
        m_reference = static_cast<int> (std::lround (referenceSeconds * spec.sampleRate));
        m_throughZero.store (param (8) >= toggleThreshold);

        for (auto& line : m_lines)
            line.prepare (2.0 * maxDelayMs / 1000.0 * spec.sampleRate + m_reference + modfx::DelayLine::minDelay);

        reset();
    }

    void reset() noexcept override
    {
        resetShared();

        for (auto& line : m_lines)
            line.reset();

        m_active = m_throughZero.load();
    }

    // The reference while through-zero was on when the graph was last compiled (X8).
    [[nodiscard]] int latencySamples() const noexcept override { return m_throughZero.load() ? m_reference : 0; }

private:
    static constexpr float maxDelayMs = 10.0f;
    static constexpr double referenceSeconds = 0.005;

    void render (const ProcessContext& context) noexcept override
    {
        beginBlock (context);

        if (const auto wanted = m_throughZero.load(); wanted != m_active)
        {
            m_active = wanted;

            for (auto& line : m_lines)
                line.reset();
        }

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto* delays = values (5);
        const auto* depths = values (6);
        const auto* feedbacks = values (7);
        const auto* mixes = values (mixIndex);
        const auto reference = static_cast<double> (m_reference);

        for (size_t s = 0; s < static_cast<size_t> (context.numSamples); ++s)
        {
            const auto spread = advance (s);
            const auto delay = static_cast<double> (delays[s]) / 1000.0 * m_sampleRate;
            const auto depth = static_cast<double> (depths[s]) / 100.0;
            const auto feedback = feedbacks[s] / 100.0f;
            const auto mix = mixes[s] / 100.0f;
            const auto amplitude = std::min (delay * depth, reference - modfx::DelayLine::minDelay);

            for (size_t ch = 0; ch < 2; ++ch)
            {
                auto& line = m_lines[ch];
                const auto m = m_lfo.value (ch == 0 ? 0.0 : spread);
                const auto tap = m_active ? reference + amplitude * m : delay * (1.0 + depth * m);
                const auto wet = line.read (tap);
                const auto dry = m_active ? line.read (reference) : in[ch][s];
                line.write (in[ch][s] + feedback * wet);
                out[ch][s] = dry + (wet - dry) * mix;
            }
        }

        endBlock();
    }

    std::array<modfx::DelayLine, 2> m_lines;
    std::atomic<bool> m_throughZero { false };
    bool m_active = false;
    int m_reference = 240;
};

// Phaser (X10): twelve first-order allpasses per channel at one swept frequency, Centre 2^(4 Depth m) octaves, the
// output and the feedback taken after stage N, crossfaded over 20 ms when N changes; the feedback goes back to the
// chain's input one sample later.
class PhaserNode final : public ModulationEffectNode
{
public:
    PhaserNode()
        : ModulationEffectNode (phaserDescriptor, phaserParams)
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        prepareShared (spec);
        reset();
    }

    void reset() noexcept override
    {
        resetShared();
        m_channels = {};
        m_stages = stages();
        m_previousStages = m_stages;
        m_stageFade = 1.0f;
    }

    // The allpass frequency for a centre, a depth in % and an LFO value, clamped (X10); the panel uses the same.
    [[nodiscard]] static double sweepFrequency (double centre, double depthPercent, double m,
                                                double sampleRate) noexcept
    {
        return std::clamp (centre * std::exp2 (octaves * depthPercent / 100.0 * m), minFrequency,
                           maxFrequencyRatio * sampleRate);
    }

private:
    static constexpr size_t maxStages = 12;
    static constexpr double octaves = 4.0;
    static constexpr double minFrequency = 20.0;
    static constexpr double maxFrequencyRatio = 0.45;

    struct Channel
    {
        std::array<modfx::AllpassStage, maxStages> stages;
        float last = 0.0f; // the tapped output, fed back at the next sample
    };

    [[nodiscard]] int stages() const noexcept
    {
        return 2 * (std::clamp (static_cast<int> (std::lround (param (5))), 0, 5) + 1);
    }

    void render (const ProcessContext& context) noexcept override
    {
        beginBlock (context);

        if (const auto wanted = stages(); wanted != m_stages)
        {
            m_previousStages = m_stageFade < 0.5f ? m_previousStages : m_stages;
            m_stages = wanted;
            m_stageFade = 0.0f;
        }

        const auto& in = context.audioInputs[0];
        const auto& out = context.audioOutputs[0];
        const auto* centres = values (6);
        const auto* depths = values (7);
        const auto* feedbacks = values (8);
        const auto* mixes = values (mixIndex);

        for (size_t s = 0; s < static_cast<size_t> (context.numSamples); ++s)
        {
            const auto spread = advance (s);
            const auto feedback = feedbacks[s] / 100.0f;
            const auto mix = mixes[s] / 100.0f;

            for (size_t ch = 0; ch < 2; ++ch)
            {
                auto& channel = m_channels[ch];
                const auto m = m_lfo.value (ch == 0 ? 0.0 : spread);
                const auto frequency =
                    sweepFrequency (static_cast<double> (centres[s]), static_cast<double> (depths[s]), m, m_sampleRate);
                const auto a = modfx::allpassCoefficient (frequency, m_sampleRate);
                const auto dry = in[ch][s];
                auto signal = dry + feedback * channel.last;
                auto tapped = 0.0f;
                auto previous = 0.0f;

                for (size_t i = 0; i < maxStages; ++i)
                {
                    signal = channel.stages[i].process (signal, a);

                    if (static_cast<int> (i) + 1 == m_stages)
                        tapped = signal;

                    if (static_cast<int> (i) + 1 == m_previousStages)
                        previous = signal;
                }

                const auto wet = m_stageFade < 1.0f ? previous + (tapped - previous) * m_stageFade : tapped;
                channel.last = std::clamp (wet, -modfx::DelayLine::writeLimit, modfx::DelayLine::writeLimit);
                out[ch][s] = dry + (wet - dry) * mix;
            }

            if (m_stageFade < 1.0f)
                m_stageFade = std::min (1.0f, m_stageFade + m_fadeStep);
        }

        endBlock();
    }

    std::array<Channel, 2> m_channels {};
    int m_stages = 4;
    int m_previousStages = 4;
    float m_stageFade = 1.0f;
};
} // namespace

float compressorGainChangeDb (float levelDb, float thresholdDb, float ratio) noexcept
{
    return CompressorNode::gainChangeDb (levelDb, thresholdDb, ratio);
}

float expanderGainChangeDb (float levelDb, float thresholdDb, float ratio, float rangeDb) noexcept
{
    return ExpanderNode::gainChangeDb (levelDb, thresholdDb, ratio, rangeDb);
}

namespace
{
template <size_t N>
[[nodiscard]] std::array<float, N> valuesOf (const std::array<ParamInfo, N>& infos,
                                             const std::map<juce::String, double>& params)
{
    std::array<float, N> values {};

    for (size_t i = 0; i < N; ++i)
    {
        const auto it = params.find (infos[i].id);
        values[i] = it != params.end()
                        ? static_cast<float> (std::clamp (it->second, static_cast<double> (infos[i].minValue),
                                                          static_cast<double> (infos[i].maxValue)))
                        : infos[i].defaultValue;
    }

    return values;
}

[[nodiscard]] std::vector<double> responseFrequencies()
{
    std::vector<double> frequencies (static_cast<size_t> (responsePoints));

    for (size_t i = 0; i < frequencies.size(); ++i)
        frequencies[i] = 20.0 * std::pow (1000.0, static_cast<double> (i) / static_cast<double> (responsePoints - 1));

    return frequencies;
}

// Adds log-spaced frequencies over `octaves` either side of `centre`. The fixed grid alone can leave a high-Q band's
// peak between two points, which both flattens it and draws as a visible corner.
void addDenseFrequencies (std::vector<double>& frequencies, double centre, double octaves, int intervals)
{
    if (intervals < 2 || ! (centre > 0.0))
        return;

    const auto low = std::max (20.0, centre * std::pow (2.0, -octaves));
    const auto high = std::min (20000.0, centre * std::pow (2.0, octaves));

    if (! (high > low))
        return;

    for (int i = 0; i <= intervals; ++i)
        frequencies.push_back (low * std::pow (high / low, static_cast<double> (i) / static_cast<double> (intervals)));
}

// A bell's skirt: its -3 dB bandwidth is about 1.44/Q octaves, so this covers a few bandwidths either side, with
// enough points that the drawn peak is smooth and sits at its true height.
void addBellFrequencies (std::vector<double>& frequencies, double centre, double q)
{
    const auto quality = std::clamp (q, 0.05, 64.0);
    const auto intervals = std::clamp (static_cast<int> (std::lround (32.0 * std::sqrt (quality))), 32, 127);
    // An even number of intervals puts one sample exactly on the centre, where a bell's peak is.
    addDenseFrequencies (frequencies, centre, std::clamp (1.8 / quality, 0.03, 3.0), intervals + intervals % 2);
}

// Sorts the grid and drops points that sit on top of each other (a band's points overlap the fixed grid).
void tidyFrequencies (std::vector<double>& frequencies)
{
    std::sort (frequencies.begin(), frequencies.end());
    frequencies.erase (
        std::unique (frequencies.begin(), frequencies.end(), [] (double a, double b) { return b <= a * 1.00005; }),
        frequencies.end());
}

// Two decimals (of dB or Hz) are all a panel can show, and halve the size of the curve's JSON while a band is dragged.
// Curves of linear values in -1..1 need more.
[[nodiscard]] juce::Array<juce::var> toArray (const std::vector<double>& values, double scale = 100.0)
{
    juce::Array<juce::var> result;
    result.ensureStorageAllocated (static_cast<int> (values.size()));

    for (const auto v : values)
        result.add (std::round (v * scale) / scale);

    return result;
}

// A band's response in dB at the frequencies: its running sections' together, flat when none runs.
[[nodiscard]] std::vector<double> bandResponseDb (const EqNode::Sections& sections,
                                                  const std::array<int, EqNode::passSections>& band,
                                                  const std::vector<double>& frequencies, double sampleRate)
{
    std::vector<double> db (frequencies.size(), 0.0);

    for (const auto section : band)
    {
        if (section < 0 || ! sections.active[static_cast<size_t> (section)])
            continue;

        for (size_t i = 0; i < frequencies.size(); ++i)
        {
            const auto w = 2.0 * pi * std::min (frequencies[i], 0.4999 * sampleRate) / sampleRate;
            db[i] +=
                20.0 * std::log10 (std::max (sections.filters[static_cast<size_t> (section)].magnitude (w), 1.0e-9));
        }
    }

    return db;
}

[[nodiscard]] juce::var eqResponse (const std::map<juce::String, double>& params, double sampleRate)
{
    const auto values = valuesOf (eqParams, params);
    EqNode::Sections sections;
    EqNode::configureSections (sections, sampleRate, values);
    auto frequencies = responseFrequencies();

    // Dense points where the curve bends most around each running band: a pass filter's knee, a bell's or notch's
    // peak, and a shelf's or tilt's corner, which with a high Q has a bump on either side, like a bell's skirt.
    for (size_t band = 0; band < eqBands.size(); ++band)
    {
        if (! sections.active[static_cast<size_t> (EqNode::bandSections[band][0])])
            continue;

        const double frequency = values[eqBands[band].freq];
        const double q = values[eqBands[band].q];

        switch (EqNode::typeOf (values, band))
        {
            case EqType::highPass:
            case EqType::lowPass:
                addDenseFrequencies (frequencies, frequency, 0.8, 24);
                break;
            case EqType::bell:
            case EqType::notch:
                addBellFrequencies (frequencies, frequency, q);
                break;
            case EqType::lowShelf:
            case EqType::highShelf:
            case EqType::tilt:
                addBellFrequencies (frequencies, frequency, std::max (1.0, q));
                break;
        }
    }

    tidyFrequencies (frequencies);

    // The bands on both channels add to the mid's and the side's curves, the others to one of them.
    std::vector<double> mid (frequencies.size(), 0.0);
    std::vector<double> side (frequencies.size(), 0.0);
    juce::Array<juce::var> perBand;

    // One curve per band (an HP or LP is its sections together); bands that don't run are flat.
    for (const auto& band : EqNode::bandSections)
    {
        const auto db = bandResponseDb (sections, band, frequencies, sampleRate);
        const auto channels = sections.channels[static_cast<size_t> (band[0])];

        for (size_t i = 0; i < frequencies.size(); ++i)
        {
            if (channels != EqNode::Channels::side)
                mid[i] += db[i];

            if (channels != EqNode::Channels::mid)
                side[i] += db[i];
        }

        perBand.add (toArray (db));
    }

    // With bands on the mid or the side only, y is the mid's curve and side the side's.
    auto result = makeObject ({ { "x", toArray (frequencies) }, { "y", toArray (mid) }, { "bands", perBand } });

    if (sections.midSide())
        result.getDynamicObject()->setProperty ("side", toArray (side));

    return result;
}

[[nodiscard]] juce::var filterResponse (const std::map<juce::String, double>& params, double sampleRate)
{
    const auto values = valuesOf (filterParams, params);
    auto frequencies = responseFrequencies();

    const auto slope = std::clamp (static_cast<int> (std::lround (values[4])), 0, 3);
    const auto stages = slope + 1;

    // The resonance peak (Q = 1/k, of the sharpest stage, which is the last) needs the same dense points as a bell.
    const auto k = FilterNode::stageDamping (static_cast<double> (values[2]), stages, stages - 1);
    addBellFrequencies (frequencies, values[1], 1.0 / k);
    tidyFrequencies (frequencies);

    std::vector<double> db (frequencies.size());

    for (size_t i = 0; i < frequencies.size(); ++i)
        db[i] = FilterNode::responseDb (static_cast<int> (std::lround (values[0])), values[1], values[2], sampleRate,
                                        frequencies[i], stages, static_cast<int> (std::lround (values[5])));

    return makeObject ({ { "x", toArray (frequencies) }, { "y", toArray (db) } });
}

juce::var FilterNode::meters() const
{
    const auto activeCutoff = m_activeCutoff.load (std::memory_order_relaxed);
    const auto activeResonance = m_activeResonance.load (std::memory_order_relaxed);
    const auto activeDrive = m_activeDrive.load (std::memory_order_relaxed);
    const auto activeSlope = m_activeSlope.load (std::memory_order_relaxed);
    const auto activeMode = m_activeMode.load (std::memory_order_relaxed);
    const auto hasModInput = m_hasModulationInput.load (std::memory_order_relaxed);

    const auto baseMode = static_cast<int> (std::lround (param (0)));
    const auto baseCutoff = param (1);
    const auto baseResonance = param (2);
    const auto baseDrive = param (3);
    const auto baseSlope = static_cast<int> (std::lround (param (4)));

    const bool differsFromBase =
        hasModInput || std::abs (activeCutoff - baseCutoff) > 0.5f || std::abs (activeResonance - baseResonance) > 0.2f
        || std::abs (activeDrive - baseDrive) > 0.1f || std::abs (activeSlope - static_cast<float> (baseSlope)) > 0.1f
        || std::abs (activeMode - static_cast<float> (baseMode)) > 0.1f;

    return makeObject ({ { "cutoff", activeCutoff },
                         { "resonance", activeResonance },
                         { "drive", activeDrive },
                         { "slope", activeSlope },
                         { "mode", activeMode },
                         { "hasModulationInput", hasModInput },
                         { "isModulated", differsFromBase } });
}

juce::var FilterNode::panelMeters() const
{
    const std::map<juce::String, double> active {
        { "mode", static_cast<double> (m_activeMode.load (std::memory_order_relaxed)) },
        { "cutoff", static_cast<double> (m_activeCutoff.load (std::memory_order_relaxed)) },
        { "resonance", static_cast<double> (m_activeResonance.load (std::memory_order_relaxed)) },
        { "drive", static_cast<double> (m_activeDrive.load (std::memory_order_relaxed)) },
        { "slope", static_cast<double> (m_activeSlope.load (std::memory_order_relaxed)) },
        { "oversampling", static_cast<double> (m_choice.load (std::memory_order_relaxed)) }
    };

    auto result = meters();
    result.getDynamicObject()->setProperty ("curve", filterResponse (active, m_sampleRate));
    return result;
}

[[nodiscard]] juce::var compressorResponse (const std::map<juce::String, double>& params)
{
    constexpr int points = 133; // -60 to +6 dB in 0.5 dB steps
    const auto values = valuesOf (compressorParams, params);
    std::vector<double> levels (points);
    std::vector<double> output (points);

    for (size_t i = 0; i < levels.size(); ++i)
    {
        levels[i] = -60.0 + 0.5 * static_cast<double> (i);
        output[i] =
            levels[i]
            + static_cast<double> (CompressorNode::gainChangeDb (static_cast<float> (levels[i]), values[0], values[1]))
            + static_cast<double> (values[4]);
    }

    return makeObject ({ { "x", toArray (levels) }, { "y", toArray (output) } });
}

[[nodiscard]] juce::var saturatorResponse (const std::map<juce::String, double>& params)
{
    constexpr int points = 201; // -1 to 1 in steps of 0.01
    const auto values = valuesOf (saturatorParams, params);
    const auto gain = static_cast<double> (dbToGain (values[2]));
    std::vector<double> input (points);
    std::vector<double> output (points);

    for (size_t i = 0; i < input.size(); ++i)
    {
        input[i] = -1.0 + 0.01 * static_cast<double> (i);
        output[i] = gain
                    * SaturatorNode::shape (input[i], static_cast<double> (values[0]), static_cast<double> (values[1]),
                                            static_cast<double> (values[3]));
    }

    return makeObject ({ { "x", toArray (input) }, { "y", toArray (output, 10000.0) } });
}

[[nodiscard]] juce::var clipperResponse (const std::map<juce::String, double>& params)
{
    constexpr int points = 201; // -1 to 1 in steps of 0.01
    const auto values = valuesOf (clipperParams, params);
    const auto gain = static_cast<double> (dbToGain (values[3]));
    std::vector<double> input (points);
    std::vector<double> output (points);

    for (size_t i = 0; i < input.size(); ++i)
    {
        input[i] = -1.0 + 0.01 * static_cast<double> (i);
        output[i] = gain
                    * ClipperNode::shape (input[i], static_cast<double> (values[0]), static_cast<double> (values[1]),
                                          static_cast<double> (values[2]), static_cast<double> (values[4]));
    }

    return makeObject ({ { "x", toArray (input) }, { "y", toArray (output, 10000.0) } });
}

[[nodiscard]] juce::var waveshaperResponse (const std::map<juce::String, double>& params)
{
    constexpr int points = 201; // -1 to 1 in steps of 0.01
    const auto values = valuesOf (waveshaperParams, params);
    const auto gain = static_cast<double> (dbToGain (values[4]));
    std::vector<double> input (points);
    std::vector<double> output (points);

    for (size_t i = 0; i < input.size(); ++i)
    {
        input[i] = -1.0 + 0.01 * static_cast<double> (i);
        output[i] = gain
                    * WaveshaperNode::shape (input[i], static_cast<int> (std::lround (values[0])),
                                             static_cast<double> (values[1]), static_cast<double> (values[2]),
                                             static_cast<double> (values[5]));
    }

    return makeObject ({ { "x", toArray (input) }, { "y", toArray (output, 10000.0) } });
}
} // namespace

juce::var effectResponse (const juce::String& type, const std::map<juce::String, double>& params, double sampleRate)
{
    if (type == nodetype::eq)
        return eqResponse (params, sampleRate);

    if (type == nodetype::filter)
        return filterResponse (params, sampleRate);

    if (type == nodetype::compressor)
        return compressorResponse (params);

    if (type == nodetype::saturator)
        return saturatorResponse (params);

    if (type == nodetype::clipper)
        return clipperResponse (params);

    if (type == nodetype::waveshaper)
        return waveshaperResponse (params);

    return {};
}

void registerEffectNodes (NodeRegistry& registry)
{
    registry.add (eqDescriptor, [] { return std::make_unique<EqNode>(); });
    registry.add (filterDescriptor, [] { return std::make_unique<FilterNode>(); });
    registry.add (saturatorDescriptor, [] { return std::make_unique<SaturatorNode>(); });
    registry.add (clipperDescriptor, [] { return std::make_unique<ClipperNode>(); });
    registry.add (waveshaperDescriptor, [] { return std::make_unique<WaveshaperNode>(); });
    registry.add (bitcrusherDescriptor, [] { return std::make_unique<BitCrusherNode>(); });
    registry.add (delayDescriptor, [] { return std::make_unique<DelayNode>(); });
    registry.add (feedbackDescriptor, [] { return std::make_unique<FeedbackNode>(); });
    registry.add (limiterDescriptor, [] { return std::make_unique<LimiterNode>(); });
    registry.add (compressorDescriptor, [] { return std::make_unique<CompressorNode>(); });
    registry.add (gateDescriptor, [] { return std::make_unique<GateNode>(); });
    registry.add (expanderDescriptor, [] { return std::make_unique<ExpanderNode>(); });
    registry.add (utilityDescriptor, [] { return std::make_unique<UtilityNode>(); });
    registry.add (mixDescriptor, [] { return std::make_unique<MixNode>(); });
    registry.add (convolutionDescriptor, [] { return std::make_unique<ConvolutionReverbNode>(); });
    registry.add (chorusDescriptor, [] { return std::make_unique<ChorusNode>(); });
    registry.add (flangerDescriptor, [] { return std::make_unique<FlangerNode>(); });
    registry.add (phaserDescriptor, [] { return std::make_unique<PhaserNode>(); });
    registerLoopReverb (registry);
}

void waitForConvolutionBuilds()
{
    convolution::Builder::shared().waitIdle();
}

} // namespace dawg
