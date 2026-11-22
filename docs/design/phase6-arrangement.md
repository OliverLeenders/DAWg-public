# Phase 6 design: arrangement view

Status: **accepted** (implementation notes in section 9). The model and engine decisions are numbered A1–A9 and summarised in section 8.
The UI decisions are in section 7.

## 1. Goals and non-goals

Goals:

* A timeline with one lane per track.
* Clips can be created, selected, moved (also to other tracks), resized from either edge, split, duplicated,
  looped, renamed, recoloured and deleted.
* Double-clicking a clip opens it in the piano roll, which then edits that clip.
* Markers, plus a loop range that the transport plays in a loop.
* The same quality bar as the piano roll: Canvas 2D rendering, snapping, zoom and scroll, undo for every edit.

Non-goals:

* Audio clips (phase 7). The design leaves room for them: a clip gets a kind.
* Automation lanes, folder tracks and track groups (phase 11 or later).
* Comping, take lanes, time stretching and tempo changes. The tempo stays a single value until a tempo map
  phase.

## 2. Clip model

Today a `NoteClip` has a `start`, a `length` and notes whose positions are relative to the clip start.
Notes starting at or after `length` are silent, and notes reaching past it are cut.

**(A1) Clips own their notes.** Recommended. Duplicating a clip copies its notes with new ids (cheap: the
note data is shared and immutable until edited).

Linked duplicates, where editing one edits all copies (Cubase "shared copies", Bitwig "clip aliases"), are
left for later. With immutable shared notes they would be easy to add as a `contentId` indirection.

**(A2) A content offset, so trimming the left edge doesn't move the notes.** Recommended. The clip gains
`offset` (ticks): the content position shown at the clip's left edge.

* A note sounds at `clip.start + note.start - offset`.
* It sounds only if `offset <= note.start < offset + length`. Notes reaching past the clip end are cut, as
  before.
* Dragging the left edge changes `start` and `offset` together, so the notes stay where they are on the
  timeline and are hidden or revealed.

This is how every DAW behaves. Today `resizeClip` with a `start` shifts the notes along with the edge.

**(A3) Clip looping.** Recommended. The clip gains `loopLength` (ticks; 0 means no loop).

* With a loop, the content range `[offset, offset + loopLength)` repeats until the clip ends. Notes outside
  that range are not played.
* Making a clip longer then repeats its content, like Ableton and Bitwig. Without a loop, making it longer
  adds empty space, like Cubase.
* The piano roll edits the content once and shows the repeats as ghost copies.

**(A4) Split is destructive.** Recommended. Splitting at a position creates two clips:

* Each note goes to the clip it starts in. Notes in the second clip are rebased to its start.
* A note crossing the split stays in the first clip and is cut at the split, as any note reaching past a
  clip's end is. It is not continued in the second clip.

A looped clip is first "unrolled" up to the split point, so both parts sound exactly as before.

A clip split doesn't split notes into two (Cubase's option). Notes are split in the piano roll instead
(docs/design/phase3-piano-roll.md, section 6), which also splits their curves.

**(A5) Clip names and colours.**

* `name` defaults to empty; the UI then shows the track name.
* `colour` defaults to empty, which means the track colour.

**(A6) A clip moved, resized, duplicated or created onto another clip replaces the part it covers.** Since takes-lanes.md (T1, T2), clips stack instead: the placed clip goes on top in a takes lane of its own, and what it covers stays whole underneath. This is
how Ableton and Bitwig behave, and the user chose it (section 7).

The edit command trims the covered clip, using A2 to trim from the left. If the new clip lands in the middle
of an old one, the old clip is split around it (A4). If it covers the old clip completely, the old clip is
removed.

This happens inside the same command, so it is one undo step, and the model never contains overlapping
clips. Audio clips (phase 7) follow the same rule.

New and changed commands:

* `moveClips`, `resizeClip`, `deleteClips` and `duplicateClips` take selections and are one undo step.
* `splitClips { ids, at }`.
* `setClipLoop { id, loopLength }`.
* `setClipProps { id, name?, colour? }`.
* `glueClips { ids }`: merges clips on the same track into one.
* `resizeClip { id, edge: "start" | "end", to }` keeps the content in place (A2).
* `moveClips { ids, delta, trackDelta }` moves clips by an offset, also across tracks, keeping relative
  positions.

## 3. Sequencing changes

`TrackSequence` flattens clips with offset and loops into absolute note instances.

