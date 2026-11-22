#include "dawg/BuiltinNodes.h"
#include "dawg/Model.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "loopreverb/Reverb.h"

#include <array>
#include <string_view>

// Loop Reverb: adapts the clean-room reverb (loopreverb/, README.md there; docs/design/loop-reverb.md) to DAWg's
// node API. The reverb keeps its own copy of the node API (loopreverb/dawg_sdk/), so this node owns one and hands
// it the parameters, the audio and the transport each block.

namespace dawg
{

namespace
{
[[nodiscard]] constexpr ParamKind toDawg (reverb::ParamKind kind) noexcept
{
    switch (kind)
    {
        case reverb::ParamKind::continuous:
            return ParamKind::continuous;
        case reverb::ParamKind::integer:
            return ParamKind::integer;
        case reverb::ParamKind::choice:
            return ParamKind::choice;
        case reverb::ParamKind::toggle:
            return ParamKind::toggle;
    }

    return ParamKind::continuous;
}

[[nodiscard]] constexpr ModulationPolarity toDawg (reverb::ModulationPolarity polarity) noexcept
{
    switch (polarity)
    {
        case reverb::ModulationPolarity::bipolar:
            return ModulationPolarity::bipolar;
        case reverb::ModulationPolarity::positive:
            return ModulationPolarity::positive;
        case reverb::ModulationPolarity::negative:
            return ModulationPolarity::negative;
    }

    return ModulationPolarity::bipolar;
}

// The reverb's parameters, field by field, as DAWg's.
constexpr auto reverbParams = []
{
    std::array<ParamInfo, reverb::params.size()> result {};

    for (size_t i = 0; i < result.size(); ++i)
    {
        const auto& from = reverb::params[i];
        result[i] = { from.id,           from.name,
                      from.minValue,     from.maxValue,
                      from.defaultValue, toDawg (from.kind),
                      from.unit,         from.midValue,
                      from.choices,      toDawg (from.modulationPolarity) };
    }

    return result;
}();

constexpr std::array<PortInfo, 1> stereoPort { { { PortType::audio, "Audio" } } };

// Mono in, stereo out: a mono source is read on both sides (docs/design/mono-ports.md, section 6).
constexpr std::array<PortInfo, 1> adaptivePort { { { PortType::audio, "Audio", adaptiveChannels } } };
constexpr auto adaptiveModPorts = withModulationInputs (adaptivePort);

static_assert (std::string_view (reverb::reverbDescriptor.type) == nodetype::loopReverb);

const NodeDescriptor reverbDescriptor { nodetype::loopReverb, reverb::reverbDescriptor.displayName,
                                        adaptiveModPorts,     stereoPort,
                                        reverbParams,         reverb::reverbDescriptor.userEditable,
                                        NodeCategory::effect };

class LoopReverbNode final : public Node
{
public:
    LoopReverbNode()
        : Node (reverbDescriptor)
    {
    }

    void configure (const GraphNode& node) override { m_reverb.setBypassed (node.bypassed); }

    void prepare (const ProcessSpec& spec) override
    {
        copyParams();
        m_sampleRate = spec.sampleRate;
        m_reverb.prepare ({ spec.sampleRate, spec.maxBlockSize });
    }

    void reset() noexcept override { m_reverb.clear(); }

    void process (const ProcessContext& context) noexcept override
    {
        // The engine has applied automation and modulation to this node's parameters.
        copyParams();

        reverb::Transport transport;

        if (context.transport != nullptr)
            transport = { context.transport->playing, context.transport->startTick, context.transport->endTick };

        const std::array<reverb::AudioInput, 1> inputs { context.audioInputs[0] };
        const std::array<reverb::AudioOutput, 1> outputs { context.audioOutputs[0] };
        m_reverb.process ({ context.numSamples, inputs, outputs, &transport });
    }

    [[nodiscard]] int latencySamples() const noexcept override { return m_reverb.latencySamples(); }

    // The reverb's measurements (SPEC.md 6: "arrivalsLeft", "arrivalsRight", "level"), as an object of arrays. The
    // arrivals come from this node's parameters, which the reverb only receives while it processes: a node that
    // doesn't reach the output still shows its controls' settings.
    [[nodiscard]] juce::var meters() const override
    {
        auto measured = m_reverb.meters();
        reverb::ParamValues settings {};

        for (size_t i = 0; i < settings.size(); ++i)
            settings[i] = params().get (i);

        auto times = reverb::arrivals (settings, m_sampleRate);
        measured["arrivalsLeft"] = std::move (times[0]);
        measured["arrivalsRight"] = std::move (times[1]);
        juce::var result (new juce::DynamicObject());
        auto* object = result.getDynamicObject();

        for (const auto& [key, values] : measured)
        {
            juce::Array<juce::var> array;

            for (const auto value : values)
                array.add (value);

            object->setProperty (juce::Identifier (key.c_str()), array);
        }

        return result;
    }

private:
    void copyParams() noexcept
    {
        for (size_t i = 0; i < reverbParams.size(); ++i)
            m_reverb.params().set (i, params().get (i));
    }

    reverb::ReverbNode m_reverb;
    double m_sampleRate = 48000.0;
};
} // namespace

void registerLoopReverb (NodeRegistry& registry)
{
    registry.add (reverbDescriptor, [] { return std::make_unique<LoopReverbNode>(); });
}

} // namespace dawg
