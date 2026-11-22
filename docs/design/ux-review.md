# UX and workflow review

A review of DAWg's workflows for reasonableness and discoverability before release (phase 15), 2026-11-03. It is a list of findings, not a design: each one names what's wrong, where, why it matters, a suggested fix and a severity.

**Method.** Reading `ui/src` (shortcuts, menus, editing hooks, the inspector, the dock, the browser, the dialogs) and the design notes, then walking the main flows in the running app over CDP: the welcome screen, a new project, adding tracks and clips, the graph view, the piano roll and mixer windows, export, and closing with unsaved changes. Conventions are compared with Ableton Live, Bitwig Studio, Reaper and Cubase.

**Not covered.** Automation recording, sidechain set-up, Faust editing, MIDI learn, recording audio and MPE input, and MIDI file import were read in code at most, not walked. Neither was a first run with no audio device or no plugins scanned. These deserve a hands-on pass, ideally by someone who hasn't used DAWg before.

**Follow-up, 2026-11-03.** Done after the review, on the owner's call:
- U1: the Empty template, which New project also uses, is now empty: no tracks, only the master. More starter templates are still to come.
- U5: browser items drop onto the device strip (in a gap, on a card or anywhere in it), onto a track's header or lane, and below the tracks for a new track (instrument track for an instrument, audio track for an effect). A double-click adds an item to the selected track.
- U6 and U7: one device menu (`components/DeviceMenu.tsx`) in the inspector, the strip, the mixer and the graph view, and Delete in the strip. The inspector's sections stay for now.
- U8: the demo-clip button and `ui/src/demo.ts` are gone.
- U12: Alt+drag on empty arrangement space creates a clip and sets its length, as Alt+drag does for a note. Alt stays the way to create. Double-click to create was not added, and the Linux window-manager conflict is left for the Linux builds.
- U17: plain Up and Down transpose in the piano roll (Shift: an octave), and Ctrl+Up and Down select the note above or below.

