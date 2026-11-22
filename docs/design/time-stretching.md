# Design: time stretching and warping

Status: **implemented**, steps 1–4 (2026-11-04). The decisions are numbered TS1–TS15 and summarised in section 10.
The owner's answers are in section 9, and section 11 has the notes on what was built, with what differs from the plan.

The owner's answers so far (2026-11-04):

* Audio is never stretched automatically.
* Each file is either **one-shot** (it does not follow tempo changes) or **musical** (it does), and new files are
  one-shot.
* Recordings need adjustable timing: markers that can be placed and moved.
* No real-time stretching. Stretched audio is rendered offline with Rubber Band, for the best quality. DAWg stays
  free software and is never sold, so Rubber Band's GPL is no constraint (TS1).

## 1. Goals and non-goals

Goals:

* **Two modes per file.**
  * A *one-shot* file plays at its own speed and keeps its length in seconds, as all audio does today
    (tempo-map.md, TM8).
  * A *musical* file is musical material: its clips stay on their bars and beats when the tempo map changes, as note
    clips do.
* **Timing markers.** Pinning a point in a file, such as a note onset in a recording or a downbeat in a loop, and
  dragging it to fix the timing. Placement is helped by detected transients. In a musical file a marker sits at a
  musical position, so it follows the tempo; in a one-shot file it sits at a time in seconds, so the tempo never moves
  it (TS15).
* **Stretching a clip by hand.** For a musical file, dragging a clip's edge with a modifier stretches the audio to the
  new length instead of trimming it.
* **Pitch shifting per clip,** in semitones and cents, independent of the speed.
* **A repitch variant** of musical mode (varispeed, as on tape), where speed and pitch change together. It has no
  artefacts and costs almost nothing.
* **Nothing is stretched unless the user asks.**
  * Importing, recording, adding clips and tempo edits never switch a file to musical.
  * One-shot audio plays exactly as today, sample for sample.
  * A musical file played at its own tempo, with no moved markers and no transpose, takes that same path, so it is
    bit-identical as well.
* Playback, export, seeks, loops and per-track offsets all work with musical files, without added latency. The
  stretching is done ahead of time, so playing a render costs no more than playing a one-shot file.

Non-goals for v1 (later steps or other designs):

* **Automatic tempo detection** from the audio itself, beyond metadata, file names and a bar-length guess (TS11).
  These only *suggest* a source tempo when the user switches a file to musical.
* **Quantise audio**, which snaps markers to the grid in one go. Section 7 lists this as later.
* **Audio to MIDI,** slicing to a sampler, and a full sample editor.
* **Formant correction** beyond what the library offers out of the box.
* **Pitch and stretch automation** (a curve over the clip). The clip's transpose is one value in v1.
* **Elastic audio for live input.** Only file playback is stretched, so the monitored input path never gains latency.

## 2. What exists today

Phase 7 and the tempo map left time stretching out on purpose:

* **Clips have a fixed length in seconds.** An audio clip is a `NoteClip` with `fileId != 0`. Its content
  positions (offset, loop range, fades) are ticks at the tempo where the clip starts. After every command,
  `fitAudioClips` in `Commands.cpp` refits each clip so it covers the same seconds as before.
* **The compiler maps clips to time with one rate.** `compileAudio` in `AudioPlayer.cpp` gives each clip a
  `secondsPerTick`, a `startSeconds` and a `localLength`.
* **The player resamples, nothing more.** `AudioPlayerNode::renderPass` turns each output sample into a file frame
  through `(content tick) × secondsPerTick × fileRate`, then reads it with a 16-tap windowed-sinc interpolator.
  That interpolator also converts the file's sample rate to the device's (phase7, T7).
* **Reading is random access.** Files stream through a `BufferingAudioReader`, and files under 30 seconds are held
  in memory. The player can read any frame range, a little ahead of time.

So the only thing missing is a mapping from content ticks to file frames that is not a straight line through the
tempo, and a processor that changes speed without changing pitch.

## 3. The library

**(TS1) Rubber Band, with the R3 ("finer") engine in offline mode.** The owner's choice.

* **Quality.** R3 is the reference among open-source stretchers, and it preserves formants, which keeps transposed
  vocals natural. Its real-time CPU cost is not a factor, because DAWg renders offline (TS7).
