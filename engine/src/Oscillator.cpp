#include "dawg/BuiltinNodes.h"
#include "dawg/CompiledEngine.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/PolyBlep.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

// The oscillator (docs/design/phase12-voice-scope.md, V3, V14): band-limited sine, triangle, saw and pulse, and white
// noise, with up to eight detuned unison copies spread across the stereo field. It has no note input: its pitch is a
// parameter, which Note Expression's pitch signal drives at 100 % depth.

namespace dawg
{

namespace
{
constexpr double twoPi = juce::MathConstants<double>::twoPi;
constexpr size_t maxUnison = 8;
constexpr float toggleThreshold = 0.5f;

constexpr std::array<const char*, 5> waveformChoices { "Sine", "Triangle", "Saw", "Pulse", "Noise" };

enum class Waveform
{
    sine,
    triangle,
    saw,
    pulse,
    noise,
};

constexpr std::array<ParamInfo, 12> oscillatorParams { {
    { "waveform", "Waveform", 0.0f, 4.0f, 2.0f, ParamKind::choice, "", 0.0f, waveformChoices },
    { "pitch", "Pitch", -120.0f, 120.0f, 0.0f, ParamKind::continuous, "st" },
    { "coarse", "Coarse", -48.0f, 48.0f, 0.0f, ParamKind::integer, "st" },
    { "fine", "Fine", -100.0f, 100.0f, 0.0f, ParamKind::continuous, "ct" },
    { "pulseWidth", "Pulse width", 1.0f, 99.0f, 50.0f, ParamKind::continuous, "%" },
    { "level", "Level", -60.0f, 0.0f, -12.0f, ParamKind::continuous, "dB" },
    { "unison", "Unison", 1.0f, static_cast<float> (maxUnison), 1.0f, ParamKind::integer },
    { "detune", "Detune", 0.0f, 100.0f, 20.0f, ParamKind::continuous, "ct" },
    { "spread", "Spread", 0.0f, 100.0f, 50.0f, ParamKind::continuous, "%" },
    { "phase", "Phase", 0.0f, 360.0f, 0.0f, ParamKind::continuous, "deg" },
    { "randomPhase", "Random phase", 0.0f, 100.0f, 0.0f, ParamKind::continuous, "%" },
    { "free", "Free phase", 0.0f, 1.0f, 0.0f, ParamKind::toggle },
} };

enum OscillatorParam : size_t
{
    waveformParam,
    pitchParam,
    coarseParam,
    fineParam,
    pulseWidthParam,
    levelParam,
    unisonParam,
    detuneParam,
    spreadParam,
    phaseParam,
    randomPhaseParam,
    freeParam,
};

constexpr std::array<PortInfo, 0> noInputs {};
constexpr auto oscillatorInputs = withModulationInputs (noInputs);
constexpr std::array<PortInfo, 1> stereoOutput { { { PortType::audio, "Audio" } } };

const NodeDescriptor oscillatorDescriptor { nodetype::oscillator, "Oscillator", oscillatorInputs,     stereoOutput,
                                            oscillatorParams,     true,         NodeCategory::utility };

using polyblep::blamp;
using polyblep::blep;

[[nodiscard]] double wrap (double phase) noexcept
{
    return phase - std::floor (phase);
}

[[nodiscard]] float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db / 20.0f);
}

class OscillatorNode final : public Node
{
public:
    OscillatorNode()
        : Node (oscillatorDescriptor),
          m_phaseRandom (nextPhaseSeed())
    {
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        m_levelCoefficient = modulationSmoothingCoefficient (spec.sampleRate);
        m_started = false;
        m_snapLevel = true;
    }

    void reset() noexcept override
    {
        // A free-running oscillator keeps its phases; otherwise the next block starts at the phase parameter.
        if (params().get (freeParam) < toggleThreshold)
            m_started = false;

        m_snapLevel = true;
    }

    void configure (const GraphNode& node) override { m_bypassed.store (node.bypassed); }

