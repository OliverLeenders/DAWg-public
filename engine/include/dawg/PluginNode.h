#pragma once

#include "dawg/ClapNoteEncoder.h"
#include "dawg/MpeEncoder.h"
#include "dawg/Node.h"

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <atomic>
#include <memory>

namespace dawg
{

/**
    The most outputs an instrument plugin node has: Main and Out 2 to Out 16, one per output bus of the plugin
    (docs/design/multi-output-instruments.md, MO1).
*/
inline constexpr int maxPluginOutputs = 16;

/**
    Hosts a plugin (or any juce::AudioProcessor) in a track graph. See docs/design/phase4-plugin-hosting.md.

    The node is created empty, in the loading state, by the graph compiler; the app instantiates the plugin
    asynchronously and attach()es it. Until then, and if loading fails, an effect passes audio through and an
    instrument is silent. Instruments receive their notes as MPE (or plain MIDI) through an MpeEncoder.

    @note  Construction, attach(), fail(), prepare(), configure() and the accessors: message thread.
           process(): audio thread. process() takes the processor's callback lock with a try-lock and skips
           the plugin while it is suspended, so the message thread may re-prepare it or set its state under
           juce::AudioProcessor::suspendProcessing().
*/
class PluginNode final : public Node
{
public:
    /**
        Where the plugin is in its life cycle.
    */
    enum class Status
    {
        loading, ///< Waiting for the plugin to be instantiated.
        ready,   ///< The plugin is attached and running.
        failed,  ///< The plugin could not be loaded (see error()).
        crashed, ///< The plugin's process went away (see error()). It can be reloaded.
    };

    /**
        Creates an empty node.

        @param instrument  True for an instrument (events in, audio out), false for an effect (audio in and out).
    */
    explicit PluginNode (bool instrument);

    /**
        Releases the plugin (message thread, once no compiled engine references the node).
    */
    ~PluginNode() override;

    PluginNode (const PluginNode&) = delete;
    PluginNode& operator= (const PluginNode&) = delete;
    PluginNode (PluginNode&&) = delete;
    PluginNode& operator= (PluginNode&&) = delete;

    /**
        Takes ownership of the loaded plugin, prepares it and starts using it from the next block.

        @param processor  The plugin instance, with its state already restored. Must not be null.
    */
    void attach (std::unique_ptr<juce::AudioProcessor> processor);

    /**
        Finds a parameter by its stable id (docs/design/phase10-automation.md, A1).

        @param id  The parameter id (or the index as text for plugins without ids).
        @return    The parameter index, or -1 if the plugin isn't loaded or has no such parameter.
        @note  Message thread.
    */
    [[nodiscard]] int parameterIndex (const juce::String& id) const;

    /**
        Sets a parameter from automation, before the next block.

        @param index  The parameter index (from parameterIndex()).
        @param value  The normalised value.
        @note  Audio thread.
    */
    void setAutomatedParameter (int index, float value) noexcept;

    /**
        Reads a parameter's current normalised value.

        @param index  The parameter index.
        @return       The value, or 0 if the plugin isn't loaded or has no such parameter.
        @note  Audio thread (the value is read from the plugin's parameter).
    */
    [[nodiscard]] float parameterValue (int index) const noexcept;

    /**
        Sets a parameter from modulation, around a base the caller supplies.

        @param index  The parameter index.
        @param value  The normalised value.
        @note  Audio thread.
    */
    void setModulatedParameter (int index, float value) noexcept;

    /**
        Returns whether a parameter takes modulation apart from its value, as CLAP's can (docs/design/clap-hosting.md, C9).

        @param index  The parameter index.
        @note  Audio thread.
    */
    [[nodiscard]] bool isModulatable (int index) const noexcept;

    /**
        Returns whether a parameter takes modulation per note (docs/design/clap-hosting.md, C10).

        @param index  The parameter index.
    */
    [[nodiscard]] bool isModulatablePerNote (int index) const noexcept;

    /**
        Sets how far modulation moves such a parameter from its value.

        @param index   The parameter index.
        @param amount  The amount, in units of the parameter's whole range.
        @param time    The sample in the next block.
        @note  Audio thread.
    */
    void modulateParameter (int index, float amount, uint32_t time = 0) noexcept;

    /**
        Returns whether the plugin takes parameter values with sample offsets (docs/design/clap-hosting.md, C4).
    */
    [[nodiscard]] bool takesParameterEvents() const noexcept { return m_native.load() != nullptr; }

    /**
        Sets a parameter from automation at a sample of the next block, for such a plugin.

        @param index  The parameter index.
        @param value  The normalised value.
        @param time   The sample in the block.
        @note  Audio thread.
    */
    void automateParameter (int index, float value, uint32_t time) noexcept;

    /**
        Returns whether the plugin takes its notes natively, by note id (C7), so the node's MIDI settings don't apply.
    */
    [[nodiscard]] bool usesNativeNotes() const noexcept
    {
        return m_native.load() != nullptr && m_native.load()->takesNativeNotes();
    }

    /**
        Lists the plugin's automatable parameters.

        @return  [{ id, name }], empty while the plugin isn't loaded.
        @note  Message thread.
    */
    [[nodiscard]] juce::var parameterList() const;

    /**
        Marks the node as failed.

        @param message  Why loading failed, for the user.
    */
    void fail (const juce::String& message);

    /**
        Marks the node as crashed, or as loading or running again.

        @param status   The status.
        @param message  Why, for the user, or an empty string.
        @note  Message thread.
    */
    void setStatus (Status status, const juce::String& message);

    /**
        Returns the node's status.
    */
    [[nodiscard]] Status status() const noexcept { return m_status; }

