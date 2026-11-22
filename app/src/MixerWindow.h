#pragma once

#include "Bridge.h"
#include "WindowChrome.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace dawg
{

/**
    The mixer's own window (docs/design/mixer.md, X1 and X11): a second web view loading the UI with
    `?window=mixer`, attached to the same bridge as the main window. Its position and size are remembered in the
    app settings.

    @note  Message thread only. Destroy it before the bridge.
*/
class MixerWindow final : public juce::DocumentWindow
{
public:
    /**
        Creates and shows the window.

        @param bridge    The bridge the web view talks to. Must outlive the window.
        @param options   The web view's options, with the bridge installed.
        @param url       The page to load.
        @param settings  Where the window's bounds are stored. Must outlive the window.
        @param onClose   Called (asynchronously) after the user closed the window; it should destroy it.
    */
    MixerWindow (Bridge& bridge, const juce::WebBrowserComponent::Options& options, const juce::String& url,
                 juce::PropertiesFile& settings, std::function<void()> onClose);

    /**
        Remembers the bounds and detaches the web view from the bridge.
    */
    ~MixerWindow() override;

    MixerWindow (const MixerWindow&) = delete;
    MixerWindow& operator= (const MixerWindow&) = delete;
    MixerWindow (MixerWindow&&) = delete;
    MixerWindow& operator= (MixerWindow&&) = delete;

    /**
        Asks the owner to close the window.
    */
    void closeButtonPressed() override;

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
    /**
        Replaces the web view with a new one on the same page: the window menu's Reload interface
        (docs/design/ui-recovery.md, UR3).
    */
    void reloadBrowser();

    Bridge& m_bridge;
    juce::WebBrowserComponent::Options m_options;
    juce::String m_url;
    juce::PropertiesFile& m_settings;
    std::function<void()> m_onClose;
    juce::WebBrowserComponent* m_browser = nullptr;
};

} // namespace dawg