Second round, the rest of the should-fix findings:
- U1: three starter templates beside Empty, built in code (`ProjectManager.cpp`): MPE synth (with a Loop Reverb return fed at −12 dB), Drums (the Sampler in drum mode) and Audio recording (two audio tracks).
- U7: tried, then undone on the owner's call: the inspector keeps the instrument and inserts whether the dock is shown or not.
- U9: deleting a saved component asks first.
- U12: a double-click on empty space creates a one-bar clip on an instrument track, and a note in the piano roll (dragging on sets its length).
- U12, Linux: every Alt gesture has a way without Alt. The piano roll's note menu gained Add pitch point here and Link to the next note (or, with several selected, Link the selected notes in a row). On Linux, `app/src/DesktopCheck.cpp` reads the window manager's window-move key once (kreadconfig for KDE Plasma, xfconf-query for Xfce, gsettings for Cinnamon, MATE, Budgie and GNOME); when it's Alt, the UI says once what to use instead and where to change the key. A setting to use another modifier for DAWg's gestures waits until testing on Linux shows it's needed. The check hasn't run on Linux yet.
- U13: S splits the selected clips, or the selected notes, at the playhead in both editors; Alt+click splits at the pointer.
- U23: window-wide shortcuts ignore key presses in text fields, dropdowns and editable text, and while a dialog is open (`ui/src/keys.ts`), in the main, mixer and piano roll windows.
- U24: an Edit menu (undo, redo, and the centre view's cut, copy, paste, duplicate, delete, select all and its own commands, sent to the view as their shortcuts) and a Help menu with a keyboard and mouse reference built from one table (`components/shortcuts.ts`), which the graph's help popover shares.

**Severities.** *Blocker*: should not ship in 1.0 like this. *Should fix*: a real cost to new or regular users. *Polish*: worth doing when nearby.

Third round, the polish findings, and two requests from the owner (2026-11-03):
- Dragging between the ends of an active loop range in either ruler moves the whole range; the ends still resize it.
- A clip is resized from the piano roll by the handles at its ends along the bottom of the roll's ruler.
- U2: the default inserts stay as channel-strip.md (C2) decided; a bypassed device now looks off (dimmed, its power switch off) rather than struck through. Whether new tracks get them is still the owner's question.
- U3: welcome-screen dates are in English. U4: the welcome screen shows no transport, tempo or signature.
- U10: the header's × is gone; the header menu has Rename… and the colours. U11: the timing offset comes after the sends. U16: Delete on the track headers removes the selected tracks (a group asks first).
- U14: the arrangement's editing keys, pressed with nothing focused, reached the timeline; undone on the owner's call. U15: a marker's right-click opens a menu (Rename…, Delete).
- U19: the piano roll window has the shared shortcuts too: L, R, Insert and Ctrl+M besides undo, redo, save and Space (`sharedShortcut` in `ui/src/keys.ts`).
- U20: the roll's vibrato controls are labelled. U22: a help button in the arrangement's and the roll's toolbars opens their part of the reference.
- U21: the graph opens at a readable zoom (60 % to 100 %); Fit graph still shows everything, at most at 100 %.
- U25: F6 and Shift+F6 move the focus between the panels; on a control reached that way, Tab moves on to the next control instead of switching the view.
- U26: the play button shows pause while playing. U27: the top bar's signature opens the signature dialog.
- U28: a new error also shows as a toast that fades after six seconds.

From the owner's testing, second batch: no help text in the panels, only tooltips (the arrangement's toolbar hint, the EQ's and the sampler's, the empty arrangement's and others went); the keyboard and mouse reference shows one area at a time, mouse and keys apart, in short lines, and the editors' help buttons (the graph's too) open it at their area; U14 is undone, so the arrangement's keys act only while the timeline has the focus; a new clip inside an active loop range fills the range; markers are selected by a click and removed with Delete; a double-click adds a point in the automation, expression and controller lanes and a pitch point on a note; the clip colour menu's "Use track colour" is a checkbox.

From the owner's testing: every right-click menu has icons (`components/MenuItem.tsx`); only Delete removes a device or a track, never Backspace; a device card's controls start below its selection line; the strip's route line is taller, so a cable's arrowhead sits on a straight stem rather than in its bend.

## 1. First launch and new projects

**U1. The only template is called Empty, and it isn't.** *Should fix.* Welcome screen and `Commands.cpp` (a new track's chain). "Empty" makes a project with one instrument track holding an MPE Synth, an EQ and a bypassed compressor, the same as New project. A user who wants an empty project can't get one, and a user who reads the name is surprised. Fix: rename it (Basic, or Instrument track), and either make New project truly empty or say what it holds. Ship two or three starter templates as well (an MPE instrument, a drum part with the Sampler, an audio recording set-up), since the template list is the first thing the welcome screen offers.

**U2. Every new track gets an EQ and a bypassed compressor.** *Polish, open question.* `Commands.cpp:1035`. Ableton, Bitwig and Reaper start tracks empty. The default inserts cost a strip of dock space on every track, and the bypassed compressor shows as struck-through text in the inspector and mixer and as a dimmed body in the strip, which reads as broken rather than off. If the defaults stay, add them active or not at all. A half-on default needs explaining.

**U3. Dates on the welcome screen are in the system's locale.** *Polish.* `WelcomeScreen.tsx:19`. The UI is English, but recent projects show "28. Okt. 2026" on a German system. Either format in English to match the UI, or plan for localisation as a whole.

**U4. The top bar shows a disabled transport, BPM and signature on the welcome screen.** *Polish.* `TopBar.tsx:88`. They're inert, but they show values (120 BPM, 4/4) from no project. Hide them there.

## 2. Tracks, instruments and plugins

**U5. Plugins and built-ins drag only onto the graph canvas.** *Should fix.* `PluginBrowser.tsx:10`. The browser's rows are draggable, but only the graph view accepts them: not the arrangement (where Ableton and Bitwig make a new track for a dropped instrument), not a track header, not the device strip, not the mixer's insert slots or the inspector's chain. A double-click on a row does nothing either. Most users' first instinct, dragging a synth from the browser into the song, does nothing and gives no hint why. Fix: accept browser drops on the strip (in a gap, as the insert + does), on a track header or lane (instrument to the instrument slot, effect to the end), and on empty arrangement space (a new track). Make a double-click add to the selected track.

**U6. The device strip can't remove or replace a device.** *Should fix.* `DeviceStrip.tsx`. Since step 5 the strip can add, reorder and bypass, but removing or replacing a device needs the inspector, the mixer or the graph. There's no context menu and no Delete key on a selected card. A crashed plugin's card has no Reload button, which the inspector has (`TrackInspector.tsx:89`). Fix: give a card the same context menu as an inspector slot (Replace, Bypass, Remove, MIDI/MPE settings, Convert to voice graph, Reload), and Delete for a selected card.

**U7. Three places edit the same chain, each with a different subset of actions.** *Should fix.* The inspector (instrument and inserts: replace, move, bypass, remove, MPE and pitch-bend settings), the device strip (add between, reorder, bypass) and the mixer's inserts (add, reorder, replace, remove). A user learns one place and finds actions missing in another. Fix: one shared chain menu used by all three. Once the strip has it (U6), the inspector's instrument and inserts sections duplicate the dock right under them and could shrink to a summary, or go.

**U8. A developer button in the inspector header.** *Should fix.* `Inspector.tsx:40`. "Add a demo MPE clip" (a music-note icon) sits beside the panel's hide button on every instrument and audio track. Remove it, or move it behind `--dev`.

**U9. Deleting a saved component has no confirmation and no undo.** *Should fix.* `PluginBrowser.tsx:93`. The small × on a saved component's row deletes its file at once. Everything else in DAWg is undoable, so users will expect this to be too. Ask first.

**U10. The track header's × sits right beside record arm.** *Polish.* `TrackHeaders.tsx`. A mis-click on arm removes the track. It's undoable, but a destructive button this small belongs in the context menu (which already has Remove track) or needs more space. The context menu, for its part, lacks Rename and Colour, which are only on the name (double-click) and the colour swatch.

**U11. The inspector puts the timing offset first.** *Polish.* `Inspector.tsx:56`. It's the first thing under the track name on every track, above the instrument and inserts, though few tracks ever need one. Move it to the end, or fold it.

## 3. Arrangement

**U12. Clips and notes are created only with Alt+click.** *Should fix.* `useArrangementEditing.ts:109`, `useRollEditing.ts:59`. A double-click on empty lane or roll space does nothing (checked in the app). Ableton and Bitwig create a clip or note on double-click, FL and Reaper on a plain click or drag. The toolbar hint mentions Alt+click but is cut off at common widths. On Linux many window managers (KDE, Xfce) take Alt+drag to move the window, which would take note entry and Alt+drag copying with it. Fix: double-click on empty space creates a clip in the arrangement and a note in the roll. Keep Alt+click as well. Check Alt on the Linux builds.

**U13. S splits at the pointer, the clip menu says "Split at playhead".** *Should fix.* `useArrangementEditing.ts:452`, `ClipMenu.tsx:124`. The key and the menu item do different things under one name. Reaper's S splits at the edit cursor, and Ableton's Ctrl+E at the selection. A split at the pointer is already Alt+click. Fix: make S split at the playhead in both the arrangement and the roll (where it also splits at the pointer), or label each by what it does.

**U14. Arrangement shortcuts work only while the timeline canvas has focus.** *Polish.* `Arrangement.tsx` (`onKeyDown` on the canvas). After clicking a track header, a clip's inspector or the ruler, Delete, Ctrl+C and Ctrl+D do nothing with the selected clips. Fix: route them from the window while the arrangement is the centre view and no text field has focus, as the global shortcuts are.

**U15. A right-click on a marker deletes it.** *Polish.* `Arrangement.tsx:568`. Everywhere else a right-click opens a menu. Give markers a menu (Rename, Delete), and keep the double-click rename.

**U16. A selected track can't be deleted with Delete.** *Polish.* Delete acts on clips only. Ableton and Bitwig delete the selected tracks when the headers have focus.

## 4. Piano roll

**U17. Plain Up and Down select a note instead of transposing.** *Should fix.* `useRollEditing.ts:978`. Ableton, Bitwig, FL, Reaper and Cubase all transpose the selected notes with the plain arrows, with Shift for an octave. In DAWg they move the selection to the note above or below, and transposing needs Ctrl. Muscle memory from any other DAW does the wrong thing, silently. Fix: plain Up/Down transposes (Shift: octave). Keep Left/Right selecting (as Cubase does), and move "select above/below" to Alt or Ctrl.

**U18. The piano roll is only a separate window.** *Open question.* `TopBar.tsx:187`. On a single laptop screen the roll window covers the main one, and the two have to be arranged by hand. Ableton and Bitwig show the editor in the main window's lower half, with detaching as an option. Consider a docked roll (in the dock's place, or the centre view) with the window as an option.

