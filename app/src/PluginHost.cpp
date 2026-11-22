#include "PluginHost.h"

#include "dawg/ClapPlugin.h"
#include "dawg/RemotePlugin.h"
#include "dawg/VarUtils.h"

namespace dawg
{

namespace
{
[[nodiscard]] int lateBlocksOf (const PluginNode& node)
{
    const auto* remote = dynamic_cast<const RemoteInstance*> (node.processor());
    return remote != nullptr ? remote->lateBlocks() : 0;
}

[[nodiscard]] const char* statusName (PluginNode::Status status) noexcept
{
    switch (status)
    {
        case PluginNode::Status::ready:
            return "ready";
        case PluginNode::Status::failed:
            return "failed";
        case PluginNode::Status::crashed:
            return "crashed";
        case PluginNode::Status::loading:
            break;
    }

    return "loading";
}
} // namespace

PluginHost::PluginHost (juce::AudioPluginFormatManager& formats, juce::KnownPluginList& known,
                        std::function<ProcessSpec()> spec, std::function<void (bool recompile)> onChange)
    : m_formats (formats),
      m_known (known),
      m_spec (std::move (spec)),
      m_onChange (std::move (onChange))
{
}

PluginHost::~PluginHost()
{
    *m_alive = false;
    cancelPendingUpdate();

    for (const auto& entry : m_entries)
        if (const auto node = entry.node.lock(); node != nullptr && node->processor() != nullptr)
            node->processor()->removeListener (this);
}

void PluginHost::load (ObjectId trackId, const GraphNode& node, const std::shared_ptr<PluginNode>& target)
{
    std::erase_if (m_entries, [] (const Entry& e) { return e.node.expired(); });
    m_entries.push_back ({ trackId, node.id, target });

    if (! node.plugin.has_value())
    {
        target->fail ("No plugin set");
        return;
    }

    auto description = m_known.getTypeForIdentifierString (node.plugin->identifier);

    // A CLAP plugin is its id, wherever its file went (docs/design/clap-hosting.md, section 8).
    if (description == nullptr)
        if (const auto moved = findMovedClapPlugin (m_known.getTypes(), node.plugin->identifier))
            description = std::make_unique<juce::PluginDescription> (*moved);

    if (description == nullptr)
    {
        target->fail (node.plugin->name + " is not installed (or not scanned yet)");
        return;
    }

    const auto spec = m_spec();
    const std::weak_ptr<PluginNode> weakTarget = target;
    const std::weak_ptr<bool> alive = m_alive;
    auto state = node.pluginState;

    const auto finish = [this, alive, weakTarget, state] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                          const juce::String& error)
    {
        if (alive.lock() == nullptr || ! *alive.lock())
            return;

        try
        {
            finishLoading (weakTarget, state, std::move (instance), error);
        }
        catch (const std::exception& failure)
        {
            if (const auto failed = weakTarget.lock())
                failed->fail ("Plugin initialisation failed: " + juce::String (failure.what()));
            m_onChange (false);
        }
    };

    if (m_outOfProcess && remotePluginsAvailable() && ! m_runInside.contains (node.plugin->identifier))
    {
        // The answer comes on the pipe's thread; the rest happens here.
        createRemotePlugin (*description, spec.sampleRate, spec.maxBlockSize,
                            [finish] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
                            {
                                juce::MessageManager::callAsync (
                                    [finish, raw = instance.release(), error]
                                    { finish (std::unique_ptr<juce::AudioPluginInstance> (raw), error); });
                            });
        return;
    }

    m_formats.createPluginInstanceAsync (*description, spec.sampleRate, spec.maxBlockSize, finish);
}

void PluginHost::reload (ObjectId trackId, const GraphNode& node, const std::shared_ptr<PluginNode>& target)
{
    // It crashed while loading: there's nothing yet to start again.
    if (target->processor() == nullptr)
    {
        target->setStatus (PluginNode::Status::loading, {});
        m_onChange (false);
        load (trackId, node, target);
        return;
    }

    auto* remote = dynamic_cast<RemoteInstance*> (target->processor());

    if (remote == nullptr)
        return;

    juce::String error;

    if (remote->reload (error))
    {
        target->setStatus (PluginNode::Status::ready, {});
        std::ignore = target->refreshLatency();
        m_onChange (true);
    }
    else
    {
        target->setStatus (PluginNode::Status::crashed, error);
        m_onChange (false);
    }
}

