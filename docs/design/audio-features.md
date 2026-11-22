# Design: audio clip operations, crossfades, punch recording, freeze and bounce

Status: **implemented**, steps 1–7 (2026-11-09); the owner's answers are in section 9, and section 11 has the notes
on what was built, with what differs from the plan. The decisions are numbered AU1–AU17 and summarised in
section 10.

These are the "missing audio features" left on the gap list after time stretching (phase 18). The owner put them
before MIDI tools and DAWproject.

## 1. Goals and non-goals

Goals:

1. **Clip operations.** Reverse and normalise per clip. Slicing an audio clip into a Sampler, with one zone per slice
   and a MIDI clip that plays them. Audio to MIDI: drums (onsets) to notes, and monophonic pitch to notes.
2. **Freeze, bounce in place and render selection.** Freeze renders a track to audio and plays that instead of its
   live chain until it is unfrozen. Bounce in place replaces clips with rendered audio. Render selection renders a
   time range of selected tracks to a new audio track.
3. **Crossfades** between overlapping audio clips on one track: creation, shapes, editing in the arrangement, and
   the engine.
4. **Punch in and out recording**, with pre-roll, and with loop recording into takes.

Non-goals for v1:

* **Polyphonic audio to MIDI** (chords, piano, guitar). Monophonic pitch and drum onsets only.
* **Drum classification by a trained model.** Drum onsets map to pitches by a simple spectral rule at most (AU4).
* **A sample editor** that edits audio destructively (draw, silence, cut inside a file). Reverse writes a new file,
  never changes one.
* **Freezing group and return tracks.** Instrument and audio tracks only (question 9).
* **Crossfades between clips on different tracks**, and crossfades of note clips.
* **Auto punch on MIDI controllers' foot switches** (punching in by hand while playing). Punch is a range.
* **Loudness normalisation.** Normalise is to a peak level (question 2).

## 2. What exists today

* **Clips** (`NoteClip`, Model.h) are note or audio clips. An audio clip has a pool file (`fileId`), gain, fades
  in and out with a shape (`FadeShape`: linear, equal power, exponential), a loop range and `transpose`. Content
  positions are ticks of content time (one-shot files) or musical ticks (musical files, time-stretching.md TS2).
* **Takes lanes** (takes-lanes.md). Clips on a track stack: where they overlap, the one with the highest `order`
  plays (T1). `playedClips()` (Model.cpp) cuts the covered clips around the ones above them with `clipPart()`,
  which gives cut edges no fade. Playback, export and MIDI export all go through it (T8). Recordings go into a new
  lane on top (T3), and loop recording makes one take per pass (T4, `loopTakes` in MainComponent.cpp).
* **The audio player** (AudioPlayer.cpp) plays each compiled clip through a 16-tap sinc interpolator, with the clip's
  gain, fades and a 2 ms declick at edges without a fade. Overlapping compiled clips simply sum.
* **Recording** (AudioEngine.cpp `recordChunk`, MainComponent.cpp `startRecording` / `finishRecording`): armed audio
  tracks write their input to a WAV file from the record position on. A count-in seeks one bar back and plays from
  there. On stop, the take becomes an `addAudioClip` per take (or per loop pass), shifted earlier by the round-trip
  latency.
* **Offline rendering** (Export.h `renderRange`, ExportControl.cpp): the device callback is removed, the engine
  renders a range block by block, the mix or one track as a stem, with an automatic tail.
* **The stretch renderer** (StretchRenderer.h): a job queue of worker threads and a cache keyed by a hash of what a
  render depends on. Its jobs are pure functions of a file and a timing map.
* **Onsets** (Onsets.h `detectOnsets`): spectral flux, each onset refined to its attack, cached per file in
  `<app data>/Cache/onsets`, shown in the audio editor.
* **The Sampler** (sampler-midi-delay-limiter.md): `GraphNode::sampleZones`, each zone a pool file with a frame
  range, a key range, a root key, a velocity range, a mode (one-shot, gate, loop) and an envelope.
* **The audio editor** window (TS13): the file's waveform with the clip's range, transients and timing markers. Its
  design said it would later hold reverse, normalise and gain.

Two things are missing that several features need:

* **There is no time selection** in the arrangement, only clip selection and the loop range. Render selection needs a
  range (question 8).
* **Commands are pure functions** of the project. Anything that reads or writes audio (reverse, normalise, slicing,
  audio to MIDI, every render) runs in the app first, on a background thread, and then applies an ordinary command
  with the result. So undo never has to touch the disk.

## 3. Clip operations

### Reverse

**(AU1) Reverse writes a reversed copy of the file and points the clip at it.** The recommendation (question 1).

