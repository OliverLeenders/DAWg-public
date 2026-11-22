# Design: linked notes (phase 14)

Status: **built** (2026-11-01). The decisions are numbered L1–L13; section 0 holds what the owner decided, and overrides
the sections below where they differ.

## 0. As built

* **Linking is manual:** Ctrl+Alt+drag from one note to another, or a note's menu: Link to the next note, or Link the selected notes in a row. Nothing links by itself, and chords aren't paired up.
* **Glide time is in milliseconds** (`NoteLink { to, glideMs }`, 100 ms for a new link), set in the roll's lane bar with an
  Unlink button. There is no glide shape or handle: pitch moves linearly in semitones.
* **Pressure and timbre crossfade** over the glide time, from what the voice had at the hand-over into the new note's
  curves. **Velocity crossfades too.** The owner wasn't sure; a jump would click on the MPE Synth, where velocity scales
  the output, so levels take at least 5 ms even with no glide. The Sampler keeps the velocity its sample started with.
* **The glide starts from what the voice sounds like**, measured at the hand-over (`LinkGlide`), rather than computed from
  the notes: it includes vibrato and any glide still under way. So the compiler holds no glide offset, and a chase into a
  linked note starts it fresh without the rest of the glide. Playing from exactly a linked note's start plays the earlier
  note for no time, so the glide is heard.
* **Commands:** `setNoteLinks`, and `addNotes` takes `link: { toIndex, glideMs }`, so pasted notes keep links between each
  other; links to notes that weren't copied are dropped. Alt+drag copies and `updateNotes` don't take links. A linked
  note's ribbon in the roll follows its glide, and duplicating a clip keeps its links.
* **Plugins:** MPE keeps the channel and bends (a fresh note past the bend range); plain MIDI starts the new note just
  before ending the old one, and the plugin's portamento decides the glide.

## 1. Goals and non-goals

Goals:

* **Legato and glides between separate notes.** A link from note A to a later note B makes B continue A's voice
  instead of starting its own: the envelope doesn't restart, and the pitch glides from A to B over a set time and
  shape. Each note keeps its own start, length, base pitch and expression curves.
* The same behaviour everywhere a note can play: the MPE Synth, voice graphs (oscillators, ADSRs, LFOs, Note
  Expression, polyphonic Faust), the track graph's mono Note Expression, and hosted plugins over MPE or plain MIDI.
* The usual guarantees: sample-accurate hand-overs, chase into the middle of a linked chain, seek, loop, stealing,
  release tails, undo, copy and paste, project files.
* Linking is quick in the roll: select notes, press a key.

Non-goals for v1:

* Links across clips, and from the last note of a looped clip into its next repetition (L3).
* Recording links from MIDI input (a mono legato detector could add them later).
* Glides between notes of different tracks, or chords that glide as one (each note links to at most one note).
* CLAP note expressions and per-note glides for plugins; the CLAP research (docs/design/clap-hosting.md, when it
  exists) decides what phase 15 adds.

## 2. What exists

* A glide can be drawn today only as one note's pitch curve. That stays: a link is for when the two pitches should
  be separate notes (their own velocities, curves, lengths, editing).
* Phase 2 planned the field (`Note::glideTo: std::optional<ObjectId>`, phase2-engine.md, section 3.2) and said the
  voice allocation could hand a voice from one note to the next. The voice scope left links out of v1
  (phase12-voice-scope.md, section 1).
* `VoiceAllocator` (VoiceAllocator.h) owns note-to-voice bookkeeping for the MPE Synth and the Voice Instrument: a
  voice records `noteId`, the compiled `note`, `basePitch` and `velocity`, and `handle()` returns the index of a
  voice that must start afresh.
* The Sequencer node (BuiltinNodes.cpp) turns the `TrackSequence` into note-on and note-off events, chases notes
  that span the playhead on start, seek and loop, and releases everything on a loop wrap.
* `MpeEncoder` gives each note its own member channel with per-note pitch bend of ±48 semitones relative to the
  note-on key; plain MIDI mode (the default for plugin instruments since V5) sends everything on channel 1, no bends.