* **Timing markers.** The offline API takes a key-frame map (`setKeyFrameMap`: input frame → output frame), which
  places each timing marker exactly where it should land. That is the timing map (TS3) rendered as is.
* **Licence.**
  * Rubber Band is GPL v2 or later, or commercial. DAWg uses it under GPLv3.
  * DAWg stays AGPL-3.0-or-later, as JUCE's open-source licence requires (`docs/PLAN.md`), and GPLv3 code may be
    combined with AGPLv3 code. Nothing changes for users or contributors.
  * The cost is that builds with Rubber Band can never be closed source without its commercial licence. That is not
    a concern: DAWg is free software and will not be sold. A donation link in the README is unaffected.
  * Its copyright notice is added to the README's third-party list.
* **Building.** Rubber Band ships a single-file build (`single/RubberBandSingle.cpp`) with its own FFT and resampler,
  so it compiles into the engine through CPM without FFTW or other system libraries.
  * Pin the latest release in `cmake/Dependencies.cmake`, and add it to the dependency table in `docs/PLAN.md`.
  * Check the R3 options at that version: the engine, formant preservation, and window and pitch options.
* **Behind an interface.** The renderer calls Rubber Band through one small interface (`Stretcher`: render a source
  range with a key-frame map and a transposition). A different library could then replace it without touching the
  cache or the player.

The alternatives considered:

| Library | Licence | Why not |
|---|---|---|
| Signalsmith Stretch | MIT | Built for real time; somewhat behind R3 on large ratios and vocals. |
| Bungee | MPL-2.0 | Grain-based, random access; less proven on dense material. |
| élastique (zplane) | Commercial | Not an option for free software. |

## 4. Model

**(TS2) The timing mode belongs to the pool file.** The owner's choice. `AudioFile` (phase7, T4) gains:

```
timing: {
  mode: "oneShot" | "musical",           // default oneShot
  musicalStyle: "stretch" | "repitch",   // keep the pitch, or change it with the speed (tape)
  sourceBpm,                             // musical: the file's own tempo; sets the default content-to-file rate
  markers: [ { tick, frame } ],          // content position -> file frame, sorted, both strictly increasing;
                                         // the position is in ticks (musical) or seconds (one-shot, TS15)
}
```

and `NoteClip` gains `transpose` (semitones, float, cents as the fraction), which applies to musical files in the
stretch style only.

* **One-shot:** content ticks are tempo-scaled seconds, and `fitAudioClips` keeps refitting them. Without markers
  nothing changes; markers pin frames to content seconds (TS15).
* **Musical:** content ticks are **musical** ticks, like a note clip's. A clip keeps its start, length, offset, loop
  range and fades in ticks when the tempo map changes, so `fitAudioClips` skips clips of musical files.
* **The mode and the markers are shared by every clip of the file.** Fixing the timing of a recording therefore fixes
  every clip cut from it, including its takes and comped pieces.
  * Splitting, duplicating and comping need no marker handling, because the markers live in the file's content time
    and every clip simply reads through them.
  * To stretch two uses of one file differently, **"Make copy"** (`copyPoolFile { clipId }`) gives the clip a new pool
    entry with a copy of the timing settings and markers. The audio on disk is shared, not copied. The copy's markers
    then move independently. It is offered in the clip's context menu and in the audio editor (TS13), and the new
    entry is named "<name> (2)".

**(TS3) The timing map.** The content-to-file mapping is piecewise linear through the markers:

* There are always two implicit end points: content 0 maps to frame 0, and the line past the last marker
  continues at the file's natural rate. For a musical file that is the `sourceBpm` rate,
  `fileRate × 60 / (sourceBpm × ticksPerBeat)` frames per tick; for a one-shot file it is `fileRate` frames per
  second.
* So with no markers, a musical file is simply the file at `sourceBpm`, played at the project's tempo. That is the
  common case for loops.
* The maths lives in one class (`TimingMap`: `frameAt (tick)`, `tickAt (frame)`, `rateAt (tick)`), with a
  TypeScript twin in `ui/src/time/timingMap.ts` tested against the same cases, as `TempoMap` is (tempo-map.md, TM3).

