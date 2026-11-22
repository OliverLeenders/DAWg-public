#include "dawg/BuiltinNodes.h"
#include "dawg/Crossover.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>

// Split nodes (docs/design/split-nodes.md): the Band Split, and the Stereo Split and Merge in mid/side or left/right.

namespace dawg
{

namespace
{
constexpr double smoothingSeconds = 0.02;
constexpr int coefficientInterval = 16; // samples between crossover updates, as the Filter node's

[[nodiscard]] float dbToGain (float db) noexcept
{
    return db <= -96.0f ? 0.0f : std::pow (10.0f, db / 20.0f);
}

// ---------------------------------------------------------------------------------------------------------
// Band Split (SP1-SP3, SP7, SP11)

constexpr std::array<PortInfo, 1> adaptiveInput { { { PortType::audio, "Audio", adaptiveChannels } } };
constexpr auto bandSplitInputs = withModulationInputs (adaptiveInput);

// Four bands and the Dry port, fixed by index so cables keep their ports. With fewer bands the higher band ports are
// silent and the graph view hides them and names the rest after the count (Low, Mid, High).
constexpr std::array<PortInfo, 5> bandSplitOutputs { {
    { PortType::audio, "Low", adaptiveChannels },
    { PortType::audio, "Band 2", adaptiveChannels },
    { PortType::audio, "Band 3", adaptiveChannels },
    { PortType::audio, "High", adaptiveChannels },
    { PortType::audio, "Dry", adaptiveChannels },
} };

constexpr size_t dryPort = 4;

constexpr std::array<const char*, 5> soloChoices { "Off", "Band 1", "Band 2", "Band 3", "Band 4" };
constexpr std::array<const char*, 2> phaseChoices { "Minimum", "Linear" };

// The crossovers' defaults are 3 bands' (SP2); setBandCount moves untouched ones to another count's.
constexpr std::array<ParamInfo, 5> bandSplitParams { {
    { "crossover1", "Crossover 1", 20.0f, 20000.0f, 200.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "crossover2", "Crossover 2", 20.0f, 20000.0f, 2500.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "crossover3", "Crossover 3", 20.0f, 20000.0f, 6000.0f, ParamKind::continuous, "Hz", 1000.0f },
    { "solo", "Solo", 0.0f, 4.0f, 0.0f, ParamKind::choice, "", 0.0f, soloChoices },
    // Linear phase changes the latency, so it isn't automatable (SP11).
    { "phase", "Phase", 0.0f, 1.0f, 0.0f, ParamKind::choice, "", 0.0f, phaseChoices, ModulationPolarity::bipolar, false,
      true, 0.0f, false },
} };

const NodeDescriptor bandSplitDescriptor { nodetype::bandSplit, "Band Split", bandSplitInputs,      bandSplitOutputs,
                                           bandSplitParams,     true,         NodeCategory::utility };

class BandSplitNode final : public Node
{
public:
    BandSplitNode()
        : Node (bandSplitDescriptor)
    {
    }

    void configure (const GraphNode& node) override
    {
        m_bypassed.store (node.bypassed);
        m_bands.store (std::clamp (node.bands, 2, CrossoverBank::maxBands));

        // The linear-phase bank is made the first time it's chosen and kept, as the audio thread may still run it
        // until the recompile that the choice starts has taken over (SP11).
        const auto linear = params().get (phaseParam) >= 0.5f;

        if (linear && m_linear == nullptr)
        {
            m_linear = std::make_unique<LinearPhaseCrossovers>();

            if (m_prepared)
                m_linear->prepare (m_sampleRate, 2);
        }

        if (linear && m_prepared)
            m_linear->setInitial (initialFrequencies());

        m_useLinear.store (linear);
    }

    void prepare (const ProcessSpec& spec) override
    {
        m_sampleRate = spec.sampleRate;
        m_bank.prepare (spec.sampleRate, 2);

        if (m_linear != nullptr)
        {
            m_linear->prepare (spec.sampleRate, 2);
            m_linear->setInitial (initialFrequencies());
        }

        for (auto& smoother : m_frequencies)
            smoother.reset (spec.sampleRate, smoothingSeconds);

        m_prepared = true;
        reset();
    }

    void reset() noexcept override
    {
        m_bank.reset();

        if (m_linear != nullptr)
            m_linear->reset();

        m_snap = true;
    }

    // Linear phase delays every band by the bank's latency; the choice recompiles, which reads this again.
    [[nodiscard]] int latencySamples() const noexcept override
    {
        return m_useLinear.load() ? LinearPhaseCrossovers::latencyFor (m_sampleRate) : 0;
    }

