#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace dawg::windowchrome
{

/**
    What a web view window does about its own trouble (docs/design/ui-recovery.md): a way to rebuild its web view, and
    a listener for what happens to the window that can upset one (minimising, display and DPI changes, sleep).
*/
struct WindowHooks
{
    /** Rebuilds the window's web view: the window menu's Reload interface. None: no such item. */
    std::function<void()> reload;

    /** Hears what happened to the window, in words ("restored", "display changed", "resumed"...). */
    std::function<void (const juce::String&)> event;
};

/**
    Whether the UI draws the title bar of DAWg's web view windows (docs/design/window-chrome.md). Windows only so far;
    elsewhere they keep the native one.

    @return  true if apply() removes the native title bar.
*/
[[nodiscard]] bool usesCustomTitleBar() noexcept;

/**
    The border around a window's content: a few pixels at each edge that the web view leaves free while the window can
    be resized. Without a native title bar, JUCE answers the resize edges inside the window, but only where its own
    window is under the mouse; a web view filling the window took nearly all of them, so the resize cursor seldom
    showed. Windows override getContentComponentBorder() with this.

    @param window  The window.
    @param border  The border DocumentWindow gives the content (its title bar's).
    @return        That border, with the resize margin while the window is resizable and not maximised.
*/
[[nodiscard]] juce::BorderSize<int> contentBorder (const juce::DocumentWindow& window, juce::BorderSize<int> border);

/**
    Gets a window's web views drawing again after it is restored from minimised (docs/design/ui-recovery.md, UR7). A
    maximised window restored at the size it had gave its web views the same bounds, and WebView2 went on showing
    nothing (the window's accent blue) until the window was resized: it is made a pixel shorter and back, a few times. Windows call this from
    minimisationStateChanged().

    @param window          The window.
    @param nowMinimised    Whether it has just been minimised (nothing is done) or restored.
*/
void redrawWebViews (juce::DocumentWindow& window, bool nowMinimised);

/**
    Sets up a web view window's title bar. Call before its content is set.

    @param window  The window.
*/
void apply (juce::DocumentWindow& window);

/**
    Finishes the title bar's setup once the window has its native peer: on Windows, a maximised window without a native
    title bar would cover the taskbar. The window keeps a window menu (Alt+Space), so it can be minimised, moved and
    closed even when its web view draws nothing, and the menu offers Reload interface when the hooks have one. Call
    after the window is made visible.

    @param window  The window.
    @param hooks   What the window does about its web view's trouble.
*/
void attachToPeer (juce::DocumentWindow& window, const WindowHooks& hooks = {});

/**
    Lets a debugger reach every web view this process creates afterwards, through WebView2's remote debugging port
    (docs/design/ui-recovery.md, UR2). Call before the first web view is made. Windows only; elsewhere does nothing.

    @param port  The port, such as 9222.
    @return      false if the web views already have one from the environment.
*/
bool allowWebViewDebugging (int port);

/**
    Lets the system move a window while the left mouse button stays down, as if its title bar were held (so Aero Snap
    works). Returns when the button is released; does nothing if it already is.

    @param window  The window.
*/
void startDrag (juce::DocumentWindow& window);

} // namespace dawg::windowchrome