**U19. The roll window's shortcuts are a subset of the main window's.** *Polish.* `PianoRollApp.tsx:10`. Undo, redo, save and Space work there, but L (loop), R (record), Ctrl+E, Ctrl+M and Ctrl+W don't. Share one shortcut table between the windows, and leave out only what makes no sense in the roll.

**U20. The roll's toolbar shows the vibrato controls with no label.** *Polish.* `PianoRoll.tsx:479`. Depth, Rate, Shape, Delay and Fade appear after an icon, and it isn't clear that they edit the selected notes' vibrato. A label ("Vibrato of the selected notes") or a popover would help, and so would leaving them out when nothing is selected.

## 5. Graph view and components

**U21. Fit graph zooms a small graph down to unreadable nodes.** *Polish.* `GraphView.tsx:722`. A new track's six-node chain fits to about a third of normal size (screenshot during the walk), because the fit has no zoom ceiling. Cap the fit at 100 %.

**U22. The graph's help popover is good. The other views have nothing like it.** *Polish.* `GraphView.tsx:770`. The arrangement has a one-line hint that gets cut off, and the roll has nothing, though both have more hidden gestures than the graph. See U24.

## 6. Shortcuts, menus and discoverability

**U23. Global single-key shortcuts fire while a dropdown has focus.** *Should fix.* `App.tsx:31`. The guard skips only `INPUT` and `TEXTAREA`. With a `<select>` focused (the Snap menu, a preset or the export range), L toggles the loop (checked in the app), R starts recording and Tab switches the view. Typing a letter in a focused select is how users jump to an option, so R can start a recording by surprise. Fix: also skip `SELECT`, `contenteditable` and elements inside a modal, in both windows.