**(A7) Runtime ids for looped repeats.** A looped note plays several times, but `CompiledNote::id` must be
unique per sounding instance. Note-offs, chokes and re-resolving notes after an edit all look notes up by
it. Each repeat therefore gets the id `noteId + repetition * 2^40` (repetition 0 keeps the note's own id; see section 9).

* This fits easily in 64 bits. At least one beat per repeat over the 10 000-bar timeline is fewer than
  2^20 repeats.
* Re-resolving a sounding note after an edit keeps working, because the same note and repeat map to the
  same id.
* Audition ids stay negative.

Expression curves and vibrato are evaluated relative to each repeat's own start.

## 4. Markers and loop range

**(A8) Project-level markers and loop range.**

* `Project` gains:
  * `markers: [{ id, position, name }]`, kept sorted;
  * `loop: { enabled, start, end }`.
* Commands: `addMarker`, `moveMarker`, `renameMarker`, `removeMarker`, `setLoop { enabled?, start?, end? }`.
* Markers are keyed by id in JSON (D3).

**(A9) Sample-accurate transport looping.** Recommended.

* While playing with the loop enabled, a block that crosses `loop.end` is split. The part up to the end is
  rendered, then the time map is re-anchored at `loop.start` and the rest is rendered.
* At the wrap:
  1. Notes still sounding are released with normal note-offs, not choked, so the release tails ring
     naturally.
  2. Notes overlapping `loop.start` are chased.
* A new transport flag `looped` (next to `jumped`) tells the sequencer and the MPE encoder about the wrap.
* Playing from outside the loop runs into it and then stays in it.
* Stop still returns to where playback started (phase 3).
* The metronome follows the wrap. The playhead in the UI jumps back.

Seeking inside the loop and editing the loop while playing take effect at the next block.

## 5. Piano roll changes

Since this was written the piano roll has moved to its own window (PianoRollWindow); double-clicking a clip opens or
fronts that window.

The roll gets an "edited clip". Double-clicking a clip in the arrangement sets it, and the roll scrolls to
it.

* New notes go into the edited clip. Its length grows if a note ends past it. This replaces phase 3's rule of
  extending the nearest clip.
* Looped clips show their repeats as ghost notes that can't be edited.
* How other clips of the track appear (hidden, ghosted or editable) is a UI question (section 7).
* The clips' extents run along the bottom of the roll's ruler, with a handle at each end. Dragging a handle resizes
  the clip as dragging its edge does in the arrangement (on the grid, Ctrl/Cmd off it; the start edge keeps the
  content in place), the edited clip's handles first where two meet.

## 6. Rendering and interaction (technical)

* The arrangement is its own Canvas 2D component, sharing the time-axis code (ruler, grid and snap) with the
  piano roll. The shared code moves from `pianoroll/geometry.ts` into `ui/src/timeline/`.
* **Clip drawing:** a body in the clip's colour, a name bar, and a miniature of its notes (pitch range
  fitted to the clip height, curves left out). Looped repeats get a notch at each loop boundary.
* **Lanes** are virtualised: only visible tracks are drawn. Track height is per track and stored in the
  project (`Track::height`, command `setTrackHeight`), as decided in section 7.
* **Snapping:** to the grid of the current zoom (bars, beats, 1/16). Ctrl/Cmd places anything off the grid, as
  in the roll: clips, the playhead and loop range on the ruler, markers, tempo points and automation points.
  Shift locks a clip move to one axis. Only the S key's split keeps Shift for off the grid, since Ctrl+S saves.
