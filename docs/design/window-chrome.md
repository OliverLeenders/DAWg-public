# Design: window chrome

Status: **implemented** on Windows (2026-11-01). macOS and Linux keep the native title bar.

## Why

The native title bar is a strip that only holds the window's name and three buttons. DAWg's windows already have a bar
of their own at the top, so the two are merged: the UI's bar is the title bar.

## How

* `windowchrome::apply` (app/src/WindowChrome.cpp) removes the native title bar from the main, mixer and piano roll
  windows. Plugin editor windows keep theirs: their content isn't ours.
* The window keeps JUCE's resizable border, a few pixels around the web view. JUCE reports those pixels to Windows as
  sizing edges, so resizing and edge snapping are the system's. The web view can't take part in that hit testing: it
  is its own native window in another process, which answers every hit test itself.
* The empty parts of the top bar (`DRAG_TARGETS` in components/WindowControls.tsx) call `window.control` with `drag`
  on a press. The native side lets the system take over the move (`windowchrome::startDrag`), so Aero Snap works. A
  double-click maximises or restores.
* Snapping also needs the window's minimise and maximise boxes and a sizing frame. JUCE drops the boxes along with its
  own title bar buttons, so `windowchrome::attachToPeer` puts the style bits back (2026-11-04: dragging to the top or
  the sides didn't snap). With a sizing frame, a maximised window reaches past the screen by the
  frame's width, so the subclass sets a maximised window's client area to the monitor's work area.
  A maximised window has no sizing edges: the subclass turns JUCE's emulated ones into client area while it is
  maximised (dragging the top edge sized it while it stayed maximised and cut off its top).
* The UI draws minimise, maximise and close. `app.info` says whether it should (`customTitleBar`).
* Without a native title bar a maximised window covers the taskbar and JUCE keeps it that size, so a small window
  subclass (`windowchrome::attachToPeer`) maximises to the monitor's work area instead.
* If the UI files are missing the main window shows a message and has no bar of its own, so it keeps the native title
  bar.

## Limits

* The Windows 11 snap layouts flyout doesn't open over the maximise button. It needs the system to see that button in a
  hit test, which the web view prevents. WebView2 has non-client region support for this
  (`IsNonClientRegionSupportEnabled`, `app-region: drag`), but JUCE doesn't expose the settings object, so it would mean
  patching JUCE. Win+Z still opens the layouts.
* Window name: the taskbar still shows it, but the windows no longer do, except for the project name in the top bar.