* `audio.reverseClips { clipIds }` (bridge, app): for each file the clips play, the app writes
  `Audio/<name> (reversed).wav` (32-bit float, the file's own rate and channels) on a background thread, adds it to
  the pool, and applies `reverseClips { clipIds, files: { <fileId>: <new pool entry> } }`.
* The new pool entry carries `reverseOf: <original fileId>`, and the original learns nothing. Reversing a clip of a
  reversed file points it back at the original: no new file, and reversing twice is exact.
* A second clip of the same file reuses the reversed entry that already exists.
* **The clip mirrors around its file.** Its content range `[offset, offset + length)` becomes
  `[fileLength − offset − length, fileLength − offset)`, so the same audio plays backwards in the same place. Its
  loop range mirrors the same way, and its fade-in and fade-out swap (lengths and shapes).
* **Musical files** mirror their timing too. The reversed entry copies the timing settings with each marker mirrored
  (`frame' = frames − frame`, `tick' = contentLength − tick`), so a retimed take reverses with its timing.
* Onsets and peaks of the new file are made as for any new file.
* The alternative is a `reversed` flag on the clip and a player that reads backwards. It needs no file, but every
  reader then has to support it: the sinc reader, the buffering reader (which reads ahead, forwards), the stretch
  renders, onsets, peaks, slicing and audio to MIDI. A reversed file needs none of that.

### Normalise

**(AU2) Normalise sets the clip's gain; the audio is not changed.**

* `audio.normaliseClips { clipIds, targetDb, together }` (bridge, app): the app finds the sample peak of what each
  clip plays (its content range through its loop, in file frames; the timing map for musical files) and applies
  `setClipProps { gainDb }` for each clip in one batch, one undo step.
* `gainDb = targetDb − peakDb`. A small dialog asks for the target each time (the owner's answer to question 2),
  remembering the last one (0 dBFS at first).
* With `together`, all the clips get the same change, set by the loudest of them, so their balance stays. The menu
  item is "Normalise" for one clip and "Normalise (together)" / "Normalise (each)" for several.
* The peak ignores fades and is taken from the source file. A stretched render's peak can differ by a fraction of a
  dB; that is accepted.
* Silent clips are left alone. The gain is clamped to clip gain's range (at most +12 dB), so a very quiet clip may
  stay below the target; the dialog says so.

### Slicing to a Sampler

**(AU3) "Slice to Sampler" makes a new instrument track with a Sampler and a MIDI clip that plays the slices.**

* **Where slices go:** the clip's content range is cut at one of
  * **transients**: the file's cached onsets (TS11) above a sensitivity, with a minimum slice length (30 ms);
  * **the grid**: every 1/4, 1/8, 1/16 or 1/32 of the clip's timeline, through its tempo (and timing map);
  * **timing markers**, for musical files.
* **The audio editor shows the slice lines** as the settings change, before anything is made. The dialog has the
  mode, the sensitivity or grid, the start key, and the zone mode.
* **The Sampler.** One zone per slice: the same pool file (no copies), `start` and `end` at the slice's frames,
  `lowKey = highKey = rootKey` = the slice's key, ascending from C1 (36) by default. One-shot mode by default, gate as
  the option. A 1 ms attack and a 5 ms release take the clicks off cut edges; one-shots get the 5 ms fade at their
  end the Sampler already has.
* **The MIDI clip** sits where the audio clip was, with one note per slice at the slice's timeline position, as long
  as the slice (to the next slice), at velocity 100. Played as is, it sounds like the audio clip; moved, quantised or
  reordered, it rearranges the slices.
* **Musical files**: slice positions come through the timing map, so the notes land on the clip's timeline even after
  a tempo change. Each zone then plays its slice at its own speed: slicing is how a loop follows the tempo without
  stretching.
* **Limits:** at most 128 slices; past 92 the start key moves down to fit. Beyond 128, the dialog refuses.
* **The source clip stays** on its track as it is (clips have no mute of their own), and the new track is added
  right after that track (question 5).
* The command is `sliceToSampler { clipId, slices: [frame…], startKey, mode }`, pure: the app computes the frames
  (onsets or grid) and the command builds the track, the Sampler node, its zones and the clip.

### Audio to MIDI

**(AU4) Two converters, both making a new instrument track with a MIDI clip in the audio clip's place.**

* **Drums / onsets to notes** (`audio.drumsToMidi`):
  * one note per onset above the sensitivity, at the onset's timeline position;
  * the velocity from the peak level in the 10 ms after the onset (−40 dBFS → 1, 0 dBFS → 127, linear in dB);
  * the length to the next onset, at most a sixteenth;
  * the pitch: one key (C1, 36) by default. With "Split kick / snare / hats", each onset's spectral centroid in its
    first 30 ms picks 36, 38 or 42 by two fixed boundaries (question 6). The new track gets a drum kit instrument if
    one exists, else the default instrument.
* **Monophonic pitch to notes** (`audio.pitchToMidi`):
  * **pitch detection:** YIN with a cumulative mean normalised difference, a 2048-frame window and a 256-frame hop at
    48 kHz (5.3 ms), 50 Hz–2 kHz, with parabolic interpolation; a frame is voiced where the YIN minimum is below 0.15
    and the level is above −50 dBFS. Written in the engine (`dawg/PitchDetect.h`), no new dependency;
  * **notes:** voiced runs, split where the pitch moves by more than 0.6 semitone and stays there for 40 ms, or at an
    onset; runs shorter than 40 ms are dropped;
  * **each note's base pitch** is its median, rounded to the semitone, so the notes sit on the piano roll's keys;
  * **the pitch curve:** the deviation from the base pitch becomes the note's `pitch` expression curve, thinned to
    points about 20 ms apart (and fewer where it is flat). Vibrato, bends and slides are kept, which is what the MPE
    note model is for. An option, "Notes only", leaves the curves out;
  * the velocity from the note's peak level, as for drums;
  * an option adds the level envelope as the `pressure` curve.
* Both run on a background thread in the app (the audio is read through the timing map for musical files) and apply
  `addTrack` plus `addClip` with the notes in one batch.
* Both work on the clip's played range only, and take the file's first two channels' mono sum.

## 4. Crossfades

**(AU5) A crossfade is a clip's fade over a clip below it.** No new object.

* Where a clip in the stack covers another (T1), the top clip's **fade-in** at its left edge, or its **fade-out** at
  its right edge, becomes a crossfade if a clip below plays right across that edge. The covered clip then keeps
  playing under the fade, with the complementary fade.
* So the model already holds everything: a crossfade is the top clip's fade length and shape. Moving the top clip,
  trimming it, deleting it or lifting another clip over it all keep crossfades right without bookkeeping, and flatten
  takes (T7) keeps them as fades.
* **The crossfade lies inside the top clip,** starting at its edge: the covered clip plays on under it for the fade's
  length. Moving the edit point is trimming the top clip's edge, as today. A crossfade centred on the edge would need
  the top clip to have audio before its start, which clips don't (their left edge is where their content begins);
  question 3 asks whether centred matters.

**(AU6) The engine extends the covered part under the fade.** In `compileAudio`, after `playedClips()`:

* for every part cut at an edge where a clip above it fades in, the part reaches on past the cut by the fade's length
  (no further than the covered clip's own end), with a fade-out of that length and the same shape;
* likewise, a part that resumes after a clip above fades out starts that much earlier, with a fade-in;
* nothing else changes: the parts are ordinary compiled clips, and the player already sums overlapping clips.
* **Complementary shapes.** `fadeGain (shape, x)` is used mirrored for fade-outs, so the pair sums to unity gain
  for linear (x + (1 − x)) and to unity power for equal power (sin² + cos²). That covers both cases that matter:
  linear for the same material (comping takes of one performance, which are correlated) and equal power for
  different material. Exponential stays a fade shape only.
* **A new shape, S-curve** (`FadeShape::sCurve`, `0.5 − 0.5 cos πx`), complementary in gain, smoother at both ends.
  The common default for comp edits in other DAWs.
* Musical clips work the same way: their pieces are planned for the extended part.
* Note clips are not crossfaded: their parts are cut as today.

**(AU7) Short crossfades are made automatically.** Recommended (question 4).

* Whenever an audio clip is placed over another on its track (moved, pasted, recorded, or a comp pick, T6) and an
  edge of it has no fade, that edge gets a 10 ms equal-power crossfade, converted to the clip's content ticks.
* The fade is only set where the edge actually covers audio. A clip placed into an empty part of a lane gets nothing.
* A setting turns it off (`audio.autoCrossfade`, on by default) and sets the length (5–50 ms).
* This replaces the 2 ms declick at those edges with something that doesn't dip: a declick fades both sides down,
  which is a 2 ms gap at every comp edit.

**(AU8) Editing in the arrangement.**

* **Drawing:** a crossfade is drawn across both clips as an X: the top clip's fade curve and, over the covered clip's
  extension, the mirrored curve, in the covered clip's colour. In expanded takes lanes, the covered take shows its
  extension too.
* **Handles:** the existing fade handle at the top clip's edge sets the length. Dragging the clip's edge moves the
  edit point (trims). Alt + drag on the crossfade's middle moves the edit point and keeps the length.
* **The crossfade's menu** (right-click on it): Linear, Equal power, S-curve, and Remove crossfade (length 0, so the
  declick applies again).
* **Selection of several edges:** "Crossfade selected" in the clip menu applies the default length to every covering
  edge of the selected clips. Setting the shape in the inspector applies to all of them.
* The clip inspector's fade controls show "Crossfade" when the fade is one.

**(AU9) File format.** `FadeShape::sCurve` is added. Version 17 (or the next free number when this lands): no
migration, the existing shapes keep their numbers. AU1's `reverseOf` and AU11's `punch` and AU13's `freeze` land
in the same or later versions, each as its step lands.

## 5. Punch recording

**(AU10) A punch range of its own.** Recommended (question 7).

* `Project.punch { enabled, start, end }`, beside `loop`. It shows in the ruler as a second bar, red, under the loop
  bar. Dragging on that strip of the ruler sets it, as the loop bar does.
* A **Punch** button in the transport toggles it; `P` is the shortcut. "Set punch to loop" and "Set loop to punch"
  sit in the ruler's menu.
* The alternative, punching on the loop range (Ableton's way), makes loop recording and punching one thing: a loop
  range can't be a punch range inside a longer loop (AU12).

**(AU11) Recording with punch on.**

* **The file still records from the start of playback,** pre-roll included, to the punch-out plus one second. It is
  the take's handles: the clip's edges can be dragged out to hear more, as with any clip.
* **The clip covers the punch range only.** It goes on top in a new lane (T3), so it plays over what was there,
  which stays whole underneath. Punching is therefore non-destructive for free.
* Its edges get auto crossfades (AU7) over what they cover.
* **Notes:** only notes that start inside the punch range are kept. Notes still held at the punch-out are cut there.
  Controller curves are cut at both ends.
* **When to stop:** playback goes on after the punch-out until the user stops, as with any recording; the file stops
  growing one second after the punch-out. An option stops the transport one bar after it (question 7).
* **The record button** with punch on doesn't start recording at the playhead: recording starts at the punch-in,
  and playback starts at the punch-in minus the pre-roll (AU12). If the playhead is already before that point,
  playback starts from the playhead.

**(AU12) Pre-roll replaces the one-bar count-in.**

* A **pre-roll** setting in bars: 0, 1, 2 or 4 (default 1). The count-in button becomes the pre-roll's switch: on,
  recording begins after the pre-roll, as the count-in does now; off, it begins at once.
* **With punch on,** the pre-roll is before the punch-in. With punch off, it is before the playhead, as today.
* The metronome plays during the pre-roll if it is on. A separate "click during pre-roll only" is left out.
* The pre-roll is a setting of the app, not of the project (`record.preRollBars`).

**(AU13) Punch with loop recording.**

* **Punch inside the loop:** each full pass records the punch range as a take, the newest on top (T4). The passes all
  go into one file, as now, and `loopTakes` cuts each pass to the punch range instead of the loop range.
* Playback starts at the punch-in minus the pre-roll, loops at the loop end back to the loop start, and keeps
  recording until the user stops. The partial last pass makes no take, as today.
* **Punch partly or wholly outside the loop:** the punch range is clipped to the loop; if nothing is left, recording
  is refused with "Punch range is outside the loop".
* **Note overdub** (`record.loopOverdub`): passes merge as now, but only inside the punch range.

## 6. Freeze, bounce and render

### Rendering track graphs

**(AU14) Track renders run in the engine, offline, like an export.** A deviation from time-stretching.md (TS10),
which said freeze and bounce would use the stretch renderer's job queue.

* That queue runs pure functions of a file. A track render runs the track's graph: its instrument, its plugins,
  their state and automation. Hosted plugins have one instance each, which the live engine is using, and many can't
  render faster than real time on another thread. So track renders go through `renderRange` with the device
  callback removed, as export does: playback stops while a track freezes, with a progress bar and Cancel.
* What carries over from the stretch renderer is the keyed cache: a render's key is a hash of everything it depends
  on (the track's clips, its graph and plugin states, the tempo map over the range, the files it plays, and the
  tracks it is keyed from), and a render with the same key is reused instead of made again.
* **The tap point** is new: the track's output **before** its fader, pan, sends and output (`renderRange` gains a
  `preFader` flag beside `stemTrack`). Freeze and bounce use it; render selection uses the post-fader mix.
* Stretched audio is rendered first, as export does (TS10).
* **Range:** from the track's first clip start to its last clip end, plus the automatic tail (−90 dBFS for a second,
  at most 30 s).
* **Sidechain keys** from other tracks are rendered in as they were.

### Freeze

**(AU15) Freeze renders the track's own chain and plays the render instead.**

* `audio.freezeTrack { trackId }` renders as AU14 into `Audio/Frozen/<track> <key>.wav` (32-bit float, in the
  project, so a frozen track plays where its plugins aren't installed), then applies
  `freezeTrack { trackId, file, start, key }`.
* The track gains `freeze { fileId, start, key }`. The file is a pool entry marked `frozen`, hidden from the pool and
  the sound explorer.
* **The compiler** gives a frozen track an audio player on the render in place of its graph: the clips, instrument and
  inserts are not run. Fader, pan, mute, solo, sends, output, automation of gain and pan, and the timing offset stay
  live, because the render is pre-fader.
* **Plugins stay loaded** but aren't processed, so unfreezing is instant (question 9 asks about unloading them).
* **A frozen track is locked:** clip edits, graph edits and node parameter automation are refused with "Unfreeze
  the track to edit it", and the arrangement draws its clips greyed with a snowflake in the header. Moving the track,
  renaming it, its fader and its sends are not locked.
* **Unfreeze** (`unfreezeTrack`) drops `freeze`. The file is kept until the project is saved without it, so freezing
  again without changes finds it by its key and is instant.
* **Stale freezes:** a tempo map change, or a change to a track that keys the frozen one, makes the freeze stale (its
  key no longer matches). The track keeps playing the render, and its header shows a warning with "Refreeze".
* **Output tracks** (docs/design/multi-output-instruments.md): the same render also taps each Output to track node
  that feeds an output track of the frozen track (`ExportRange::taps`, rendered to output channel pairs after the
  first in one pass, so the renders stay sample-aligned), into `Audio/Frozen/<track> <key> <node>.wav`.
  `freezeTrack` takes them as `outputs: [{ node, file }]` and the track keeps them in `freeze.outputs`. While frozen,
  each Output to track node plays its render through an audio player of its own (one per pool file), so the output
  tracks, with their own chains and faders, play on. An Output to track node without a render is silent.
* Instrument and audio tracks only; groups and returns are refused (question 9).

### Bounce in place

**(AU16) Bounce in place renders clips through the chain and puts the audio back where they were.** Recommended
(question 10).

* **On an audio track,** two variants in the clip menu:
  * **"Bounce in place"**: the selected clips' span is rendered through the track's chain, pre-fader. The clips are
    replaced by one audio clip of the render, and the track's inserts are bypassed (so they don't apply twice), not
    removed, so they can be switched back on.
  * **"Consolidate"**: the clips only, without the chain: what they play (gain, fades, crossfades, stretching,
    reverse, the top take where they stack) in one new file. This is the glue that audio clips lack. It needs no
    engine render: the audio player alone renders it, on a background thread.
* **On an instrument track,** which can't hold audio clips: the selected clips (or the whole track with none
  selected) are rendered through the instrument and the inserts into a **new audio track** placed right after it,
  with the original's name + " (audio)", colour, fader, pan, sends and output. The original track is muted, not
  deleted, so it can be gone back to.