    void process (const ProcessContext& context) noexcept override
    {
        const auto& out = context.audioOutputs[0];

        if (m_bypassed.load())
        {
            juce::FloatVectorOperations::clear (out[0], context.numSamples);
            juce::FloatVectorOperations::clear (out[1], context.numSamples);
            return;
        }

        // The parameters are applied after prepare(), so the start phases are read on the first block.
        if (! m_started)
        {
            startPhases();
            m_started = true;
        }

        const auto unison = layout();
        const Signals signals { context.modulatedValues (pitchParam), context.modulatedValues (fineParam),
                                context.modulatedValues (pulseWidthParam), context.modulatedValues (levelParam) };

        if (m_snapLevel)
        {
            m_level = dbToGain (signals.level != nullptr ? signals.level[0] : params().get (levelParam));
            m_snapLevel = false;
        }

        const auto waveform = static_cast<Waveform> (std::clamp (
            static_cast<int> (std::lround (params().get (waveformParam))), 0, static_cast<int> (Waveform::noise)));

        // The pitch and the level usually hold still for many samples (a note's pitch, an unmodulated level): their
        // exponentials, and the copies' phase steps, are worked out again only when what they come from changes.
        Recent pitch;
        Recent level;
        std::array<double, maxUnison> steps {};

        for (int s = 0; s < context.numSamples; ++s)
        {
            const auto pitchValue = signals.pitch != nullptr ? signals.pitch[s] : params().get (pitchParam);
            const auto fineValue = signals.fine != nullptr ? signals.fine[s] : params().get (fineParam);

            if (pitch.changed (pitchValue, fineValue))
            {
                const auto frequency = frequencyOf (pitchValue, fineValue);

                for (size_t i = 0; i < unison.count; ++i)
                    steps[i] = std::min (0.49, frequency * unison.ratios[i] / m_sampleRate);
            }

            const auto width = std::clamp (
                static_cast<double> (signals.width != nullptr ? signals.width[s] : params().get (pulseWidthParam))
                    / 100.0,
                0.01, 0.99);
            const auto levelDb = signals.level != nullptr ? signals.level[s] : params().get (levelParam);

            if (level.changed (levelDb, 0.0f))
                m_levelGain = dbToGain (levelDb);

            m_level += (m_levelGain - m_level) * m_levelCoefficient;

            float left = 0.0f;
            float right = 0.0f;

            for (size_t i = 0; i < unison.count; ++i)
            {
                const auto dt = steps[i];
                const auto value = static_cast<float> (next (waveform, m_phases[i], dt, width));
                left += value * unison.gains[i][0];
                right += value * unison.gains[i][1];
            }

            out[0][s] = left * m_level;
            out[1][s] = right * m_level;
        }
    }

private:
    // The per-sample modulated parameters, or nullptr where the parameter is not modulated.
    struct Signals
    {
        const float* pitch = nullptr;
        const float* fine = nullptr;
        const float* width = nullptr;
        const float* level = nullptr;
    };

    // The unison copies for this block: each one's frequency ratio and stereo gains.
    struct Unison
    {
        size_t count = 1;
        std::array<double, maxUnison> ratios {};
        std::array<std::array<float, 2>, maxUnison> gains {};
    };

    [[nodiscard]] Unison layout() const noexcept
    {
        Unison result;
        result.count = static_cast<size_t> (
            std::clamp (static_cast<int> (std::lround (params().get (unisonParam))), 1, static_cast<int> (maxUnison)));
        const auto detune = static_cast<double> (params().get (detuneParam));
        const auto spread = params().get (spreadParam) / 100.0f;
        const auto normalise = 1.0f / std::sqrt (static_cast<float> (result.count));

        for (size_t i = 0; i < result.count; ++i)
        {
            // Spread evenly from -1 to 1 across the copies: detuned by half the detune each way, panned by the spread.
            const auto position =
                result.count > 1 ? 2.0 * static_cast<double> (i) / static_cast<double> (result.count - 1) - 1.0 : 0.0;
            result.ratios[i] = std::exp2 (position * detune / 2.0 / 1200.0);
            const auto pan = panGains (static_cast<float> (position) * spread);
            result.gains[i] = { pan[0] * normalise, pan[1] * normalise };
        }

        return result;
    }

    // Two values, and whether they differ from the last ones (to the bit: a NaN never repeats).
    class Recent
    {
    public:
        [[nodiscard]] bool changed (float a, float b) noexcept
        {
            const auto bits = std::array { std::bit_cast<std::uint32_t> (a), std::bit_cast<std::uint32_t> (b) };

            if (m_known && bits == m_bits && ! std::isnan (a) && ! std::isnan (b))
                return false;

            m_bits = bits;
            m_known = true;
            return true;
        }

    private:
        std::array<std::uint32_t, 2> m_bits {};
        bool m_known = false;
    };

