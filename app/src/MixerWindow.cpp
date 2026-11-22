#include "MixerWindow.h"

#include "AppLog.h"
#include "WindowChrome.h"

namespace dawg
{

namespace
{
const juce::Colour windowColour { 0xff111318 };
constexpr const char* boundsKey = "mixerWindow";
constexpr int defaultWidth = 1100;
constexpr int defaultHeight = 620;
constexpr int minWidth = 320;
constexpr int minHeight = 360;
constexpr int maxSize = 10000;
} // namespace

MixerWindow::MixerWindow (Bridge& bridge, const juce::WebBrowserComponent::Options& options, const juce::String& url,
                          juce::PropertiesFile& settings, std::function<void()> onClose)
    : DocumentWindow ("Mixer - DAWg", windowColour, allButtons),
      m_bridge (bridge),
      m_options (options),
      m_url (url),
      m_settings (settings),
      m_onClose (std::move (onClose))
{
    windowchrome::apply (*this);

    auto browser = std::make_unique<juce::WebBrowserComponent> (options);
    m_browser = browser.get();
    m_bridge.attach (m_browser);
    browser->setSize (defaultWidth, defaultHeight);
    setContentOwned (browser.release(), true);
    setResizable (true, false);
    setResizeLimits (minWidth, minHeight, maxSize, maxSize);

    if (! restoreWindowStateFromString (m_settings.getValue (boundsKey)))
        centreWithSize (defaultWidth, defaultHeight);

    m_browser->goToURL (url);
    setVisible (true);
    windowchrome::attachToPeer (*this, { [safe = juce::Component::SafePointer (this)]
                                         {
                                             if (safe != nullptr)
                                                 safe->reloadBrowser();
                                         },
                                         [name = getName()] (const juce::String& what)
                                         { applog::write (name + ": " + what); } });
    toFront (true);
}

void MixerWindow::reloadBrowser()
{
    applog::write (getName() + ": reloading its interface");
    m_bridge.detach (m_browser);
    clearContentComponent();

    auto browser = std::make_unique<juce::WebBrowserComponent> (m_options);
    m_browser = browser.get();
    m_bridge.attach (m_browser);
    setContentOwned (browser.release(), false);
    m_browser->goToURL (m_url);
}

MixerWindow::~MixerWindow()
{
    m_settings.setValue (boundsKey, getWindowStateAsString());
    m_bridge.detach (m_browser);
    clearContentComponent();
}

void MixerWindow::closeButtonPressed()
{
    // The callback destroys this window, so run it after this call has returned.
    juce::MessageManager::callAsync ([onClose = m_onClose] { onClose(); });
}

} // namespace dawg