**(TS4) Switching a file to musical keeps its audio where it is.**

* `setFileTiming` from one-shot to musical sets `sourceBpm` to the tempo at the start of the file's first clip, or to
  the suggestion from TS11 if the user picks it. It then adds markers so that every clip of the file sounds at the
  same time as before under the current tempo map.
* With one tempo, and `sourceBpm` equal to it, that needs no markers. Across tempo changes, it needs one marker per
  tempo point the file's content passes through, plus samples along ramps.
* Switching back to one-shot converts the same way: each clip keeps the seconds it currently plays for, and
  `fitAudioClips` takes over again. The markers convert to seconds (TS15), so the audio stays where it sounds; the
  markers the switch to musical added only for the tempo map fall away again.
* Switching modes therefore never moves audio. Only later tempo edits or marker moves do.
* A file whose clips start at different tempos converts through the tempo at each clip's start. Where the clips
  disagree, the first clip's timing wins and the others may shift by the tempo difference. The command result reports
  this, so the UI can warn about it.

**(TS5) Edits on clips of musical files.**

| Edit | Behaviour |
|---|---|
| Move, resize, split, loop, fades, comping | Work unchanged, because everything is ticks and the markers live on the file. |
| Glue | Still refused for audio clips. |
| **Stretch-resize** (Alt + drag of a clip's right edge, TS12) | Scales the file's `sourceBpm` and every marker's tick by old length / new length, so the audio fills the new length. Because markers are per file, this applies to every clip of the file. The other clips keep their start, and their lengths scale too. |
| Stretch-resize on a clip of a one-shot file | Refused. The cursor shows "not allowed", and a hint says to make the file musical first. Nothing is converted automatically. One-shot files are retimed with markers instead (TS15). |

**(TS6) File format.**

* Version 8 adds `timing` to pool files, defaulting to one-shot, and `transpose` to clips, defaulting to 0. Existing
  projects therefore sound the same.
* The migration only fills in the defaults. Only test projects exist, so no compatibility mode is needed.

## 5. Engine

**(TS7) Stretched audio is rendered offline in the background and played from a cache.** The owner's choice.

* **What is rendered: segments of a file's timeline.** For each musical file in the stretch style, the timing map and
  the tempo map together give a curve from timeline seconds to file frames.
  * The renderer turns each stretch of the file that clips use into an audio file at the file's sample rate, in
    timeline time: a *render*.
  * One render serves every clip that plays that stretch of the file at the same timeline position, tempo and
    transpose.
  * Duplicates in other places, or other tempos, get their own render. The key (TS8) tells them apart.
* **Playback.** The player plays a render through the existing one-shot path: the sinc resampler from the render's
  rate to the device rate, gains, fades and the declick.
  * The player itself doesn't change, apart from choosing between the file and its render. So there is no
    audio-thread cost, no latency, and nothing to prime after a seek or loop wrap.
  * Clip loops on a musical file render each pass where it plays. Passes over the same content and tempo share a
    render.
* **No stretching needed.** Where the ratio is 1 and the transpose is 0 over a whole segment, the segment isn't
  rendered. The clip plays the file directly, so a file that was made musical but never retimed costs nothing and
  sounds identical to one-shot playback. The check allows for floating-point error (a relative 1e-9), so markers made
  from the tempo map still count as ratio 1.
* **Repitch is not rendered.** It is the existing sinc path, with the read rate following the timing map, so it plays
  instantly and exactly. It ignores `transpose`.

**(TS8) The render cache.**

* **Location.** Renders are float WAV files in `<app data>/Cache/stretch/`. They are not saved in the project: they
  can always be made again from the file and the model.
* **The key** is a hash of everything a render depends on:
  * the file's content (path, size, modification time, as for peaks);
  * its markers and source tempo;
  * the tempo map over the segment's range;
  * the transpose;
  * the Rubber Band version and options.

  Undo and redo find their earlier renders still in the cache, so they are instant.
* **Size.** A least-recently-used limit (10 GB by default, a user setting) removes old renders. "Clear stretch
  cache" sits in the settings next to the peaks cache.

**(TS9) Rendering jobs and what plays meanwhile.**

* **The job queue.** Renders run on a background job queue with one worker per spare core, below the audio thread's
  priority. Clips nearest the playhead go first.
* **Renders are split at markers.** A render covers the content between two markers, plus half a second of context on
  each side, which is rendered and thrown away so the joins are seamless. Neighbouring segments are joined with a
  10 ms crossfade inside that context. So moving one marker re-renders only the two segments beside it, which takes
  well under a second for typical spacing. A tempo edit re-renders only the segments in the changed range.
* **While a segment is rendering** (and during a marker drag), the clip plays that segment in **repitch** from the
  file: the timing is already right and only the pitch is off by the ratio. For timing fixes, that is a few percent.
  The clip shows a thin progress bar while its renders are pending.
* **The UI is told** when a render is ready, through a `dawg_stretch` event with the clip ids. The engine swaps the
  render in at the next compile, with a crossfade if the clip is playing.
* **Expected speed.** Offline R3 runs many times faster than real time on one core, so a three-minute take renders in
  a few seconds the first time it is made musical. The benchmark in section 8 confirms this before the numbers are
  promised in the UI.

**(TS10) Export waits for renders.** Export first renders anything missing, synchronously and with progress shown,
then runs as today. It never uses the repitch preview, so exports are always the final quality and are identical
every time. Freeze and bounce (later) will reuse the same job queue and cache.

**(TS11) Source tempo suggestions and transients.** Both are computed in the background on import and after
recording, and cached in `<app data>/Cache/` next to the peaks. Neither changes a file's mode.

* **Source tempo suggestion** (`AudioFile.suggestedBpm`, optional), found by trying, in order:
  1. Metadata: a WAV `acid` chunk, or AIFF / Apple Loops tempo.
  2. The file name: a number from 60 to 200 followed by "bpm" (`"Drums 128bpm.wav"`, `"128_BPM"`).
  3. A bar guess, for files under 30 seconds: the tempo between 80 and 160 BPM at which the file is a whole number
     of bars, 1, 2, 4 or 8, in the project's signature. If two candidates are close, the one nearest the project
     tempo wins.

  Recordings need none, because their source tempo is the tempo map they were recorded against.
* **Transients**, the onsets that markers snap to: spectral-flux onset detection on a mono sum, giving a list of
  frames, each with a strength. The UI shows the stronger onsets at the current zoom. Detection runs once per file
  and is invalidated only if the file changes.

## 6. UI

**(TS12) Controls.**

* **The Clip inspector gains a Timing section** for the clip's file:
  * Mode: One-shot or Musical.
  * Musical style: Stretch or Repitch.
  * Source tempo, with ×2 and ÷2 buttons, and "Use 128 BPM" when a suggestion exists (TS11).
  * Transpose, in semitones with cents (per clip, stretch style only).
  * "Reset markers".
  * A note saying how many clips share the file, because these settings apply to all of them.
* The context menu of an audio clip and of a pool file offers "Make musical" and "Make one-shot".
* **Alt + drag on the right edge of a clip of a musical file** stretches it (TS5). The cursor and a readout show the
  ratio and the resulting source tempo. On a one-shot file it is refused with a hint (TS5). Dragging without Alt still
  trims, as today.
* **The waveform is drawn through the timing map.** The UI maps each pixel's content tick to file frames with the
  TypeScript `TimingMap` and asks for peaks as today, so stretched audio looks stretched.
* **A badge on clips of musical files** (a small note symbol, or "R" for repitch) shows the mode at a glance. A tinted
  badge marks a segment whose ratio is beyond ×0.5 to ×2, where artefacts grow.

**(TS13) Timing markers are edited in an audio editor window.** The owner's choice.

* **The window.** Double-clicking an audio clip opens the **audio editor**, its own window next to the piano roll's,
  on the same bridge. It shows the clip's file, with the clip's range highlighted.
  * Double-clicking a clip used to select it and show the Clip section in the inspector (phase7, section 9.5). A
    single click now shows the inspector, as it does for note clips.
  * Its header holds the Timing section's controls (TS12) and "Make copy" (TS2), with a note saying how many clips
    share the file.
  * It has its own zoom and scroll, a ruler in bars and beats from the arrangement's tempo map, and the transport.
    Later it is the home of reverse, normalise, gain and quantise audio.
* **Drawing:** the waveform is drawn large, through the timing map. Markers are handles across its full height, and
  detected transients are faint lines, with strong onsets darker.
* **One-shot files** take markers too, edited the same way, in seconds (TS15). The mode never changes by itself.
* **Editing:**
  * double-clicking the waveform adds a marker, snapped to the nearest transient within a few pixels;
  * dragging a marker moves that point of the audio in time, and the segments on either side stretch to fit. Only
    the two segments between its neighbours change, so the rest of the take stays put;
  * dragging snaps to the grid, and Ctrl inverts snapping;
  * Shift + drag slides the audio under a fixed marker instead (it changes the marker's frame, not its tick);
  * Delete removes the selected markers; a box selection selects several, and moving them moves them together;
  * markers are per file (TS2), so editing them changes every clip of the file. Clips of the file outside the
    highlighted range are shown as faint ranges along the top.
* **In the arrangement,** clips of musical files show their markers as small read-only ticks along the top, so the
  timing work stays visible.

**(TS14) Commands.** Each is one undo step, and drags coalesce as with other drags:

* `setFileTiming { fileId, mode?, musicalStyle?, sourceBpm? }`, which applies TS4 when the mode changes;
* `setClipTranspose { clipId, semitones }`;
* `addTimingMarker { fileId, tick, frame? }`, where `frame` defaults to the current mapping at `tick`;
* `moveTimingMarker { fileId, index, tick?, frame? }`;
* `removeTimingMarker { fileId, index }`;
* `resetTimingMarkers { fileId }`;
* `copyPoolFile { clipId }`, which gives the clip its own pool entry with copied timing (TS2);
* `stretchClip { clipId, length }` (TS5).

`setClipTranspose` and `stretchClip` are refused for one-shot files. The marker commands and `copyPoolFile` work on
both: a one-shot file's marker positions (`tick`) are in seconds (TS15).

**(TS15) One-shot files have markers in seconds.** The owner's choice (2026-11-22).

* A marker on a one-shot file pins a frame to a content second: a time from where the file's content starts, which
  no tempo change moves. Between markers the audio stretches to fit, and past the last one it plays at its own speed.
  So a recording's timing can be fixed without making it follow the tempo, and without making it musical.
* `TimingMap` reads a one-shot file's markers in seconds, at `fileRate` frames per second past the last marker, so
  the same class serves both modes. One-shot clips keep their content positions in ticks at the tempo where they
  start; a content tick is that many seconds, which the map turns into frames.
* One-shot files with markers play through pieces like musical files (TS7): the file where a segment runs at its own
  speed, renders (stretch style) or the repitch preview where it doesn't. The style applies to them; transpose, the
  source tempo and stretch-resize stay musical only.
* The two modes share one marker list. Switching converts it through the tempo map so the first clip plays every
  frame at the same time (TS4); points the natural rate gives anyway are dropped, so switching to musical and back
  leaves a file without markers as it was.
* The smallest gap between markers is a tick for musical files and a millisecond for one-shot files.

## 7. Steps

1. **Model and repitch.**
   * Deliverables: `timing` on pool files, `transpose` on clips, `TimingMap` in C++ and TypeScript, file version 8,
     `setFileTiming` with TS4, `fitAudioClips` skipping clips of musical files, and repitch playback through the
     existing sinc path.
   * This proves the model end to end without the library.
   * About 3 days.
2. **Offline stretch rendering.**
   * Deliverables:
     * Rubber Band through CPM (single-file build) and the `Stretcher` interface;
     * renders split at markers, with context and crossfaded joins;
     * the cache with its key and size limit;
     * the job queue and the repitch preview while rendering;
     * the ratio-1 shortcut, transpose, export waiting for renders, and the `dawg_stretch` event.
   * About 4 days. Splitting at markers with seamless joins is the fiddly part.
3. **Audio editor, timing markers and transients.**
   * Deliverables:
     * onset detection and its cache;
     * the marker commands and `copyPoolFile`;
     * the audio editor window: opening by double-click, the waveform through the timing map, the ruler, zoom, and
       marker editing with snapping to transients (TS13);
     * read-only marker ticks in the arrangement.
   * About 6–7 days.
4. **Controls.**
   * Deliverables: the Timing section, Make musical / Make one-shot, Alt-resize stretching (`stretchClip`), the warped
     waveform, badges, and source tempo suggestions (TS11).
   * About 3 days.
5. **Later.**
   * Quantise audio, which moves the markers to the grid.
   * Tempo detection from onsets.
   * Freeze and bounce on the same job queue and cache.

## 8. Tests

* **`TimingMap`:**
  * round trips tick ↔ frame with and without markers;
  * the implicit end points;
  * the TypeScript twin against the same table.
* **Commands:**
  * new and recorded files are one-shot, and no command except `setFileTiming` changes a file's mode;
  * switching a file to musical and back leaves every point of every clip at the same time, with one tempo and across
    steps and ramps;
  * a tempo change keeps the ticks of a musical file's clips, but refits one-shot clips;
  * moving a marker changes only the two segments beside it;
  * the stretch-resize maths, its effect on the file's other clips, and its refusal on one-shot files;
  * the marker commands work on one-shot files, in seconds (TS15);
  * undo of each.
* **Rendering:**
  * a musical file at its source tempo, with markers made from the map and no transpose, renders nothing and is
    bit-identical to one-shot playback, including across a tempo ramp;
  * a 1 kHz sine stretched ×0.5, ×0.8, ×1.25 and ×2 keeps its frequency (FFT peak) and fills the expected length;
  * transpose +12 doubles the frequency at an unchanged length;
  * a click file at 120 BPM in a 140 BPM project lands each click on its beat within 1 ms;
  * a moved marker puts its click on the new beat, re-renders only the two segments beside it, and clicks outside
    them stay bit-identical;
  * joins between segments: a sine across a join has no step in level or phase beyond a small tolerance;
  * playback after a seek mid-clip, and across a transport loop wrap and a clip loop wrap, has no gap or jump;
  * repitch at a tempo of ×1.5 raises the pitch by a fifth, and so does the preview of a pending render;
  * export renders whatever is missing and gives the same file twice.
* **Cache:** the key changes with each input it depends on and with nothing else; undo finds the earlier render; the
  size limit removes the least recently used renders first.
* **Onsets:** generated clicks and a plucked tone are found within 2 ms.
* **Speed:** a benchmark of offline R3 on a three-minute stereo take at 48 kHz on the Release build, and of a
  single-marker re-render, sets the numbers the UI may promise (TS9).
* **UI:** checked by hand over CDP. Make musical, marker drags on a recording with transient snapping, Alt-resize and
  its refusal on one-shot files, the warped waveform, and a tempo-lane edit moving musical audio while one-shot audio
  keeps its seconds.

## 9. The owner's answers (2026-11-04)

* Nothing is stretched automatically.
* The timing mode is per file: one-shot or musical, with one-shot the default.
* Recordings need timing markers that can be placed and moved.
* No real-time stretching: offline rendering with Rubber Band R3. DAWg stays free software under the AGPL.
* Markers are edited in a separate audio editor window (TS13).
* Markers are per file. "Make copy" gives a clip its own copy of the pool entry, so that it can be stretched
  differently (TS2).
* Alt + drag on a clip's edge stretches it (TS5).

## 10. Summary of decisions

| # | Decision | Status |
|---|----------|--------|
| TS1 | Rubber Band R3, offline, under GPLv3 inside AGPL DAWg, behind a `Stretcher` interface | Owner |
| TS2 | Timing mode per pool file: one-shot (default) or musical (stretch / repitch), with source tempo and markers; transpose per clip | Owner, with "Make copy" |
| TS3 | Piecewise-linear `TimingMap` (C++ and TS twin), source-tempo rate past the last marker | Recommended |
| TS4 | Switching mode converts with markers so nothing moves | Recommended |
| TS5 | Edits work in ticks; Alt-resize stretches musical files only | Owner |
| TS6 | File format version 8, defaults only | Recommended |
| TS7 | Offline renders in timeline time, played through the one-shot path; ratio-1 segments and repitch not rendered | No real time: owner; the rest: recommended |
| TS8 | Render cache in app data, keyed by everything a render depends on, with an LRU size limit | Recommended |
| TS9 | Background jobs split at markers; repitch preview while rendering | Recommended |
| TS10 | Export renders what is missing first; never the preview | Recommended |
| TS11 | Source tempo suggestions and cached onsets; neither changes the mode | Never automatic: owner; detection: recommended |
| TS12 | Timing section in the inspector, Make musical / one-shot, Alt-resize, warped waveform, badges | Recommended |
| TS13 | Audio editor window for timing markers, snapping to transients; read-only ticks in the arrangement | Owner |
| TS14 | Commands for file timing, markers and stretching | Recommended |
| TS15 | One-shot files have markers in seconds; switching modes converts them | Owner |

## 11. Implementation notes

What was built in steps 1–4, and where it differs from the plan above.

### Step 1: model and repitch

* `FileTiming` (mode, `musicalStyle`, `sourceBpm`, `markers`) is on `AudioFile`, with `suggestedBpm`; `transpose` is
  on `NoteClip` (engine/include/dawg/TimingMap.h, Model.h). Marker ticks and frames are doubles: markers made from a
  tempo map rarely fall on whole ticks.
* **The file format is version 16, not 8.** The format had moved on since the design was written. The migration only
  bumps the version; the parser fills in the defaults.
* `TimingMap` and its twin ui/src/time/timingMap.ts share their test cases (tests/TimeStretchTests.cpp,
  timingMap.test.ts). Before tick 0 the map also runs at the source tempo.
* TS4: `markersFromTempo` pins the frames at each tempo point the file passes through. It samples ramps so that no
  point is more than about 10 µs off, and pins the file's end where the tempo there differs from the source tempo.
  A file that has markers from an earlier musical stint keeps them when it is switched to musical again, so its
  timing is restored. Each clip then keeps the frame at its left edge.
* Not built: the command result that reports clips shifting when their starts have different tempos (TS4). Commands
  return only the project, so this needs a way to carry warnings first.

### Step 2: offline rendering

* Rubber Band 4.0.0, single-file build, through `Stretcher` (engine/src/RubberBandStretcher.cpp). The options are R3,
  offline, formants preserved, high-quality pitch, channels together, and no threads of its own.
* Musical clips compile to *pieces* (`CompiledAudioPiece` in AudioPlayer.h):
  * the file itself where a segment's ratio is 1 (within 1e-9) and there is no transpose;
  * a render where the segment is stretched;
  * a repitch preview while that render is made, and everywhere in the repitch style.

  Neighbouring pieces that play the file the same way merge, so a file made musical but not retimed plays as one
  piece. Its output matches one-shot playback to within 1e-5, or 1e-3 along a tempo ramp, where the declick follows
  the timeline's ticks. It is not bit-identical: the sums are done in a different order.
* Renders cover one marker segment, cut into chunks of at most 8 s. The half second of context on each side runs at
  the segment's own rate, so a render depends only on its own segment, and moving a marker re-renders the two
  segments beside it.
* **Joins.** Crossfading two separate renders of tonal audio cancels it: a phase vocoder doesn't keep the source's
  phase, so the two sides of a join are out of phase.
  * Each piece after a join with a render therefore plays at a constant offset (`CompiledAudioPiece::shift`). It is
    the offset, within ±10 ms, at which it correlates best with the piece before it over 20 ms around the join.
  * Of offsets that correlate nearly as well, the one nearest 0 wins, so offsets stay within ±10 ms and never add up
    along a clip.
  * A moved marker can change the offsets of the pieces after it (by at most 10 ms), but not the audio before it.
  * The offsets are cached with the renders' keys.
  * The first version slid the outgoing tail into line instead. That changed the speed by up to 5 % before each
    join, an audible pitch wobble of almost a semitone on a recording with many markers. A test now checks that the
    pitch holds just before every join.
* **Speed** (the section 8 benchmark, Release, this machine): R3 offline runs about **5.6× real time on one core**.
  A three-minute stereo take takes 32 s on one core, and the two 5 s segments of a marker move take 1.7 s. That is
  slower than TS9 expected. Hence the 8 s chunks, which spread a long segment over the workers (one per spare core).
  The UI promises no times.
* Rubber Band's transient timing at large ratios: a click at the end of a segment stretched ×1.8 lands about 5 ms
  early. At the ratios of typical timing fixes, the tests' clicks land within 1 ms.
* `StretchRenderer` keeps the cache in `<app data>/Cache/stretch` and the job queue. The key is a hash of the file's
  identity (path, size, modification time), the input range, the key frames, the output length, the pitch and the
  stretcher's identity. These follow from the markers, source tempo, tempo map and transpose (TS8), but are exactly
  what the render depends on. The LRU limit defaults to 10 GB and is settable through `audio.setStretchCacheLimit`;
  there is no settings page for it yet. Each compilation drops the queued renders it no longer asks for.
* When renders are ready, the app compiles again, and the `dawg_stretch` event lists the clips still waiting and
  those just served. The arrangement draws a thin bar on waiting clips. **Not built:** the crossfade when a render
  replaces the preview while the clip plays; it swaps at the next compilation.
* Export (TS10) renders what is missing first, in a "stretching" state with progress, then compiles and exports.

### Step 3: the audio editor and markers

* The marker commands keep a moved marker between its neighbours, so a drag never reorders markers. `copyPoolFile`
  was refused for one-shot files, like the other commands (TS14), until one-shot files got markers (TS15).
* Onsets (engine/src/Onsets.cpp): spectral flux of the mono sum (1024-frame windows, 128-frame hop) with an adaptive
  threshold. Each peak is refined to the attack in the waveform. Clicks and plucks are found within 2 ms. The app
  finds them **when the audio editor first asks**, not on import, and caches them in `<app data>/Cache/onsets`.
* The audio editor is a second instance of the editor window class (PianoRollWindow, which now takes a title and a
  settings key) loading `?window=audio&clip=<id>`. Double-clicking an audio clip opens it, or points the open one at
  the clip (`dawg_audioEditorClip`).
  * It draws the whole file in the arrangement's bars, as the clip plays it on its first pass.
  * The Timing section sits in a side panel rather than the header.
  * It has a play button and the playhead. Clicking the ruler seeks.
* Not built: a context menu on pool files (DAWg has no pool view yet; the clip's menu has the entries).

### Step 4: controls

* **`stretchClip` scales the source tempo by new length / old length**, as it does the markers' ticks. The design
  said old / new for the tempo, but with content in musical ticks a longer clip needs fewer frames per tick, which
  is a higher source tempo.
* The readout during an Alt-drag shows the ratio and the resulting source tempo. A refused Alt-drag on a one-shot file
  shows the hint in the error bar.
* Source tempo suggestions are made when a file enters the pool. They come from the WAV acid chunk or Apple Loops'
  beats (read on import), then the name, then the bar guess. Recordings get none.
* The mode badge is a note symbol, or "R" for repitch, in the name bar. It turns amber where a segment the clip
  plays is stretched beyond ×0.5 to ×2.
* "Clear stretch cache" is in the File menu, with the cache's size.

### Checked by hand (over CDP, 2026-11-04)

* An imported "Clicks 120bpm.wav" came in one-shot, with 120 BPM suggested.
* Alt + drag on it was refused with the hint, and nothing changed.
* Make musical from the inspector worked, and so did a tempo change to 100 BPM: the clip kept its 16 beats, and its
  two render chunks appeared in the cache. `dawg_stretch` reported the clip pending, then ready, within 0.6 s.
* The clip's menu shows the timing entries.
* Double-click opened the audio editor, with the waveform and transients. A double-click 4 px off a click added a
  marker at the click's onset; dragging it snapped to the grid and stretched the audio before it. Delete removed it,
  and undo brought it back. The arrangement showed the warped waveform and the marker tick.
* Alt + drag stretched the clip to 20 beats (source tempo 150 BPM), and "Use 120 BPM" appeared.
* Export after clearing the cache went through "stretching" before writing.
* In the export, repitch put every click on its beat. Stretch put them 2–6 ms early, at ratios of ×1.7 to ×2.2:
  Rubber Band's transient timing, as noted under step 2.
* Not tried by hand: Shift + drag, box selection, and playing audio out loud.