void PluginHost::finishLoading (const std::weak_ptr<PluginNode>& target,
                                const std::shared_ptr<const juce::MemoryBlock>& state,
                                std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
{
    const auto node = target.lock();

    if (node == nullptr)
        return; // removed while loading

    if (instance == nullptr)
    {
        // Its helper died in loading it: that's a crash, which a project should open with, not a failure to find it.
        if (error == remotePluginProcessGone)
            node->setStatus (PluginNode::Status::crashed, "It crashed while loading");
        else
            node->fail (error.isNotEmpty() ? error : juce::String ("The plugin could not be loaded"));

        m_onChange (false);
        return;
    }

    // Stereo main buses where the plugin allows it; anything else is adapted by the node.
    auto layout = instance->getBusesLayout();

    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();

    if (! layout.inputBuses.isEmpty() && ! node->isInstrument())
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();

    if (instance->checkBusesLayoutSupported (layout))
        std::ignore = instance->setBusesLayout (layout);

    if (state != nullptr && state->getSize() > 0)
        instance->setStateInformation (state->getData(), static_cast<int> (state->getSize()));

    instance->addListener (this);

    // A plugin in a helper can go away, and then says so here (docs/design/out-of-process-plugins.md, O8).
    if (auto* remote = dynamic_cast<RemoteInstance*> (instance.get()))
    {
        const std::weak_ptr<bool> alive = m_alive;
        remote->setCrashCallback (
            [this, alive, target, remote]
            {
                juce::MessageManager::callAsync (
                    [this, alive, target, remote]
                    {
                        const auto stillHere = alive.lock();
                        const auto crashedNode = target.lock();

                        if (stillHere == nullptr || ! *stillHere || crashedNode == nullptr)
                            return;

                        crashedNode->setStatus (PluginNode::Status::crashed, remote->crashReason());
                        std::ignore = crashedNode->refreshLatency();
                        m_onChange (true);
                    });
            });
    }

    // Parameters added or removed: a new instance, later, as the call comes from inside this one.
    if (auto* source = dynamic_cast<ParameterListSource*> (instance.get()))
    {
        const std::weak_ptr<bool> alive = m_alive;
        source->setParameterListCallback (
            [this, alive, target]
            {
                juce::MessageManager::callAsync (
                    [this, alive, target]
                    {
                        const auto stillHere = alive.lock();
                        const auto changed = target.lock();

                        if (stillHere == nullptr || ! *stillHere || changed == nullptr || m_parameterList == nullptr)
                            return;

                        for (const auto& entry : m_entries)
                            if (entry.node.lock() == changed)
                            {
                                m_parameterList (entry.trackId, entry.nodeId);
                                return;
                            }
                    });
            });
    }

    const auto* justLoaded = dynamic_cast<RemoteInstance*> (instance.get());
    const auto alreadyGone = justLoaded != nullptr && justLoaded->crashed();
    const auto goneReason = alreadyGone ? justLoaded->crashReason() : juce::String();
    node->attach (std::move (instance));

    // It went between loading and here, before anyone was listening.
    if (alreadyGone)
        node->setStatus (PluginNode::Status::crashed, goneReason);

    // Recompile when the latency needs aligning, or when automation lanes can now find the plugin's parameters
    // (docs/design/phase10-automation.md, A3).
    m_onChange (true);
}

juce::var PluginHost::statusVar()
{
    std::erase_if (m_entries, [] (const Entry& e) { return e.node.expired(); });
    const auto result = newObject();

    for (const auto& entry : m_entries)
    {
        const auto node = entry.node.lock();

        // Editor availability is deliberately not asked here: JUCE's VST3 hasEditor() creates and destroys a view
        // to answer, which blocks the message thread for up to seconds per plugin while a project loads. A ready
        // plugin always shows the editor button, and PluginWindows falls back to the generic editor
        // (docs/design/phase4-plugin-hosting.md, section 7).
        result->setProperty (
            juce::String (entry.trackId) + ":" + juce::String (entry.nodeId),
            makeObject ({
                { "status", statusName (node->status()) },
                { "error", node->error() },
                { "latency", node->latencySamples() },
                { "hasSidechain", node->hasSidechain() },
                { "outputs", node->outputBuses() },
                { "nativeNotes", node->usesNativeNotes() },
                { "lateBlocks", lateBlocksOf (*node) },
                { "separateProcess", dynamic_cast<const RemoteInstance*> (node->processor()) != nullptr },
            }));
    }

    return juce::var (result.get());
}

void PluginHost::audioProcessorParameterChanged (juce::AudioProcessor* /*processor*/, int /*parameterIndex*/,
                                                 float /*newValue*/)
{
}

void PluginHost::audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* processor, int parameterIndex)
{
    reportGesture (processor, parameterIndex, true);
}

void PluginHost::audioProcessorParameterChangeGestureEnd (juce::AudioProcessor* processor, int parameterIndex)
{
    reportGesture (processor, parameterIndex, false);
}

void PluginHost::reportGesture (juce::AudioProcessor* processor, int parameterIndex, bool begin)
{
    // Editors may report from any thread: find the node on the message thread.
    const std::weak_ptr<bool> alive = m_alive;

    juce::MessageManager::callAsync (
        [this, alive, processor, parameterIndex, begin]
        {
            if (alive.lock() == nullptr || m_gesture == nullptr)
                return;

            for (const auto& entry : m_entries)
            {
                const auto node = entry.node.lock();

                if (node == nullptr || node->processor() != processor)
                    continue;

                const auto& parameters = processor->getParameters();

                if (parameterIndex < 0 || parameterIndex >= parameters.size())
                    return;

                const auto* parameter = parameters[parameterIndex];
                const auto* withId = dynamic_cast<const juce::HostedAudioProcessorParameter*> (parameter);
                m_gesture (entry.trackId, entry.nodeId,
                           withId != nullptr ? withId->getParameterID() : juce::String (parameterIndex),
                           parameter->getName (64), begin);
                return;
            }
        });
}

void PluginHost::audioProcessorChanged (juce::AudioProcessor* /*processor*/, const ChangeDetails& details)
{
    // May be called on any thread, including the audio thread: just flag it.
    if (details.latencyChanged)
        triggerAsyncUpdate();
}

void PluginHost::handleAsyncUpdate()
{
    bool changed = false;

    for (const auto& entry : m_entries)
        if (const auto node = entry.node.lock())
            changed = node->refreshLatency() || changed;

    if (changed)
        m_onChange (true);
}

} // namespace dawg