* **Following:** the view follows the playhead in the same way as the piano roll.
* **Playhead overlay:** the lanes, the master lane and the ruler each keep a transparent `PlayheadOverlay` canvas
  (`ui/src/components/PlayheadOverlay.tsx`) above their content. It subscribes to the telemetry position, so
  playback repaints only the playhead line (and the ruler's triangle), not every clip and waveform. The played
  automation values are painted on the overlay too. Recording is the exception: the take grows with the playhead,
  so the lanes still redraw while recording.

## 7. UI decisions (answered by the user)

1. **Layout: switch views.** The centre shows either the arrangement or the piano roll, each at full
   height.
   * Double-clicking a clip switches to the roll with that clip.
   * A view switch in the top bar, plus the Tab key, toggles back.
   * Each view keeps its own scroll and zoom.
2. **Track headers: in the arrangement.** Each lane has a header aligned with it: colour, name and gain,
   with mute and solo later.
   * The left panel keeps only the selected track's inspector: instrument and inserts, from phase 4.
   * Adding tracks moves to the arrangement's header area.
3. **Editing: the same Cubase-style scheme as the piano roll.**
   * Click selects (Shift toggles).
   * Drag moves; Ctrl/Cmd places the clip off the grid, Shift locks the direction, and dragging vertically moves
     the clip to another track.
   * The edges resize.
   * Dragging on an empty area box-selects.
   * Alt+click on an empty area creates a clip one bar long from the bar under the pointer; dragging on to the right
     sets its length on the grid instead (Ctrl/Cmd: off the grid), as Alt+drag does for a note in the piano roll. A double-click on
     empty space of an instrument track also makes a one-bar clip. Inside an active loop range, a new clip fills the
     range instead of one bar.
   * Alt+drag copies.
   * Double-click opens the clip in the roll.
   * Ctrl+Shift+D (or the track header's right-click menu) duplicates the selected track, right below it.
   * Ctrl+G groups the selected tracks and Ctrl+Shift+G ungroups the selected group (docs/design/groups-and-sends.md);
     Ctrl+click and Shift+click select several tracks in the headers, and dragging headers reorders them.
   * Delete removes, Ctrl+D duplicates after the selection, Ctrl+C/X/V paste at the playhead on the
     selected track.
   * Deleting (Delete or the clip menu) selects, on each track that lost clips, the first clip after the last deleted
     one in time, else the one before it, else nothing on that track (`ui/src/arrangement/clipSelection.ts`). Cut
     (Ctrl+X) does not change the selection.
   * S splits the selected clips at the playhead (as the clip menu's Split at playhead), Alt+click on a clip at the
     pointer; Ctrl+J glues.
4. **Overlap: the covered part is replaced** (A6).
5. **Piano roll scope: the other clips are ghosted.** Only the opened clip can be edited. The track's other
   clips are drawn dimmed as context; clicking a ghost note switches to that note's clip. Clip boundaries are
   shown.
6. **Looping a clip: a corner handle.**
   * A handle at the clip's top-right corner extends the clip by repeating its content (Bitwig style). This
     sets `loopLength` to the content length the first time.
   * The normal edge drag only resizes.
7. **Loop range and markers: ruler zones and a marker row.**
   * The ruler is split. Dragging in its upper half sets the loop range, dragging an end of an active range
     moves that end, dragging between the ends moves the whole range (its start on the grid, its length kept),
     and clicking there toggles the loop. Clicking or dragging in the lower half seeks, as today.
   * A thin marker row sits below the ruler:
     * double-click adds a marker;
     * drag moves it;
     * double-clicking its name renames it;
     * right-click removes it.
   * Shortcuts: L toggles the loop, Insert adds a marker at the playhead.
8. **Track height: per track, saved in the project.**
   * Dragging the bottom edge of a track header resizes that lane.
   * Alt+wheel over the lanes scales all heights.
9. **Global mute and solo (added after phase 12, Cubase-style).** The track list's header, next to "+ Track", and
   the mixer window's toolbar have an M and an S switch.
   * Each lights while any track is muted (or soloed), and its tooltip counts them.
   * A click turns that switch off on every track, as one undo step ("Unmute all tracks", "Unsolo all tracks"),
     and remembers those tracks.
   * While none is on, a click turns it back on for the remembered tracks that still exist ("Restore mutes",
     "Restore solos"). The switch shows this with a coloured outline. Clearing again replaces what is remembered.
   * Each window remembers its own last clear, and forgets it when another project opens.
   * They are dimmed when there is nothing to clear or restore, and they stay visible in the graph view.

## 8. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| A1 | Clips own their notes; duplicates copy (linked clips later) | Yes |
| A2 | Content offset; trimming the left edge hides notes instead of moving them | Yes |
| A3 | Clip looping with `loopLength`; repeats are rendered and shown as ghosts | Yes |
| A4 | Destructive split: notes go to the clip they start in; crossing notes are cut | Yes |
| A5 | Optional clip name and colour, defaulting to the track's | Yes |
| A6 | Clips placed onto others replace the covered part (trim, split or remove), in the same undo step | Yes (user's choice) |
| A7 | Unique runtime ids for looped repeats: `noteId + repetition * 2^40` | Yes |
| A8 | Markers and a loop range in the project model, with commands | Yes |
| A9 | Sample-accurate transport loop; note-offs at the wrap, chase at the loop start | Yes |
| A10 | Track height stored in the project (`Track::height`) | Yes (user's choice) |

---

## 9. Implementation notes (phase 6 as built)

* **Runtime ids (A7)** are `noteId + repetition * 2^40` rather than `noteId * 2^20 + repetition`. A note in
  a clip without a loop then keeps its own id, so nothing that already identifies notes by model id needed to
  change. `runtimeNoteId()` is in `engine/include/dawg/Sequence.h`.
* **Clip model.** `NoteClip` gained `offset`, `loopStart`, `loopLength`, `name` and `colour`.
  * A looped clip plays from `offset` to the loop end, then repeats `[loopStart, loopStart + loopLength)`
    (Ableton-style start and loop start).
  * The first drag of the corner handle sets `loopStart = offset` and `loopLength = length`.
  * The minimum loop length is a 1/16 note.
  * `forEachAudibleNote()` (`Model.h`) defines what a clip plays. The sequencer, glue, and the UI
    (`ui/src/timeline/clips.ts`) all follow it.
* **Splitting (A4)** is destructive for clips without a loop: the notes are partitioned and the right part
  gets the matching `offset`, so no positions are rebased. A looped clip is split into two looped copies
  (the right one with new note ids), so both sound exactly as before.
* **Overlap replacement (A6)** is `clearRange()` in `engine/src/Commands.cpp`, used by `addClip`,
  `moveClips`, `resizeClip`, `duplicateClips` and `glueClips`.
* **Transport loop (A9).** `AudioEngine::renderBlock()` ends a chunk exactly at the loop end and re-anchors
  the time map at the loop start. The new `Transport::looped` flag makes the sequencer release its notes and
  chase. Tracks and the master running ahead for latency compensation or a negative timing offset (phase 13)
  wrap early: `aheadOf()` splits the chunk at the loop end and renders the second half from the loop start
  `split` samples into the same output buffer. A lead longer than the loop maps back into the current iteration
  with a modulo, so every iteration enters at the same content position. Live events only apply to the first
  half; the second half replays the loop's own events.
* **UI.**
  * The arrangement is in `ui/src/arrangement/`. The centre switches between it and the piano roll (top bar
    buttons or Tab).
  * The left panel is now the selected track's inspector; the old track list is gone.
  * Clip copy and paste carries the full clip (including offset and loop) through `addClip`, so pasting
    works after a cut.
  * L toggles the loop and Insert adds a marker at the playhead.
  * The loop zone in the ruler works in the piano roll too. Markers are shown there but edited only in the
    arrangement's marker row.

### Follow-up: clip menu, rename, colours, linked clips (approved by the user)

* **Clip context menu.** Right-clicking a clip selects it if it isn't already, and the menu acts on the
  selection:
  * Open in piano roll (Enter);
  * Rename… (F2);
  * the colour palette;
  * Loop clip (Ctrl+L): loops the current length, or removes the loop when every selected clip loops;
  * Unroll loop: `glueClips` with one clip;
  * Split at playhead;
  * Duplicate (Ctrl+D) and Linked duplicate (Ctrl+Alt+D; it was Ctrl+Shift+D, which now duplicates the selected
    track);
  * Make unique;
  * Glue (Ctrl+J);
  * Cut, Copy, Paste and Delete.

  Right-clicking an empty lane offers Paste here, New clip here and Add marker here.
* **Rename.** F2, or double-clicking the name bar, opens an inline field over the name bar. Enter or
  leaving the field commits; Esc cancels. With several clips selected, all of them are renamed.
* **Colours.**
  * A 12-swatch palette (`ui/src/colours.ts`; the engine's `trackColours()` uses the same order) plus "Use
    track colour".
  * Clicking a track header's colour strip opens the same palette (`setTrackColour`).
  * The palette sits inline in the menu rather than in a submenu.
* **Linked clips.**
  * Linked clips share a `linkId` and always hold the same notes, with the same note ids.
  * After `addNotes`, `updateNotes`, `removeNotes` or `setCurve`, `applyCommand` copies the edited clip's
    notes to its links. Notes are shared and immutable, so this costs only the vectors.
  * This replaces the `contentId` indirection from the proposal. It behaves the same, but the clip and patch
    structure stays unchanged, and the file format only gains an optional field instead of a version 2
    migration.
  * Runtime ids now number every placement of a note id per track (loop repeats and linked clips alike).
  * Ways to create them: Ctrl+Alt+D, the menu, or Alt+Shift+drag.
  * The name bar shows a link icon, and clips linked with the selection get a dashed outline.
  * `makeUnique` gives clips their own note copies; a link left with one member is dissolved.
  * Splitting a linked clip keeps the left part linked and makes the right part unique.
  * Glue and paste always create unique clips.