    void process (const ProcessContext& context) noexcept override
    {
        const juce::ScopedNoDenormals noDenormals;
        const auto& in = context.audioInputs[0];
        const auto& outs = context.audioOutputs;
        const auto channels = static_cast<size_t> (context.channels);
        const auto n = context.numSamples;
        const auto bands = static_cast<size_t> (m_bands.load());

        // Bypassed: everything in the lowest band, so the bands still add up to the input.
        if (m_bypassed.load())
        {
            for (size_t port = 0; port < outs.size(); ++port)
                for (size_t ch = 0; ch < channels; ++ch)
                    if (port == 0 || port == dryPort)
                        juce::FloatVectorOperations::copy (outs[port][ch], in[ch], n);
                    else
                        juce::FloatVectorOperations::clear (outs[port][ch], n);

            return;
        }

        CrossoverBank& bank =
            m_useLinear.load() && m_linear != nullptr ? static_cast<CrossoverBank&> (*m_linear) : m_bank;

        for (int start = 0; start < n; start += coefficientInterval)
        {
            const auto length = std::min (coefficientInterval, n - start);
            bank.setFrequencies (frequencies (context, start, length, bands - 1));

            for (size_t ch = 0; ch < channels; ++ch)
            {
                std::array<float*, CrossoverBank::maxBands> destinations {};

                for (size_t b = 0; b < bands; ++b)
                    destinations[b] = outs[b][ch] + start;

                bank.process (ch, in[ch] + start, std::span<float* const> (destinations.data(), bands),
                              outs[dryPort][ch] + start, length);
            }
        }

        // Bands the count leaves out are silent, and Solo silences all but one (SP2).
        const auto solo = static_cast<size_t> (std::lround (params().get (3)));

        for (size_t b = 0; b < static_cast<size_t> (CrossoverBank::maxBands); ++b)
            if (b >= bands || (solo > 0 && solo <= bands && b + 1 != solo))
                for (size_t ch = 0; ch < channels; ++ch)
                    juce::FloatVectorOperations::clear (outs[b][ch], n);
    }

private:
    // The crossovers for a stretch of samples: modulated or set, kept in order a third of an octave apart, smoothed.
    std::span<const float> frequencies (const ProcessContext& context, int start, int length, size_t count) noexcept
    {
        static constexpr float minimumRatio = 1.2599210f; // a third of an octave
        const auto top = static_cast<float> (std::min (20000.0, 0.45 * m_sampleRate));
        auto below = 0.0f;

        for (size_t i = 0; i < count; ++i)
        {
            const auto* modulated = context.modulatedValues (i);
            auto target = modulated != nullptr ? modulated[start + length / 2] : params().get (i);
            target = std::clamp (std::max (target, below * minimumRatio), 20.0f, top);
            below = target;

            auto& smoother = m_frequencies[i];

            if (m_snap)
                smoother.setCurrentAndTargetValue (target);
            else
                smoother.setTargetValue (target);

            m_current[i] = smoother.skip (length);
        }

        m_snap = false;
        return { m_current.data(), count };
    }

    // The crossovers as set, in order, for the linear-phase bank's first kernels (message thread).
    [[nodiscard]] std::vector<float> initialFrequencies() const
    {
        const auto count = static_cast<size_t> (m_bands.load()) - 1;
        const auto top = static_cast<float> (std::min (20000.0, 0.45 * m_sampleRate));
        std::vector<float> result;
        auto below = 0.0f;

        for (size_t i = 0; i < count; ++i)
        {
            below = std::clamp (std::max (params().get (i), below * 1.2599210f), 20.0f, top);
            result.push_back (below);
        }

        return result;
    }

    static constexpr size_t phaseParam = 4;

