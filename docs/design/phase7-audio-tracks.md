# Phase 7 design: audio tracks

Status: **accepted** (implementation notes in section 11). The model, engine and app decisions are numbered T1–T11 and summarised in
section 10. The UI decisions are in section 9.

## 1. Goals and non-goals

Goals:

* An audio track kind next to instrument tracks.
* Audio clips: import from files, move, resize, split, duplicate and loop with the same arrangement editing
  as note clips, plus clip gain and fade in/out.
* Waveform display at any zoom.
* Playback streamed from disk, at any device sample rate.
* Recording audio from the input device onto armed tracks, with input monitoring and latency-correct
  placement.

Non-goals:

* Time-stretching, warping and pitch-shifting audio. Clips play at their own speed.
* A sample editor (editing inside an audio file), destructive processing, comping and take lanes.
* Recording MIDI or MPE from controllers. That is a separate topic (live input to the piano roll).
* Crossfades between overlapping clips. Clips never overlap (phase 6, A6); fades cover the transitions.
* Freezing and bouncing tracks (phase 11, export).

## 2. Model

**(T1) Track kind.**

* `Track` gains `kind: "instrument" | "audio"`.
* An audio track's graph is `Audio player → [effects…] → Gain → Track Output`. The player
  (`dawg.audioPlayer`) takes the instrument's place in the chain, and it can't be removed or replaced.
* Audio tracks have no sequencer and no note clips. Instrument tracks have no audio clips.
* Commands that don't fit the track kind are refused.

**(T2) Audio clips.** `Track` gains `audioClips`, sorted like `clips`. An audio clip has:

```
AudioClip { id, start, length,           // timeline, ticks (like note clips)
            fileId,                      // into the project's audio pool (T3)
            sourceOffset,                // where in the file the clip begins, in file samples
            loopStart, loopLength,       // optional looping, in file samples (0 = no loop)
            gainDb, fadeIn, fadeOut,     // fades in ticks
            fadeInShape, fadeOutShape,   // linear | equal power | exponential
            name, colour }
```

* Arrangement edits (move, resize, split, duplicate, loop, glue, overlap replacement) work on audio clips
  through the same commands. The commands find a clip by id in either list; mixing kinds is refused, and so is
  moving a clip to a track of the other kind.
* Linking (phase 6) does not apply to audio clips: there is nothing to edit inside them.

**(T3) Audio clips are time-based; their tick lengths follow the tempo.** Audio isn't stretched, so a clip's
duration in seconds is fixed.

* `setTempo` keeps every audio clip's start (a musical position) and rescales its `length`, `fadeIn` and
  `fadeOut` in ticks, so the same audio still fits.
* Source positions are stored in file samples, so they never depend on the tempo.

All arrangement code can then keep working in ticks.

**(T4) The audio pool.** `Project` gains `audioFiles`: `{ id, path, name, sampleRate, channels, frames }`.

* `path` is relative to the project folder (normally `Audio/<name>.wav`).
* Clips refer to files by id, so one file can be used by many clips.
* Files no clip uses stay in the pool until "Remove unused audio" (a later command), so undo can always
  restore a deleted clip.

## 3. Importing

**(T5) Imported files are copied into the project's `Audio/` folder** (phase 5, F1). Recommended, because
projects then stay self-contained and can be moved or shared.