**U24. There's no Edit menu, no Help menu and no list of shortcuts.** *Should fix.* `TopBar.tsx`. File and View are the only menus. Undo and redo are buttons. Cut, copy, paste, duplicate, delete, select all, split, glue, quantise and humanise exist only as keys or context menu items, and the many modifier gestures (Alt+click, Alt+drag, Ctrl+Alt+drag linking, Shift+Alt+click pitch points, Ctrl+Shift+drag velocity) are documented only in code comments. Fix: an Edit menu acting on the focused view's selection, with each item's shortcut, and a Help menu with a keyboard and mouse reference built from one table (which U19 can share).

**U25. Tab is taken by the view switch.** *Polish.* `App.tsx:76`. Tab never moves the focus between controls, so the UI can't be used from the keyboard at all. Ableton uses Tab for this too, so the binding is defensible, but then Ctrl+Tab or F6 should move the focus between panels.

**U26. The play button never shows pause.** *Polish.* `TopBar.tsx:119`. Its tooltip says "Play / pause", but the icon stays a play triangle while playing (it only turns active). Show a pause icon while playing, or make it Play only, as in Ableton.

**U27. The top bar's time signature edits only the numerator, by dragging.** *Polish.* `TopBar.tsx:155`. Dragging a number for a signature is unusual, and the denominator can be changed only in the signature strip. A click that opens the signature strip's dialog would be clearer.

## 7. Errors and feedback

**U28. Errors appear only in the status bar.** *Polish.* `App.tsx:184`. A failed plugin load, a Faust compile error or invalid audio is a small red text in the bottom-right corner that stays until clicked. It's easy to miss, and an old error is easy to mistake for a new one. Fix: a toast for new errors that fades, with the status bar keeping a history.

## Top 10

1. U5: drag plugins from the browser onto tracks, the strip and the arrangement, and double-click to add.
2. U12: double-click creates clips and notes, and check Alt on Linux.
3. U17: plain Up/Down transposes in the roll.
4. U23: single-key shortcuts don't fire in dropdowns (R starts a recording).
5. U6 and U7: one chain menu, with Remove and Replace in the strip.
6. U24: an Edit menu and a keyboard and mouse reference.
7. U1: real templates, and an Empty that is empty.
8. U8: remove the demo-clip button.
9. U13: one meaning for S, in both editors.
10. U9: confirm before deleting a saved component.

## Open questions

- Should new tracks start empty (U2), or with default inserts, and if so, active?
- Should the piano roll be dockable in the main window, with the separate window as an option (U18)?
- Once the strip has a full menu, what is the inspector for: track settings, sends and the clip only, or still a second chain editor (U7)?
- Should DAWg keep Cubase's editing scheme (Alt+click to create, Alt+click to split), or move to the Ableton and Bitwig one (double-click to create, Ctrl+E to split)? U12, U13 and U17 are each smaller if the answer is "both".
- Localisation: English only for 1.0, with dates and numbers in English too (U3)?
