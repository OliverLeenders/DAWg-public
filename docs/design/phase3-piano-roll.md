# Phase 3 design: MPE piano roll

Status: **implemented** (phase 3 complete). UI decisions were made by the project owner on 2026-10-17; anything marked
**(open)** still needs their input before it is built.

## 1. UI decisions (owner)

| Topic | Decision |
|---|---|
| Placement | Center panel, between the track list and the plugin browser. It can also pop out into its own closable window from the toolbar; that window follows the track and clip selected in the main window and remembers its bounds |
| Scope | The whole selected track: all its clips on one timeline, clip boundaries drawn as bands; drawing outside a clip creates or extends one |
| Expression display | Both: the pitch curve is drawn as the note's real pitch path in the roll, pressure as brightness along the note; pressure, timbre (and pitch) are edited precisely in lanes below for the selected notes |
| Tools | **Revised (Cubase Key Editor style, 2026-10-18)** — see "Editing scheme" below. Superseded: smart pointer where a plain click created notes |
| Curve input | Point by point: click to add a breakpoint, drag to move, double-click to delete, segment tension handles that move freely inside the segment rectangle (a double-click puts one back in the middle, a straight segment) |
| Note colour | Track colour, brightness by velocity; selected notes outlined |
| Keyboard | Piano keys on the left; clicking a key (and drawing or moving a note) auditions it through the track's instrument |
| Icons | Lucide (`lucide-react`) everywhere |
| Lanes | One lane area under the roll with tabs: Velocity / Pressure / Timbre / Pitch |
| Velocity | Velocity stems in the Velocity lane, and Ctrl/Cmd+Shift-drag vertically on a note |
| Playhead | Continuous scroll during playback (playhead held near the left third) |
| Creating notes | **Alt+click** on empty space creates a note with the last used length (initially one grid step); dragging right after creating resizes it, and its length becomes the new default |
| Dragging to the grid | Moving or resizing notes snaps the grabbed edge (the start when moving or dragging the start, the end when dragging the end) to absolute grid lines, so an off-grid note lands on the grid; the other selected notes travel by the same amount. A dragged end stops at the first grid line after the start, a dragged start at the last one before the end. Ctrl/Cmd drags freely (2026-11-15) |
| Notes outside clips | The nearest clip ending before the note is extended to the end of the note's bar; a new clip is created only if there is none before it |
| Box selection | Drag on empty space draws a selection box (Shift adds to the selection); Shift+click toggles a note. A note with a pitch curve is selected only once the box (with Shift, boxes) has taken all of its pitch points; until then the box selects the points inside it, which then show on the note (2026-10-29) |
| Pitch in the roll (revised after first use) | Notes with a pitch curve are drawn as a **ribbon** one row high that follows the pitch path; selected notes also show a faint outline at their base pitch. Selected notes show their pitch breakpoints and tension handles, which are dragged freely inside their segment rectangle; Shift+Alt+click on a note adds a point (changed from Alt+click, which now splits the note); dragging elsewhere on the note still moves it |
| Pitch snap | Pitch values snap to whole semitones; hold Shift while dragging for continuous values. The pitch lane draws a semitone grid. The Pitch lane tab stays |
| Lane area | Resizable with a divider above the lane tabs. In a lane, Alt+wheel zooms the value range around the pointer and the wheel scrolls it; double-click the lane label to reset |
| First curve point | Adding the first point to an empty curve also anchors the note start at the neutral value, so only the part after the point changes |
| Shortcuts | Delete/Backspace, Ctrl+A, Esc; Left and Right select the previous and next note, by start and then lowest pitch first; Ctrl/Cmd+Up and Down select the note above and below (nearest in time, a note sounding with it counting as no distance, then nearest in pitch); Up and Down transpose by a semitone (Shift: an octave) and Ctrl/Cmd+Left and Right move by grid, as in other DAWs, acting on the selected curve points (Up/Down: pitch points by a semitone, other lanes by 5 %, Shift 25 %) and the selected notes without selected points together, as one undo step; Alt+Left/Right moves note starts, Shift+Alt+Left/Right moves note ends; Alt+Up/Down velocity ±5%; Ctrl+C/X/V (paste at playhead), Ctrl+D duplicate; Q quantise starts, Shift+Q starts and lengths; S splits the selected notes at the playhead, as in the arrangement (Alt+click splits at the pointer) |