    double m_sampleRate = 48000.0;
    bool m_prepared = false;
    MinimumPhaseCrossovers m_bank;
    std::unique_ptr<LinearPhaseCrossovers> m_linear;
    std::atomic<bool> m_useLinear { false };
    std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>, 3> m_frequencies {
        { juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> (200.0f),
          juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> (2500.0f),
          juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> (6000.0f) }
    };
    std::array<float, 3> m_current {};
    bool m_snap = true;
    std::atomic<int> m_bands { 3 };
    std::atomic<bool> m_bypassed { false };
};

// ---------------------------------------------------------------------------------------------------------
// Stereo Split and Merge (SP4, SP5, SP12)

constexpr std::array<PortInfo, 1> stereoPort { { { PortType::audio, "Audio" } } };

// Named for mid/side; the graph view names them Left and Right in that mode.
constexpr std::array<PortInfo, 2> monoParts { {
    { PortType::audio, "Mid", 1 },
    { PortType::audio, "Side", 1 },
} };
constexpr auto mergeInputs = withModulationInputs (monoParts);

const NodeDescriptor stereoSplitDescriptor { nodetype::stereoSplit, "Stereo Split", stereoPort, monoParts, {}, true,
                                             NodeCategory::utility };

constexpr std::array<ParamInfo, 2> mergeParams { {
    { "gain1", "Mid", -96.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
    { "gain2", "Side", -96.0f, 12.0f, 0.0f, ParamKind::continuous, "dB" },
} };

const NodeDescriptor stereoMergeDescriptor { nodetype::stereoMerge, "Stereo Merge", mergeInputs,          stereoPort,
                                             mergeParams,           true,           NodeCategory::utility };

class StereoSplitNode final : public Node
{
public:
    StereoSplitNode()
        : Node (stereoSplitDescriptor)
    {
    }

    void configure (const GraphNode& node) override { m_leftRight.store (node.stereoMode == StereoMode::leftRight); }

    void prepare (const ProcessSpec& /*spec*/) override {}

    // Splitting loses nothing, so a bypassed split does the same.
    void process (const ProcessContext& context) noexcept override
    {
        const auto& in = context.audioInputs[0];
        auto* first = context.audioOutputs[0][0];
        auto* second = context.audioOutputs[1][0];
        const auto n = context.numSamples;

        if (m_leftRight.load())
        {
            juce::FloatVectorOperations::copy (first, in[0], n);
            juce::FloatVectorOperations::copy (second, in[1], n);
            return;
        }

        for (int s = 0; s < n; ++s)
        {
            const auto left = in[0][s];
            const auto right = in[1][s];
            first[s] = 0.5f * (left + right);
            second[s] = 0.5f * (left - right);
        }
    }

private:
    std::atomic<bool> m_leftRight { false };
};

class StereoMergeNode final : public Node
{
public:
    StereoMergeNode()
        : Node (stereoMergeDescriptor)
    {
    }

    void configure (const GraphNode& node) override
    {
        m_leftRight.store (node.stereoMode == StereoMode::leftRight);
        m_bypassed.store (node.bypassed);
    }

    void prepare (const ProcessSpec& spec) override
    {
        for (auto& gain : m_gains)
            gain.reset (spec.sampleRate, smoothingSeconds);

        reset();
    }

    void reset() noexcept override
    {
        for (size_t i = 0; i < m_gains.size(); ++i)
            m_gains[i].setCurrentAndTargetValue (dbToGain (params().get (i)));
    }

    // A bypassed merge joins its parts at 0 dB.
    void process (const ProcessContext& context) noexcept override
    {
        const auto* first = context.audioInputs[0][0];
        const auto* second = context.audioInputs[1][0];
        const auto& out = context.audioOutputs[0];
        const auto bypassed = m_bypassed.load();
        const auto leftRight = m_leftRight.load();

        for (size_t i = 0; i < m_gains.size(); ++i)
        {
            const auto* modulated = context.modulatedValues (i);
            const auto db =
                modulated != nullptr && context.numSamples > 0 ? modulated[context.numSamples / 2] : params().get (i);
            m_gains[i].setTargetValue (bypassed ? 1.0f : dbToGain (std::clamp (db, -96.0f, 12.0f)));
        }

        for (int s = 0; s < context.numSamples; ++s)
        {
            const auto a = first[s] * m_gains[0].getNextValue();
            const auto b = second[s] * m_gains[1].getNextValue();
            out[0][s] = leftRight ? a : a + b;
            out[1][s] = leftRight ? b : a - b;
        }
    }

private:
    std::array<juce::SmoothedValue<float>, 2> m_gains;
    std::atomic<bool> m_leftRight { false };
    std::atomic<bool> m_bypassed { false };
};
} // namespace

void registerSplitNodes (NodeRegistry& registry)
{
    registry.add (bandSplitDescriptor, [] { return std::make_unique<BandSplitNode>(); });
    registry.add (stereoSplitDescriptor, [] { return std::make_unique<StereoSplitNode>(); });
    registry.add (stereoMergeDescriptor, [] { return std::make_unique<StereoMergeNode>(); });
}

} // namespace dawg
