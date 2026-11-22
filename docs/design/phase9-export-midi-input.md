# Phase 9 design: export and MIDI input

Status: **implemented** (implementation notes in section 7). Export decisions are X1–X5, MIDI input decisions are M1–M9, and all of them are
summarised in section 6. The UI questions are in section 5.

## 1. Goals and non-goals

Goals:

* Export the song, the loop range or a custom range to an audio file, as a master mix and optionally as
  per-track stems.
* Play instruments from hardware keyboards and MPE controllers (Seaboard, Linnstrument, Osmose, Push 3, Roli
  Lumi). Hear them through the built-in synth and plugins with low latency.
* Record what is played into note clips, including per-note expression curves. Loop recording works for note
  clips and audio.
* MIDI learn for DAWg's own parameters (track gain and pan, node parameters).

Non-goals:

* Automation lanes. These come in phase 10; recorded CC data other than sustain waits for them.
* MIDI output to hardware, MIDI clock and sync, MIDI 2.0.
* Freezing and bouncing in place (later, built on the export renderer).
* Sample-rate conversion on export (the file has the device rate).

## 2. Export

**(X1) Export renders offline, with the device paused.** Recommended.

* Exporting runs the same compiled engine and the same node instances, on a worker thread and faster than real
  time. Plugins can't run in two places at once, so the device callback outputs silence meanwhile.
* Plugins are switched to non-realtime mode (`setNonRealtime (true)`) for the export, so samplers load fully and
  offline-aware plugins render at their best quality.
* A progress dialog shows the position and offers Cancel. Afterwards the engine is reset (seek to the old
  position), and playback works as before.
* A **realtime** mode is an alternative for plugins that misbehave offline. It plays the range and captures the
  master output, using the recording writer from phase 7.

**(X2) The range is song, loop range, or custom.**

* **Song** runs from bar 1 to the end of the last clip.
* A **tail** follows the range: automatic by default (render until the output stays below −90 dBFS for one
  second, at most 30 s), or a fixed number of seconds, or none.
* Automatic tail stops in a looped song are not an issue, because export never loops: the transport's loop is
  ignored.

**(X3) Stems (optional).** Each selected track is rendered alone, as its own file. Every pass, the first included, starts from fresh state, so no tail from the last pass or from
playback gets in.

* A stem is the track after its own chain, fader and pan, without the master chain. That is how stems are
  usually delivered for mixing.
* Muted tracks are skipped. Solo is ignored.
* Files are named `<project> - <track>.wav`, in the chosen folder. The master mix is written as well, unless
  it's turned off.
* Each stem is a separate pass, so plugin state is reset (prepared again) between passes.

**(X4) Formats.**

* WAV at 16 or 24 bit, or 32-bit float; FLAC at 16 or 24 bit.
* 16-bit output uses TPDF dither.
* Optional peak normalisation to a target (default −1 dBFS), off by default.

**(X5) Loudness report.** After an export, a summary shows sample peak and integrated loudness (ITU-R BS.1770 /
EBU R128: K-weighting, 400 ms blocks, absolute and relative gating). These are computed during the render at
no noticeable cost. The same meter later serves a LUFS meter on the master track.

**(X6) MP3** (owner, 2026-11-09; on the 1.0 list in docs/PLAN.md).

* **Encoder:** LAME 3.100, built from source with the app (LGPL v2 or later, which the AGPL allows), pinned by
  version and hash in `cmake/Dependencies.cmake`.
* **Quality:** constant bitrate at 320 (the default), 256, 192 or 128 kbps, or variable bitrate V0 (best) or V2.
  Joint stereo, at LAME's quality 2 (its recommended high quality; 0 is much slower for no audible gain).
* **Sample rate:** MP3 holds at most 48 kHz. A 44.1 or 48 kHz render is written at its own rate; above that it is
  converted down before encoding, and the dialog says so: a multiple of 44.1 kHz to 44.1 kHz and a multiple of
  48 kHz to 48 kHz, through a linear-phase low-pass (passband to 20 kHz, 80 dB down from the new Nyquist) that keeps
  every second, fourth or eighth sample, so nothing is shifted and stems stay aligned. Any other rate goes to 48 kHz
  through LAME's own converter. This lifts the non-goal "sample-rate conversion on export" for MP3 only.