### Editing scheme (Cubase Key Editor style)

| Gesture | Action |
|---|---|
| Click note | Select it (Shift+click adds/toggles) |
| Drag note | Move the selection. Ctrl/Cmd places it off the grid; Shift locks it to time or pitch, whichever moved first |
| Drag note edges | Resize starts / ends |
| Drag empty space | Rubber-band selection: whole notes, and the pitch points of notes it covers only partly |
| Alt+click or double-click empty space | Create a note (last used length; keep dragging right to set the length) |
| Alt+drag note | Copy the selection |
| Alt+click note | Split it there (with the selected notes under the pointer; snapped) |
| Shift+Alt+click note | Add a pitch point there (snapped; drag it with Ctrl/Cmd for a free value) |
| S (or right-click → Split here) | Split the selected notes under the pointer, or the note under it, at the pointer (snapped; Shift free, since Ctrl+S saves) |
| Ctrl+Shift+drag note | Velocity (up/down) |
| Curve points (pitch in the roll, all curves in the lanes) | Click selects a point (Shift toggles); drag moves the selected points, across notes; double-click deletes; Alt+click or a double-click on empty lane space adds (in the roll: Shift+Alt+click or a double-click on a note); drag on empty lane space box-selects points; Delete removes them (and the selected notes without selected points); Up and Down and Ctrl/Cmd+Left and Right move them; Ctrl+C / Ctrl+X copy / cut them (from one note); Ctrl+V pastes them at the playhead into the selected notes that span it (else any note that does). Points snap to the time grid and pitch to semitones (Ctrl/Cmd for free values) and never leave their note |
| Ruler | Click or drag to move the playhead (snapped to the grid; Ctrl/Cmd free) |
| Stop | Stops playback and returns the playhead to where playback started; pressing Stop again goes to the project start |
| (removed 2026-10-18) | The double-click "expression focus" mode, replaced by Alt+click |
| Right-click | Context menu (vibrato presets now; more later) |

### Vibrato (parametric, per note)

Each note has vibrato settings that the engine adds on top of the pitch curve: **depth** (semitones, default 1,
never off),
**rate** (Hz, or tempo-synced cycle length), **shape** (sine, triangle, square, saw up, saw down),
**delay** and **fade-in** (beats). The **Vibrato** lane is the **intensity** (0–100 %, default 0): vibrato is
only heard where it is above zero, so it can swell and fade freely. The inspector's wave button turns full
intensity on or off; presets set both the settings and full intensity. Edited in an inspector strip above the lanes (applies to all selected notes), with a
depth handle (drag up/down) and a rate handle (drag left/right) on the first wave of a selected note in the
roll, and presets in the right-click menu. The roll draws the ribbon with the vibrato included. (MPE MIDI
output, phase 4, will apply it too.)

## 2. Technical approach

* **Rendering:** Canvas 2D inside React components, redrawn on demand (requestAnimationFrame when
  something changed) with viewport culling, so only visible notes and curve segments are drawn. React owns
  the chrome (toolbar, lane tabs). All drawing lives in `ui/src/pianoroll/draw.ts`, so a WebGL renderer
  could replace it later without touching interaction code. (Chosen over PixiJS: no extra dependency, and
  2D canvas is fast enough for thousands of notes.)
* **Coordinates:** time in ticks (as the model), pitch in semitones. A view transform (scroll, zoom) maps
  them to pixels. Snapping to a grid (1/4 … 1/32, triplets) happens in ticks.
* **Edits:** drags show a local preview and commit one command on release (`updateNotes`, `addNotes`,
  `setCurve`, or `batch` when several commands are needed), tagged with a gesture id so a drag is one undo
  step. The C++ model stays the source of truth.