    // The base frequency: the pitch relative to C4, plus coarse and fine tuning.
    [[nodiscard]] double frequencyOf (float pitchValue, float fineValue) const noexcept
    {
        const auto pitch = static_cast<double> (pitchValue);
        const auto fine = static_cast<double> (fineValue);
        const auto semitones =
            pitchSignalCentre + pitch + static_cast<double> (params().get (coarseParam)) + fine / 100.0;
        return 440.0 * std::exp2 ((semitones - 69.0) / 12.0);
    }

    // The copies start at the phase parameter, each further one offset by the golden ratio so they don't start in step.
    // Random phase moves each copy's start by up to a whole cycle, differently on every start (a new note in a voice
    // slot), so unison copies don't phase the same way at every attack. Free-running copies start at random phases.
    void startPhases() noexcept
    {
        constexpr double golden = 0.6180339887498949;
        const auto free = params().get (freeParam) >= toggleThreshold;
        const auto start = static_cast<double> (params().get (phaseParam)) / 360.0;
        const auto randomness = std::clamp (static_cast<double> (params().get (randomPhaseParam)) / 100.0, 0.0, 1.0);
        m_noise = noiseSeed;

        for (size_t i = 0; i < m_phases.size(); ++i)
        {
            const auto offset = randomness * static_cast<double> (randomPhase());
            m_phases[i] =
                free ? static_cast<double> (random()) : wrap (start + golden * static_cast<double> (i) + offset);
        }
    }

    // Each instance draws its random phases from its own sequence, so voice slots don't start alike.
    [[nodiscard]] static std::uint32_t nextPhaseSeed() noexcept
    {
        static std::atomic<std::uint32_t> instances { 0 };
        return 0x2545f491U ^ (0x9e3779b9U * (instances.fetch_add (1) + 1U));
    }

    // A xorshift32 number in 0..1 from the phase sequence, which, unlike the noise's, carries on across starts.
    [[nodiscard]] float randomPhase() noexcept
    {
        m_phaseRandom ^= m_phaseRandom << 13U;
        m_phaseRandom ^= m_phaseRandom >> 17U;
        m_phaseRandom ^= m_phaseRandom << 5U;
        return static_cast<float> (m_phaseRandom) / static_cast<float> (UINT32_MAX);
    }

    // One sample of a waveform at a phase, advancing the phase.
    [[nodiscard]] double next (Waveform waveform, double& phase, double dt, double width) noexcept
    {
        const auto t = phase;
        phase = wrap (phase + dt);

        switch (waveform)
        {
            case Waveform::sine:
                return std::sin (twoPi * t);

            case Waveform::triangle:
                // 1 - 4 |t - 0.5| has corners at 0 (the slope rises by 8 dt a sample) and at 0.5 (it falls by as much).
                return 1.0 - 4.0 * std::abs (t - 0.5) + 8.0 * dt * (blamp (t, dt) - blamp (wrap (t + 0.5), dt));

            case Waveform::saw:
                return 2.0 * t - 1.0 - blep (t, dt);

            case Waveform::pulse:
                return (t < width ? 1.0 : -1.0) + blep (t, dt) - blep (wrap (t - width), dt);

            case Waveform::noise:
                return static_cast<double> (random()) * 2.0 - 1.0;
        }

        return 0.0;
    }

    // A xorshift32 number in 0..1.
    [[nodiscard]] float random() noexcept
    {
        m_noise ^= m_noise << 13U;
        m_noise ^= m_noise >> 17U;
        m_noise ^= m_noise << 5U;
        return static_cast<float> (m_noise) / static_cast<float> (UINT32_MAX);
    }

    static constexpr std::uint32_t noiseSeed = 0x9e3779b9U;

    double m_sampleRate = 48000.0;
    std::array<double, maxUnison> m_phases {};
    std::uint32_t m_noise = noiseSeed;
    std::uint32_t m_phaseRandom;
    float m_level = 0.0f;
    float m_levelGain = 0.0f; // the level's gain, worked out in process()
    float m_levelCoefficient = 1.0f;
    bool m_started = false;   // whether the phases have been set from the parameters
    bool m_snapLevel = false; // whether the level jumps to its value on the next block
    std::atomic<bool> m_bypassed { false };
};
} // namespace

void registerOscillator (NodeRegistry& registry)
{
    registry.add (oscillatorDescriptor, [] { return std::make_unique<OscillatorNode>(); });
}

} // namespace dawg
