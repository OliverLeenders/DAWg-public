#include "dawg/PluginNode.h"

#include "dawg/AllocationGuard.h"
#include "dawg/Modulation.h"
#include "dawg/NodeRegistry.h"
#include "dawg/VarUtils.h"

#include <algorithm>

namespace dawg
{

namespace
{
constexpr std::array<PortInfo, 1> eventInput { { { PortType::events, "Notes" } } };
// An effect's main buses follow its input (docs/design/mono-ports.md, MP4): the input is adaptive, and the output takes
// the width of the bus the plugin gets (PluginNode::outputWidth()).
constexpr std::array<PortInfo, 1> adaptive { { { PortType::audio, "Audio", adaptiveChannels } } };
// Effects can be keyed through a plugin's sidechain bus (docs/design/sidechain.md, K7).
constexpr std::array<PortInfo, 2> keyed { {
    { PortType::audio, "Audio", adaptiveChannels },
    { PortType::audio, "Sidechain" },
} };
constexpr auto instrumentInputs = withModulationInputs (eventInput);
constexpr auto effectInputs = withModulationInputs (keyed);

// Main, then Out 2 to Out 16, one per output bus after the main one (docs/design/multi-output-instruments.md, MO1). A
// bus plays from its port while that is wired, and in Main otherwise (MO2). The graph view names them after the buses.
constexpr std::array<PortInfo, maxPluginOutputs> instrumentOutputs { {
    { PortType::audio, "Main" },
    { PortType::audio, "Out 2" },
    { PortType::audio, "Out 3" },
    { PortType::audio, "Out 4" },
    { PortType::audio, "Out 5" },
    { PortType::audio, "Out 6" },
    { PortType::audio, "Out 7" },
    { PortType::audio, "Out 8" },
    { PortType::audio, "Out 9" },
    { PortType::audio, "Out 10" },
    { PortType::audio, "Out 11" },
    { PortType::audio, "Out 12" },
    { PortType::audio, "Out 13" },
    { PortType::audio, "Out 14" },
    { PortType::audio, "Out 15" },
    { PortType::audio, "Out 16" },
} };

const NodeDescriptor instrumentDescriptor {
    nodetype::pluginInstrument, "Instrument plugin", instrumentInputs, instrumentOutputs, {}, true,
    NodeCategory::instrument
};

[[nodiscard]] bool wired (const ProcessContext& context, size_t port) noexcept
{
    return port < context.audioOutputs.size() && port < 32 && (context.wiredOutputs & (1u << port)) != 0;
}
const NodeDescriptor effectDescriptor { nodetype::pluginEffect, "Effect plugin", effectInputs, adaptive, {} };
} // namespace

const NodeDescriptor& pluginInstrumentDescriptor() noexcept
{
    return instrumentDescriptor;
}

const NodeDescriptor& pluginEffectDescriptor() noexcept
{
    return effectDescriptor;
}

PluginNode::PluginNode (bool instrument)
    : Node (instrument ? instrumentDescriptor : effectDescriptor),
      m_instrument (instrument)
{
}

PluginNode::~PluginNode()
{
    if (auto* native = m_native.load())
        native->setNoteEndListener (nullptr);

    if (m_processor != nullptr && m_prepared)
        m_processor->releaseResources();
}

juce::AudioPlayHead::PositionInfo positionInfo (const Transport& transport) noexcept
{
    const auto& map = transport.timeMap.tempoMap();
    const auto tick = transport.startTick;
    const auto& signature = map.signatureAt (tick);
    const auto seconds = map.secondsAt (tick);
    const auto beats = [] (double ticks) { return ticks / static_cast<double> (ticksPerBeat); };

    juce::AudioPlayHead::PositionInfo info;
    info.setBpm (map.bpmAt (tick));
    info.setTimeSignature (juce::AudioPlayHead::TimeSignature { signature.numerator, signature.denominator });
    info.setPpqPosition (beats (tick));
    info.setPpqPositionOfLastBarStart (beats (static_cast<double> (map.tickOfBar (map.barAt (tick).bar))));
    info.setTimeInSeconds (seconds);
    info.setTimeInSamples (static_cast<juce::int64> (std::llround (seconds * transport.timeMap.sampleRate())));
    info.setIsPlaying (transport.playing);
    return info;
}

void PluginNode::attach (std::unique_ptr<juce::AudioProcessor> processor)
{
    jassert (processor != nullptr && m_processor == nullptr);
    m_processor = std::move (processor);
    m_processor->setPlayHead (&m_playHead);

    if (! m_instrument)
    {
        enableSidechain();
        findMainLayouts();
    }
    else if (m_allOutputs)
    {
        applyOutputLayout();
    }

    m_status = Status::ready;

    if (m_prepared)
        preparePlugin();

    m_latency.store (m_processor->getLatencySamples());

    if (auto* native = dynamic_cast<ClapNativeInput*> (m_processor.get()))
    {
        native->setNoteEndListener (&m_nativeNotes);
        m_native.store (native);
    }

    m_active.store (m_processor.get());
}

int PluginNode::parameterIndex (const juce::String& id) const
{
    if (m_processor == nullptr)
        return -1;

    const auto& parameters = m_processor->getParameters();

    for (int i = 0; i < parameters.size(); ++i)
    {
        const auto* withId = dynamic_cast<const juce::HostedAudioProcessorParameter*> (parameters[i]);

        if ((withId != nullptr ? withId->getParameterID() : juce::String (i)) == id)
            return i;
    }

    return -1;
}

void PluginNode::setAutomatedParameter (int index, float value) noexcept
{
    setModulatedParameter (index, value);
}

float PluginNode::parameterValue (int index) const noexcept
{
    auto* processor = m_active.load();

    if (processor == nullptr)
        return 0.0f;

    const auto& parameters = processor->getParameters();

    return index >= 0 && index < parameters.size()
               ? std::clamp (parameters.getUnchecked (index)->getValue(), 0.0f, 1.0f)
               : 0.0f;
}

void PluginNode::setModulatedParameter (int index, float value) noexcept
{
    auto* processor = m_active.load();

    if (processor == nullptr)
        return;

    const auto& parameters = processor->getParameters();

    if (index >= 0 && index < parameters.size())
        parameters.getUnchecked (index)->setValue (std::clamp (value, 0.0f, 1.0f));
}

bool PluginNode::isModulatable (int index) const noexcept
{
    const auto* native = m_native.load();
    return native != nullptr && native->isModulatable (index);
}

bool PluginNode::isModulatablePerNote (int index) const noexcept
{
    const auto* native = m_native.load();
    return m_instrument && native != nullptr && native->isModulatablePerNote (index);
}

void PluginNode::modulateParameter (int index, float amount, uint32_t time) noexcept
{
    if (auto* native = m_native.load())
        native->modulateParameter (index, static_cast<double> (amount), time);
}

void PluginNode::automateParameter (int index, float value, uint32_t time) noexcept
{
    if (auto* native = m_native.load())
        native->automateParameter (index, std::clamp (value, 0.0f, 1.0f), time);
}

juce::var PluginNode::parameterList() const
{
    juce::Array<juce::var> result;

    if (m_processor == nullptr)
        return result;

    const auto& parameters = m_processor->getParameters();

    for (int i = 0; i < parameters.size(); ++i)
    {
        const auto* parameter = parameters[i];

        if (! parameter->isAutomatable())
            continue;

        const auto* withId = dynamic_cast<const juce::HostedAudioProcessorParameter*> (parameter);
        result.add (makeObject ({ { "id", withId != nullptr ? withId->getParameterID() : juce::String (i) },
                                  { "name", parameter->getName (64) } }));
    }

    return result;
}

void PluginNode::fail (const juce::String& message)
{
    m_status = Status::failed;
    m_error = message;
}

void PluginNode::setStatus (Status status, const juce::String& message)
{
    m_status = status;
    m_error = message;
}

bool PluginNode::refreshLatency() noexcept
{
    if (m_processor == nullptr)
        return false;

    return m_latency.exchange (m_processor->getLatencySamples()) != m_processor->getLatencySamples();
}

void PluginNode::prepare (const ProcessSpec& spec)
{
    m_spec = spec;
    m_prepared = true;

    m_nativeNotes.prepare (spec.sampleRate, spec.maxBlockSize);

    if (m_processor == nullptr)
    {
        m_encoder.prepare (spec.sampleRate);
        return;
    }

    // The node may be referenced by a running engine: keep process() away while everything is re-prepared.
    m_processor->suspendProcessing (true);
    m_processor->releaseResources();
    preparePlugin();
    m_processor->suspendProcessing (false);
}

// Turns on an effect's second input bus as its sidechain, stereo or else mono, when it has one (docs/design/sidechain.md,
// K7). The plugin itself decides whether to listen to it.
void PluginNode::enableSidechain()
{
    auto& processor = *m_processor;

    if (processor.getBusCount (true) < 2)
        return;

    for (const auto& set : { juce::AudioChannelSet::stereo(), juce::AudioChannelSet::mono() })
    {
        auto layout = processor.getBusesLayout();
        layout.inputBuses.getReference (1) = set;

        if (processor.checkBusesLayoutSupported (layout) && processor.setBusesLayout (layout))
            break;
    }

    m_sidechainChannels.store (std::min (2, processor.getChannelCountOfBus (true, 1)));
}

// The main bus layouts tried, in the order of m_mainLayouts (docs/design/mono-ports.md, MP4).
constexpr std::array<PluginNode::MainLayout, 3> mainLayouts { { { 1, 1 }, { 1, 2 }, { 2, 2 } } };

[[nodiscard]] juce::AudioChannelSet channelSet (int channels)
{
    return channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
}

[[nodiscard]] int clampedChannels (int channels) noexcept
{
    return std::clamp (channels, 1, 2);
}

// Asks an effect once which main bus layouts it accepts, with its other buses as they are. Remote plugins answer over
// their pipe, so this is not asked again while compiling.
void PluginNode::findMainLayouts()
{
    auto& processor = *m_processor;
    m_negotiates = processor.getBusCount (true) > 0 && processor.getBusCount (false) > 0;
    m_attachedLayout = {
        clampedChannels (processor.getBusCount (true) > 0 ? processor.getChannelCountOfBus (true, 0) : 2),
        clampedChannels (processor.getBusCount (false) > 0 ? processor.getChannelCountOfBus (false, 0) : 2)
    };
    m_mainOutputs.store (m_attachedLayout.outputs);

    for (size_t i = 0; i < mainLayouts.size(); ++i)
    {
        auto layout = processor.getBusesLayout();

        if (m_negotiates)
        {
            layout.inputBuses.getReference (0) = channelSet (mainLayouts[i].inputs);
            layout.outputBuses.getReference (0) = channelSet (mainLayouts[i].outputs);
        }

        m_mainLayouts[i] = m_negotiates && processor.checkBusesLayoutSupported (layout);
    }
}

PluginNode::MainLayout PluginNode::layoutFor (int width) const
{
    if (m_processor == nullptr)
        return { width, width };

    const auto order = [&]() -> std::vector<size_t>
    {
        switch (m_channels)
        {
            case PluginChannels::monoToStereo:
                return { 1, 2 };
            case PluginChannels::stereo:
                return { 2 };
            case PluginChannels::automatic:
                break;
        }

        // Fed mono, mono → mono first (the owner's answer 2); fed stereo, stereo first.
        return width == 1 ? std::vector<size_t> { 0, 1, 2 } : std::vector<size_t> { 2, 0, 1 };
    }();

    for (const auto i : order)
        if (m_mainLayouts[i])
            return mainLayouts[i];

    return m_attachedLayout;
}

int PluginNode::outputWidth (size_t port, int width, juce::uint32 wiredOutputs) const
{
    if (! m_instrument)
        return port == 0 ? layoutFor (width).outputs : 2;

    if (m_processor == nullptr)
        return 2;

    const auto& processor = *m_processor;
    const auto buses = std::min (processor.getBusCount (false), maxPluginOutputs);
    const auto channelsOf = [&] (int bus) { return processor.getChannelCountOfBus (false, bus); };

    if (port > 0)
        return static_cast<int> (port) < buses && channelsOf (static_cast<int> (port)) == 1 ? 1 : 2;

    // Main carries its own bus and every bus whose port isn't wired (MO2): mono only when all of those are.
    if (buses == 0 || channelsOf (0) != 1)
        return 2;

    for (int bus = 1; bus < buses; ++bus)
        if ((bus >= 32 || (wiredOutputs & (1u << bus)) == 0) && channelsOf (bus) > 1)
            return 2;

    return 1;
}

void PluginNode::useWidth (int width)
{
    if (m_instrument || m_processor == nullptr || ! m_negotiates)
        return;

    auto& processor = *m_processor;
    const auto wanted = layoutFor (width);

    if (wanted == MainLayout { processor.getChannelCountOfBus (true, 0), processor.getChannelCountOfBus (false, 0) })
        return;

    // The node may be referenced by a running engine: keep process() away while the buses change.
    processor.suspendProcessing (true);

    if (m_prepared)
        processor.releaseResources();

    auto layout = processor.getBusesLayout();
    layout.inputBuses.getReference (0) = channelSet (wanted.inputs);
    layout.outputBuses.getReference (0) = channelSet (wanted.outputs);
    std::ignore = processor.setBusesLayout (layout);
    m_sidechainChannels.store (processor.getBusCount (true) > 1 ? std::min (2, processor.getChannelCountOfBus (true, 1))
                                                                : 0);
    m_mainOutputs.store (clampedChannels (processor.getChannelCountOfBus (false, 0)));

    if (m_prepared)
        preparePlugin();

    m_latency.store (processor.getLatencySamples());
    processor.suspendProcessing (false);
}

// Turns every output bus after the main one on (stereo, else mono, else as the plugin has it), or off again
// (docs/design/multi-output-instruments.md, MO1). Each bus is tried on its own, so one the plugin refuses stays as it was.
void PluginNode::applyOutputLayout()
{
    auto& processor = *m_processor;

    for (int bus = 1; bus < std::min (processor.getBusCount (false), maxPluginOutputs); ++bus)
    {
        const auto* info = processor.getBus (false, bus);
        const auto wanted = m_allOutputs ? std::vector { juce::AudioChannelSet::stereo(), juce::AudioChannelSet::mono(),
                                                         info->getDefaultLayout() }
                                         : std::vector { juce::AudioChannelSet::disabled() };

        for (const auto& set : wanted)
        {
            if (set.size() == 0 && m_allOutputs)
                continue;

            auto layout = processor.getBusesLayout();
            layout.outputBuses.getReference (bus) = set;

            if (processor.checkBusesLayoutSupported (layout) && processor.setBusesLayout (layout))
                break;
        }
    }
}

juce::var PluginNode::outputBuses() const
{
    juce::Array<juce::var> buses;

    if (m_processor == nullptr || ! m_instrument)
        return buses;

    for (int bus = 0; bus < std::min (m_processor->getBusCount (false), maxPluginOutputs); ++bus)
        buses.add (makeObject ({ { "name", m_processor->getBus (false, bus)->getName() },
                                 { "on", m_processor->getChannelCountOfBus (false, bus) > 0 } }));

    return buses;
}

void PluginNode::preparePlugin()
{
    auto& processor = *m_processor;
    const auto channels = std::max ({ 2, processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels() });

    processor.setRateAndBufferSizeDetails (m_spec.sampleRate, m_spec.maxBlockSize);
    processor.prepareToPlay (m_spec.sampleRate, m_spec.maxBlockSize);
    m_buffer.setSize (channels, m_spec.maxBlockSize);
    m_midi.ensureSize (MpeEncoder::requiredBufferBytes (m_spec.maxBlockSize));
    m_encoder.prepare (m_spec.sampleRate);
}

void PluginNode::configure (const GraphNode& node)
{
    m_bypassed.store (node.bypassed);
    m_encoder.setSettings (node.midi);
    m_channels = node.pluginChannels;

    if (! m_instrument || node.allOutputs == m_allOutputs)
        return;

    m_allOutputs = node.allOutputs;

    if (m_processor == nullptr)
        return;

    // The node may be referenced by a running engine: keep process() away while the buses change.
    m_processor->suspendProcessing (true);

    if (m_prepared)
        m_processor->releaseResources();

    applyOutputLayout();

    if (m_prepared)
        preparePlugin();

    m_latency.store (m_processor->getLatencySamples());
    m_processor->suspendProcessing (false);
}

void PluginNode::passThrough (const ProcessContext& context) const noexcept
{
    const auto& out = context.audioOutputs[0];

    if (m_instrument)
    {
        for (auto* channel : out)
            juce::FloatVectorOperations::clear (channel, context.numSamples);
    }
    else
    {
        const auto& in = context.audioInputs[0];

        for (size_t ch = 0; ch < 2; ++ch)
            if (in[ch] != out[ch])
                juce::FloatVectorOperations::copy (out[ch], in[ch], context.numSamples);

        // A mono output fed stereo hears the fold, as the plugin would (docs/design/mono-ports.md, MP2).
        if (context.channels == 2 && m_active.load() != nullptr && m_mainOutputs.load() == 1)
        {
            juce::FloatVectorOperations::add (out[0], in[1], context.numSamples);
            juce::FloatVectorOperations::multiply (out[0], 0.5f, context.numSamples);
        }
    }

    // An instrument's wired outputs are silent too.
    for (size_t port = 1; port < context.audioOutputs.size(); ++port)
        if (wired (context, port))
            for (auto* channel : context.audioOutputs[port])
                juce::FloatVectorOperations::clear (channel, context.numSamples);
}

void PluginNode::process (const ProcessContext& context) noexcept
{
    auto* processor = m_active.load();

    if (processor == nullptr)
    {
        passThrough (context);
        return;
    }

    const juce::ScopedTryLock lock (processor->getCallbackLock());

    if (! lock.isLocked() || processor->isSuspended())
    {
        passThrough (context);
        return;
    }

    const auto bypassed = m_bypassed.load();

    // A bypassed effect is skipped. A bypassed instrument keeps running (so it never misses a note-off) and
    // is muted.
    if (bypassed && ! m_instrument)
    {
        passThrough (context);
        return;
    }

    const auto numSamples = context.numSamples;
    m_midi.clear();

    if (auto* native = m_native.load(); m_instrument && native != nullptr && native->takesNativeNotes())
        m_nativeNotes.process (context, *native);
    else if (m_instrument)
        m_encoder.process (context, m_midi);

    m_buffer.setSize (m_buffer.getNumChannels(), numSamples, false, false, true);
    m_buffer.clear();

    // The main input's channels first, then the sidechain bus's (K7). A mono input fed stereo hears the fold
    // (docs/design/mono-ports.md, MP2); a stereo one fed mono, the one channel on both sides.
    const auto inputs = std::min (2, processor->getBusCount (true) > 0 ? processor->getChannelCountOfBus (true, 0) : 0);

    if (! m_instrument)
    {
        for (int ch = 0; ch < inputs; ++ch)
            m_buffer.copyFrom (ch, 0, context.audioInputs[0][static_cast<size_t> (ch)], numSamples);

        if (inputs == 1 && context.channels == 2)
        {
            m_buffer.addFrom (0, 0, context.audioInputs[0][1], numSamples);
            m_buffer.applyGain (0, 0, numSamples, 0.5f);
        }

        if (context.audioInputs.size() > 1)
            for (int ch = 0; ch < m_sidechainChannels.load(); ++ch)
                m_buffer.copyFrom (processor->getChannelIndexInProcessBlockBuffer (true, 1, ch), 0,
                                   context.audioInputs[1][static_cast<size_t> (ch)], numSamples);
    }

    m_playHead.info = positionInfo (*context.transport);

    {
        // Third-party code: allocations here are counted separately, not as DAWg violations.
        const ScopedAllowAllocation allow;
        processor->processBlock (m_buffer, m_midi);
    }

    const auto& outs = context.audioOutputs;

    for (size_t port = 0; port < outs.size(); ++port)
        if (port == 0 || wired (context, port))
            for (auto* channel : outs[port])
                juce::FloatVectorOperations::clear (channel, numSamples);

    if (bypassed && m_instrument)
        return;

    // Each output bus into its port while that is wired, else into Main (docs/design/multi-output-instruments.md, MO2).
    // A mono bus plays on both sides.
    const auto buses = std::min (processor->getBusCount (false), static_cast<int> (outs.size()));

    for (int bus = 0; bus < buses; ++bus)
    {
        const auto channels = processor->getChannelCountOfBus (false, bus);

        if (channels == 0)
            continue;

        const auto& out = outs[wired (context, static_cast<size_t> (bus)) ? static_cast<size_t> (bus) : 0];

        for (int side = 0; side < 2; ++side)
            juce::FloatVectorOperations::add (out[static_cast<size_t> (side)],
                                              m_buffer.getReadPointer (processor->getChannelIndexInProcessBlockBuffer (
                                                  false, bus, std::min (side, channels - 1))),
                                              numSamples);
    }
}

} // namespace dawg