* **Selection** is UI-only state (not undoable, not in the model).
* **Audition:** a new bridge method sends live note-on/off for a track. The audio engine receives them
  through a lock-free queue and the track's sequencer node emits them alongside clip notes. Live notes use
  ids that can never clash with model ids, and instruments don't try to re-resolve them when the sequence
  changes.

## 3. Build order

1. Audition path (engine + bridge) with tests.
2. Roll canvas: grid, ruler, keyboard, clip bands, notes from the selected track, scroll and zoom.
3. Note editing with the smart pointer: draw, select (click, rubber band), move, resize, delete,
   velocity (lane and Ctrl/Cmd+Shift-drag).
4. Expression: pitch paths and pressure shading on notes; lanes with point editing.
5. Transport playhead in the roll with continuous-scroll follow.

## 4. Open questions for the owner

Collected here and asked before the step that needs them. (None open right now.)

## 5. Implementation notes

* Code: `ui/src/pianoroll/` - `geometry.ts` (coordinates, curve evaluation), `draw.ts` (all rendering),
  `edit.ts` (edits to model commands), `curveEditing.ts` (point/handle editing), `useRollEditing.ts`
  (pointer state machine, preview, shortcuts), `PianoRoll.tsx` (layout), and `PianoRollApp.tsx` (the detached
  window). The app side is `PianoRollWindow` (`app/src/PianoRollWindow.h`), a `DocumentWindow` like the mixer's.
* The detached window loads `?window=roll`, optionally with `track` and `clip` query parameters. `editor.show` and
  `editor.state` open, close and report it; `dawg_editor` tells both windows, and the selection relay carries the
  edited clip so it follows the arrangement.
* Curve points are not snapped to the grid; pitch-curve values are continuous (semitone fractions).
* Every roll edit is a `batch` command carrying its own label, so the undo history reads "Move notes",
  "Quantise", "Edit expression" and so on.
* After creating, pasting or duplicating, the new notes become the selection; deleting clears it.
* Development builds expose `window.__dawg` (stores) and `window.__dawgView` for debugging and automated
  UI checks over the WebView2 DevTools protocol.
* Unit tests for the pure logic run with Vitest (`npm --prefix ui test`), also in CI.

## 6. Later changes

* **Curve anchors** (after phase 9, at the owner's request). The MPE lanes (pitch, pressure, timbre) of every note
  always have a point at the note start. The engine adds it (`anchorCurve`, in `addNotes`, `updateNotes`,
  `setCurve` and when files are read), with the value the curve already has there, so nothing sounds different.
* **No anchor at the note end** (later, at the owner's request: a point there sat on the note's resize edge and
  made resizing hard to hit). A curve holds its last value to the end of the note by itself, so the end has a
  point only when the user adds one; resizing a note moves such a point along. When a file is read, an end point
  that only holds the previous value (as the old end anchor did) is dropped.
* In the roll, the start anchor moves only vertically, other points stay after it, and deleting skips it. The
  vibrato lane has no anchor.
* **Splitting notes** (`splitNotes { clipId, ids, at }`; Alt+click, S or the context menu in the roll). The left part keeps
  the note's id; the right part is a new note with the same pitch and velocities. Every curve is cut at the split
  (`splitCurve`): the right part starts with the value the curve had there, and the left part gets a point at the
  split only where the curve was still changing. The vibrato carries on with what is left of its delay and
  fade-in. A cut power segment keeps its tension on both sides, so its bend is close to the original but not
  exact.
* **No browser context menu:** right-clicking anywhere shows only DAWg's own menus (`main.tsx` prevents the
  default `contextmenu`).
* **The playhead overlay.** The roll, the controller lane and the ruler no longer redraw when the playhead moves.
  Each has a transparent `PlayheadOverlay` canvas on top (`ui/src/components/PlayheadOverlay.tsx`) that subscribes
  to the telemetry position, so playback repaints a thin line (and the ruler's triangle) instead of every note,
  curve and clip. Follow mode still moves the view, which redraws the roll as before.
