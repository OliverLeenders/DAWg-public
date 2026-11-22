# Design: output protection

Status: **implemented** (notes in section 6). The owner decided the questions on 2026-10-28 (section 4). Pulled forward
from the feedback-routing phase (PLAN.md, phase 16) because the risk exists today.

## 1. Why

Nothing stands between the engine and the speakers. A resonant filter pushed into instability, a Faust script with a
division by zero, a plugin that misbehaves or, later, a feedback loop can produce values far above full scale, a
sustained full-scale signal, or NaN and infinity, which then poison every node they reach (a filter's state stays NaN
forever) and reach the driver. Loud enough, that hurts ears and can damage speakers and headphones. The protection
must be invisible in normal work: nothing a sane mix does may trigger it or change its sound.

## 2. Decisions

**(S1) Invalid audio is silenced where it appears, and its track is reset.** After a track renders, its output is
checked for NaN and infinity. A block that has any:

* is left out of the mix: the track's output, its sends and its key sends play nothing for that block;
* resets the track's nodes (`Node::reset()`), so a node whose state went NaN can recover (built-in nodes and Faust do;
  a plugin can't be reset, so its track stays silent for as long as the plugin outputs invalid audio);
* is reported: the track's state counts invalid blocks, the telemetry names the tracks (`invalidTracks`), and the UI
  says which track produced invalid audio and was reset.

The master track is checked the same way after its chain. Groups and returns are tracks, so they are covered too. S1
applies everywhere: to the device, to exports and to tests (invalid audio in a file is useless).

**(S2) A hard ceiling at the device output.** The device output is clipped to ±1 (0 dBFS) after everything, the sound
explorer's preview included. A converter clips there anyway; this keeps larger values from a driver that would pass
them on. Nothing below full scale changes.

**(S3) A runaway output is muted until the user unmutes it.** The device output, before the ceiling, mutes when:

* any sample exceeds ±16 (+24 dBFS): no sane mix gets there, a blow-up does at once;
* or its level stays above full scale: the RMS of each block is above 1 (0 dBFS) for 200 ms in a row. A full-scale
  sine has an RMS of 0.71, a full-scale square 1.0, so music never stays there.

Muting fades out over 10 ms and then outputs silence. Playback and rendering carry on underneath, so nothing is lost
and the meters still show what is happening. The telemetry reports the mute and its reason (`outputMute`:
`overload` or `extreme`); the UI shows a banner with the reason and an Unmute button (`audio.unmute`). Unmuting fades
back in over 10 ms and starts the detection afresh: a cause that is still there mutes again at once.

**(S4) The device only.** S2 and S3 guard ears and speakers, so they apply to the device output only. Offline
rendering (exports, tests) and the realtime export's capture get the mix as it is (S1 applies to them).

**(S5) Always on.** There is no setting to turn S1–S3 off: their thresholds sit far above any sane mix.

## 3. Implementation sketch

* `TrackState::invalidBlocks` (atomic counter, read and reset by the message thread like the meters).
* `TrackMix::invalid` (audio-thread scratch per track): set by `renderTrackSteps` when the track's output has a
  non-finite sample; `mixRenderedTrack` then skips the track's output and sends, and the track's nodes are reset.
* The master: checked after its chain in `renderBlock`; silenced and reset like a track.
* `AudioEngine::protectOutput()`: called by the device callback after `process()`. Detects S3, fades and mutes, and
  clips (S2) every output channel. `unmuteOutput()` (message thread) asks the audio thread to fade back in.
* Telemetry: `outputMute` (0 none, 1 overload, 2 extreme) and the per-track counters; the app sends `outputMute` and
  `invalidTracks`, and handles `audio.unmute`.
* UI: a banner for the mute with Unmute; a status message for invalid audio naming the tracks.

## 4. Owner decisions (2026-10-28)

1. A runaway output mutes until the user unmutes it; playback continues (S3).
2. The ceiling is a hard clip at 0 dBFS, not a limiter (S2).
3. Always on, no off switch (S5).

## 5. Testing

* A node that outputs NaN: its track is silent for that block, its nodes are reset, the counter rises; other tracks
  play on. The same on the master. Exports skip the invalid block.
* The device path: a signal beyond +24 dBFS mutes at once; a sustained overload mutes after 200 ms; a loud but sane
  mix (a full-scale sine, peaks above full scale for a moment) never mutes; the ceiling clips at ±1; the mute fades
  out and in over 10 ms; unmuting with the cause still there mutes again.
* No allocation on the audio thread.

## 6. Implementation notes

* S1 needs `Node::reset()` to clear a node's state; the EQ, compressor, MPE Synth, Loop Reverb (`ReverbNode::clear()`)
  and the delay-compensation node gained one. Hosted plugins can't be reset: a plugin that keeps producing invalid
  audio keeps its track silent, block by block, and the message repeats.
* The check runs at the end of `renderTrackSteps` (so also on the worker pool) and after the master chain;
  `mixRenderedTrack` skips a track whose `TrackMix::invalid` is set.
* `AudioEngine::protectOutput()` runs only in the device callback; `detectRunaway()` holds the S3 detection. The
  fade gain carries across blocks, so a mute fades over 10 ms whatever the block size.
* The banner shows in the main window only, not in the mixer window.
* Tests: tests/OutputProtectionTests.cpp drives audio tracks that monitor the device input, rendered offline and
  through the device callback. In the app, a Faust script whose output overflows to infinity silences its track
  each block and names it in the status bar; the mute was not triggered in the app, to keep a full-scale click off
  the speakers.