* **Clipping:** encoding can push peaks a little over the source's. With MP3 chosen, peak normalisation is on at
  −1 dBFS unless the user turned it off. The loudness report reads the written MP3 back and shows its peak, with
  a warning when it is above 0 dBFS.
* **Tags:** an ID3v2 tag with the title (the file name unless typed), artist, album, year, track number, genre and
  comment from the export dialog, and "DAWg" as the encoder (UTF-16, so any script works). The dialog remembers
  the quality and every field but the title on this computer. Stems get the same tags; a stem's title is the typed
  title and the track's name, or else its file name. The file starts with LAME's Xing/LAME frame, so players know
  its length and skip the encoder's delay and padding (gapless).
* **Stems** export as MP3 too; WAV and FLAC are unchanged.

## 3. MIDI input

**(M1) Devices.**

* The audio settings dialog already lists MIDI inputs (JUCE's device selector). Enabled inputs are opened through
  `AudioDeviceManager::addMidiInputDeviceCallback`.
* Each device also gets DAWg settings:
  * **mode:** MPE or normal. MPE is detected automatically from the MPE configuration message (RPN 6) when the
    controller sends one, and can be set by hand.
  * **pitch bend range:** 48 semitones per note for MPE by default, 2 semitones for normal mode.
  * **MPE zone:** lower, upper or both.
* These are app settings, not project data.

**(M2) Live notes carry live expression.**

* Today's live notes (audition) have fixed curves. A new `NoteEventType::expression` event carries
  `{ noteId, lane, value }` for pitch (semitones relative to the base pitch), pressure and timbre (0..1).
* The built-in MPE synth applies them per voice. Its expression then comes from live values instead of curves,
  smoothed over its 32-sample control interval.
* The MPE encoder forwards them to plugins as per-note pitch bend, channel pressure and CC74.
* **Sustain** (CC64) becomes a `sustain` event per track. The built-in synth holds released notes while it's
  down; plugins get CC64.
* **Other CCs** (mod wheel, expression and so on) and pitch bend in normal mode go to plugin instruments as they
  are, on the plugin's main channel. The built-in synth ignores them.

**(M3) A realtime path.**

* The MIDI input callback runs on a JUCE MIDI thread. It decodes MPE (our own small decoder, modelled on
  `juce::MPEInstrument` but allocation-free), turns each message into a live request with a timestamp, and
  pushes it into a lock-free queue.
* The audio thread converts timestamps into sample offsets within the next block, the way
  `juce::MidiMessageCollector` does. Events are then sample-accurate, one block late at worst.
* Notes that are held when the input target changes, or when the transport stops, are released correctly:
  each live note remembers the track it started on.

**(M4) Which track hears MIDI input.**

* Armed instrument tracks hear it, so several armed tracks can be layered. With none armed, the selected
  instrument track hears it (Cubase's "MIDI thru on the selected track").
* Instrument tracks therefore get the arm button audio tracks have. They don't get monitoring: they always
  play through.

**(M5) Recording notes.**

* `transport.record` records audio on armed audio tracks, and notes on armed instrument tracks, together.
* On stop, each armed instrument track gets one new note clip covering the recorded range (bar-aligned, like
  audio takes), placed with the usual overlap replacement.
* Each recorded note keeps:
  * its base pitch, velocity and release velocity;
  * **expression curves** sampled from the live values at the 32-sample control rate, then simplified
    (Ramer–Douglas–Peucker with a small tolerance per lane) to a sparse curve, with times relative to the note
    start.
* **Pitch** is stored relative to the base pitch, so glides keep working. A note's base pitch is its note-on
  key; slides go into the pitch curve.
* **Timing:** notes are placed by when they were played, corrected for the device's output latency (the player
  heard the output that much later). Engine latency is already handled by the track look-ahead.
* **Sustain** is not recorded in this phase (the user's choice). Note lengths stay as played. The pedal only
  affects play-through until phase 10 stores it in a CC lane.

**(M6) Loop recording.** Replaced by takes-lanes.md (T4): each pass is a clip of its own in a takes lane, notes too unless overdub is on, and the Takes submenu and the clip's takes fields are gone.

* **Notes:** each pass is merged into the same clip (overdub), so drum patterns build up over passes. Undo
  removes the whole take.
* **Audio:** the recording runs into one file across all passes. The new clip plays the last pass, and a
  **Takes** submenu in the clip menu switches to an earlier pass by moving the clip's content offset.
  `NoteClip` gains `takes` (the number of passes) and `takeLength` (the loop length in content ticks). Other
  clips have `takes = 1`.

**(M7) Recording rules.**

* The count-in from phase 7 applies to note recording too.
* Recording into an existing clip region replaces that part, the same rule as for audio.
* Notes still held when recording stops end at the stop position.

**(M8) MIDI learn for DAWg parameters.**

* Right-click a track's gain or pan, or a node parameter in the inspector, and choose "MIDI learn". The next CC
  received is mapped to it. "Forget MIDI mapping" removes it.
* Mappings belong to the project (`Project::midiMappings`, saved in the file): device name (optional), channel,
  CC number, and target (track gain or pan, or node parameter).
* Incoming values become commands. One continuous controller movement is one undo step, merged until the
  controller rests for 500 ms.
* Plugins keep their own MIDI learn. Their parameters can be targeted through automation in phase 10.

**(M9) Computer keyboard as MIDI input: deferred** (the user's choice). Kept here for later:

* A toggle turns the computer keyboard into a two-octave piano, Ableton-style:
  * A–K row: white keys; W, E, T, Y, U: black keys;
  * Z and X: octave down and up; C and V: velocity down and up.
* It feeds the same live path as a hardware device, without expression.
* While it's on, those keys don't trigger their usual shortcuts.

## 4. Implementation order

1. Live expression and sustain events in the engine (synth, MPE encoder) with tests.
2. MIDI device handling and the MPE decoder, the realtime queue, and input routing; arm on instrument tracks.
3. Note recording (curve sampling and simplification, latency correction) with tests.
4. Loop recording: note overdub, audio takes (model, commands, clip menu).
5. Offline export (renderer, tail detection, stems, formats, dither, loudness) with tests; realtime export.
6. Export dialog, MIDI settings, MIDI learn.

## 5. UI decisions (answered by the user)

1. **Export UI:** File › Export audio (Ctrl+E) opens one modal page. It has the range (song, loop, custom), the
   tail, the format, normalisation, the master mix, and a checklist of tracks for stems. Stems are in v1. A
   progress view follows, then the loudness report.
2. **MIDI input routing:** armed instrument tracks hear and record input; with none armed, the selected
   instrument track plays it (M4).
3. **Sustain pedal:** not recorded in this phase. It is stored once phase 10 brings CC lanes.
4. **Computer keyboard as MIDI input:** later.

## 6. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| X1 | Offline export with the device paused, plugins non-realtime; realtime mode as an alternative | Yes |
| X2 | Song, loop or custom range; automatic tail until silence | Yes |
| X3 | Optional stems: post-fader, without the master chain | Yes |
| X4 | WAV 16/24/32f, FLAC 16/24, TPDF dither, optional normalisation | Yes |
| X5 | Peak and integrated loudness report | Yes |
| M1 | MIDI inputs from the audio settings; per-device MPE mode, bend range and zone | Yes |
| M2 | Live expression and sustain events for the synth and plugins | Yes |
| M3 | Lock-free, sample-accurate realtime path from the MIDI thread | Yes |
| M4 | Armed instrument tracks hear input, else the selected one; arm on instrument tracks | Yes |
| M5 | Note recording with simplified expression curves and latency correction; sustain not recorded yet | Yes |
| M6 | Loop recording: note overdub, audio takes in one file with a Takes menu | Yes |
| M7 | Count-in, overlap replacement, held notes end at stop | Yes |
| M8 | MIDI learn for DAWg parameters, saved in the project | Yes |
| M9 | Computer keyboard as MIDI input: deferred | — |

## 7. Implementation notes

* **Live events.**
  * `NoteEventType` gains `expression`, `sustain` and `controller`. `NoteEvent` gains `lane` and `value`.
  * The built-in synth switches a voice to live values on its first expression event, and holds released voices
    while the pedal is down.
  * The MPE encoder sends live pitch, pressure and timbre per channel. In plain-MIDI output, live pitch becomes
    channel pitch bend (±2). Sustain goes out as CC64, and other controllers on the master channel.
  * The encoder no longer reads `context.liveEvents` itself: live events already reach it through the
    sequencer. Before, auditioned notes reached plugin instruments twice.
* **Engine input path** (AudioEngine).
  * `LiveInput` events come from two single-producer queues: audition from the message thread, MIDI from the
    `MidiInputHub`, whose lock serialises the MIDI threads.
  * Timestamps map to sample offsets within the next block. `m_callbackSeconds` is the start time of the
    device callback; offline rendering uses offset 0.
  * A note-on carries its initial pitch offset, pressure and timbre as one-point curves on the live
    `CompiledNote`, so the note starts with them.
  * Expression and controller events are coalesced per block (latest value per note, lane and track). Up to
    256 events per block.
* **Recording.**
  * While recording runs, the coalesced events go into a queue with their timeline position, minus
    `setInputLatency()` (output latency plus one block).
  * `NoteRecorder` (engine/src/MidiInput.cpp) builds the notes. It simplifies curves with Ramer–Douglas–Peucker
    (tolerance 0.03 semitones for pitch, 0.01 for pressure and timbre) and leaves out lanes that stay at their
    default.
  * The app adds one note clip per armed instrument track: bar-aligned without a loop, or the loop range when
    the loop wrapped (overdub).
  * `AudioEngine::recordLoopWraps()` counts the wraps.
  * Audio loop takes: `NoteClip::takes`, `takeStart` and `takeLength`, the `addAudioClip` arguments of the
    same names, and `selectTake { id, take }`, which keeps the clip's place within a take. `setTempo` rescales
    the take fields.
* **Decoder.**
  * `MidiInputDecoder` handles MPE and plain MIDI as designed. MPE master channels are 1 and 16. A member
    channel's bend adds to its zone's master bend.
  * RPN 6 switches MPE on or off; RPN 0 sets the bend range. Each is remembered per device in the app settings
    (`midiInputs`).
  * A key pressed again without a note-off first ends the old note.
* **App.**
  * `MidiInputHub` (app/src) opens all enabled devices through the device manager and routes notes to the
    target tracks. A note keeps the tracks it started on. Controller messages are forwarded for MIDI learn.
  * MidiControl.cpp holds the targets (armed instrument tracks, else the selected one, which the UI reports
    through `track.select`), the MIDI-learn flow, and turning recorded notes into commands.
  * Mapped controllers become commands with a gesture that ends after 500 ms of rest. Gain uses a square-law
    fader curve (0 dB at about 72 %); node parameters use the same mid-skewed curve as the UI sliders.
  * Removing a track or node removes its mappings.
* **Export.**
  * engine/src/Export.cpp has `renderRange` (offline; while exporting the engine ignores the loop and leaves
    out the metronome and preview), the automatic tail, stems, `LoudnessMeter` (BS.1770 with libebur128's
    K-weighting coefficients), and `writeExport` (WAV and FLAC, TPDF dither for 16 bit, gain for
    normalisation).
  * ExportControl.cpp drives it:
    * **offline:** the device callback is removed, plugins go non-realtime, and a worker thread renders each
      pass to a float file, re-preparing every node between passes;
    * **realtime:** the device plays the range and `setOutputCapture` writes it.
  * Both then measure each file and write it in the chosen format.
  * Stems don't move by the master chain's latency (they skip the master track).
* **UI.**
  * `ExportDialog` (File › Export audio, Ctrl+E).
  * `MidiSettings`: a top-bar button with an activity light and a popover for devices, MPE mode and bend
    ranges.
  * The arm button on instrument tracks.
  * `MidiLearnable` (right-click track gain and pan, and node parameter names): "MIDI learn" and "Forget MIDI
    mapping"; a learning control blinks, and a mapped one is underlined.
  * A "Takes" section in the clip menu.
* **Tests.**
  * tests/MidiInputTests.cpp: MPE and plain decoding, configuration detection, live bend and sustain on the
    synth, the recording tap and latency correction, note building and simplification, notes held across a
    loop wrap, takes, MIDI mappings.
  * tests/ExportTests.cpp: the EBU reference tones (±0.05 LU), range and tail rendering without looping,
    cancelling, stems, all five formats.
  * PluginTests.cpp: live expression, sustain and controllers reaching plugins, with a single note-on.
* **Not tested with hardware here:** real MIDI and MPE controllers, MIDI learn with a physical controller, and
  loop recording with a microphone.
