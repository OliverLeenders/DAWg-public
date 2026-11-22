# Scale highlighting and folding in the piano roll

Status: accepted (owner, 2026-11-09: "Go with your recommendations"), built.

The piano roll shows every semitone the same way. With a key set, it should show which rows are in the scale, fold
away the rest, and let Scale Lock follow the same key. The scales are the ones Scale Lock already uses
(`engine/include/dawg/Scales.h`, copied in `ui/src/music/scales.ts`; docs/design/midi-effects.md, ME12).

## Decisions

**SH1. One key per project.** `Project::key` holds whether a key is set, its root (a pitch class, 0 = C) and its
scale (an index into `dawg::scales`). The `setProjectKey` command changes it, with undo. It is saved in the project
file and read with defaults (no key), so the file format version stays as it is. Key changes along the timeline (a
key lane beside the time signatures) are left for later; the model keeps one key so that lane can replace it.

**SH2. Set in the piano roll's toolbar.** A Key control beside the grid and quantise controls: Off, or a root and a
scale. It changes the project's key, so every clip and track shows the same key.

**SH3. Highlighting.** With a key set, the rows of keys in the scale are drawn lighter than the rows outside it, and
the root's rows are tinted with the accent colour. The black/white key shading gives way to this while a key is set
(the keyboard beside the roll keeps its black and white keys). The keyboard marks the root and the scale's keys. A
note outside the scale gets a small marker at its start, so stray notes are easy to find.

**SH4. Two fold modes.** The toolbar's Fold choice: Off, Scale (only the rows of keys in the scale) or Used (only the
pitches the edited notes use, as drum editors do). Scale folding needs a key; without one the choice offers only Off
and Used. While folded to the scale, a note outside it still shows: its pitch gets a row of its own, drawn as an
outside row (not lightened) between its neighbours, and the note carries the outside mark, so nothing disappears.
Rows are a list of pitches; the geometry maps a pitch to its row by that list, and a fractional pitch (a bend, a
glide, vibrato) is placed between the rows around it, so pitch curves stay continuous.

**SH5. Snapping to the scale.** While folded, drawing and dragging notes land on the shown rows only, so on scale
notes (Scale) or used pitches (Used). Unfolded, a Snap to scale toggle (off by default, needs a key) moves drawn and
dragged notes to the nearest key in the scale, ties going down, as Scale Lock's Nearest mode does. Pitch curves are
never snapped.

**SH6. Scale Lock follows the project key.** Scale Lock gets a Follow choice: Own (its Root and Scale) or Project key,
which is the default for a new instance. Following, it uses the project's key when one is set and its own Root and
Scale when none is. The compiler hands the key to every node (`Node::setProjectKey`), at a compilation and when only
the key changed, which needs no recompile.

**SH7. View state.** Fold and Snap to scale are view settings of the piano roll window, kept per window like its
zoom, not in the project.

**SH8. Drum mode.** A drum track's piano roll keeps its drum map labels. Fold Used works there too; highlighting is
shown only when a key is set.

**SH9. Key changes along the timeline** (owner, 2026-11-09: "Address the 'not done yet' items"). The project's keys
are a list of key changes on bars, the first on bar 0, like the time signatures (`Project::keys`; `setKey` with a bar,
`moveKey`, `removeKey`). A key change can also turn the key off from its bar on.
- The arrangement has a Key strip under the signatures, used like it: double-click adds a change, click edits it, drag
  moves it to another bar, right-click removes it.
- The piano roll draws each stretch in its own key, with a line where the key changes. The outside mark and Snap to
  scale use the key where the note starts. Folding to the scale shows the rows of every key the song uses, so the rows
  stay put while scrolling. The toolbar's Key control and the keyboard's marks follow the key in effect at the
  playhead, and the control's label names the bar its key starts on when there are changes.
- Scale Lock follows the key in effect where the note plays (at the playhead while stopped). The compiler hands nodes
  the keys in ticks (`KeyTimeline`, `Node::setKeys`), so a tempo map change, which recompiles, moves them too.

**SH10. Up and Down in a folded roll** step the selected notes by one shown row; unfolded with Snap to scale, to the
next key in the scale. Shift+Up and Down are always an octave.

## Implementation order

1. Engine: `ProjectKey`, the command, the file, the compiler handing the key to nodes, Scale Lock's Follow choice;
   tests.
2. UI: the protocol, the toolbar's Key control, row highlighting, the keyboard's marks and the outside-note marker.
3. UI: rows as a list (folding), fractional pitches between rows, Snap to scale; tests.

## Notes from building it

- The view keeps the rows its top edge refers to (`View.rows`); on other rows (folding, unfolding, a Used fold whose
  pitches changed) it is converted keeping the pitch at the top edge (`withRows()` in `ui/src/pianoroll/geometry.ts`).
  Folded, `View.topPitch` is a row index.
- Folded rows come from the stored notes, not the drag preview, so a drag never reshapes the rows under it.
- Unfolded with Snap to scale, a dragged selection follows the grabbed note: it goes to the scale key nearest where it
  is dragged, and the others move by the same interval, each then moved into the scale.
- Checked in the app (a separate dev instance): highlighting and the outside mark in D minor, Scale and Used folds,
  and a note drawn in a Used fold landing on the row's pitch; then two keys (D minor, C major from bar 2) in the Key
  strip, each bar drawn and marked in its own key, the fold showing both scales' rows, and Up stepping D to E in it.
  Snap to scale and Scale Lock following were not tried by hand.
