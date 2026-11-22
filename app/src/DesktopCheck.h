#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>

namespace dawg::desktop
{

/** A desktop that moves windows on Alt+drag, and where to change that. */
struct AltDragGrab
{
    /** The desktop's name, as the user knows it ("KDE Plasma", "Xfce"…). */
    juce::String desktop;
    /** Where its window-move key is set. */
    juce::String setting;
};

/**
    Runs a command and returns what it printed, trimmed; empty when it can't run or fails.
*/
using CommandRunner = std::function<juce::String (const juce::StringArray& command)>;

/**
    Whether a Linux desktop's window manager moves windows on Alt+drag, which would take DAWg's Alt gestures (new clips
    and notes, copying, splitting, linking) before they reach it (docs/design/ux-review.md, U12). Reads the window
    manager's own setting with its command-line tool; it changes nothing.

    @param currentDesktop  $XDG_CURRENT_DESKTOP, e.g. "KDE" or "X-Cinnamon".
    @param run             Runs a settings tool.
    @return  The desktop and where to change the key, or nothing when Alt+drag is free or the desktop is unknown.
*/
[[nodiscard]] std::optional<AltDragGrab> altDragGrab (const juce::String& currentDesktop, const CommandRunner& run);

/**
    altDragGrab() for this session's desktop, worked out once. Always nothing outside Linux.
*/
[[nodiscard]] const std::optional<AltDragGrab>& altDragGrabHere();

} // namespace dawg::desktop
