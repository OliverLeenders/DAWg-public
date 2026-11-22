#include "DesktopCheck.h"

#include <array>

namespace dawg::desktop
{

namespace
{
// A gsettings modifier ("'<Alt>'", "'<Super>'", "''"): whether it's Alt.
[[nodiscard]] bool isAlt (const juce::String& value)
{
    return value.containsIgnoreCase ("alt") || value.containsIgnoreCase ("mod1");
}

[[nodiscard]] juce::String runForOutput (const juce::StringArray& command)
{
    juce::ChildProcess process;

    if (! process.start (command, juce::ChildProcess::wantStdOut))
        return {};

    const auto output = process.readAllProcessOutput();

    if (! process.waitForProcessToFinish (2000) || process.getExitCode() != 0)
        return {};

    return output.trim();
}
} // namespace

std::optional<AltDragGrab> altDragGrab (const juce::String& currentDesktop, const CommandRunner& run)
{
    const auto desktop = currentDesktop.toUpperCase();

    if (desktop.contains ("KDE"))
    {
        // Plasma 6 moved the default to Meta; Plasma 5 (kreadconfig5 only) kept Alt. An empty answer is the default.
        auto key = run ({ "kreadconfig6", "--file", "kwinrc", "--group", "MouseBindings", "--key", "CommandAllKey" });
        auto defaultKey = juce::String ("Meta");

        if (key.isEmpty() && run ({ "kreadconfig6", "--help" }).isEmpty())
        {
            key = run ({ "kreadconfig5", "--file", "kwinrc", "--group", "MouseBindings", "--key", "CommandAllKey" });
            defaultKey = "Alt";
        }

        if ((key.isEmpty() ? defaultKey : key).containsIgnoreCase ("alt"))
            return AltDragGrab {
                "KDE Plasma", "System Settings › Window Management › Window Behavior › Window Actions › Modifier key"
            };

        return std::nullopt;
    }

    if (desktop.contains ("XFCE"))
    {
        // Alt by default; "None" switches it off.
        const auto key = run ({ "xfconf-query", "-c", "xfwm4", "-p", "/general/easy_click" });

        if (key.isEmpty() || key.containsIgnoreCase ("alt"))
            return AltDragGrab {
                "Xfce", "Settings › Window Manager Tweaks › Accessibility › Key used to grab and move windows"
            };

        return std::nullopt;
    }

    struct Gnomish
    {
        const char* marker;
        const char* name;
        const char* schema;
        const char* key;
        const char* setting;
    };

    static const auto gnomish = std::to_array<Gnomish> ({
        { "CINNAMON", "Cinnamon", "org.cinnamon.desktop.wm.preferences", "mouse-button-modifier",
          "System Settings › Windows › Behavior › Special key to move and resize windows" },
        { "MATE", "MATE", "org.mate.Marco.general", "mouse-button-modifier",
          "Control Center › Windows › Behaviour › Movement Key" },
        { "BUDGIE", "Budgie", "com.solus-project.budgie-wm", "mouse-button-modifier",
          "Budgie Desktop Settings › Windows" },
        { "GNOME", "GNOME", "org.gnome.desktop.wm.preferences", "mouse-button-modifier",
          "GNOME Tweaks › Windows › Window Action Key" },
    });

    for (const auto& d : gnomish)
        if (desktop.contains (d.marker))
        {
            if (isAlt (run ({ "gsettings", "get", d.schema, d.key })))
                return AltDragGrab { d.name, d.setting };

            return std::nullopt;
        }

    return std::nullopt;
}

const std::optional<AltDragGrab>& altDragGrabHere()
{
    static const auto grab = []() -> std::optional<AltDragGrab>
    {
#if JUCE_LINUX
        return altDragGrab (juce::SystemStats::getEnvironmentVariable ("XDG_CURRENT_DESKTOP", {}), runForOutput);
#else
        juce::ignoreUnused (runForOutput);
        return std::nullopt;
#endif
    }();
    return grab;
}

} // namespace dawg::desktop