* The ADSR and LFO gate on the events they receive (`NoteGate`); in a voice graph each voice receives only its own
  note's events.

## 3. Model

**(L1) The link lives on the earlier note.**

```
Note { ..., link: std::optional<NoteLink> }
NoteLink { ObjectId to; Ticks glide; CurveShape shape; double tension; }
```

`to` is the note that continues this note's voice. `glide` is how long the pitch takes to reach the new note,
measured from the hand-over (L4); 0 is legato without a glide. `shape` and `tension` are the curve segment's
(`CurveShape::linear` or `power`, as in `Curve.h`), so the glide reuses the curve code and the roll's tension
handles. Storing the link on the earlier note matches phase 2's plan and keeps "what comes after this note" next to
the note whose voice is handed on.

**(L2) Links form chains, one to one.** A note links to at most one note, and at most one note links to it, so a
chain is a mono line: A → B → C. The commands keep this: linking a note that is already linked replaces its link,
and linking to a note that something else already links to moves that link.

**(L3) Both notes are in the same clip, and the link is played only when it is valid.** A link is valid when its
target exists in the same clip and starts after the source starts. Edits never delete a link because it became
invalid (moving B before A, say); the compiler skips invalid links and the roll draws them as broken, so moving
the note back restores the glide. Deleting the target removes the link (L11). In a looped clip a link plays within
each repetition; the last note doesn't link into the next one.

**(L4) The hand-over is at B's start.** With an overlap, A's voice moves to B when B starts (A's remaining length is
ignored while linked). With a gap, A's voice holds through it: A's curves keep their last values, as they do in a
release tail, and A's note-off is never sent. The chain ends with the last note's note-off and release velocity.

**(L5) The glide is an offset that fades out.** At the hand-over the voice's pitch is A's sounding pitch there
(base pitch + pitch curve, without vibrato). B's pitch at time t after its start is

```
pitchB(t) = B.basePitch + B.pitchCurve(t) + B.vibrato(t) + glideOffset · (1 − shape(t / glide))   for t < glide
glideOffset = A.basePitch + A.pitchCurve(B.start − A.start) − B.basePitch
```

so B's own curve and vibrato play through the glide, and after it B sounds exactly as it would unlinked. The offset
depends only on the sequence, so the compiler computes it once per link (`CompiledNote::glideOffset`), and chase can
evaluate a glide that is already under way.