* Each is one undo step. Undo leaves the rendered file in `Audio/`, unused, as recordings that are undone do.

### Render selection

**(AU17) Render selection mixes the selected tracks over a range into a new audio track.**

* The range is the loop range if it is on, else the span of the selected clips (question 8; the arrangement has no
  time selection).
* The selected tracks are rendered **post-fader** (as they sound in the mix, sends left out), summed, into
  `Audio/<first track> render.wav`, and a new audio track with that clip goes after the last selected track. The
  source tracks are left as they are.
* The tail is included up to the automatic tail's end, so a reverb's decay isn't cut.
* `renderRange` gains a track set (`std::vector<ObjectId> tracks`) in place of the single `stemTrack`.

## 7. Commands and bridge calls

Pure commands (Commands.cpp), each one undo step:

| Command | Args |
|---|---|
| `reverseClips` | `clipIds`, `files: { fileId: AudioFile }` (the reversed entries to add) |
| `setPunch` | `enabled?`, `start?`, `end?` |
| `freezeTrack` | `trackId`, `file: AudioFile`, `start`, `key` |
| `unfreezeTrack` | `trackId` |
| `bounceInPlace` | `trackId`, `clipIds`, `file: AudioFile`, `start`, `mode: "inPlace" \| "consolidate" \| "newTrack"` |
| `addRenderTrack` | `after`, `file: AudioFile`, `start`, `name` |
| `sliceToSampler` | `clipId`, `slices: [frame…]`, `startKey`, `mode` |
| `crossfadeClips` | `clipIds`, `length?`, `shape?` (every covering edge of the clips) |

