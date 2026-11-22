#pragma once

#include "Dsp.h"
#include "Pattern.h"
#include "dawg_sdk/Node.h"

#include <array>
#include <atomic>
#include <string_view>
#include <vector>

// The reverb (SPEC.md). The type, the control ids, names, ranges and units are fixed by SPEC.md 4.1, except the two
// early-reflection controls (5.5). DESIGN.md explains the design, every default and every curve.

namespace dawg::reverb
{

/**
    The reverb's controls (SPEC.md, 4.1; defaults: DESIGN.md, section 2).
*/
inline constexpr std::array<ParamInfo, 12> params { {
    { "feedback", "Feedback", 0.0f, 100.0f, 45.0f, ParamKind::continuous, "%" },
    { "diffusion", "Diffusion", 0.0f, 100.0f, 70.0f, ParamKind::continuous, "%" },
    { "size", "Size", 0.0f, 100.0f, 55.0f, ParamKind::continuous, "%" },
    { "predelay", "Pre-delay", 0.0f, 250.0f, 10.0f, ParamKind::continuous, "ms" },
    { "earlyTiming", "Early Timing", 0.0f, 100.0f, 40.0f, ParamKind::continuous, "%" },
    { "width", "Width", 0.0f, 100.0f, 75.0f, ParamKind::continuous, "%" },
    { "damp", "Damp highs", 0.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "thin", "Thin lows", 0.0f, 100.0f, 20.0f, ParamKind::continuous, "%" },
    { "modulation", "Modulation", 0.0f, 100.0f, 25.0f, ParamKind::continuous, "%" },
    { "mute", "Mute", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
    { "mix", "Mix", 0.0f, 100.0f, 25.0f, ParamKind::continuous, "%" },
    { "seed", "Seed", 1.0f, 999999.0f, 1.0f, ParamKind::integer },
} };

/**
    Returns a control's index in params.

    @param id  The control's id.
    @return    Its index, or params.size() if there is none.
*/
[[nodiscard]] constexpr size_t paramIndex (std::string_view id) noexcept
{
    for (size_t i = 0; i < params.size(); ++i)
        if (std::string_view (params[i].id) == id)
            return i;

    return params.size();
}

/**
    The controls' indices.
*/
namespace param
{
inline constexpr size_t feedback = paramIndex ("feedback");
inline constexpr size_t diffusion = paramIndex ("diffusion");
inline constexpr size_t size = paramIndex ("size");
inline constexpr size_t predelay = paramIndex ("predelay");
inline constexpr size_t earlyTiming = paramIndex ("earlyTiming");
inline constexpr size_t width = paramIndex ("width");
inline constexpr size_t damp = paramIndex ("damp");
inline constexpr size_t thin = paramIndex ("thin");
inline constexpr size_t modulation = paramIndex ("modulation");
inline constexpr size_t mute = paramIndex ("mute");
inline constexpr size_t mix = paramIndex ("mix");
inline constexpr size_t seed = paramIndex ("seed");
} // namespace param

static_assert (param::feedback < params.size() && param::diffusion < params.size() && param::size < params.size()
               && param::predelay < params.size() && param::earlyTiming < params.size() && param::width < params.size()
               && param::damp < params.size() && param::thin < params.size() && param::modulation < params.size()
               && param::mute < params.size() && param::mix < params.size() && param::seed < params.size());

/**
    One stereo audio port.
*/
inline constexpr std::array<PortInfo, 1> stereoPort { { { PortType::audio, "Audio" } } };

/**
    The reverb's type (SPEC.md, 4).
*/
inline constexpr NodeDescriptor reverbDescriptor { "dawg.loopReverb",   "Loop Reverb", stereoPort,
                                                   stereoPort,          params,        true,
                                                   NodeCategory::effect };

/**
    Parameter values by index, in their own units.
*/
using ParamValues = std::array<float, params.size()>;

/**
    Early taps in all three patterns: shared (0-15), left (16-31), right (32-47).
*/
inline constexpr size_t numTaps = 3 * tapsPerPattern;

/**
    Indices of the scalar gains in Targets::Gains::scalar.
*/
namespace scalar
{
inline constexpr size_t earlyAllpass = 0; ///< Gain of the early allpasses.
inline constexpr size_t inputAllpass = 1; ///< Gain of the FDN's input allpasses.
inline constexpr size_t rotationCos = 2;  ///< Cosine of the FDN's rotation angle.
inline constexpr size_t rotationSin = 3;  ///< Sine of the FDN's rotation angle.
inline constexpr size_t loopSin = 4;      ///< Sine of the in-loop allpasses' centre angle (their gain).
inline constexpr size_t loopCos = 5;      ///< Cosine of the same.
inline constexpr size_t loopDepth = 6;    ///< Modulation depth of the in-loop allpasses' angle, in radians.
inline constexpr size_t tail = 7;         ///< The tail's output gain.
inline constexpr size_t dry = 8;          ///< Dry gain (Mix).
inline constexpr size_t wet = 9;          ///< Wet gain (Mix).
inline constexpr size_t midLow = 10;      ///< The tail's mid gain below the crossover (Width).
inline constexpr size_t midHigh = 11;     ///< Its mid gain above.
inline constexpr size_t sideLow = 12;     ///< Its side gain below.
inline constexpr size_t sideHigh = 13;    ///< Its side gain above.
inline constexpr size_t chorusDepth = 14; ///< Modulation depth of the FDN lines' delays, in samples (revision 1.6).
inline constexpr size_t count = 15;       ///< Number of scalars.
} // namespace scalar

/**
    Everything the controls set, at one sample rate: a pure function of the parameters and the seed.
*/
struct Targets
{
    /**
        The early taps. They crossfade when they change.
    */
    struct Early
    {
        std::array<float, numTaps> delay {}; ///< Delays in whole samples.
        std::array<float, numTaps> gain {};  ///< Gains to the input's own side, with timing, width and normalisation.
        std::array<float, numTaps> cross {}; ///< Gains to the other side (side patterns only).
        std::array<float, numTaps> crossDelay {}; ///< Delays to the other side, in whole samples.

        /**
            Compares two tap sets.
        */
        [[nodiscard]] bool operator== (const Early&) const = default;
    } early;

    /**
        Delays, in samples. They glide when they change.
    */
    struct Delays
    {
        std::array<float, 8> earlyAllpass {};       ///< Early allpasses: left 0-3, right 4-7.
        std::array<float, 8> inputAllpass {};       ///< The FDN's input allpasses: left 0-3, right 4-7.
        std::array<float, numLines> line {};        ///< FDN lines.
        std::array<float, numLines> loopAllpass {}; ///< The allpass inside each FDN line.
        std::array<float, numLines> innerTap {};    ///< A second output tap partway along each FDN line.
    } delays;

    /**
        Gains and filter settings after the input stage. They are smoothed when they change.
    */
    struct Gains
    {
        std::array<float, numLines> line {};        ///< FDN lines' gains per pass (mid frequencies).
        std::array<float, numLines> lineLow {};     ///< FDN lines' low-shelf gains, relative to line.
        std::array<float, numLines> lineHigh {};    ///< FDN lines' high-shelf gains (Damp), relative to line.
        std::array<float, numLines> lineAir {};     ///< FDN lines' darkening gains (above its corner).
        std::array<float, numLines> loopAllpass {}; ///< In-loop allpasses' decay gains (for their own delay).
        std::array<float, scalar::count> scalar {}; ///< The rest, indexed by the scalar namespace.
    } gains;

    /**
        The input stage's shelf gains (Thin lows, Damp highs), smoothed like the gains.
    */
    std::array<float, 2> tone {};

    /**
        The pre-delay, in whole samples.
    */
    float predelay = 0.0f;

    /**
        The mute gain: 0 when muted, 1 otherwise.
    */
    float mute = 1.0f;
};

/**
    Computes what the controls set.

    @param values      The parameter values.
    @param pattern     The pattern for the values' seed.
    @param sampleRate  The sample rate.
    @param darkening   Whether the tail darkens as it decays (always, except to measure it: SPEC A15).
    @return            The targets.
    @note              Any thread; bounded work, no allocation.
*/
[[nodiscard]] Targets computeTargets (const ParamValues& values, const Pattern& pattern, double sampleRate,
                                      bool darkening = true) noexcept;

/**
    Returns the main early reflections' times for the panel (SPEC.md 6), in ms after the input, ascending, at most 32
    per channel.

    @param values      The parameter values.
    @param sampleRate  The sample rate.
    @return            Left and right arrival times.
    @note              Message thread (allocates).
*/
[[nodiscard]] std::array<std::vector<double>, 2> arrivals (const ParamValues& values, double sampleRate);

/**
    The reverb.

    @note  Construction, prepare() and meters(): message thread. process(): audio thread.
*/
class ReverbNode final : public EffectNode
{
public:
    /**
        Creates the node with its parameters at their defaults.
    */
    ReverbNode();

    void prepare (const ProcessSpec& spec) override;

    /**
        Silences the reverb: clears its delay lines and filter states, keeping its settings. For DAWg's output
        protection, after the reverb's output went invalid (docs/design/output-protection.md, S1).

        @note  Audio thread; never allocates.
    */
    void clear() noexcept;

    [[nodiscard]] Meters meters() const override;

    /**
        Turns the tail's own darkening (SPEC 5.2) off or on. For measurement only (SPEC A15 compares the reverb with
        a version without the darkening); DAWg never calls it.

        @param on  True (the default) for the reverb as designed.
        @note      Message thread, before prepare().
    */
    void setDarkening (bool on);

private:
    void render (const ProcessContext& context) noexcept override;
    [[nodiscard]] size_t renderAwake (const AudioInput& in, const AudioOutput& out, size_t count) noexcept;
    void clearState() noexcept;
    void restart() noexcept;
    [[nodiscard]] ParamValues readParams() const noexcept;
    void updateTargets() noexcept;
    void advanceInput() noexcept;
    void advanceOutput() noexcept;
    void planOutput() noexcept;
    void wholeDelays() noexcept;
    void advanceRamps() noexcept;
    void modulateLoop() noexcept;
    [[nodiscard]] std::array<float, 2> processTone (float left, float right) noexcept;
    void sumEarly (size_t count) noexcept;
    [[nodiscard]] std::array<float, 2> diffuseEarly (float left, float right) noexcept;
    [[nodiscard]] std::array<float, 2> processTail (float left, float right) noexcept;
    template <bool thin, bool damp>
    void absorb (std::array<float, numLines>& y) noexcept;

    double m_sampleRate = 48000.0;
    ParamValues m_values {};
    Pattern m_pattern {};
    Targets m_target {};
    Targets m_current {};
    Targets::Early m_earlyFrom {}; // the early taps a crossfade starts from; m_current.early is where it goes
    size_t m_fadePosition = 0;     // samples into the early crossfade
    bool m_fading = false;
    bool m_toneSettled = true;
    bool m_delaysSettled = true;    // every delay exactly at its target
    bool m_gainsSettled = true;     // every gain exactly at its target
    bool m_outputMoving = false;    // some delay or gain is still on its way
    size_t m_sampleIndex = 0;       // samples since prepare() or the last silence, for the control-rate grid
    size_t m_quietSamples = 0;      // consecutive samples with the input exactly 0 and the output below -120 dBFS
    size_t m_sleepAfter = 1;        // so many of them, and the node sleeps (SPEC 7.9)
    bool m_sleeping = false;        // asleep: all state is zero, the output is zero, until input that isn't zero
    Targets::Delays m_delayNext {}; // where the current ramps end
    Targets::Delays m_delayStep {}; // their change per sample
    Targets::Gains m_gainNext {};
    Targets::Gains m_gainStep {};
    // While m_delaysSettled, the delays as whole numbers, converted once (revision 1.7). The lines' are one less: they
    // are read before the push.
    std::array<size_t, 8> m_wholeEarlyAllpass {};
    std::array<size_t, 8> m_wholeInputAllpass {};
    std::array<size_t, numLines> m_wholeLine {};
    std::array<size_t, numLines> m_wholeInnerTap {};
    std::array<size_t, numLines> m_wholeLoopAllpass {};

    float m_gainCoefficient = 0.0f;
    float m_rampDelayCoefficient = 0.0f;
    float m_rampGainCoefficient = 0.0f;
    float m_maxDelayChange = 0.0f;
    float m_predelayCoefficient = 0.0f;
    float m_maxPredelayRate = 0.0f;
    float m_muteStep = 0.0f;
    size_t m_fadeLength = 1;
    float m_thinCoefficient = 0.0f;
    float m_dampCoefficient = 0.0f;
    float m_lineLowCoefficient = 0.0f;
    float m_lineHighCoefficient = 0.0f;
    std::array<float, 3> m_lineAirCoefficients {}; // the darkening lowpass's (state-variable filter)
    dsp::LowShelf m_tailMid {};                    // lifts the tail's mid signal below the crossover
    float m_tailMidCoefficient = 0.0f;
    dsp::Crossover m_tailSide {}; // the tail's side signal
    bool m_darkening = true;
    bool m_thinActive = true; // the lines' Thin shelves are needed in this block
    bool m_dampActive = true; // the lines' Damp shelves are needed in this block

    std::array<dsp::LowShelf, 2> m_thinFirst {};
    std::array<dsp::LowShelf, 2> m_thinSecond {};
    std::array<dsp::HighShelf, 2> m_dampFirst {};
    std::array<dsp::HighShelf, 2> m_dampSecond {};
    std::array<dsp::DelayLine, 2> m_input {};      // left, right
    std::array<dsp::Allpass, 8> m_earlyAllpass {}; // left 0-3, right 4-7
    std::array<dsp::Allpass, 8> m_inputAllpass {}; // left 0-3, right 4-7
    dsp::DelayBank<numLines> m_lines {};
    dsp::DelayBank<numLines> m_loopAllpass {};      // the delays inside the lines' allpasses
    std::array<float, numLines> m_lineLowState {};  // the lines' low-shelf lowpass states
    std::array<float, numLines> m_lineHighState {}; // the lines' high-shelf lowpass states
    std::array<float, numLines> m_lineAirFirst {};  // the lines' darkening lowpass states
    std::array<float, numLines> m_lineAirSecond {};
    std::array<float, numLines> m_phase {};   // the in-loop allpasses' modulation phases
    std::array<float, numLines> m_loopSin {}; // their current sin and cos (updated every control period)
    std::array<float, numLines> m_loopCos {};
    std::array<float, numLines> m_chorusPhase {};  // the lines' delay modulation phases
    std::array<float, numLines> m_chorusOffset {}; // their current delay offsets, in samples
    std::array<float, numLines> m_chorusNext {};   // the offsets at the next control tick
    std::array<float, numLines> m_chorusStep {};   // the offsets' change per sample until then
    bool m_chorusActive = false;                   // some offset is not zero until the next tick
    std::array<dsp::DelayLine, 2> m_predelay {};

    // One block of: dry (after mute), wet (after tone), early (new taps) and early (old taps), left and right.
    std::array<std::vector<float>, 8> m_scratch {};

    // Peak output level per channel since the last meters() call: written by the audio thread, read and reset by the
    // message thread.
    mutable std::array<std::atomic<float>, 2> m_peak {};
};

} // namespace dawg::reverb
