#include "PluginWindows.h"

#include "ClapEditor.h"

namespace dawg
{

namespace
{
const juce::Colour windowColour { 0xff1a1d24 };

[[nodiscard]] juce::String settingsKey (const juce::String& positionKey)
{
    return "pluginWindow." + positionKey;
}
} // namespace

class PluginWindows::Window final : public juce::DocumentWindow
{
public:
    Window (const juce::String& title, juce::AudioProcessor& processor, juce::String positionKey,
            std::function<void()> onClose)
        : DocumentWindow (title, windowColour, closeButton),
          m_positionKey (std::move (positionKey)),
          m_onClose (std::move (onClose))
    {
        setUsingNativeTitleBar (true);

        auto editor = createClapEditor (processor);

        if (editor == nullptr && processor.hasEditor())
            editor.reset (processor.createEditorAndMakeActive());

        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (processor);

        const auto editorResizable = editor->isResizable();
        setContentOwned (editor.release(), true);
        setResizable (editorResizable, false);
    }

    [[nodiscard]] const juce::String& positionKey() const noexcept { return m_positionKey; }

    void closeButtonPressed() override
    {
        // The callback destroys this window, so run it after this call has returned.
        juce::MessageManager::callAsync ([onClose = m_onClose] { onClose(); });
    }

private:
    juce::String m_positionKey;
    std::function<void()> m_onClose;
};

namespace
{
template <typename WindowType>
class RemoteWindowHolder final : public RemoteEditorWindow
{
public:
    explicit RemoteWindowHolder (std::unique_ptr<WindowType> window)
        : m_window (std::move (window))
    {
    }

    void toFront() override { m_window->toFront (true); }

    void setShown (bool onTop, bool visible) override
    {
        if (m_window->isAlwaysOnTop() != onTop)
            m_window->setAlwaysOnTop (onTop);

        if (m_window->isVisible() != visible)
            m_window->setVisible (visible);
    }