Audio to MIDI and normalise apply existing commands (`addTrack`, `addClip`, `setClipProps`) in a batch.

Bridge calls (MainComponent, background work with a `dawg_job` progress event and Cancel): `audio.reverseClips`,
`audio.normaliseClips`, `audio.sliceToSampler`, `audio.slicePoints` (the preview), `audio.drumsToMidi`,
`audio.pitchToMidi`, `audio.freezeTrack`, `audio.unfreezeTrack`, `audio.bounceInPlace`, `audio.renderSelection`.

## 8. Tests

* **Reverse:** a ramp file reversed plays the mirrored ramp sample for sample in the clip's place; fades swap; a
  looped clip's loop mirrors; reversing twice gives back the original clip exactly; a second clip reuses the reversed
  entry; markers of a musical file mirror.
* **Normalise:** a clip peaking at −12 dBFS gets +12 dB; "together" keeps the difference between two clips; a
  silent clip is unchanged; the cap.
* **Crossfades (engine):** two DC clips stacked with an equal-power fade sum to constant power across the edit; with
  linear and S-curve, two copies of one sine sum to the sine at unit gain (no dip, no bump, within 1e-4); the covered
  part stops at its own end when it is shorter than the fade; a fade-out over a covered clip resumes it; musical
  clips crossfade the same way; flattening keeps the fades.