**(L6) Only pitch glides; the rest follows B.** Pressure and timbre switch to B's curves at the hand-over. They are
usually drawn continuous by hand, and a jump is easy to see and fix in the lanes. Velocity: the voice keeps the
envelope it has, and B's velocity becomes the voice's velocity from the hand-over on (Note Expression's velocity
output and plugins' note data follow it), so a link can still accent without retriggering.

## 4. Engine

**(L7) A new note event, `legato`.** The Sequencer sends, at B's start, `{ legato, noteId: B, note: &B, fromNoteId:
A }` instead of a note-on, and leaves out A's note-off. Every consumer of note events handles it:

| Consumer | On `legato` |
|---|---|
| `VoiceAllocator` | Finds the voice playing A (held, or sustained) and moves it to B: `noteId`, `note`, `basePitch`, `velocity`; keeps its age and state. If no voice plays A (it was stolen, or the chain was chased from B), treats the event as a note-on. Returns which voice continued, so the owner can pass the event on to it. |
| MPE Synth | Keeps the voice's phase and envelope; the pitch follows `updateExpression()`, which now includes the glide (L5). |
| Voice Instrument | Passes `legato` to the voice's own event buffer instead of a note-on; does not call `reset()` on the voice's nodes. |
| ADSR, LFO (`NoteGate`) | Swap A's id for B's in the held list; no retrigger. |
| Note Expression (voice and mono track graph) | Follows B's curves, with the glide; velocity becomes B's. In a track graph the mono last-note rule treats B as the newest note. |
| Polyphonic Faust | The voice's instance is not cleared; its parameters follow the patched modulation as usual. |
| `MpeEncoder` | See section 5. |

An unknown event type must never be ignored silently: a consumer that only knows note-ons would leave a voice
playing A forever. The switches over `NoteEventType` already list every case (no `default:`), so the compiler points
at each one that needs the new case.

**(L8) Chase and seek start the chain where the playhead is.** Chasing at a position inside B sends B as a plain
note-on with `fromNoteId` set, so the voice starts afresh (there is no voice to continue) but its pitch includes the
glide offset that is still under way. A seek still chokes everything first; a loop wrap still releases everything
and chases at the loop start. Chasing at a position inside A's gap holds A, as playback would.

**(L9) Compiling links.** The `TrackSequence` resolves each valid link to the target's runtime id (repetitions
included) and stores, per compiled note, `linkedFrom` (the source's runtime id, or 0) and `glideOffset`, `glide`,
`shape` and `tension`. The Sequencer keeps a source note in its active list past its end until its target starts,
and never sends its note-off.

## 5. Plugin output

**(L10) MPE keeps the channel; plain MIDI overlaps the notes.**

* **MPE:** on `legato` the encoder renames the member channel's note from A to B, sends no note-on or note-off,
  and keeps sending per-note bend relative to the key it sent for A. The bend then covers B's base pitch minus A's,
  plus B's curve and the glide. If that leaves the ±48 semitone bend range, the encoder ends A and starts B on a
  fresh channel instead (an audible restart, but in tune).
* **Plain MIDI:** there are no per-note bends, and a channel-wide bend would move every other note. The encoder
  sends B's note-on a moment before A's note-off (legato overlap), which mono and legato-mode synths treat as a
  slide and glide with their own portamento. DAWg's glide time and shape aren't sent; the plugin's own settings
  decide. The inspector says so for a plain MIDI plugin instrument.

## 6. UI

* **Linking:** `L` links the selected notes in start order. Where several selected notes start together (a chord
  moving to another chord), each note links to the note of the next start group nearest in pitch, so voices lead
  separately; notes that find no partner stay unlinked. `Shift+L` removes the selected notes' links. The context
  menu offers Link and Unlink.
* **Drawing:** a thin line in the track colour from A's end (or the hand-over, if they overlap) to B's start, at
  the two pitches; a broken link (L3) is drawn dashed and dim. B's ribbon includes the glide, since the roll's
  `pitchAtTick` adds the glide offset as the engine does, so what is drawn is what is heard.
* **Glide editing:** a selected linked note shows a glide handle at the end of its glide: drag it left or right to
  set the time (snapped to the grid, Ctrl/Cmd free), and its tension handle bends the shape like a curve segment's.
  The note inspector (beside the vibrato strip) shows Glide time and Shape for the selected linked notes and edits
  them together.
* **Default glide time:** new links get the last used glide time, initially one sixteenth (L12).
* **Editing linked notes:** moving, resizing and transposing notes keeps their links (a link points at an id, not a
  position). Copy and paste keep links between copied notes and drop links to notes that weren't copied. Splitting
  a linked source note gives its link to the right half. Delete removes links to deleted notes.

## 7. Commands and file

**(L11) Commands.**

| Command | Change |
|---|---|
| `addNotes` | A note may carry `link: { toIndex, glide, shape, tension }`, where `toIndex` is the index of another note in the same call (pasting needs links between notes that have no ids yet). |
| `updateNotes` | A note may carry `link: { to, glide, shape, tension }` or `link: null`. |
| `setNoteLinks` | `{ clipId, links: [{ from, to \| null, glide?, shape?, tension? }] }`: the roll's L and Shift+L, and the glide handle; keeps L2. |
| `removeNotes` | Also clears links pointing at the removed notes. |
| `splitNotes` | Moves a split note's link to its right half. |
| `duplicateClips`, `duplicateTrack`, linked clip copies | Remap link ids with the note ids. |

All undoable, and batched as usual.

**(L13) The next format version** (12 went to the track drum mode). `link` is written only on linked notes; older files have none, so the migration is
the identity. The loader drops links whose target isn't in the same clip, rather than keeping broken data (L3 keeps
edit-time breakage only, where the target still exists).

## 8. Decisions

| # | Decision | Status |
|---|---|---|
| L1 | The link lives on the earlier note: target, glide time, shape, tension | Proposed |
| L2 | One-to-one chains; commands keep the invariant | Proposed |
| L3 | Same clip only; invalid links are kept but not played; per repetition in looped clips | Proposed |
| L4 | Hand-over at B's start; a gap holds A; the chain ends with the last note-off | Proposed |
| L5 | The glide is an offset from A's pitch that fades out over B's own curve | Proposed |
| L6 | Only pitch glides; pressure and timbre switch; velocity becomes B's without a retrigger | Proposed |
| L7 | A `legato` note event, handled by every consumer | Proposed |
| L8 | Chase inside a chain starts it fresh with the glide still applied | Proposed |
| L9 | Links resolved in the `TrackSequence`, per repetition | Proposed |
| L10 | MPE keeps the channel and bends (fresh note past ±48); plain MIDI overlaps the notes | Proposed |
| L11 | Link fields in `addNotes` / `updateNotes`, plus `setNoteLinks` | Proposed |
| L12 | `L` / `Shift+L`, a link line, a glide handle and inspector fields; default glide one sixteenth | Proposed |
| L13 | The next format version, identity migration | Proposed |

## 9. Questions the owner answered or left (see section 0)

1. **Glide time unit:** ticks (follows the tempo, like everything in the roll; proposed) or milliseconds (like a
   synth's portamento knob)?
2. **Pressure and timbre:** switch at the hand-over (proposed, L6), or glide with the pitch?
3. **Velocity on a link:** B's velocity takes over without a retrigger (proposed), or the chain keeps A's velocity
   throughout?
4. **Gaps:** may a link bridge a gap (A's voice holds through it; proposed), or must B start at or before A's end?
5. **Across clips:** is same-clip only acceptable for v1 (proposed), given that a phrase split over two clips can't
   glide between them?
6. **Plain MIDI plugins:** is the legato overlap (the plugin's portamento decides the glide) good enough, or should
   linked notes switch a plugin to MPE automatically?
7. **Chords:** is "nearest pitch in the next start group" the right pairing for `L` on chords, or should chords
   simply not link (only a monophonic selection links)?

## 10. Tests

* **Model and commands:** link, relink, unlink; the one-to-one invariant; links kept through move, resize and
  transpose; dropped with a deleted target; moved by split; remapped by paste (`toIndex`) and clip duplicate; undo.
* **File:** round trip in the new version; older files unchanged; links to missing or other-clip notes dropped on load.
* **Sequence:** valid and invalid links; runtime ids in looped clips; `glideOffset` from A's curve at the hand-over.
* **Engine:** a linked pair plays one voice (no second envelope attack: the output's envelope stays continuous at
  the hand-over); the pitch at B's start equals A's and reaches B's after the glide; after the glide B sounds as it
  does unlinked; overlap and gap; chase inside B and inside a gap; seek and loop; stealing A's voice before B
  starts falls back to a note-on; sustain pedal across a link; voice graph ADSR and LFO don't retrigger; polyphonic
  Faust isn't cleared; no allocation.
* **MPE output:** one channel, no note-on for B, bend continuity; the fresh-channel fallback past ±48; plain MIDI
  overlap order.
* **UI:** `L` on a mono line and on chords; the link line and broken links; glide handle and inspector edits; the
  ribbon matches the engine's pitch (shared formula, tested against engine values).

## 11. Steps

Each step is its own branch with tests, approved before it starts.

1. **Model, commands and file** (L1–L3, L11, L13): `NoteLink`, the commands and their invariants, the format version, the
   UI protocol types. No sound yet.
2. **Sequence and Sequencer** (L4, L5, L8, L9): compiled links and glide offsets, the `legato` event, chase.
3. **Instruments** (L6, L7): `VoiceAllocator`, MPE Synth, Voice Instrument, ADSR, LFO, Note Expression, polyphonic
   Faust.
4. **Plugin output** (L10): `MpeEncoder` for MPE and plain MIDI.
5. **Piano roll** (L12): linking keys and menu, drawing, glide handle, inspector, copy, paste and split.
