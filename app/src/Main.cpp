#include "AppLog.h"
#include "MainComponent.h"
#include "ScanWorker.h"
#include "WindowChrome.h"
#include "dawg/ClapPlugin.h"
#include "dawg/RemotePlugin.h"

namespace dawg
{

namespace
{
constexpr const char* devSwitch = "--dev";
constexpr const char* defaultDevServerUrl = "http://localhost:5173";
const juce::Colour windowColour { 0xff111318 };
constexpr int minWindowWidth = 800;
constexpr int minWindowHeight = 500;
constexpr int maxWindowSize = 10000;

/**
    Reads the --dev [url] switch.

    @param args  The command line.
    @return      The dev server URL, or std::nullopt if --dev was not given.
*/
[[nodiscard]] std::optional<juce::String> devServerUrlFrom (const juce::StringArray& args)
{
    const auto index = args.indexOf (devSwitch);

    if (index < 0)
        return std::nullopt;

    const auto next = args[index + 1].unquoted();
    return next.startsWith ("http") ? next : juce::String (defaultDevServerUrl);
}

/**
    The main application window.
*/
class MainWindow final : public juce::DocumentWindow
{
public:
    /**
        Creates and shows the window.

        @param name          The window title.
        @param appProperties Application settings, passed to MainComponent.
        @param devServerUrl  Optional dev server URL, passed to MainComponent.
    */
    MainWindow (const juce::String& name, juce::ApplicationProperties& appProperties,
                std::optional<juce::String> devServerUrl)
        : DocumentWindow (name, windowColour, allButtons)
    {
        auto content = std::make_unique<MainComponent> (appProperties, std::move (devServerUrl));
        auto hooks = content->windowHooks();

        // Without its UI files the window has no bar of its own to be moved or closed by.
        if (content->hasUi())
            windowchrome::apply (*this);
        else
            setUsingNativeTitleBar (true);

        setContentOwned (content.release(), true);
        setResizable (true, false);
        setResizeLimits (minWindowWidth, minWindowHeight, maxWindowSize, maxWindowSize);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        windowchrome::attachToPeer (*this, hooks);
    }

    /**
        Destroys the window and its content.
    */
    ~MainWindow() override = default;

    /**
        Quits the application.
    */
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    /**
        Leaves the resize edges free of the web view (windowchrome::contentBorder).
    */
    [[nodiscard]] juce::BorderSize<int> getContentComponentBorder() const override
    {
        return windowchrome::contentBorder (*this, DocumentWindow::getContentComponentBorder());
    }

    /**
        Gets the web view drawing again after the window is restored (windowchrome::redrawWebViews).
    */
    void minimisationStateChanged (bool nowMinimised) override { windowchrome::redrawWebViews (*this, nowMinimised); }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
} // namespace

/**
    The DAWg application. Also runs as a plugin scan worker when started with --scan-plugin.
*/
class DawgApplication final : public juce::JUCEApplication
{
public:
    /**
        Returns the application name.
    */
    [[nodiscard]] const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }

    /**
        Returns the application version.
    */
    [[nodiscard]] const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    /**
        Returns true: scan workers run as extra instances.
    */
    [[nodiscard]] bool moreThanOneInstanceAllowed() override { return true; }

    /**
        Starts the app, or runs a single plugin scan and quits.

        @param commandLine  The command line arguments.
    */
    void initialise (const juce::String& commandLine) override
    {
        const auto args = juce::StringArray::fromTokens (commandLine, true);

        // A plugin helper (docs/design/out-of-process-plugins.md): the message loop runs on until the app goes away.
        m_pluginHelper = startRemotePluginHelper (
            commandLine,
            [] (juce::AudioPluginFormatManager& formats)
            {
                juce::addDefaultFormatsToManager (formats);
                formats.addFormat (std::make_unique<ClapPluginFormat>());
            },
            [] { juce::MessageManager::callAsync ([] { juce::JUCEApplicationBase::quit(); }); },
            createRemoteEditorWindow);

        if (m_pluginHelper != nullptr)
            return;

        if (args.contains (scanWorkerSwitch))
        {
            setApplicationReturnValue (static_cast<int> (runScanWorker (args)));
            quit();
            return;
        }

        juce::PropertiesFile::Options settingsOptions;
        settingsOptions.applicationName = "DAWg";
        settingsOptions.folderName = "DAWg";
        settingsOptions.filenameSuffix = ".settings";
        settingsOptions.osxLibrarySubFolder = "Application Support";
        m_properties.setStorageParameters (settingsOptions);

        // The log of what happens to the app and its windows (docs/design/ui-recovery.md, UR1).
        applog::open (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("DAWg"));
        m_mainWindow = std::make_unique<MainWindow> (getApplicationName(), m_properties, devServerUrlFrom (args));
    }

    /**
        Closes the window and flushes settings.
    */
    void shutdown() override
    {
        m_mainWindow = nullptr;
        m_properties.closeFiles();
        applog::close();
    }

    /**
        Quits when the OS or the window asks to.
    */
    void systemRequestedQuit() override
    {
        // The main component resolves unsaved changes first (and calls quit() once they are).
        auto* content =
            m_mainWindow != nullptr ? dynamic_cast<MainComponent*> (m_mainWindow->getContentComponent()) : nullptr;

        if (content != nullptr)
            content->requestQuit();
        else
            quit();
    }

private:
    juce::ApplicationProperties m_properties;
    std::unique_ptr<MainWindow> m_mainWindow;
    std::unique_ptr<RemotePluginHelper> m_pluginHelper;
};

} // namespace dawg

START_JUCE_APPLICATION (dawg::DawgApplication)
