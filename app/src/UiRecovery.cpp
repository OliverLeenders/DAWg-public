#include "AppLog.h"
#include "MainComponent.h"
#include "WindowChrome.h"
#include "dawg/VarUtils.h"

// When the web view stops showing the UI (docs/design/ui-recovery.md): the log, the debugging port, rebuilding the web
// view (by hand, and when the page stops answering), nudging it after display changes, and a quit prompt of the app's
// own when the page's can't be seen.

namespace dawg
{

namespace
{
constexpr int defaultDebugPort = 9222;
constexpr juce::uint32 heartbeatTimeoutMs = 30000;
constexpr int quitPromptWaitMs = 2000;
constexpr const char* debugPortSetting = "ui.debugPort";
constexpr const char* debugPortSwitch = "--ui-debug-port";

// The port the web views open for a debugger: from --ui-debug-port [port], or the setting Help offers; 0 for none.
[[nodiscard]] int debugPortFrom (juce::PropertiesFile& settings)
{
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();

    if (const auto index = args.indexOf (debugPortSwitch); index >= 0)
        return args[index + 1].getIntValue() > 0 ? args[index + 1].getIntValue() : defaultDebugPort;

    return settings.getValue (debugPortSetting).getIntValue();
}
} // namespace

void MainComponent::startUiRecovery()
{
    if (const auto port = debugPortFrom (*m_properties.getUserSettings()); port > 0)
        applog::write (windowchrome::allowWebViewDebugging (port)
                           ? "Interface debugging on port " + juce::String (port)
                           : juce::String ("Interface debugging set up by the environment already"));
}

windowchrome::WindowHooks MainComponent::windowHooks()
{
    return { [safe = juce::Component::SafePointer (this)]
             {
                 if (safe != nullptr)
                     safe->reloadUi ("Reload interface from the window menu");
             },
             [safe = juce::Component::SafePointer (this)] (const juce::String& what)
             {
                 if (safe != nullptr)
                     safe->windowEvent (what);
             } };
}

void MainComponent::reloadUi (const juce::String& reason)
{
    if (m_browser == nullptr || ! hasUi())
        return;

    // Never from inside one of the web view's own callbacks: it is destroyed here.
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer (this), reason]
        {
            if (safe == nullptr)
                return;

            applog::write ("Reloading the interface: " + reason);
            safe->m_bridge.detach (safe->m_browser.get());
            safe->removeChildComponent (safe->m_browser.get());
            safe->m_browser.reset();
            safe->m_heartbeatSeen = false;
            safe->initialiseBrowser();
            safe->resized();
        });
}

void MainComponent::windowEvent (const juce::String& what)
{
    applog::write ("Main window: " + what);

    // After the window comes back or the display changes, the web view is told again that it is shown, which makes
    // it set up its drawing surface anew (UR4).
    if (what == "restored" || what == "maximised" || what == "display changed" || what == "DPI changed"
        || what == "resumed")
    {
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer (this)]
            {
                if (safe == nullptr || safe->m_browser == nullptr || ! safe->hasUi())
                    return;

                safe->m_browser->setVisible (false);
                safe->m_browser->setVisible (true);
                safe->resized();
            });
    }
}

void MainComponent::checkUiHeartbeat (bool appActive, bool minimised)
{
    // A page that answered and then went quiet for long while DAWg is in front and its window shown has stopped:
    // its renderer died or hangs (UR3). Hidden pages are slowed down by the browser, so they are left alone.
    if (! m_heartbeatSeen || ! appActive || minimised)
        return;

    if (juce::Time::getMillisecondCounter() - m_lastHeartbeat > heartbeatTimeoutMs)
    {
        m_heartbeatSeen = false;
        reloadUi ("the page stopped answering for " + juce::String (heartbeatTimeoutMs / 1000) + " s");
    }
}

void MainComponent::registerRecoveryHandlers()
{
    m_bridge.on ("ui.alive",
                 [this] (const juce::var&)
                 {
                     if (! m_heartbeatSeen)
                         applog::write ("Interface answering");

                     m_heartbeatSeen = true;
                     m_lastHeartbeat = juce::Time::getMillisecondCounter();
                     return juce::var();
                 });

    m_bridge.on ("app.reloadUi",
                 [this] (const juce::var&)
                 {
                     reloadUi ("Reload interface in the Help menu");
                     return juce::var();
                 });

    m_bridge.on ("app.logFile",
                 [] (const juce::var&)
                 {
                     const auto log = applog::file();

                     if (log.existsAsFile())
                         log.revealToUser();

                     return juce::var (log.getFullPathName());
                 });

    // The page shows its unsaved-changes prompt (UR6): no need for the app's own.
    m_bridge.on ("project.confirmShown",
                 [this] (const juce::var&)
                 {
                     m_quitPromptShown = true;
                     return juce::var();
                 });

    m_bridge.on ("project.confirmDone",
                 [this] (const juce::var&)
                 {
                     m_quitPromptPending = false;
                     m_quitPromptShown = false;
                     return juce::var();
                 });
}

void MainComponent::askToQuit()
{
    // Asked again while the page's prompt should be up: it can't be seen, so the app asks itself (UR6).
    if (m_quitPromptPending)
    {
        showNativeQuitPrompt();
        return;
    }

    m_quitPromptPending = true;
    m_quitPromptShown = false;
    m_bridge.emit ("dawg_confirm", makeObject ({ { "action", "quit" } }));

    juce::Timer::callAfterDelay (quitPromptWaitMs,
                                 [safe = juce::Component::SafePointer (this)]
                                 {
                                     if (safe != nullptr && safe->m_quitPromptPending && ! safe->m_quitPromptShown)
                                         safe->showNativeQuitPrompt();
                                 });
}

void MainComponent::showNativeQuitPrompt()
{
    applog::write ("Unsaved changes: asking with the app's own prompt");
    const auto name = m_projects->stateVar()["name"].toString();
    const auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::QuestionIcon)
                             .withTitle ("DAWg")
                             .withMessage ("Save changes to \"" + name + "\" before quitting?")
                             .withButton ("Save")
                             .withButton ("Don't save")
                             .withButton ("Cancel")
                             .withAssociatedComponent (this);

    juce::AlertWindow::showAsync (options,
                                  [safe = juce::Component::SafePointer (this)] (int result)
                                  {
                                      if (safe == nullptr)
                                          return;

                                      safe->m_quitPromptPending = false;

                                      // 1 is the first button, 2 the second, 0 the last (Cancel).
                                      if (result == 1)
                                          std::ignore = safe->m_projects->requestQuit (ProjectManager::Mode::save);
                                      else if (result == 2)
                                          std::ignore = safe->m_projects->requestQuit (ProjectManager::Mode::discard);
                                  });
}

} // namespace dawg
