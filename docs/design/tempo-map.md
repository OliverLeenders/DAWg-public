# Design: tempo map and time signatures

Status: **implemented** in three steps (section 6; notes in sections 10–12). The second part of phase 13 (routing and time). The
decisions are numbered TM1–TM14; the owner's answers are in section 9.

## 1. Goals and non-goals

Goals:

* **Tempo changes** along the timeline: steps (a new tempo from a point on) and ramps (an accelerando or
  ritardando between two points).
* **Time-signature changes** on bar lines, with any numerator and a denominator of 2, 4, 8 or 16 (7/8, 6/8, 5/4).
* Everything that turns musical time into real time follows the map: the transport, the loop, the metronome and
  count-in, recording, export, automation, vibrato, audio clips, rulers, grids, snapping and the position display.
* Hosted plugins learn the tempo, the time signature and the position (they get nothing today), so tempo-synced
  delays and arpeggiators follow the song.

Non-goals (later):

* **Audio warping** (stretching audio to follow tempo). Audio clips keep their real-time length, as today
  (phase7-audio-tracks.md, T3), see TM8.
* Recording tempo changes live from a tap or the BPM field during playback.
* Tempo detection from audio.
* Import of tempo maps from MIDI files (comes with DAWproject import, phase 15).

## 2. What exists today

* `Project::tempo` (one BPM) and `Project::beatsPerBar` (bars of quarter-note beats). Ticks are the musical clock
  (`ticksPerBeat` = 15 360 per quarter note); clips, notes, markers, automation and the loop are in ticks.
* `TimeMap` maps ticks to samples linearly from an anchor; the engine re-anchors it on seeks, loops and tempo
  changes.
* `setTempo` rescales every audio clip's tick lengths by old/new tempo so the audio still fits; audio playback uses
  one compiled `secondsPerTick`.
* The UI draws bars as `beatsPerBar × 15 360` ticks everywhere; nothing shows real time.

## 3. Model

**(TM1) The tempo map is a list of points.** Recommended.

```
TempoPoint { tick, bpm, ramp }          // ramp: the tempo moves linearly (in BPM, over ticks) to the next point
Project::tempoMap: [TempoPoint]         // sorted by tick, never empty, the first at tick 0
```

* A point without `ramp` holds its tempo until the next point (a step). The last point's `ramp` is ignored.
* The real time of a tick is the integral of 60 / (bpm · ticksPerBeat) over the ticks before it. For a step
  segment that is linear; for a ramp it has a closed form (a logarithm), and its inverse an exponential, so
  conversions are exact and cheap in both directions.
* Points snap to the grid when edited but may sit anywhere (no bar constraint).

**(TM2) Time signatures are a list of changes on bars.** Recommended.

```
TimeSignature { bar, numerator, denominator }   // bar: 0-based bar index; denominator 2, 4, 8 or 16
Project::signatures: [TimeSignature]            // sorted by bar, never empty, the first at bar 0
```

* Changes are stored by **bar number**, not tick, so a change always falls on a bar line and moving an earlier
  change can't leave a later one mid-bar. The tick of each change follows from the bars before it.
* A bar lasts `numerator × (4 / denominator)` quarter notes; 7/8 is 3.5 quarters. Tempo stays in quarter notes per
  minute (TM6).
* `beatsPerBar` goes away; `setBeatsPerBar` becomes "set the signature at bar 0" (as a numerator over 4) for old
  callers, then is removed.

**(TM3) One shared `TempoMap` class does the maths.** Recommended. Built from the model's points and signatures,
immutable, with each segment's start time precomputed, so lookups are a binary search:

* `secondsAt (tick)`, `tickAt (seconds)`, `bpmAt (tick)`;
* `barAt (tick)` → { bar, beat, tick in beat }, `tickOfBar (bar)`, `signatureAt (tick)`.

