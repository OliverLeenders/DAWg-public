#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <functional>
#include <memory>
#include <optional>

namespace dawg
{

/**
    The switch part of the command line that runs the executable as a plugin helper
    (docs/design/out-of-process-plugins.md).
*/
inline constexpr const char* remotePluginHelperId = "dawg-plugin-host";

/**
    Returns whether plugins can run in helper processes on this platform. Only Windows so far.
*/
[[nodiscard]] bool remotePluginsAvailable() noexcept;

/**
    Sets which executable helpers run. The default is the running one.

    @param executable  The executable, which must start a helper when it finds remotePluginHelperId on its command line.
*/
void setRemotePluginExecutable (const juce::File& executable);

/**
    Loads a plugin in a helper process, one helper per plugin file (O8a), behind a stand-in processor.

    @param description  The plugin.
    @param sampleRate   The sample rate to start at.
    @param blockSize    The block size to start at.
    @param callback     Receives the stand-in, or an error. Called on another thread, and possibly before this returns.
*/
void createRemotePlugin (
    const juce::PluginDescription& description, double sampleRate, int blockSize,
    const std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)>& callback);

/**
    Where a plugin's editor window is on screen.
*/
struct RemoteEditorPosition
{
    int x = 0; ///< From the left of the screen.
    int y = 0; ///< From the top of the screen.
};

/**
    A plugin's editor window in the helper. This module has no windows, so the helper's executable provides them
    (docs/design/out-of-process-plugins.md, O7).
*/
class RemoteEditorWindow
{
public:
    /**
        Closes the window.
    */
    virtual ~RemoteEditorWindow() = default;

    /**
        Brings the window to the front.
    */
    virtual void toFront() = 0;

    /**
        Floats the window above other windows, or not, and shows or hides it.

        @param onTop    Whether it floats.
        @param visible  Whether it shows.
    */
    virtual void setShown (bool onTop, bool visible) = 0;

    /**
        Returns where the window is.
    */
    [[nodiscard]] virtual RemoteEditorPosition position() const = 0;
};

/**
    Makes a plugin's editor window in the helper.

    @param plugin       The plugin. Outlives the window.
    @param title        The window title.
    @param position     Where it goes, or nothing for the middle of the screen.
    @param visible      Whether it shows from the start. If so, it comes to the front.
    @param onUserClose  To call on the message thread, after the call that noticed, when the user closes the window.
*/
using RemoteEditorFactory = std::function<std::unique_ptr<RemoteEditorWindow> (
    juce::AudioPluginInstance& plugin, const juce::String& title, std::optional<RemoteEditorPosition> position,
    bool visible, std::function<void()> onUserClose)>;

/**
    Returns whether the foreground window belongs to a helper, which for the user is still DAWg.
*/
[[nodiscard]] bool remoteHelperIsForeground();

/**
    Sets how often a plugin whose parameters moved has its state kept, for starting it again after a crash.

    @param seconds  The time between snapshots. 30 unless changed, which is for tests.
*/
void setRemotePluginSnapshotInterval (double seconds);

/**
    What a plugin in a helper process offers beyond a processor: how it is doing, and starting it again
    (docs/design/out-of-process-plugins.md, O4, O8). A stand-in implements this; dynamic_cast finds it.
*/
class RemoteInstance
{
public:
    /**
        Destroys the interface.
    */
    virtual ~RemoteInstance() = default;

    /**
        Returns whether the helper has gone, by crashing or because it hung and was closed. The stand-in then passes audio
        through or is silent, and reports no latency, until reload().

        @note  Any thread.
    */
    [[nodiscard]] virtual bool crashed() const noexcept = 0;

    /**
        Returns what happened, for the user, or an empty string.
    */
    [[nodiscard]] virtual juce::String crashReason() const = 0;

    /**
        Sets who hears of a crash.

        @param callback  Called once per crash, on another thread. Must return quickly.
    */
    virtual void setCrashCallback (std::function<void()> callback) = 0;

    /**
        Returns how many blocks came back too late since the plugin was loaded.

        @note  Any thread.
    */
    [[nodiscard]] virtual int lateBlocks() const noexcept = 0;

    /**
        Starts the plugin again in a new helper, with its last known state. Waits for that, up to a limit.

        @param error  Receives what went wrong.
        @return       True if it runs again.
        @note  Not on the helper's pipe thread. Blocks for as long as the plugin takes to load.
    */
    [[nodiscard]] virtual bool reload (juce::String& error) = 0;

    /**
        Opens the plugin's editor window in the helper, or brings it to the front if it is open.

        @param title     The window title.
        @param position  Where it goes, or nothing for the middle of the screen.
        @param visible   Whether it shows from the start.
        @param onGone    Called once when the user closed the window, with where it was, or when the helper went away,
                         with nothing. On another thread, and must return quickly. Not called after closeEditor().
    */
    virtual void openEditor (const juce::String& title, std::optional<RemoteEditorPosition> position, bool visible,
                             std::function<void (std::optional<RemoteEditorPosition>)> onGone) = 0;

    /**
        Closes the editor window. Waits briefly for the helper, to learn where the window was.

        @return  Where it was, or nothing if that can't be known.
    */
    virtual std::optional<RemoteEditorPosition> closeEditor() = 0;

    /**
        Floats the open editor window above other windows, or not, and shows or hides it.

        @param onTop    Whether it floats.
        @param visible  Whether it shows.
    */
    virtual void showEditor (bool onTop, bool visible) = 0;
};

/**
    The error a load gets when its helper went away during it, so the plugin's loading itself may be what crashed.
*/
inline constexpr const char* remotePluginProcessGone = "The plugin's process has gone";

/**
    Runs the helper's side until the app goes away.
*/
class RemotePluginHelper
{
public:
    /**
        Destroys the helper, its plugins first.
    */
    virtual ~RemotePluginHelper() = default;
};

/**
    Starts being a helper, if the command line says so. The caller keeps the message loop running.

    @param commandLine   The process's command line.
    @param addFormats    Registers the plugin formats the helper loads with.
    @param onDisconnect  Called when the app has gone, on another thread: the caller quits.
    @param makeEditor    Makes plugin editor windows.
    @return              The helper, or nullptr if the command line isn't a helper's.
*/
[[nodiscard]] std::unique_ptr<RemotePluginHelper>
startRemotePluginHelper (const juce::String& commandLine,
                         const std::function<void (juce::AudioPluginFormatManager&)>& addFormats,
                         std::function<void()> onDisconnect, RemoteEditorFactory makeEditor);

} // namespace dawg
