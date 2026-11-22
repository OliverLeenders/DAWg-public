# UI recovery

Status: accepted (owner, 2026-11-10: "start with 1, 4"), built.

On 2026-11-10 an installed DAWg showed a window filled with plain blue. The app, its web view's processes and the page
were all alive: Windows' accessibility interface read the whole UI out of the window, and the project autosaved on
schedule. The web view had stopped putting the page on screen. Nothing recorded why, the window could not be
minimised or closed (DAWg draws its own title bar in the page), and closing it would have asked about unsaved changes
in that same invisible page. The cause is not known yet; these decisions make the next occurrence diagnosable and
recoverable without losing anything. Finding and fixing the cause is a separate step, once there is evidence.

## Decisions

**UR1. A log file.** `Logs/DAWg.log` in the app's data folder (`%APPDATA%\DAWg` on Windows), trimmed to its last
megabyte at each start (`app/src/AppLog.h`). It records the start and end of each session, the audio device, every
window's minimising, maximising and restoring, display, DPI, sleep and resume events, the interface reloads and why,
when the page starts answering, and when the app asks about unsaved changes itself. Help › Show the log file opens
its folder.

**UR2. Interface debugging.** Help › Allow interface debugging (from the next start), or `--ui-debug-port [port]`,
opens WebView2's remote debugging port (9222 unless given) for every DAWg web view, so a debugger can look inside
the installed app: take a screenshot of the page itself, read its DOM and console. Off by default. The environment's
own `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` wins when it sets a port already.

**UR3. Reload interface.** Rebuilds a window's web view on the same page; the project lives in the engine, not the
page, so nothing is lost, and the page reads everything again when it connects. It is in the window menu (Alt+Space)
of every window with DAWg's own title bar, and in Help. The main window's page also says it is alive every 5 seconds
(`ui.alive`); when it stops for 30 seconds while DAWg is in front and its window not minimised, the app reloads it by
itself. Hidden pages are slowed down by the browser, so they are left alone.

**UR4. A nudge after window and display changes.** When the main window is restored or maximised, the display or its
DPI changes, or the computer resumes, the web view is hidden and shown again, which makes it set up its drawing
surface anew.

**UR5. The window stays controllable.** Windows with DAWg's own title bar keep a window menu (Alt+Space: restore,
move, size, minimise, maximise, close, and Reload interface), and Win+arrow keys work as for any window.

**UR6. A quit prompt of the app's own.** Quitting with unsaved changes asks in the page first. When the page doesn't
say within 2 seconds that it shows the prompt (`project.confirmShown`), or quitting is asked again while it should be
up, the app asks with a native Save / Don't save / Cancel box.

**UR7. Bounds that really change after a restore (2026-11-12).** A maximised window that is minimised and restored
comes back at the same size, so its web view gets the same bounds; WebView2 then showed nothing but the window's accent
blue until the window was resized, every time (UR4's hiding and showing didn't help, and it covered only the main
window). Every web view window is now made a pixel shorter after it is restored, and back 60 ms later, four times: after 0.1,
0.4, 1 and 2.5 s. Moving the web view inside the window was enough after a short minimise but not after half a minute
minimised, when a window maximised from the taskbar stayed blue 4 times out of 4: only the window changing size made
Windows compose it anew, and WebView2 takes longer to wake the longer it was hidden.
Found by opening and closing the windows of a copy of a project at random while checking the screen for the accent
colour; the sequence on its own (maximise, minimise, restore) then turned the Mixer blue 12 times out of 12, and a
resize brought it back each time.

## Notes from building it

- JUCE turns WebView2's DevTools off in release builds and doesn't expose its process failure events, so UR2 uses the
  remote debugging port, which works regardless, and UR3 notices a dead or hung page by the heartbeat instead.
- Not seen yet: whether UR4's nudge brings back a web view in the state of 2026-11-10; the log and UR2 are there to
  find out next time.