* **Auto crossfades:** placing a clip over another sets 10 ms at the covering edges only; nothing over empty space;
  the setting off sets nothing.
* **Punch:** recording from before the punch-in gives a clip covering exactly the punch range, on top, with the file
  holding the pre-roll and one second after; notes outside the range are dropped and held notes cut; with a loop, one
  take per pass, each covering the punch range; a punch outside the loop is refused; pre-roll 0, 1, 2 bars start
  playback where they should.
* **Freeze:** a frozen track's output equals the live track's pre-fader output within 1e-6 (built-in nodes), with
  its fader, pan and sends still applied live; edits are refused while frozen; unfreeze restores live playback;
  refreezing without changes reuses the file; a tempo change marks it stale; no allocation on the audio thread with a
  frozen track (Debug).
* **Bounce:** in place equals the original playback within 1e-6 with the inserts bypassed; consolidate equals the
  clips' summed playback (crossfades and stretching included); the instrument-track variant makes the new track and
  mutes the original.
* **Render selection:** the new track's clip equals the selected tracks' post-fader sum over the range.
* **Slicing:** onset slices of a click file put one zone per click; grid slices at 1/16 of a 120 BPM bar; the MIDI
  clip through the Sampler reproduces the clip within the declick fades; keys move down past 92 slices; 129 slices
  are refused.
* **Audio to MIDI:** a click track gives one note per click within 2 ms; the drum split puts a sine kick, a noise
  burst and a high noise burst on 36, 38 and 42; a sine sequence C4 E4 G4 gives those notes within 10 ms; a sine
  with ±50 cent vibrato gives one note with a pitch curve of that depth; a glide gives one note with a ramp, or two
  notes past the split threshold.
* **UI (vitest):** the crossfade geometry (drawing and hit testing of the X), the punch bar in the ruler, the slice
  preview positions, and the frozen-track lock in the editing hooks.
* **By hand over CDP,** in a separate dev instance: each menu entry, a crossfade drag and shape change, punch
  recording with a loop (with a loopback input), freeze and unfreeze with a plugin.

## 9. The owner's answers (2026-11-09)

The recommended option for every question, except question 2: Normalise asks for its target in a dialog. The
implementation order below stands.

The questions as asked:

1. **Reverse:** write a reversed copy of the file (AU1, recommended), or a `reversed` flag on the clip that every
   reader must support?
2. **Normalise:** to a sample peak of 0 dBFS (recommended), −0.1 dBFS, or a target set in a small dialog each time?
   Loudness (LUFS) normalisation later?
