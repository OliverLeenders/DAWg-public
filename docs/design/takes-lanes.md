# Design: takes lanes

Status: **accepted, built**. The owner chose takes lanes per track and accepted this on 2026-11-03, with the answers to the open questions in section 5. It changes how clips overlap (phase6-arrangement.md, A6) and how loop recording keeps its passes (phase9-export-midi-input.md, M6).

## 1. What this is

Clips on a track can stack. Where they overlap, only the top one plays. Recording over a clip doesn't shorten it: the new recording goes on top, and the old one stays whole underneath. A track can be expanded to show its takes lanes, one row each, and the parts that play are picked there.

```
 ▾ Vocals  M S ●   │▓▓▓▓▓▓▓ take 3 ▓▓▓▓│▓▓ take 1 ▓▓▓│      ← the track: what plays
   Take 3          │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│              │
   Take 2          │░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░│      ← under take 3 and take 1: silent
   Take 1     │▓▓▓▓░░░░░░░░░░░░░░░░░░░▓▓▓▓▓▓▓▓▓▓▓▓▓▓│
```

Bright parts play, dim parts are covered by something on top. Reaper 7's fixed item lanes and Logic's take folders work this way. Ableton's take lanes differ: they hold the recordings and a separate main lane holds the comp. This design has no separate comp lane: what plays is always whatever is on top.

## 2. Decisions

**(T1) Clips stack, the top one plays.** Each clip has an `order`: a newer placement is on top. At every point in time, a track plays the clip with the highest order that covers it, and nothing else. Clips are never shortened or deleted by being covered, so lifting or deleting the top one brings back what was underneath.

**(T2) A track has takes lanes.** A track holds a number of lanes, at least one, the top row first. Each clip sits in one lane, and within a lane clips don't overlap. A placed clip (added, moved, resized, pasted) keeps its own lane if it overlaps nothing there, else goes into the first free lane from the top, else into a new lane on top; only a track already holding the most lanes (64) falls back to replacing the covered part of the lane (A6). Lanes left empty go. Lanes are only where clips are shown and kept; which clip plays depends on order alone (T1), not on which lane it's in.

**(T3) Recording goes into a new lane.** A recording goes into the top lane if that lane is empty over the recorded range, or else into a new lane on top. Its clips are placed on top (T1), so the newest take plays. Audio and notes follow the same rule. When the take arrives (the "Record" undo step), the arrangement selects exactly the clips it added: one per armed track, or one per pass when loop recording (`addedClipIds` in `ui/src/arrangement/clipSelection.ts`).

**(T4) Loop recording: one take per pass.** Each full pass becomes its own clip in its own lane, the newest on top, so the last pass plays. For audio, all passes stay in one file, as now, and each pass's clip points at its part of the file. The clip menu's Takes submenu and the `takes`, `takeStart` and `takeLength` clip fields go away. For notes, a setting keeps today's overdub, where every pass merges into one clip: drum patterns are built that way. *Open question: which is the default for notes.*

**(T5) Expanding a track.** A track with more than one lane gets a takes button in its header, showing how many. Expanded, its lanes show as rows under the track's own row, newest on top. Each lane row has a small header: its name ("Take 3", numbered from the bottom, so the newest take has the highest number) and a button that deletes the lane with its clips. The track's own row always shows what plays. Renaming lanes, soloing a lane and moving a lane to a new track were left out of this first version.

**(T6) Picking what plays (comping).** In a lane row:
* a click on a clip brings that whole clip to the top;
* a drag across a clip brings that part to the top, and the clip is split at the drag's ends;
* the parts that now play brighten, and what they cover dims.

Each pick is one undo step. In the track's own row, clips move, resize and split as now; a moved clip comes to the top where it lands.

**(T7) Flattening.** A track menu item "Flatten takes" keeps what plays and removes the rest: covered parts are cut away, and the lanes collapse into one. This is the A6 result, on request.

**(T8) Playback flattens at compile time.** The graph compiler turns a track's stacked clips into the parts that play before handing them to the sequencer and the audio players, as `clearRange` does today but without changing the project. Playback, export, MIDI export and the piano roll's ghost notes all see that flattened view; the sequencer and the audio players don't change. Notes and audio at the edge of a covered part are cut as a split would cut them.

**(T9) The project file.** Format version 15: a clip gains `lane` (its row) and `order`, a track gains `lanes` (how many) and `lanesExpanded`. On load, a version 14 clip goes into the track's first lane, with orders by position, and a clip with several takes becomes one clip per take, each in its own lane, the take it played on top. Nothing else is migrated: only test projects exist (no backwards compatibility is needed yet).

## 3. What changes where

* **Engine:** `NoteClip` (`lane`, `order`), `Track` (`lanes`, `lanesExpanded`), `placeClip` with its lane rules and `clearRange` per lane, `playedClips()` and `clipPart()` in `Model.cpp`, new commands (`bringToTop` with an optional range, `flattenTakes`, `removeLane`, `setTrackLanesExpanded`), the flattening in `GraphCompiler`, `compileAudio` and the MIDI file export, recorded clips (`recorded: true` on `addClip` and `addAudioClip`), each recorded event's loop pass (`RecordedInput::pass`, `NoteRecorder`), loop passes as clips (`loopTakes` in `MainComponent.cpp`, `noteRecordingCommands`), the overdub setting (`record.loopOverdub`), project format 15, and tests (`tests/TakesTests.cpp`).
* **UI:** the arrangement's lane layout gains takes rows (`arrangement/layout.ts`), the track's row draws what plays and the takes rows dim what's covered (`timeline/takes.ts`, `draw.ts`), the comping gestures, the take headers, the takes button and the track menu's Show takes and Flatten takes. The clip menu loses Takes.
* **Docs:** A6 and M6 point here.

## 4. Steps

1. Engine: the model, format v4 and the flattening in the compiler; overlapping clips play top-first. Tests.
2. Engine: placement per lane, `bringToTop`, `flattenTakes` and the lane commands. Tests.
3. Recording: new lanes for recordings, one take per pass for audio, the overdub setting for notes.
4. UI: expanding a track, the lane rows and headers, dimmed covered parts.
5. UI: comping by click and drag, Flatten takes, and the Takes submenu removed.

## 5. Open questions, answered

* Loop recording of notes: a new take per pass by default, with overdub as a setting (`record.loopOverdub`; no UI for it yet).
* In the track's own row, dragging a clip onto another stacks it (T2), so one rule holds everywhere.
* Lanes aren't muteable. Soloing a lane while auditioning was left for later.

## 6. Known limits

* The piano roll shows every clip of a track, covered takes too.
* Notes linked across a cut (linked-notes.md) don't glide over it.
