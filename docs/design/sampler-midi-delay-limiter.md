# Design: delay, limiter, MIDI files and sampler

Status: **in progress**. The owner decided the questions on 2026-11-01 (section 5). The delay, limiter and MIDI files are done; the
sampler is designed here at the level those answers settle, and gets its details when it starts.

These fill the gaps a user meets first: without a plugin or a script DAWg can't play a kick sample, echo a sound or stop a
mix from clipping transparently, and without MIDI files existing parts must be redrawn.

## 1. Delay (`dawg.delay`, EffectNodes.cpp)

| Parameter | Range | Default | |
|---|---|---|---|
| Sync | toggle | on | Times follow the tempo |
| Link | toggle | on | Right time follows the left |
| Ping-pong | toggle | off | The mono input enters left, and echoes cross sides |
| Left, Right note | 1/64T to 4/1 | 1/8. | As the LFO's cycle lengths |
| Left, Right time | 1 to 4000 ms | 375 ms | When not synced |
| Feedback | 0 to 100 % | 35 % | |
| Low cut, High cut | 20 to 2000 Hz, 500 to 20000 Hz | 20 Hz, 12 kHz | One-pole filters on the echoes, so repeats get thinner and darker |
| Wet | 0 to 100 % | 30 % | The echoes' level, added to the dry sound, which always passes at full (2026-11-12; it was a crossfade that turned the dry down) |

The read position glides to a new time (a one-pole, about 100 ms), read with four-point interpolation, so changing the
time or the tempo bends the pitch of the echoes instead of clicking, as tape does. The line holds 8 seconds, the longest
time at any tempo. A value written back is clamped well above full scale, so interpolation overshoot at 100 % feedback
can't grow without bound.

In a voice graph each voice has its own line. A note start clears it, about 3 MB per voice. It has no latency.

## 2. Limiter (`dawg.limiter`, EffectNodes.cpp)

| Parameter | Range | Default |
|---|---|---|
| Gain | 0 to 24 dB | 0 dB |
| Ceiling | -24 to 0 dB | -0.3 dB |
| Release | 1 to 1000 ms | 100 ms |

A look-ahead limiter with a fixed 5 ms look-ahead, reported as latency and compensated like any node. The channels share
one gain, so the stereo image holds. For each sample:

1. The gain needed is the ceiling over the louder channel's peak, at most 1.
2. An envelope follows it down at once and recovers with the release.
3. A sliding minimum over the look-ahead holds the lowest gain, and a moving average over the same length smooths it. Every
   gain in the average is at or below what the peak needs by the time the delayed peak arrives, so it can't exceed the
   ceiling and the gain has no steps. A last clamp catches rounding.

Bypassed, it passes its input straight through and adds no latency, like every node: the compiler counts none and
recompiles when a latent node's bypass changes, so bypassing an amp sim or a limiter takes its delay out of the project for
monitoring. The project's total latency, and so the timing of what plays, shifts at that moment.

It's a node you add where you want it. The 0 dBFS safety ceiling at the device output (output-protection.md, S2) stays.
Peaks are sample peaks; true-peak detection is a later option.

## 3. MIDI files (engine/src/MidiFileIO.cpp)

