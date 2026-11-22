# Design: the mixer view

Status: **implemented** (notes in section 6). The last part of phase 13. The decisions are numbered X1–X11; the
owner's answers are in section 5.

## 1. Goals and non-goals

Goals:

* See and adjust the whole mix at once: a channel strip per track, group, return and the master.
* Everything the arrangement and the dock already do, laid out side by side: inserts, sends (sidechain sends
  included), pan, mute, solo, arm, fader, meter.
* No new engine or model work: the mixer edits the same tracks with the same commands.

Non-goals (later):

* Routing a track's output to any bus (outputs follow group membership, as now).
* VCA faders, fader groups, channel linking.
* Plugin editors inside the strips (they open in their windows, built-ins in the dock).
* EQ curves and gain-reduction bars in insert slots (plain names for now).

## 2. What exists

* Groups, returns and the master are tracks; groups nest; returns sit in a fixed section (groups-and-sends.md).
* The dock's fader section (channel-strip.md, C7): pan, fader with meter and clip indicator, mute, solo, gain readout,
  with automation touch and played values.
* The inspector's chain and sends lists, and the channel strip's EQ and compressor panels.

## 3. Decisions

**(X1) The mixer is its own window.** One mixer window, opened and closed from a top-bar button and a shortcut, so it
can sit on a second screen next to the arrangement. It remembers its position and size (per user) and stays open
across project changes; closing the app closes it. It holds only the mixer: the device dock stays in the main window,
and selecting a strip or an insert in the mixer shows that track's channel strip or that device there (X6).

**(X11) Several UI windows.** The UI runs in a WebView; the mixer window gets a second one, loading the same UI bundle
with `?window=mixer`, which renders only the mixer.

* The app's `Bridge` serves several WebViews: model patches, telemetry, meters and project state go to all of them;
  each call is answered in the WebView that made it. Commands from either window go through the one Session, so
  undo, the history and the model stay single.
* The mixer window loads the model with `model.get` and follows patches like the main window. Opening a project
  re-sends the snapshot.
* Selection is UI state, so it is relayed: a `ui.select` call (tracks, the selected node) that the app re-emits as
  `dawg_selection` to the other windows.
* Global shortcuts work in the mixer window too: Space (play), Ctrl+Z / Ctrl+Shift+Z (undo, redo), Ctrl+S (save).
* A second WebView costs memory (another browser process, roughly 50-100 MB); it exists only while the window is
  open. The MIDI editor uses the same mechanism with `?window=roll`, so there can be three windows (docs/design/
  phase3-piano-roll.md).

**(X2) Strips follow the signal flow.** Left to right in the arrangement's order, except that a **group's strip comes
after its members** (they feed it), then a gap and the returns, then the master pinned at the right edge (always
visible while the strips scroll sideways). Nested groups do the same at every level. A group's members and its strip
are spanned by a bracket in the group's colour; folding the group (its ▾) hides its members' strips, and is the same
fold as in the arrangement.

**(X3) A strip, top to bottom:**

* **I/O**: the input for audio tracks (channel and monitoring), the output (its group, or Master) shown read-only.
* **Inserts**: the track's chain, one slot per node: a dot for on/off (click to bypass) and the node's name. Click shows
  it in the main window's dock, drag reorders, "+ insert" adds one (the same picker as the inspector), right-click
  removes or replaces.
* **Sends**: a list per strip, a small knob per send (drag for level, double-click for 0 dB), the return's name, on/off
  by clicking the name, pre/post in its context menu, "+ send" to add one. Sidechain sends in purple, named by the node
  they key.
* **Pan**: the dock's pan slider.
* **Mute, solo and arm** (arm for instrument and audio tracks; no switches on the master).
* **Fader and meter**: the dock's fader, scale and stereo meter with clip indicator.
* **Name and level**: the track's colour on top, its name (double-click to rename), the gain readout (drag or type).

**(X4) Sections fold.** Toggles in the mixer's toolbar show or hide I/O, Inserts and Sends, so the faders get the
height. Remembered per user.