3. **Crossfade position:** inside the top clip, starting at its edge (AU5, recommended), or centred on the edge
   (which needs the top clip's content to reach before its start)?
4. **Auto crossfades:** a 10 ms equal-power crossfade on every edge placed over audio, on by default (AU7,
   recommended), or only when asked for ("Crossfade selected", the fade handle)?
5. **Slicing:** keep the source clip as it is and add the Sampler track after its track (recommended), or delete
   the source clip (moving it to the new track's place)? And the zones one-shot by default (recommended) or gate?
6. **Drums to MIDI:** one key by default with the kick / snare / hats split as an option (recommended), the split by
   default, or one key only in v1?
7. **Punch:** a punch range of its own (AU10, recommended) or punch on the loop range? After the punch-out, keep
   playing until stopped (recommended), or stop one bar later?
8. **Render selection's range:** the loop range when on, else the selected clips' span (recommended), or add a
   proper time selection to the arrangement first (a larger UI step)?
9. **Freeze:** only instrument and audio tracks (recommended), or groups too (which freeze their members)? Keep
   plugins loaded while frozen (recommended: unfreezing is instant) or unload them to save memory?
10. **Bounce in place on an instrument track:** a new audio track after it, the original muted (AU16, recommended),
    or replace the instrument track (its instrument and inserts removed)?

The recommended implementation order (each step one commit, with tests):

1. **Reverse and normalise** (AU1, AU2): `reverseOf`, the commands, the app's file writing, the clip menu and the
   audio editor's buttons.
2. **Crossfades** (AU5–AU9): the S-curve, the engine's extension, auto crossfades, `crossfadeClips`, drawing and
   editing.
3. **Punch and pre-roll** (AU10–AU13): the model, the record window, punch in loop takes, the ruler, the transport
   button and the setting.
4. **Track renders and freeze** (AU14, AU15): `renderRange` with a pre-fader tap and a track set, the render key,
   the frozen player in the compiler, the lock, the UI.
5. **Bounce in place, consolidate and render selection** (AU16, AU17).
6. **Slice to Sampler** (AU3), with the slice preview in the audio editor.
7. **Audio to MIDI** (AU4): drums, then monophonic pitch.

## 10. Summary of decisions

| # | Decision | Status |
|---|----------|--------|
| AU1 | Reverse writes `<name> (reversed).wav`, linked by `reverseOf`; the clip, its loop, fades and markers mirror | Owner (recommended) |
| AU2 | Normalise sets clip gain from the played range's sample peak; each or together; within clip gain's +12 dB | Owner: target in a dialog |
| AU3 | Slice to Sampler: transients, grid or markers; one zone per slice on the same file; a MIDI clip of the slices | Owner (recommended) |
| AU4 | Audio to MIDI: onsets to notes (optional 3-way split), YIN pitch to notes with pitch curves | Owner (recommended) |
| AU5 | A crossfade is a top clip's fade over a covered clip; inside the top clip | Owner (recommended) |
| AU6 | The compiler extends covered parts under the fade with the mirrored shape; S-curve shape added | Recommended |
| AU7 | Automatic 10 ms equal-power crossfades on edges placed over audio, with a setting | Owner (recommended) |
| AU8 | Crossfades drawn as an X, edited with the fade handle and a menu; Crossfade selected | Recommended |
| AU9 | File format: S-curve, then `reverseOf`, `punch`, `freeze` as their steps land | Recommended |
| AU10 | A punch range of its own, shown in the ruler, with a transport button | Owner (recommended) |
| AU11 | Punch records the whole pass with handles; the clip covers the punch range, on top, crossfaded | Recommended |
| AU12 | Pre-roll in bars replaces the one-bar count-in | Recommended |
| AU13 | Punch inside a loop: one take per pass over the punch range | Recommended |
| AU14 | Track renders run offline in the engine, as export, with a keyed cache and a pre-fader tap | Recommended (deviation from TS10) |
| AU15 | Freeze plays a pre-fader render in place of the graph; the track is locked; stale freezes warn | Owner (recommended) |
| AU16 | Bounce in place (through the chain, inserts bypassed), Consolidate (clips only), new audio track for instruments | Owner (recommended) |
| AU17 | Render selection: selected tracks post-fader over the loop or the clips' span, to a new audio track | Owner (recommended) |

## 11. Implementation notes

### Step 1: reverse and normalise

* `AudioFile::reverseOf` (Model.h), format version 17. The helpers are in `dawg/ClipOps.h`: `fileContentTicks`,
  `reversedTiming`, `mirrorClip`, `playedFrameRanges`, `normaliseChanges` and `writeReversed`. Tests in
  tests/ClipOpsTests.cpp.
* `reverseClips { clipIds, files }` is as AU1 describes. A reused reversed copy keeps its own timing, as a copy made
  with "Make copy" does: marker edits made to the original after the copy was made don't reach it.
* A looped clip mirrors its loop range, and its first pass keeps its length, now ending at the mirrored loop's end, so
  each pass plays backwards and reversing twice is exact. (The whole clip isn't the exact reverse of its playback
  when the last pass is partial: that can't be expressed as a clip.)
* A clip whose content reaches past its file's end starts at the copy's start after reversing (there is no content
  before a file's start), so reversing such a clip twice moves its content.
* The app (`audio.reverseClips`, `AudioLibrary::reverseFiles`) writes the copies as 32-bit float WAV in the project's
  Audio folder on its audio job threads, then applies the command. Undo leaves the file there, unused.
* Normalise (`audio.normaliseClips`, `AudioLibrary::peaks`) reads the peak of the played frame ranges with
  `readMaxLevels`. **Clip gain is capped at +12 dB** (`limits::maxGainDb`), so very quiet clips stop short of the
  target; the dialog says so. The target comes from a dialog each time (the owner's answer), remembered in the UI's
  local storage.
* The menu entries are in the clip menu. **Not built:** the buttons in the audio editor window, which is a separate
  web view without the dialog; the clip menu covers both.
* No progress event (`dawg_job`): both finish in well under a second for typical clips.

### Step 2: crossfades

* `crossfadedParts` (ClipOps.h) replaces `playedClips` in `compileAudio` and `stretchJobs`: a part cut where a clip
  above starts with a fade-in (or ends with a fade-out) reaches on under that fade, as far as its own clip goes, with
  the mirrored fade of the same shape. Musical clips plan their pieces for the extended part. Tests in
  tests/CrossfadeTests.cpp: linear and S-curve over the same material sum to the level within 1e-4, equal power
  follows sin + cos, a short covered clip stops at its own end.
* `FadeShape::sCurve` (3) is the fourth fade shape, in the inspector, the clip menu and the drawing.
* **The automatic crossfade length is a project setting** (`Project::autoCrossfadeMs`, 10 by default,
  `setAutoCrossfade { ms }`), not an app setting as AU7 said: commands are pure functions of the project, and the
  pass that adds them (`autoCrossfades` after every command, in Commands.cpp) has to see it. The Edit menu toggles it
  (on at 10 ms, or off); there is no field for the length yet.
* A clip counts as placed when it is new or its start, length or stack order changed, so moves, trims, pastes,
  recordings and comp picks all get crossfades, and a crossfade set to 0 stays 0 until the clip is placed again. A
  crossfade made while a clip overlapped stays when it is later moved into empty space (it is then a plain fade).
* `crossfadeClips { clipIds, ms?, shape? }` backs the clip menu's "Crossfade" / "Crossfade selected".
* Drawing: the covered clip's mirrored fade is drawn dashed over the top clip's fade, making the X
  (ui/src/timeline/crossfades.ts, the twin of `coveredEdges` and the tail search). The inspector labels such fades
  "Crossfade in" / "Crossfade out".
* **Not built:** a crossfade's own right-click menu (the clip menu's fade shapes do the same), Alt + drag on a
  crossfade to move the edit point, and the extension drawn in expanded takes rows.

### Step 3: punch and pre-roll

* `Project::punch` (`PunchRange`: enabled, start, end), `setPunch`, saved in the project file. `recordedTakes`
  (dawg/Recording.h) turns a recording into clips for every case: the whole file, one take per loop pass, the punch
  range, and the punch range per loop pass. It replaces `loopTakes` in MainComponent.cpp. Tests in
  tests/PunchTests.cpp.
* Recording with punch on starts playback at the punch-in, or the pre-roll before it with Shift+R, or the playhead if
  that is earlier; the file records from where playback starts. Without a loop, the engine stops writing a second
  after the punch-out (`startRecording`'s new `untilBeats`); playback goes on until stopped (the owner's answer).
* Punch with the loop on: each full pass over the punch range (clipped to the loop) is a take. A punch range wholly
  outside the loop refuses to record, with "Punch range is outside the loop".
* Notes recorded with punch: those starting inside the punch range, held notes cut at the punch-out, in a clip over
  the punch range (per pass with the loop on).
* **Pre-roll**: Shift+R (and Shift-click on record) records with the pre-roll, as the count-in did; the pre-roll is
  an app setting of 0, 1, 2 or 4 bars (`record.preRoll`), cycled by a button beside the record button.
* **The punch range is edited with Alt** in the ruler's loop zone (the loop gestures: drag to set, drag its ends or
  body, Alt-click to toggle), and drawn as a red bar along the bottom of the loop zone, rather than a strip of its own.
  A Punch button in the transport and P toggle it. "Set punch to loop" and "Set loop to punch" were not built.
* The recorded clip lands on top in a new lane (T3) and gets automatic crossfades at its edges (AU7).
* Not checked by recording through a real device: the take maths, the record window and the commands are tested; the
  app was built, not run.

### Step 4: track renders and freeze

* **Stem renders take a set of tracks** (`ExportRange::tracks`, `preFader`; `AudioEngine::setExportMode` with up to 64
  tracks). A stem whose group is a stem too goes into its group as usual; the others go straight to the output. With
  `preFader`, the stems are taken before their fader and pan. Export passes one track per stem, as before.
* `Track::freeze` (`TrackFreeze`: render file, start, key); `freezeTrack { trackId, file, start }` adds the render to
  the pool and takes the key; `unfreezeTrack { trackId }`. `freezeKey` (Model.h) hashes the clips, the graph, the node
  and plugin automation lanes, the timing offset, the kind and the tempo map. The project's JSON gives a frozen
  track `freeze.stale` when its key no longer matches.
* **How a frozen track compiles** (`frozenView`): as an audio track playing one clip of the render through a new
  Audio Player (id −track id) into its Track Output, without input or timing offset (the render has the offset in
  it). Its own nodes stay in the graph but nothing connects them to the output, so the compiler keeps their
  instances (plugins stay loaded, unfreezing is instant) and doesn't run them (phase8, G5).
* **The lock**: after every command, a frozen track whose clips or graph the command itself changed refuses the
  command with "Unfreeze the track to edit it". Fader, pan, sends, name, mute and solo stay editable, and so does the
  tempo (which makes the freeze stale).
* The app (ExportControl.cpp) renders through the export machinery: `startTrackRender` makes an export job whose
  progress goes out as `dawg_render`, and whose finished render is moved into place and handed on instead of being
  written as export files. Stretched audio renders first. The render is `Audio/Frozen/<track> <key>.wav`; freezing
  again with the same key finds it and skips the render. "Refreeze" unfreezes first (one undo step), then freezes.
* UI: Freeze / Unfreeze / Refreeze in the track menu, a snowflake (or a warning when stale) before the track's name,
  the frozen track's clips drawn faded, and a progress banner with Cancel while a render runs.
* Tests in tests/FreezeTests.cpp: the pre-fader render leaves out the fader, a frozen instrument track plays its
  render (within 1e-4 of the live output past the first 2 ms) with its fader live, unfreezing gives the live output
  back exactly, edits are refused, the key follows what it should, and playback doesn't allocate (Debug).
* **Not built:** the key doesn't include the content of audio files on disk, or the tracks that key the frozen
  track; a sidechain from another track is baked into the render, and a change to it doesn't mark the freeze stale.
  The app was built, not run.

### Step 5: bounce in place, consolidate and render selection

* **Bounce in place works on the whole track**, not on selected clips: on an audio track with inserts, bypassing the
  inserts after bouncing only some clips would take the effects off the clips left as they were. So the track menu's
  "Bounce in place" renders every clip of the track through its chain, before the fader. An audio track's clips are
  replaced by one clip of the render and its inserts are bypassed (not removed); an instrument track gets a new audio
  track right after it (`addRenderTrack` with `like`: its colour, fader, pan and sends) and is muted. One undo step.
* **Consolidate** (clip menu, audio clips of one track) is the per-clip operation: what the selected clips play
  (gain, fades, crossfades, stretching, the top take), without the track's chain, as one clip in their place. It
  renders `clipsRenderProject` (ClipOps.h: the track with only those clips, its Audio Player wired straight to its
  Track Output) through the same offline path; the engine compiles that project for the render and the session's
  again after it.
* **Render selection** (clip menu "Render to new track", track menu "Render to audio"): the chosen tracks after their
  faders and pans (sends left out), summed, over the loop range when it is on, else the selected clips' span, with the
  automatic tail, onto a new audio track after the last of them (`addRenderTrack`).
* Renders go into `Audio/Bounces/`, named after the track, made unique with a number.
* Tests in tests/BounceTests.cpp: a clip render keeps only its clips and dry leaves the inserts out, a render of
  several tracks is their post-fader sum, and `addRenderTrack` places and fills the new track. The app's handlers were
  built, not run.

### Step 6: slice to Sampler

* `ClipFrames` (ClipOps.h) maps a clip's timeline to its file's frames and back (through the timing map for musical
  files). `gridSlices` and `onsetSlices` find the cut points; the app's `audio.slicePoints` returns them for the
  dialog, from the grid, the timing markers or the file's onsets (found on first use, `{ ready: false }` until then).
  Onsets need a strength of at least 1 − sensitivity, and slices are at least 30 ms long.
* `sliceToSampler { clipId, slices, startKey?, mode? }` builds the track: Sequencer → Sampler → the default inserts,
  after the clip's track, in drum mode, with one zone per slice on the same pool file (1 ms attack, 5 ms release,
  one-shot by default) and a MIDI clip in the clip's place with one note per slice at velocity 100. Keys start at C1
  (36), lower when the slices wouldn't fit below 127; more than 128 slices are refused, and so are looped clips (unloop
  first).
* The dialog (clip menu "Slice to Sampler…") picks the mode, the sensitivity or grid, the first key and gate mode, and
  shows how many slices that makes. **Not built:** the slice lines previewed in the audio editor.
* Tests in tests/SliceTests.cpp: the mapping both ways, grid and onset cuts, the zones and notes made, the slices
  played through the Sampler matching the clip (at a constant ratio, from the note velocity), gate and start key, the
  key shift past 92 slices and the refusals.

### Step 7: audio to MIDI

* dawg/AudioToMidi.h: `detectPitch` (YIN on the mono sum brought down to about 12 kHz by averaging, a 20 ms window,
  a 5.3 ms hop, 50 Hz to 1.5 kHz), `pitchNotes`, `drumNotes` and `velocityOfPeak`; `notesVar` (ClipOps.h) places the
  notes on the timeline through `ClipFrames`. `addNoteTrack` makes the instrument track (the default synth; drum mode
  for drums) after the clip's track with one clip in the clip's place. The app (`audio.toMidi`) reads the clip's
  frames and runs the analysis on its audio job threads (`AudioLibrary::analyse`).
* **A note splits against its key** (its median so far, rounded), not its raw median: the median of a short stretch
  of vibrato sits off-centre and split it. Up to two unpitched frames (about 10 ms) inside a note don't end it.
  **Not built:** splitting a held pitch at a new onset, so a repeated note without a gap stays one note.
* The pitch curve is the deviation from the key, thinned with the note recorder's simplification (0.05 semitone), and
  the level is the pressure curve (0..1 of the note's peak). Velocity follows the RMS peak for melodies and the
  sample peak in the first 10 ms for drums.
* **The drum split weighs the centroid by power**, not magnitude: with magnitudes, a little noise spread over every
  bin pulled a 1 kHz tone up into the hats.
* Clip menu: "Drums to MIDI", "Drums to MIDI (kick, snare, hats)" and "Melody to MIDI", at sensitivity 0.5, with
  curves. There is no dialog for the sensitivity yet.
* Tests in tests/AudioToMidiTests.cpp: a C–E–G sine melody within 5 cents and 15 ms, ±50 cent vibrato as one note
  with a curve of that depth, a tone jump without a gap as two notes, drum hits within 2 ms and split by sound, the
  velocity map, and notes placed through a clip with an offset onto a new track.

### Checked in the app (own instance: Vite 5297, CDP 9247, own WebView2 profile; 2026-11-09)

* A generated drum loop on an audio track: Reverse wrote `loop (reversed)` and pointed the clip at it; reversing
  again went back to the original. Normalise to −1 dBFS set +3.5 dB. The Slice dialog counted the slices and made the
  Sampler track. Drums to MIDI (split) put the 1 kHz hits on 38. A clip placed over the loop got a 10 ms crossfade,
  labelled "Crossfade in" in the inspector. The punch range showed in the ruler. Freeze rendered and played the
  render; deleting a frozen clip was refused; a tempo change marked the freeze stale.
* Found and fixed: the clip menu ran off the bottom of the window; it now scrolls. The default sensitivity for drums
  and slices is 0.75: at 0.5, low thumps (weak spectral flux) were missed.
* Not tried: recording (punch, pre-roll) with a real input, bounce and render selection, the melody converter on real
  audio, and plugins in a frozen track.
