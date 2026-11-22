#pragma once

#include "dawg/PluginNode.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

namespace dawg
{

/**
    Loads plugins into plugin nodes and keeps track of them. See docs/design/phase4-plugin-hosting.md,
    section 4.

    load() is the GraphCompiler's plugin loader: it instantiates the plugin asynchronously, restores the
    node's saved state, sets a stereo layout and attaches the instance. Latency changes reported by plugins
    (possibly on the audio thread) are collected and handled on the message thread.

    @note  Message thread only, except the AudioProcessorListener callbacks. Must outlive every plugin node
           it loaded into (it stays registered as a listener of their processors).
*/
class PluginHost final : private juce::AudioProcessorListener,
                         private juce::AsyncUpdater
{
public:
    /**
        Creates a host.

        @param formats   The plugin formats. Must outlive the host.
        @param known     The scanned plugins, to look identifiers up in. Must outlive the host.
        @param spec      Returns the current sample rate and block size.
        @param onChange  Called when a plugin finished loading or failed (status changed), with true if the
                         engine must be recompiled (a latency changed).
    */
    PluginHost (juce::AudioPluginFormatManager& formats, juce::KnownPluginList& known,
                std::function<ProcessSpec()> spec, std::function<void (bool recompile)> onChange);

    /**
        Stops listening to the plugins.
    */
    ~PluginHost() override;

    PluginHost (const PluginHost&) = delete;
    PluginHost& operator= (const PluginHost&) = delete;
    PluginHost (PluginHost&&) = delete;
    PluginHost& operator= (PluginHost&&) = delete;

    /**
        Starts loading a plugin into a node (the GraphCompiler::PluginLoader).

        @param trackId  The track holding the node.
        @param node     The node's model data.
        @param target   The node to attach the plugin to.
    */
    void load (ObjectId trackId, const GraphNode& node, const std::shared_ptr<PluginNode>& target);

    /**
        Returns the status of every live plugin node, for the UI.

        @return  { "{trackId}:{nodeId}": { status: "loading" | "ready" | "failed", error, latency,
                                                 hasSidechain, outputs: [{ name, on }] } }
    */
    [[nodiscard]] juce::var statusVar();

    /**
        Receives the start and end of parameter changes in plugin editors: the "touch" of automation writing
        (docs/design/phase10-automation.md, A7). Called on the message thread.
    */
    using GestureCallback = std::function<void (ObjectId trackId, ObjectId nodeId, const juce::String& paramId,
                                                const juce::String& name, bool begin)>;

    /**
        Sets the gesture callback.

        @param callback  The callback, or nullptr.
    */
    void setGestureCallback (GestureCallback callback) { m_gesture = std::move (callback); }

    /**
        Sets what is called when a plugin's parameters were added to or removed (ParameterListSource): the caller makes
        a new instance from the node's state. Called on the message thread, never from inside the plugin's own call.

        @param callback  The callback, or nullptr.
    */
    void setParameterListCallback (std::function<void (ObjectId trackId, ObjectId nodeId)> callback)
    {
        m_parameterList = std::move (callback);
    }

    /**
        Starts a crashed plugin again (docs/design/out-of-process-plugins.md, O8).

        @param trackId  The track holding the node.
        @param node     The node's model data.
        @param target   The node.
        @note  Blocks for as long as the plugin takes to load, if it had loaded before.
    */
    void reload (ObjectId trackId, const GraphNode& node, const std::shared_ptr<PluginNode>& target);

    /**
        Sets whether plugins loaded from now on run in helper processes (docs/design/out-of-process-plugins.md).

        @param enabled  True to run them apart. Ignored where that isn't available.
    */
    void setOutOfProcess (bool enabled) noexcept { m_outOfProcess = enabled; }

    /**
        Returns what setOutOfProcess() set.
    */
    [[nodiscard]] bool outOfProcess() const noexcept { return m_outOfProcess; }

    /**
        Sets which plugins stay in DAWg's own process whatever setOutOfProcess() says, for those that don't cope with a
        helper (docs/design/out-of-process-plugins.md, O9).

        @param identifiers  The plugins' identifiers.
    */
    void setRunInside (juce::StringArray identifiers) { m_runInside = std::move (identifiers); }

    /**
        Returns the plugins set with setRunInside().
    */
    [[nodiscard]] const juce::StringArray& runInside() const noexcept { return m_runInside; }

private:
    struct Entry
    {
        ObjectId trackId = 0;
        ObjectId nodeId = 0;
        std::weak_ptr<PluginNode> node;
    };

    void finishLoading (const std::weak_ptr<PluginNode>& target, const std::shared_ptr<const juce::MemoryBlock>& state,
                        std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error);

    void audioProcessorParameterChanged (juce::AudioProcessor* processor, int parameterIndex, float newValue) override;
    void audioProcessorChanged (juce::AudioProcessor* processor, const ChangeDetails& details) override;
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* processor, int parameterIndex) override;
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor* processor, int parameterIndex) override;
    void reportGesture (juce::AudioProcessor* processor, int parameterIndex, bool begin);
    void handleAsyncUpdate() override;

    juce::AudioPluginFormatManager& m_formats;
    juce::KnownPluginList& m_known;
    std::function<ProcessSpec()> m_spec;
    std::function<void (bool)> m_onChange;
    std::vector<Entry> m_entries;
    GestureCallback m_gesture;
    std::function<void (ObjectId, ObjectId)> m_parameterList;
    bool m_outOfProcess = false;
    juce::StringArray m_runInside;
    std::shared_ptr<bool> m_alive = std::make_shared<bool> (true); // guards async callbacks after destruction
};

} // namespace dawg
