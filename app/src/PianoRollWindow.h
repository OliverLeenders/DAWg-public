#pragma once

#include "Bridge.h"
#include "WindowChrome.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace dawg
{

/**
    An editor's own window: a second web view loading the UI with `?window=roll` (the MIDI editor) or
    `?window=audio` (the audio editor, docs/design/time-stretching.md, TS13), attached to the same bridge as the main
    window. Its position and size are remembered in the app settings.

    @note  Message thread only. Destroy it before the bridge.
*/
class PianoRollWindow final : public juce::DocumentWindow
{
public:
    /**
        Creates and shows the window.

        @param bridge    The bridge the web view talks to. Must outlive the window.
        @param options   The web view's options, with the bridge installed.
        @param url       The page to load.
        @param settings  Where the window's bounds are stored. Must outlive the window.
        @param onClose   Called (asynchronously) after the user closed the window; it should destroy it.
        @param title     The window's title.
        @param boundsKey The settings key its bounds are kept under.
    */
    PianoRollWindow (Bridge& bridge, const juce::WebBrowserComponent::Options& options, const juce::String& url,
                     juce::PropertiesFile& settings, std::function<void()> onClose,
                     const juce::String& title = "MIDI Editor - DAWg",
                     const juce::String& boundsKey = "pianoRollWindow");

    /**
        Remembers the bounds and detaches the web view from the bridge.
    */
    ~PianoRollWindow() override;

    PianoRollWindow (const PianoRollWindow&) = delete;
    PianoRollWindow& operator= (const PianoRollWindow&) = delete;
    PianoRollWindow (PianoRollWindow&&) = delete;
    PianoRollWindow& operator= (PianoRollWindow&&) = delete;

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

    /**
        Keeps the window at least this wide, for what its page shows (the Sampler's panel needs room for its
        columns), and widens it now if it's narrower. 0 goes back to the window's own minimum.
    */
    void setContentMinWidth (int width);

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
    juce::String m_boundsKey;
    std::function<void()> m_onClose;
    juce::WebBrowserComponent* m_browser = nullptr;
};

} // namespace dawg
