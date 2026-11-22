#pragma once

#include "dawg/Model.h"
#include "dawg/RemotePlugin.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace dawg
{

/**
    The native editor windows of plugin nodes, at most one per node. See docs/design/phase4-plugin-hosting.md,
    section 7.

    Windows float above DAWg's main window while DAWg is the active app (not above other apps) and hide while
    it is minimised. Positions are remembered per plugin in the app settings. The window of a plugin in a helper process
    is the helper's, and is handled the same way from here (docs/design/out-of-process-plugins.md, O7).

    @note  Message thread only. Close a node's window before its plugin is destroyed.
*/
class PluginWindows final
{
public:
    /**
        Identifies a plugin node: (track id, node id).
    */
    using Key = std::pair<ObjectId, ObjectId>;

    /**
        Creates the manager.

        @param settings  Where window positions are stored. Must outlive the manager.
        @param onClosed  Called after the user closed a window (not when close() is called).
    */
    PluginWindows (juce::PropertiesFile& settings, std::function<void (Key)> onClosed);

    /**
        Closes all windows.
    */
    ~PluginWindows();

    PluginWindows (const PluginWindows&) = delete;
    PluginWindows& operator= (const PluginWindows&) = delete;
    PluginWindows (PluginWindows&&) = delete;
    PluginWindows& operator= (PluginWindows&&) = delete;

    /**
        Opens a node's editor, or brings it to the front if it is already open.

        @param key          The node.
        @param processor    The node's plugin. Must outlive the window.
        @param title        The window title.
        @param positionKey  Identifies the plugin for remembering the window position.
    */
    void open (Key key, juce::AudioProcessor& processor, const juce::String& title, const juce::String& positionKey);

    /**
        Closes a node's editor if it is open.

        @param key  The node.
    */
    void close (Key key);

    /**
        Closes the windows of nodes for which keep() returns false.

        @param keep  Decides per node whether its window may stay open.
    */
    void closeUnless (const std::function<bool (Key)>& keep);

    /**
        Returns true if the node's editor is open.

        @param key  The node.
    */
    [[nodiscard]] bool isOpen (Key key) const { return m_windows.contains (key) || m_remote.contains (key); }

    /**
        Keeps the windows floating above the main window while DAWg is active, and hidden while it is minimised.
        Call regularly.

        @param appActive      Whether DAWg is the foreground app.
        @param mainMinimised  Whether the main window is minimised.
    */
    void update (bool appActive, bool mainMinimised);

    /**
        Makes the kind of window this class opens, for createRemoteEditorWindow().

        @param processor  The plugin. Must outlive the window.
        @param title      The window title.
        @param onClose    Called when the user closes the window.
        @return           The window, not yet showing.
    */
    [[nodiscard]] static std::unique_ptr<juce::DocumentWindow>
    makeWindowForHelper (juce::AudioProcessor& processor, const juce::String& title, std::function<void()> onClose);

private:
    class Window;

    struct RemoteWindow
    {
        RemoteInstance* remote = nullptr;
        juce::String positionKey;
        bool onTop = true;
        bool visible = true;
    };

    void remember (const Window& window);
    void remember (const juce::String& positionKey, std::optional<RemoteEditorPosition> position);
    void openRemote (Key key, RemoteInstance& remote, const juce::String& title, const juce::String& positionKey);

    juce::PropertiesFile& m_settings;
    std::function<void (Key)> m_onClosed;
    std::map<Key, std::unique_ptr<Window>> m_windows;
    std::map<Key, RemoteWindow> m_remote;
    bool m_mainMinimised = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool> (true);
};

/**
    Makes a plugin's editor window in a helper process: the same window as PluginWindows opens here.

    @param plugin       The plugin.
    @param title        The window title.
    @param position     Where it goes, or nothing for the middle of the screen.
    @param visible      Whether it shows from the start.
    @param onUserClose  Called when the user closes the window.
    @return             The window.
*/
[[nodiscard]] std::unique_ptr<RemoteEditorWindow>
createRemoteEditorWindow (juce::AudioPluginInstance& plugin, const juce::String& title,
                          std::optional<RemoteEditorPosition> position, bool visible,
                          std::function<void()> onUserClose);

} // namespace dawg