The engine's `TimeMap` keeps its anchor (for seeks and loops) but maps through it: `sample = anchorSample +
(secondsAt (tick) − secondsAt (anchorTick)) · sampleRate`. A TypeScript twin (`ui/src/time/tempoMap.ts`) does the
same for the UI, tested against the same cases.

**(TM4) The file format goes to version 2.** `migrate1To2` turns `tempo` into `[{ tick: 0, bpm: tempo }]` and
`beatsPerBar` into `[{ bar: 0, numerator: beatsPerBar, denominator: 4 }]`. The summary's length in seconds uses
the map.

**(TM5) Commands.**

* `addTempoPoint { tick, bpm, ramp }`, `setTempoPoint { index, tick?, bpm?, ramp? }`, `removeTempoPoint { index }`
  (not the first). Values coalesce while dragging, like `setTempo` does today.
* `setTempo { bpm }` stays for the BPM field (TM11).
* `setSignature { bar, numerator, denominator }` (adds or replaces), `removeSignature { bar }` (not bar 0).
* Each is one undo step, and each recomputes audio clips' tick lengths (TM8).

## 4. Engine

**(TM6) Beats stay quarter notes.** The tick grid doesn't change: a quarter note is 15 360 ticks in every
signature, tempo is quarter notes per minute, and a 6/8 bar is simply 3 quarters long. Nothing stored in ticks
moves when a signature changes, except that what is "bar 9" may now be at a different tick. (Clips stay at their
ticks; a signature change doesn't shift material.)

**(TM7) Blocks may span a tempo change.** The engine already passes `startTick` and `endTick` per block and nodes
derive ticks per sample from them. A block that crosses a tempo point (or lies on a ramp) is close to linear over
a few milliseconds, so:

* event timing (notes, automation) uses `TimeMap::tickToSample` for each event, which is exact;
* the few places that need a rate (vibrato in Hz, the MPE encoder's seconds per tick) use `secondsAt` differences
  instead of a constant.

**(TM8) Audio clips keep their length in seconds.** Recommended (no warping yet).

* A clip stays anchored at its start tick (so it moves with the bars), and its audio plays at its own speed.
* Its content positions (offset, loop range, fades, takes) are ticks at **the tempo where the clip starts**, as
  they are ticks at the project tempo today. Its length is timeline ticks.
* After every command that changes the map or moves an audio clip, one pass (`fitAudioClips` in Commands.cpp)
  refits it: content positions scale with the tempo at its start, and its length (unless the command set it)
  covers the same seconds as before. This is the general form of what `setTempo` did, and it covers moves and
  duplicates into other tempos without each command knowing about tempo.
* Playback maps a tick to a file frame through `secondsAt` rather than one `secondsPerTick`, so a clip that spans a
  tempo change still plays continuously.
* Warping, so audio follows tempo, is its own later feature.

**(TM9) Metronome and count-in follow the signature.** The click falls on each beat of the denominator (eighths in
7/8), with the bar's first beat accented. Compound meters (6/8, 9/8, 12/8) click on dotted quarters, with the
eighths quieter. The count-in is one bar of the signature at the start position.

**(TM10) Hosted plugins get a play head.** A JUCE `AudioPlayHead` on each plugin instance, filled per block:
BPM at the block start, the time signature, the PPQ position, the PPQ of the bar start, the time in seconds and
samples, playing/recording and the loop. This also fixes tempo-synced plugins today, which see no tempo at all.

## 5. UI (decisions for the owner)

**(TM11) The BPM field in the top bar.** Options:

* (a) *Shows the tempo at the playhead.* With one tempo point, editing it changes the song's tempo (as today).
  With several, it edits the point governing the playhead (the last one at or before it). **Recommended.**
* (b) Always edits the first point; the field doesn't follow the playhead.
* (c) Scales the whole map (all points by the same factor).

The signature field works the same way (the signature at the playhead). A small badge on the field ("map") shows
when the song has more than one tempo.

**(TM12) Where tempo and signatures are edited.** Options:

* (a) **A tempo lane and a signature strip at the top of the arrangement.** Recommended.
  * A *signature strip*: a thin row in the ruler with flags like "4/4" and "7/8" on their bars. Double-click the
    strip to add one, click a flag to edit (a small popover with numerator and denominator), drag to another bar,
    Delete to remove.
  * A *tempo lane*: a foldable lane between the ruler and the tracks, drawn like an automation lane, with the
    tempo as a line over a BPM scale. Click to add a point, drag points in time and BPM, double-click a point to
    type a BPM, toggle a segment between step and ramp from its context menu (or Alt+click on the segment).
    Hidden by default for songs with one tempo, with a "Tempo" toggle in the ruler's corner.
* (b) Markers only: tempo and signature changes are flags in the ruler (like markers), edited in popovers, no lane.
  Simpler, but ramps are hard to see or shape.
* (c) A list editor in the inspector (a table of changes), no graphics.

**(TM13) Real time.** Options:

* (a) **The position display toggles** between bars:beats and min:sec on click; the ruler stays in bars.
  **Recommended.**
* (b) Both always: the top bar shows both, and the ruler gets a seconds row under the bars.
* (c) Bars only, as today.

**(TM14) Grids in other signatures.** The grid's subdivisions follow the denominator (an eighth-note grid in 7/8,
quarters in 4/4), and "bar" snapping snaps to real bar lines whatever their length. The piano roll shows the same
bars and signatures as the arrangement (read-only there).

## 6. Steps

1. **Model and maths:** `TempoPoint`, `TimeSignature`, `TempoMap` (C++ and TypeScript, with shared test cases),
   commands, file version 2, audio clip lengths (TM8).
2. **Engine:** the time map through `TempoMap`, audio playback across tempo changes, vibrato and the MPE encoder,
   the metronome and count-in, recording, export, the plugin play head.
3. **UI:** bars from the map everywhere (rulers, grids, snapping, export ranges, position display), the BPM and
   signature fields, the tempo lane and signature strip, min:sec.

Each step ships with its tests and keeps today's single-tempo projects working unchanged.

## 7. Tests

* `TempoMap`: round trips tick ↔ seconds on steps and ramps; bars across signature changes; the TypeScript twin
  against the same table.
* Rendering: a click track across a tempo step lands each click on the expected sample; a note on a ramp starts at
  the integral's sample; an audio clip across a tempo change plays without a gap or jump.
* Commands: an audio clip keeps its seconds after each tempo edit; undo restores the map; migration from v1.
* Plugin play head: a test node reads back BPM, signature and PPQ at known positions.

## 8. Summary of decisions

| # | Decision | Status |
|---|----------|--------|
| TM1 | Tempo map: points with step or linear ramp | Approved (ramps in v1) |
| TM2 | Time signatures stored by bar number, denominators 2/4/8/16 | Recommended |
| TM3 | One `TempoMap` class (C++ and TS twin) | Recommended |
| TM4 | File format version 2 with migration | Recommended |
| TM5 | Commands for points and signatures | Recommended |
| TM6 | Ticks stay quarter notes | Recommended |
| TM7 | Exact per-event timing, rates from the map | Recommended |
| TM8 | Audio clips keep their seconds; warping later | Recommended |
| TM9 | Metronome follows the signature | Recommended |
| TM10 | Plugin play head | Recommended |
| TM11 | BPM field edits the tempo at the playhead | Approved |
| TM12 | Tempo lane plus signature strip | Approved |
| TM13 | Position display toggles to min:sec | Approved |
| TM14 | Grids follow the denominator | Recommended |

## 9. Owner's answers

* **Editing (TM12):** the tempo lane and the signature strip, as in the mock-up.
* **BPM and signature fields (TM11):** show and edit the value at the playhead, with a MAP badge for several tempos.
* **Real time (TM13):** clicking the position display toggles between bars:beats and min:sec.
* **Ramps (TM1):** steps and ramps in the first version.

## 10. Step 1 notes (model and maths)

* `TempoPoint`, `TimeSignature` and `TempoMap` live in Time.h; `Project::tempoMap` replaces `tempo` and
  `beatsPerBar`. The JSON has `tempoMap: [{ tick, bpm, ramp }]` and `signatures: [{ bar, numerator, denominator }]`.
* Commands: `setTempo { bpm, at? }`, `addTempoPoint`, `setTempoPoint`, `removeTempoPoint`, `setSignature`,
  `moveSignature`, `removeSignature`; `setBeatsPerBar` is gone (the UI sets the signature at the playhead).
* File format version 2, migrated from version 1.
* The UI twin is ui/src/time/tempoMap.ts (`tempoMapOf (project)` caches it). The top bar's position, BPM and
  signature fields already use the map at the playhead; rulers and grids still use the first signature until step 3.
* The engine still plays at the first tempo until step 2 (`CompiledEngine::tempo`), recording and imports already
  convert through the map, and new audio clips measure their content at the tempo where they start.
* Tests: tests/TempoMapTests.cpp and ui/src/time/tempoMap.test.ts share their cases.

## 11. Step 2 notes (engine)

* `TimeMap` maps through the tempo map: sample = anchorSample + (secondsAt (tick) − anchorSeconds) · rate. It
  refers to the compiled snapshot's `TempoMap` (copies never allocate). A new snapshot is adopted by anchoring at
  the current tick, read before the snapshot exchange retires the old one.
* The engine cuts its chunks at tempo points (as at the loop end), so ticks are linear in samples within a chunk:
  exact at steps, and off by far less than a sample along ramps. Nodes keep interpolating ticks across a chunk.
* Audio clips render in their content ticks from real time (`AudioPlayer.cpp`): a block maps to a straight line
  in content ticks whatever the map does inside the clip, so audio plays on unbroken across tempo changes.
* Free-running vibrato uses the real time since it began (`CompiledNote::pitchOffset` takes the map).
* The metronome clicks on the signature's beats with the bar's first accented; compound meters click on dotted
  quarters with quieter eighths. Recording skips to the record position through the map.
* Hosted plugins get a `juce::AudioPlayHead` with the tempo, signature, PPQ position, bar start, time and play
  state at each block start (`positionInfo()` in PluginNode.h). Loop points and the record state aren't passed yet.
* Tests: notes after a step and on a ramp start at the integral's sample, the metronome across a step and in 6/8,
  an audio clip across a tempo change, and the plugin position info.

## 12. Step 3 notes (UI)

* Rulers and grids draw bar by bar from the map (`visibleBars`, `drawTimeGrid` and `drawRuler` in
  pianoroll/draw.ts): beats follow the denominator, and the snap grid restarts at every bar line.
* Timeline positions snap with `TempoMap.snap` (bar-relative; a grid of a bar or more snaps to bar lines, which is
  the arrangement's "Bar" choice). Moves snap where the grabbed clip lands rather than by the drag distance, so clips
  stay on the grid across bars of different lengths. Clip extensions, new one-bar clips and export ranges use real
  bar lines. Controller lanes keep snapping in content time.
* Audio clips draw their waveform, fades and fade handles in content ticks (timeline/audioTime.ts), so they line
  up with what plays across tempo changes; fade drags edit content ticks.
* Vibrato drawing and its rate handle use real time through the map, as the engine does.
* The arrangement has a signature strip under the ruler (SignatureStrip.tsx) and a tempo lane (TempoLane.tsx) that
  shows by itself when the song has tempo changes, with a "Tempo" toggle in the strip's corner. Tempo point drags
  coalesce in the command queue by point (`setTempoPoint` sets tick and BPM together).
* The piano roll's ruler shows signature changes beside their bar numbers (read-only).
* The top bar's position display toggles between bars:beats and m:ss.mmm on click (remembered per user), and the
  BPM field carries a MAP badge when the song has tempo changes.