**(X5) Wide and narrow strips.** Wide strips (about 96 px) show insert and send names; narrow ones (about 56 px) show
dots and knobs, with names in tooltips, to fit large projects. Remembered per user.

**(X6) One selection.** Clicking a strip selects its track in both windows (the arrangement, inspector and dock
follow, through X11's relay); Shift and Ctrl extend the selection, as in the track headers.

**(X7) Same commands, same behaviour.** Faders, pan and sends use `setTrackGain`, `setTrackPan` and `setSend` with
gestures, write automation in Touch and Latch modes, and show played values while automation plays. Inserts use
`setNodeBypass`, `reorderNode`, `insertNode` and `removeNode`. Undo works as everywhere.

**(X8) Shared parts.** The dock's fader section is split into a pan slider, a fader-with-meter and a gain readout,
used by both the dock and the strips, so they look and behave the same.

**(X9) Meters stay cheap.** Strips read the meters the engine already sends at 30 Hz (`telemetry.meters`). Insert slots
show plain names; EQ curves and gain-reduction bars in them are left for later.

**(X10) Keyboard.** Ctrl+M opens and closes the mixer (from either window); arrows move the selection between strips;
M and S mute and solo the selected strips.

## 4. Steps

1. App: the Bridge serving several WebViews, the mixer window (open, close, remembered bounds), the selection relay
   and global shortcuts (X1, X11).
2. UI: shared fader parts (X8), the mixer with strips in signal-flow order, groups, returns and master (X2, X3, X6,
   X7).
3. UI: the inserts and sends sections with their editing, section and width toggles, keyboard (X3–X5, X10).

## 5. Owner's answers

* **Placement:** its own window (X1, X11).
* **Group strips:** after their members, matching the signal flow (X2).
* **Sends:** a list per strip (X3).
* **Inserts:** plain names for now (X3, X9).

## 6. Implementation notes

* App: `Bridge` keeps a list of web views (`attach`, `detach`) and emits every event to each; a native call is
  answered through its own completion, so in the web view that made it. `MixerWindow` (MixerWindow.h) is a
  `DocumentWindow` holding the second web view, built from the same options (`MainComponent::browserOptions()`, one
  WebView2 user data folder, so one browser process) and loading `?window=mixer` (`pageUrl()`). Its bounds are
  stored as the window state under `mixerWindow` in the settings. `mixer.show` and `mixer.state` open, close and
  report it; `dawg_mixer` tells both windows. Closing it from its own web view is deferred, so the web view isn't
  destroyed inside the call.
* Selection: `ui.select` re-emits the payload as `dawg_selection`; each window relays its own selection changes
  (`startSelectionRelay()` in uiStore.ts) and applies the others', ignoring its own by `source`. The payload carries
  the dock tab, so clicking an insert in the mixer shows that device in the main window's dock, and the edited clip,
  so the detached MIDI editor follows the arrangement.
* UI: window.ts reads the window's name; App.tsx renders `MixerApp` in the mixer window. The mixer window shows no
  unsaved-changes prompts (`dawg_confirm` is the main window's), keeps its settings under their own storage key
  (mixerStore.ts) and never writes the main window's layout. Gesture ids carry the window's name.
* Strips (ui/src/mixer): `mixerLayout()` orders them (order.ts, with tests); `MixerStrip`, `MixerInserts` and
  `MixerSends`. The dock's fader section is split into `TrackSwitches`, `PanSlider`, `FaderWithMeter` and
  `GainReadout` with `useTrackValues()` (TrackFader.tsx). The inspector's `AddSendMenu`, `useSendName`,
  `useSendLevel` and the audio track's `InputSelect` and `MonitorSelect` are shared.
* An instrument track's instrument heads its inserts (not draggable). Inserts and sends sections have fixed heights
  and scroll, and every strip keeps room for the deepest brackets, so the faders line up; the master shows an empty
  sends section for the same reason.
* Found on the way: the effect picker had stopped offering the compressor once it gained its key input; chain
  inserts may now have a second, audio key input.