    /**
        Returns why loading failed, or an empty string.
    */
    [[nodiscard]] const juce::String& error() const noexcept { return m_error; }

    /**
        Returns the attached plugin, or nullptr while loading or after a failure.
    */
    [[nodiscard]] juce::AudioProcessor* processor() const noexcept { return m_processor.get(); }

    /**
        Returns true for instrument nodes.
    */
    [[nodiscard]] bool isInstrument() const noexcept { return m_instrument; }

    /**
        Returns whether the plugin has a sidechain input bus that the node's key input feeds (docs/design/sidechain.md,
        K7).
    */
    [[nodiscard]] bool hasSidechain() const noexcept { return m_sidechainChannels.load() > 0; }

    /**
        Describes an instrument's output buses, the main one first, up to maxPluginOutputs
        (docs/design/multi-output-instruments.md, MO1).

        @return  [{ name, on }], empty for effects and while the plugin isn't loaded.
        @note    Message thread.
    */
    [[nodiscard]] juce::var outputBuses() const;

    /**
        Re-reads the plugin's latency, e.g. after it reported a change. The caller recompiles afterwards.

        @return  True if the latency changed.
    */
    [[nodiscard]] bool refreshLatency() noexcept;

    /**
        Prepares the node and, if attached, the plugin.

        @param spec  The sample rate and maximum block size.
    */
    void prepare (const ProcessSpec& spec) override;

    /**
        Runs the plugin on one block.

        @param context  Inputs, outputs, events and transport.
    */
    void process (const ProcessContext& context) noexcept override;

    /**
        Returns the plugin's latency in samples (0 while not attached).
    */
    [[nodiscard]] int latencySamples() const noexcept override { return m_latency.load(); }

    /**
        Applies bypass, MIDI output settings and, for instruments, whether all output buses are on.

        @param node  The node's model data.
    */
    void configure (const GraphNode& node) override;

    /**
        Plugins are kept and re-prepared when the device changes.
    */
    [[nodiscard]] bool keepAcrossSpecChange() const noexcept override { return true; }

    /**
        The width of an output port (docs/design/mono-ports.md, MP4): an effect's, that of the main output bus it takes
        for the width it is fed (layoutFor()); an instrument's, mono when its bus is (Main only when every bus playing
        into it is).
    */
    [[nodiscard]] int outputWidth (size_t port, int width, juce::uint32 wiredOutputs) const override;

    /**
        Gives an effect's main buses the layout for the width it is fed, re-preparing the plugin when that changes. Its
        state is kept.
    */
    void useWidth (int width) override;

    /**
        A main bus layout: the input's and the output's channels, 1 or 2.
    */
    struct MainLayout
    {
        int inputs = 2;  ///< The main input bus's channels.
        int outputs = 2; ///< The main output bus's channels.

        /**
            Compares both.
        */
        [[nodiscard]] bool operator== (const MainLayout&) const noexcept = default;
    };

    /**
        The main bus layout an effect takes when fed `width` channels: the first of those its Channels setting allows
        (PluginChannels) that the plugin accepts, else the layout it was attached with. While the plugin loads, the
        width it is fed.
    */
    [[nodiscard]] MainLayout layoutFor (int width) const;

private:
    // What the plugin sees as the host's transport, filled before each block (docs/design/tempo-map.md, TM10).
    class PlayHead final : public juce::AudioPlayHead
    {
    public:
        [[nodiscard]] juce::Optional<PositionInfo> getPosition() const override { return info; }

        PositionInfo info;
    };

    void enableSidechain();
    void findMainLayouts();
    void applyOutputLayout();
    void preparePlugin();
    void passThrough (const ProcessContext& context) const noexcept;

    bool m_instrument;
    Status m_status = Status::loading;
    juce::String m_error;
    std::unique_ptr<juce::AudioProcessor> m_processor;
    std::atomic<juce::AudioProcessor*> m_active { nullptr };
    std::atomic<int> m_latency { 0 };
    std::atomic<bool> m_bypassed { false };
    ProcessSpec m_spec;
    bool m_prepared = false;
    juce::AudioBuffer<float> m_buffer;
    juce::MidiBuffer m_midi;
    MpeEncoder m_encoder;
    ClapNoteEncoder m_nativeNotes;
    std::atomic<ClapNativeInput*> m_native { nullptr };
    PlayHead m_playHead;
    std::atomic<int> m_sidechainChannels { 0 };
    bool m_allOutputs = false;

    // Effects' main buses (docs/design/mono-ports.md, MP4): the setting, which of mono → mono, mono → stereo and
    // stereo → stereo the plugin accepts (asked once, when attached), the layout it was attached with, and the main
    // output's channels for the audio thread.
    PluginChannels m_channels = PluginChannels::automatic;
    std::array<bool, 3> m_mainLayouts {};
    bool m_negotiates = false;
    MainLayout m_attachedLayout;
    std::atomic<int> m_mainOutputs { 2 };
};

/**
    Describes a block's transport for a hosted plugin (docs/design/tempo-map.md, TM10): the tempo and time signature
    at the block start, the position in quarter notes and of the bar's start, the time from the song start, and
    whether it plays.

    @param transport  The block's transport.
    @return           The position info.
*/
[[nodiscard]] juce::AudioPlayHead::PositionInfo positionInfo (const Transport& transport) noexcept;

/**
    Description of nodetype::pluginInstrument.
*/
[[nodiscard]] const NodeDescriptor& pluginInstrumentDescriptor() noexcept;

/**
    Description of nodetype::pluginEffect.
*/
[[nodiscard]] const NodeDescriptor& pluginEffectDescriptor() noexcept;

} // namespace dawg