**Import:** File > Import MIDI file, or a drop on the arrangement (at the drop's position). The bytes go to the engine, which
reads them through the decoder and note recorder that live input uses, so a file's notes end up exactly like recorded ones:

* Each MIDI track, and each channel with notes in it, becomes an instrument track with a clip covering whole bars, named
  after the track. Channel 10 makes the track a drum part. An MPE file (one with an RPN 6 message) keeps a track as one part.
* Pitch bend and pressure become note curves, with bend ranges from RPN 0; polyphonic aftertouch is per note. In MPE, CC74 is
  a note's timbre curve too. Sustain and other controllers, including CC74 in plain MIDI where it applies to a whole
  channel, become clip controllers.
* Importing at the start with a different tempo or time signatures asks whether the project takes on the file's. At any other
  position they would not line up, so the project keeps its own. The `importMidi` command makes it all one undo step.
* Files timed in SMPTE frames are refused; signatures without a grid (x/1, x/32) are left out.

**Export:** File > Export MIDI file: a type 1 file at 960 PPQ with the tempo map on its own track, and one track per instrument
track with notes, looped clips written out. A track is plain MIDI on channel 1 (10 for a drum part) unless a note has a curve,
vibrato or a fractional pitch; then the whole track is MPE (lower zone, a member channel per note, ±48 semitones), with curves
written every 1/64 beat when their value changes. More than 15 notes at once share channels. Tempo ramps become a step per beat.

**Dropping on a track:** a MIDI file dropped on an instrument track, from the OS or the browser, becomes one clip there at the
drop, its parts merged. Anywhere else it makes new tracks. Dropped files never ask about tempo except an OS file at the start.

**MIDI files browser:** a tab in the right panel, called MIDI files rather than chords since drum patterns may join them. It
shows what ships in `midi-files/` next to the app as a folder tree with search (every word somewhere in the path, so
"C Major nostalgic" works, up to 100 results). Selecting a file plays it through the selected instrument track, or the first
one, at the project's tempo, with the transport untouched; dragging it onto the arrangement places it.

**[Free MIDI Chords](https://github.com/ldrolez/free-midi-chords)** ships there, in `midi-files/Free MIDI Chords` (about 14,000
files in a 6 MB download, pinned by version and hash in cmake/Dependencies.cmake). Its files are MIT licensed, which allows this
and suits DAWg's AGPL, and its LICENSE file is copied with them. Only the release's files ship, not its generator, whose
dependencies have licences of their own. Its second release zip holds the same progressions by style and isn't shipped.

## 4. Sampler

* An instrument like the MPE Synth (`dawg.sampler`), with 16 voices of its own, so clicking it in the inspector opens its panel
  in the device dock. It follows each note's pitch curves and live pitch; pressure and timbre do nothing for now.
* **As a voice graph:** "Convert to voice graph", as on the MPE Synth, moves its zones and settings to a Sample Voice
  (`dawg.sampleVoice`, shown as Sampler) in a Voice Instrument, with Note Expression driving its pitch. Both play through the
  same code, and an envelope, filter or anything else can be wired per note there.
* **Key zones:** each sample covers a key and velocity range, with a root key, and a simple drum-pad view over the same
  zones, named from the General MIDI drum map. One sample across the keyboard is one zone. A note plays the first zone that
  holds it.
* **Per zone or pad:** start, end, tune, volume, pan and an ADSR; a choke group; and how it follows its note: one-shot plays to
  its end whatever the note's length, so its envelope's attack, decay and sustain apply but never its release; gate plays while
  held, then releases; loop does too, looping between set points with a crossfade. A new zone's envelope is flat with a 50 ms
  release, so it plays as the file sounds. Attack, decay and release each bend at a tension point, as on the ADSR
  (`dawg/EnvelopeHandle.h`; straight by default), dragged in the zone's envelope and straightened by a double-click. These
  settings live in the zone and can't be automated or modulated; the sampler-wide level, pitch and velocity sensitivity
  can. Editing a zone isn't a structural change: the graph compiler compiles that Sampler's zones again and hands them to
  its instances (`Node::setSampleMap`) instead of recompiling the project, and a sounding note follows the edit. In the
  panel, zone edits show at once and go out at most once per frame, coalesced while a command is out (`useZoneEdit`).
* **Samples** are copied into the project like audio clips' files (`Project::audioFiles`) and read through `AudioSource`, so
  files up to 30 seconds play from memory and longer ones stream. Not decided by the owner, the reasonable default: a project
  must not lose its sounds when it moves.
* **Runtime:** zones compile to an immutable `SampleMap`, which the node gets in `ProcessContext::samples` and which is swapped
  with the engine, so editing a zone recompiles and a sounding voice finds its zone again by id. Choke groups are counts in that
  map; a voice stops when its group's count has moved on.
* In a voice graph, a released voice still ends 10 seconds after its note at the latest (phase12-voice-scope.md, V9), one-shots
  too.
* Not in v1: round robin, random layers, dropping files from the OS on the panel (its + button opens a file dialog).

## 5. Owner's answers (2026-11-01)

1. Order: delay and limiter, then MIDI files, then the sampler.
2. Sampler mapping: key zones with a drum view.
3. Delay: the full musical delay of section 1, without saturation or wow and flutter.
4. Limiter: a look-ahead effect node, not added by default, no true peak.
5. MIDI import: new tracks, expression kept.
6. MIDI export: MPE if a note has curves, else plain.
7. Sampler placement: first a voice graph node, then (2026-11-01) a normal MPE-capable instrument that can convert to a voice graph.
8. Playback in v1: start, end, tune, volume, pan and an ADSR per zone; one-shot, gate and loop; choke groups; pitch follows the note, pressure and timbre don't yet.
