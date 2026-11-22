#include "WindowChrome.h"

#include "AppLog.h"

#if JUCE_WINDOWS
// commctrl.h needs windows.h first.
// clang-format off
    #define NOMINMAX
    #include <windows.h>
    #include <commctrl.h>
 // clang-format on
 #pragma comment(lib, "comctl32.lib")

 #include <map>
#endif

namespace dawg::windowchrome
{

#if JUCE_WINDOWS
namespace
{
constexpr UINT_PTR subclassId = 1;
constexpr UINT reloadCommand = 0x0D10; // a window menu item's id: below 0xF000, a multiple of 16

struct WindowState
{
    WindowHooks hooks;
    WPARAM sizeState = SIZE_RESTORED;
};

// The hooks of each window with the custom title bar. Message thread only.
std::map<HWND, WindowState>& windows()
{
    static std::map<HWND, WindowState> map;
    return map;
}

// Whether a window is minimised, maximised or neither, as WM_SIZE says it.
WPARAM sizeStateOf (HWND hwnd)
{
    if (IsIconic (hwnd))
        return SIZE_MINIMIZED;

    return IsZoomed (hwnd) ? SIZE_MAXIMIZED : SIZE_RESTORED;
}

const char* sizeStateName (WPARAM state)
{
    if (state == SIZE_MINIMIZED)
        return "minimised";

    return state == SIZE_MAXIMIZED ? "maximised" : "restored";
}

void tell (HWND hwnd, const char* what)
{
    if (const auto it = windows().find (hwnd); it != windows().end() && it->second.hooks.event != nullptr)
        it->second.hooks.event (what);
}

// What happens to the window that can upset a web view, told to its hooks (docs/design/ui-recovery.md, UR1).
void watch (HWND hwnd, UINT message, WPARAM wParam)
{
    switch (message)
    {
        case WM_WINDOWPOSCHANGED:
            // JUCE answers this itself, so Windows sends no WM_SIZE: the state is read here.
            if (auto it = windows().find (hwnd); it != windows().end() && sizeStateOf (hwnd) != it->second.sizeState)
            {
                it->second.sizeState = sizeStateOf (hwnd);
                tell (hwnd, sizeStateName (it->second.sizeState));
            }
            break;

        case WM_DISPLAYCHANGE:
            tell (hwnd, "display changed");
            break;

        case WM_DPICHANGED:
            tell (hwnd, "DPI changed");
            break;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND)
                tell (hwnd, "suspending");
            else if (wParam == PBT_APMRESUMEAUTOMATIC)
                tell (hwnd, "resumed");
            break;

        default:
            break;
    }
}

// Maximises to the monitor's work area, and keeps that as the client area (JUCE's own handling assumes the full monitor).
LRESULT CALLBACK maximiseToWorkArea (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR /*id*/,
                                     DWORD_PTR /*data*/)
{
    watch (hwnd, message, wParam);

    switch (message)
    {
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == reloadCommand)
            {
                if (const auto it = windows().find (hwnd); it != windows().end() && it->second.hooks.reload != nullptr)
                    it->second.hooks.reload();

                return 0;
            }
            break;

        case WM_GETMINMAXINFO:
        {
            const auto result = DefSubclassProc (hwnd, message, wParam, lParam);
            MONITORINFO monitor {};
            monitor.cbSize = sizeof (monitor);

            if (GetMonitorInfoW (MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                auto* info =
                    reinterpret_cast<MINMAXINFO*> (lParam); // NOLINT(performance-no-int-to-ptr): Win32 passes it so
                info->ptMaxPosition = { monitor.rcWork.left - monitor.rcMonitor.left,
                                        monitor.rcWork.top - monitor.rcMonitor.top };
                info->ptMaxSize = { monitor.rcWork.right - monitor.rcWork.left,
                                    monitor.rcWork.bottom - monitor.rcWork.top };
            }

            return result;
        }

        case WM_NCCALCSIZE:
            // A maximised window with a sizing frame reaches past the screen by the frame's width on every side
            // (Windows does that so the frame is off-screen), so its client area is pulled in to the work area:
            // without this the top bar and the edges are cut off.
            if (wParam != 0 && IsZoomed (hwnd))
            {
                MONITORINFO monitor {};
                monitor.cbSize = sizeof (monitor);

                // Kept inside the proposed window too: should the window ever be sized while maximised, a client
                // area larger than the window would be drawn partly outside it.
                auto& proposed =
                    reinterpret_cast<NCCALCSIZE_PARAMS*> (lParam)->rgrc[0]; // NOLINT(performance-no-int-to-ptr)

                if (GetMonitorInfoW (MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                    IntersectRect (&proposed, &proposed, &monitor.rcWork);

                return 0;
            }
            break;

        case WM_NCHITTEST:
        {
            // A maximised window has no sizing edges. JUCE emulates them inside the window whenever it is resizable,
            // and a drag on them sized the window while it stayed maximised.
            const auto result = DefSubclassProc (hwnd, message, wParam, lParam);
            return IsZoomed (hwnd) && HTSIZEFIRST <= result && result <= HTSIZELAST ? HTCLIENT : result;
        }

        case WM_NCDESTROY:
            RemoveWindowSubclass (hwnd, maximiseToWorkArea, subclassId);
            windows().erase (hwnd);
            break;

        default:
            break;
    }

    return DefSubclassProc (hwnd, message, wParam, lParam);
}
} // namespace
#endif

bool usesCustomTitleBar() noexcept
{
#if JUCE_WINDOWS
    return true;
#else
    return false;
#endif
}

juce::BorderSize<int> contentBorder (const juce::DocumentWindow& window, juce::BorderSize<int> border)
{
    // Inside the eight or so physical pixels where JUCE's emulated resize edges are, so they stay reachable.
    constexpr int margin = 4;

    if (window.isUsingNativeTitleBar() || ! window.isResizable() || window.isFullScreen() || window.isMinimised())
        return border;

    return { border.getTop() + margin, border.getLeft() + margin, border.getBottom() + margin,
             border.getRight() + margin };
}

namespace
{
// The window a pixel shorter and, a moment later, as it was (UR7). Moving its web views inside it wasn't enough after a
// long minimise: the window itself has to change size for Windows to compose it anew.
void nudgeWindow (juce::DocumentWindow& window)
{
#if JUCE_WINDOWS
    auto* peer = window.getPeer();

    if (peer == nullptr)
        return;

    auto hwnd = static_cast<HWND> (peer->getNativeHandle());
    RECT rect {};

    if (! GetWindowRect (hwnd, &rect))
        return;

    const auto width = rect.right - rect.left;
    const auto height = rect.bottom - rect.top;
    constexpr UINT flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    SetWindowPos (hwnd, nullptr, 0, 0, width, height - 1, flags);

    constexpr int backMilliseconds = 60;
    juce::Timer::callAfterDelay (
        backMilliseconds,
        [safe = juce::Component::SafePointer<juce::DocumentWindow> (&window), hwnd, width, height]
        {
            RECT now {};

            // Back, unless the window has been sized or closed since.
            if (safe != nullptr && GetWindowRect (hwnd, &now) && now.right - now.left == width
                && now.bottom - now.top == height - 1)
                SetWindowPos (hwnd, nullptr, 0, 0, width, height, flags);
        });
#else
    juce::ignoreUnused (window);
#endif
}
} // namespace

void redrawWebViews (juce::DocumentWindow& window, bool nowMinimised)
{
    if (nowMinimised)
        return;

    applog::write (window.getName() + ": restored, nudged so its web view draws");

    // WebView2 takes longer to take in a restore the longer the window was hidden (after half a minute, more than a
    // tenth of a second), and a nudge before it has doesn't count: so a few, spread out. Each is a pixel.
    for (const auto delay : { 100, 400, 1000, 2500 })
        juce::Timer::callAfterDelay (delay,
                                     [safe = juce::Component::SafePointer<juce::DocumentWindow> (&window)]
                                     {
                                         if (safe != nullptr && ! safe->isMinimised())
                                             nudgeWindow (*safe);
                                     });
}

void apply (juce::DocumentWindow& window)
{
    window.setUsingNativeTitleBar (! usesCustomTitleBar());

    if (usesCustomTitleBar())
    {
        window.setTitleBarHeight (0);
        window.setTitleBarButtonsRequired (0, false);
    }
}

void attachToPeer (juce::DocumentWindow& window, const WindowHooks& hooks)
{
#if JUCE_WINDOWS
    if (auto* peer = window.getPeer(); peer != nullptr && ! window.isUsingNativeTitleBar())
    {
        auto hwnd = static_cast<HWND> (peer->getNativeHandle());
        const auto reloads = hooks.reload != nullptr;
        windows()[hwnd] = { hooks, sizeStateOf (hwnd) };
        SetWindowSubclass (hwnd, maximiseToWorkArea, subclassId, 0);

        // JUCE derives the window style from its title bar buttons, and apply() turns those off, so the window lost
        // its minimise and maximise boxes. Windows only snaps a window to the screen's edges and top (and offers its
        // snap layouts) when it has them and a sizing frame, so they're put back; the UI draws the buttons.
        // The window menu (Alt+Space) stays too: when the web view draws nothing, it is the way to minimise, close or
        // reload it (docs/design/ui-recovery.md, UR5).
        const auto style = GetWindowLongPtrW (hwnd, GWL_STYLE);
        const auto wanted =
            style | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | (window.isResizable() ? WS_THICKFRAME : 0);

        if (wanted != style)
        {
            SetWindowLongPtrW (hwnd, GWL_STYLE, wanted);
            SetWindowPos (hwnd, nullptr, 0, 0, 0, 0,
                          SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (auto* menu = GetSystemMenu (hwnd, FALSE); menu != nullptr && reloads)
        {
            AppendMenuW (menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW (menu, MF_STRING, reloadCommand, L"Reload interface");
        }
    }
#else
    juce::ignoreUnused (window, hooks);
#endif
}

bool allowWebViewDebugging (int port)
{
#if JUCE_WINDOWS
    constexpr auto variable = L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
    const auto existing = juce::SystemStats::getEnvironmentVariable ("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", {});

    if (existing.contains ("--remote-debugging-port"))
        return false;

    const auto arguments = (existing + " --remote-debugging-port=" + juce::String (port)).trim();
    return SetEnvironmentVariableW (variable, arguments.toWideCharPointer()) != 0;
#else
    juce::ignoreUnused (port);
    return false;
#endif
}

void startDrag (juce::DocumentWindow& window)
{
#if JUCE_WINDOWS
    auto* peer = window.getPeer();

    if (peer == nullptr
        || (GetAsyncKeyState (GetSystemMetrics (SM_SWAPBUTTON) != 0 ? VK_RBUTTON : VK_LBUTTON) & 0x8000) == 0)
        return;

    auto hwnd = static_cast<HWND> (peer->getNativeHandle());
    POINT cursor {};
    GetCursorPos (&cursor);

    // The web view holds the mouse. JUCE's window procedure waits for a move over the window before it hands a title
    // bar press to the system, and that move would go to the web view, so this goes to the system directly.
    ReleaseCapture();
    DefWindowProcW (hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM (cursor.x, cursor.y));
#else
    juce::ignoreUnused (window);
#endif
}

} // namespace dawg::windowchrome