    [[nodiscard]] RemoteEditorPosition position() const override
    {
        const auto at = m_window->getScreenPosition();
        return { at.x, at.y };
    }

private:
    std::unique_ptr<WindowType> m_window;
};
} // namespace

std::unique_ptr<RemoteEditorWindow> createRemoteEditorWindow (juce::AudioPluginInstance& plugin,
                                                              const juce::String& title,
                                                              std::optional<RemoteEditorPosition> position,
                                                              bool visible, std::function<void()> onUserClose)
{
    auto window = PluginWindows::makeWindowForHelper (plugin, title, std::move (onUserClose));

    if (position.has_value())
        window->setTopLeftPosition (position->x, position->y);
    else
        window->centreWithSize (window->getWidth(), window->getHeight());

    window->setAlwaysOnTop (true);

    if (visible)
    {
        window->setVisible (true);
        window->toFront (true);
    }

    return std::make_unique<RemoteWindowHolder<juce::DocumentWindow>> (std::move (window));
}

std::unique_ptr<juce::DocumentWindow> PluginWindows::makeWindowForHelper (juce::AudioProcessor& processor,
                                                                          const juce::String& title,
                                                                          std::function<void()> onClose)
{
    return std::make_unique<Window> (title, processor, juce::String(), std::move (onClose));
}

PluginWindows::PluginWindows (juce::PropertiesFile& settings, std::function<void (Key)> onClosed)
    : m_settings (settings),
      m_onClosed (std::move (onClosed))
{
}

PluginWindows::~PluginWindows()
{
    *m_alive = false;
    closeUnless ([] (Key) { return false; });
}

void PluginWindows::open (Key key, juce::AudioProcessor& processor, const juce::String& title,
                          const juce::String& positionKey)
{
    if (auto* remote = dynamic_cast<RemoteInstance*> (&processor))
    {
        openRemote (key, *remote, title, positionKey);
        return;
    }

    if (const auto it = m_windows.find (key); it != m_windows.end())
    {
        it->second->toFront (true);
        return;
    }

    auto window = std::make_unique<Window> (title, processor, positionKey,
                                            [this, key]
                                            {
                                                close (key);
                                                m_onClosed (key);
                                            });

    const auto saved = juce::StringArray::fromTokens (m_settings.getValue (settingsKey (positionKey)), false);

    if (saved.size() == 2)
        window->setTopLeftPosition (saved[0].getIntValue(), saved[1].getIntValue());
    else
        window->centreWithSize (window->getWidth(), window->getHeight());

    window->setAlwaysOnTop (true);
    window->setVisible (true);
    window->toFront (true);
    m_windows[key] = std::move (window);
}

void PluginWindows::openRemote (Key key, RemoteInstance& remote, const juce::String& title,
                                const juce::String& positionKey)
{
    std::optional<RemoteEditorPosition> position;
    const auto saved = juce::StringArray::fromTokens (m_settings.getValue (settingsKey (positionKey)), false);

    if (saved.size() == 2)
        position = RemoteEditorPosition { saved[0].getIntValue(), saved[1].getIntValue() };

    m_remote.try_emplace (key, RemoteWindow { &remote, positionKey, true, ! m_mainMinimised });

    const std::weak_ptr<bool> alive = m_alive;
    remote.openEditor (title, position, ! m_mainMinimised,
                       [this, alive, key, instance = &remote] (std::optional<RemoteEditorPosition> where)
                       {
                           juce::MessageManager::callAsync (
                               [this, alive, key, instance, where]
                               {
                                   const auto stillHere = alive.lock();

                                   if (stillHere == nullptr || ! *stillHere)
                                       return;

                                   const auto it = m_remote.find (key);

                                   if (it == m_remote.end() || it->second.remote != instance)
                                       return;

                                   remember (it->second.positionKey, where);
                                   m_remote.erase (it);
                                   m_onClosed (key);
                               });
                       });
}

void PluginWindows::close (Key key)
{
    if (const auto it = m_remote.find (key); it != m_remote.end())
    {
        remember (it->second.positionKey, it->second.remote->closeEditor());
        m_remote.erase (it);
    }
    if (const auto it = m_windows.find (key); it != m_windows.end())
    {
        remember (*it->second);
        m_windows.erase (it);
    }
}

void PluginWindows::closeUnless (const std::function<bool (Key)>& keep)
{
    for (auto it = m_remote.begin(); it != m_remote.end();)
    {
        if (keep (it->first))
        {
            ++it;
            continue;
        }

        remember (it->second.positionKey, it->second.remote->closeEditor());
        it = m_remote.erase (it);
    }
    for (auto it = m_windows.begin(); it != m_windows.end();)
    {
        if (keep (it->first))
        {
            ++it;
            continue;
        }

        remember (*it->second);
        it = m_windows.erase (it);
    }
}

void PluginWindows::update (bool appActive, bool mainMinimised)
{
    m_mainMinimised = mainMinimised;

    for (auto& [key, window] : m_remote)
    {
        if (window.onTop == appActive && window.visible != mainMinimised)
            continue;

        window.onTop = appActive;
        window.visible = ! mainMinimised;
        window.remote->showEditor (window.onTop, window.visible);
    }
    for (const auto& [key, window] : m_windows)
    {
        if (window->isAlwaysOnTop() != appActive)
            window->setAlwaysOnTop (appActive);

        if (window->isVisible() == mainMinimised)
            window->setVisible (! mainMinimised);
    }
}

void PluginWindows::remember (const juce::String& positionKey, std::optional<RemoteEditorPosition> position)
{
    if (position.has_value())
        m_settings.setValue (settingsKey (positionKey), juce::String (position->x) + " " + juce::String (position->y));
}

void PluginWindows::remember (const Window& window)
{
    const auto position = window.getScreenPosition();
    m_settings.setValue (settingsKey (window.positionKey()),
                         juce::String (position.x) + " " + juce::String (position.y));
}

} // namespace dawg