* Formats: WAV, AIFF, FLAC, Ogg Vorbis and MP3 (JUCE's readers; MP3 enabled with `JUCE_USE_MP3AUDIOFORMAT`).
  WAV, AIFF and FLAC are copied as they are. Other formats are decoded to WAV on import so that playback and
  seeking stay cheap.
* An untitled project imports into `<app data>/Unsaved/<project id>/Audio/`. The first save moves those files
  next to the project.
* The copy runs on a background thread. The clip shows "importing" until it is ready, so dropping a long file
  doesn't block the UI.

Whether to copy or to reference files in place is a UI and user question as well (section 9).

## 4. Playback

**(T6) Streaming through a shared read-ahead thread.**

* Every audio file in use gets a `juce::BufferingAudioReader` on one shared background `TimeSliceThread`. It
  reads ahead about 2 seconds from wherever playback is.
* The audio thread reads with a zero timeout. Data that isn't ready yet plays as silence and is counted as a
  dropout in telemetry, never as a wait.
* On a seek, loop wrap or transport start, the player asks for the new position before audio is needed. With
  a 2-second read-ahead that is usually in time; if not, the first few milliseconds after a jump may be
  silent.
* Files shorter than 30 seconds are loaded fully into memory instead (one-shots and loops are common, and
  this removes their dropouts entirely).

**(T7) Sample-rate conversion at playback.** A file whose sample rate differs from the device's is
resampled in the player with a windowed-sinc interpolator, per clip and per channel. Mono files play
centred.

**(T8) `dawg.audioPlayer` node.**

* The graph compiler flattens a track's audio clips into an `AudioSequence`: absolute sample positions
  (ticks → samples with the tempo), file reader, source offset, loop, gain and fades.
* The node renders whatever overlaps the block and applies a 2 ms declick fade at edges that have no fade.
* Seeks and loops need no chasing: audio simply starts mid-clip.

## 5. Recording

**(T9) Recording model.**

* The audio device opens input channels too: the device's default, chosen in audio settings.
* Each audio track has `input` (a mono channel or a stereo pair, or none), `armed` and `monitor`
  (`off | auto | on`).
  * Input and monitor are saved with the project.
  * Arm is session state; it is not saved and not undoable.
* While recording, each armed track's input is written to a new WAV file in `Audio/` by a
  `juce::AudioFormatWriter::ThreadedWriter`: a lock-free FIFO from the audio thread to a disk thread.
* **Stopping the recording** creates one audio clip per armed track, from the record start to the stop.
  * The clip is shifted earlier by the device's input + output latency, so it lines up with what was heard.
  * The clip replaces what it covers (A6).
  * The whole take is one undo step.
* **With the transport loop on**, recording keeps writing one file across wraps. The clip covers the first
  pass; later passes are in the file and can be revealed by looping or trimming. Proper take handling is
  later.
* **Monitoring** runs the input through the track's chain (effects, gain).
  * `auto` monitors while armed and not playing back recorded material.
  * The input reaches the graph through a new input buffer in `ProcessContext`, so the audio player mixes
    it in.

## 6. Waveforms

**(T10) Peak files.**

* On import or after recording, a background job computes min and max per 256 samples, plus mip levels
  (×4 each), per channel.
* The result is stored as `Audio/.peaks/<file id>.peaks` and is regenerated if missing.
* The UI requests peaks for a file, a range and a resolution over the bridge (`audio.peaks`). Peaks come
  back as base64-encoded int8 arrays, picked from the mip level closest to the zoom, and the UI caches them
  per file and level.
* While recording, the UI draws the growing clip from peaks the recorder sends a few times per second.

## 7. Missing files

**(T11) Missing audio files stay in the project.**

* Their clips draw as "missing" and play silence.
* A relink action points a pool entry at another file, which is then copied into `Audio/`. How that is
  offered is a UI question.

## 8. Testing

* **Commands:**
  * audio clip editing through the shared commands;
  * kind checks;
  * tempo rescaling (T3);
  * pool references kept for undo.
* **Engine render tests:**
  * clip placement, source offset and looping, sample-accurate, using generated WAV files in a temp
    folder;
  * gain and fade shapes;
  * resampling 44.1 kHz → 48 kHz (a sine's frequency is preserved);
  * seeks and loop wraps.
* **Recording:** an offline test feeds input through `renderOffline` and checks the written file and the
  clip position after latency correction.
* **Peaks:** min/max correctness and the mip levels.
* **UI:** checked by hand over CDP: importing by drag and drop, editing, recording from the default input.

## 9. UI decisions (answered by the user)

1. **Creating and importing.** "+ Track" opens a menu: Instrument or Audio.
   * Files dragged from the OS (or from the sound explorer, item 8) onto a lane are imported there. Dropping
     below the last lane creates a new audio track.
   * File › Import audio… imports at the playhead on the selected audio track.
2. **Imports are always copied** into the project's `Audio/` folder (T5).
3. **Audio clip handles, Cubase style.**
   * A fade-in handle at the top-left and a fade-out handle at the top-right, just inside the loop corner,
     drag horizontally.
   * A handle at the top centre drags vertically for clip gain, showing dB while dragging.
   * The fade shape is chosen in the clip's context menu.
   * The waveform is drawn mirrored and scaled by the clip gain.
4. **Recording controls.**
   * Track headers get arm (●), monitor, mute and solo buttons. Mute and solo apply to both track kinds.
   * The input selector is in the inspector.
   * A Record button sits next to Play. R records; Shift+R records with a one-bar count-in when the
     metronome is on.
5. **Double-clicking an audio clip** selects it and shows a "Clip" section in the left inspector: name,
   gain, fade lengths and shapes, and the source file. There is no sample editor yet.
6. **Missing files.**
   * After opening a project with missing files, a banner lists them with "Locate…". Other missing files in
     the same folder are then found automatically.
   * Missing clips also get "Locate file…" in their context menu.
7. **Mute and solo** come now (item 4).
8. **Sound explorer** (added by the user). The right sidebar gets two tabs, Sounds and Plugins.
   * **Roots and browsing:**
     * Sounds starts with root folders: the user's favourites (+ adds one) and the project's `Audio/`
       folder.
     * Below them is a file tree, with a search box that filters the current folder recursively.
   * **Previewing:**
     * Selecting a file (a click, or Up/Down to cycle through files) plays it immediately through the main
       output.
     * Space or Enter toggles playback; Escape stops it.
     * A waveform with a playhead and a preview volume sits at the bottom.
     * Auto-play can be switched off.
   * Files are dragged onto lanes to import them.

### T12: Sound explorer (engine and app side)

* **Browsing:**
  * `files.list { path }` returns folders and audio files.
  * Durations are read from file headers in the background and arrive in a `dawg_files` event, so large
    folders list instantly.
  * Favourite roots live in the user settings.
* **Preview player:**
  * A preview voice in the `AudioEngine`, mixed after the master. It is independent of the transport and the
    project.
  * It reads through a `BufferingAudioReader` with resampling (T6, T7).
  * Commands: `preview.play { path }`, `preview.stop`, `preview.setVolume`.
  * Its position is reported in telemetry.
* **Peaks for previewed files** are computed in the background and cached in
  `<app data>/Cache/peaks/` by path, size and modification time.

## 10. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| T1 | Track kind (instrument / audio); the audio track chain starts with a fixed audio player | Yes |
| T2 | Audio clips in their own list, edited through the shared clip commands | Yes |
| T3 | Audio clips are time-based: setTempo rescales their tick length and fades, keeping their start | Yes |
| T4 | Project-level audio pool referenced by id; unused files stay until removed explicitly | Yes |
| T5 | Imports are copied into the project's `Audio/` folder; non-WAV/AIFF/FLAC formats are decoded to WAV | Yes |
| T6 | Disk streaming with a shared read-ahead thread; short files (< 30 s) are held in memory | Yes |
| T7 | Windowed-sinc resampling at playback when the file and device rates differ | Yes |
| T8 | `dawg.audioPlayer` node with a compiled `AudioSequence`, with declick fades | Yes |
| T9 | Recording to WAV with a threaded writer; one clip per armed track; latency-corrected; one undo step | Yes |
| T10 | Mip-mapped peak files; peaks fetched over the bridge as base64 int8 | Yes |
| T11 | Missing files stay in the pool, play silence and can be relinked | Yes |
| T12 | Sound explorer: background file listing, a transport-independent preview voice, cached preview peaks | Yes |

---

## 11. Implementation notes (phase 7 as built)

* **Audio clips share `NoteClip`.** Rather than a separate audio clip type and list (T2), audio clips are
  `NoteClip`s with `fileId != 0` plus `gainDb`, fades and fade shapes.
  * Their `offset` and loop range are in ticks of content time, where content 0 is the start of the file.
    `setTempo` rescales them along with the length and fades (T3).
  * Every arrangement command (move, resize, split, duplicate, loop, overlap replacement) therefore works
    unchanged.
  * Glue and linking are refused for audio clips.
* **Peaks are cached in app data,** not in `Audio/.peaks`: `<app data>/Cache/peaks/`, keyed by path, size
  and modification time. The same cache serves pool files and explorer previews.
* **Streaming (T6)** uses `juce::BufferingAudioReader` with a zero read timeout. Its reads take a short
  internal lock, which is not strictly lock-free, so a dropout is the worst case.
* **Resampling.** Downsampling (file rate above the device rate) uses the same 16-tap sinc without extra
  low-pass scaling. The preview voice uses linear interpolation.
* **Dropping files from the OS.** The web UI gets no paths for files dragged in from the OS, so their bytes
  are sent to the app in 3 MB base64 chunks (`audio.upload`). Large files import slowly this way. Files
  dragged from the sound explorer carry their paths.
* **Untitled projects** keep their audio in `<app data>/Unsaved/<project id>/Audio/`.
  * The first save, and every "Save as" to a new folder, copies the project's audio along; an untitled
    project's folder is then removed.
  * Closing an untitled project without saving deletes its audio.
* **Recording (T9).**
  * The device opens two inputs by default.
  * Takes are written by a `ThreadedWriter` with a FIFO of about 5 seconds, and finished 150 ms after
    stop.
  * A take shifted earlier than bar 1 by the latency correction gets a content offset.
  * Arm state lives in the app (`m_armed`) and in the engine's `TrackState`.
  * Recording with the transport loop on records one continuous take, as designed.
* **Not done yet:**
  * the take's waveform while recording (a red band grows instead);
  * input meters per track (the telemetry has one input peak);
  * "Remove unused audio" for the pool.
* **Tested by hand over CDP:**
  * importing by path, by dialog command and by simulated OS drop;
  * waveforms, gain and fade handles, the clip inspector;
  * the missing-file banner;
  * saving into an existing project folder;
  * mute, solo and arm, and the record error without an armed input;
  * the explorer: listing, durations, preview, Escape, and dragging onto a lane.

  Recording from a real microphone, the first-save copy of an untitled project's audio and the folder
  chooser for favourites were not exercised in the app. The recording pipeline is covered by an offline
  engine test.
