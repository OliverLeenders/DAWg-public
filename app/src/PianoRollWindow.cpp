#include "PianoRollWindow.h"

#include "AppLog.h"
#include "WindowChrome.h"

#include <algorithm>

namespace dawg
{

namespace
{
const juce::Colour windowColour { 0xff111318 };
constexpr int defaultWidth = 1100;
constexpr int defaultHeight = 720;
constexpr int minWidth = 420;
constexpr int minHeight = 320;
constexpr int maxSize = 10000;
} // namespace

PianoRollWindow::PianoRollWindow (Bridge& bridge, const juce::WebBrowserComponent::Options& options,
                                  const juce::String& url, juce::PropertiesFile& settings,
                                  std::function<void()> onClose, const juce::String& title,
                                  const juce::String& boundsKey)
    : DocumentWindow (title, windowColour, allButtons),
      m_bridge (bridge),
      m_options (options),
      m_url (url),
      m_settings (settings),
      m_boundsKey (boundsKey),
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

    if (! restoreWindowStateFromString (m_settings.getValue (m_boundsKey)))
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

void PianoRollWindow::reloadBrowser()
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

PianoRollWindow::~PianoRollWindow()
{
    m_settings.setValue (m_boundsKey, getWindowStateAsString());
    m_bridge.detach (m_browser);
    clearContentComponent();
}

void PianoRollWindow::setContentMinWidth (int width)
{
    const auto least = std::clamp (width, minWidth, maxSize);
    setResizeLimits (least, minHeight, maxSize, maxSize);

    if (getWidth() < least && ! isFullScreen())
        setSize (least, getHeight());
}

void PianoRollWindow::closeButtonPressed()
{
    // The callback destroys this window, so run it after this call has returned.
    juce::MessageManager::callAsync ([onClose = m_onClose] { onClose(); });
}

} // namespace dawg
