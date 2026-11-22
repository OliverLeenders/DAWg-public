# Mixing basics

Status: **implemented**. A short step between phase 7 (audio tracks) and phase 8 (node graph). The engine
decisions are numbered M1–M5 and summarised in section 4. The UI decisions are in section 3; implementation notes
are in section 5.

## 1. Goals and non-goals

Goals:

* Pan per track.
* Level meters per track, an input level meter on audio tracks, and clip indicators.
* A master track: a master fader, a master meter and master insert effects (for example a limiter).

Non-goals (phase 13 routing and mixer, phase 10 automation):

* Sends, buses and groups, sidechains.
* A full mixer view with channel strips.
* Automation of pan and gain.
* Surround.

## 2. Engine and model

**(M1) Pan uses an equal-power law with unity at the centre.** Recommended.

* `Track` gains `pan` in −1 … +1, set with `setTrackPan`, which merges drags like gain does.
* The gains are `√2·cos θ` for the left channel and `√2·sin θ` for the right, with `θ = (pan + 1)·π/4`, each
  capped at 1.
* A centred track keeps exactly today's level. Hard-panned tracks keep full level on one side.
* For stereo material this acts as a balance control: each side is scaled, nothing is folded across.

**(M2) Meters are measured in the engine and sent with the telemetry.** Recommended.

* `TrackState` gains peak-hold atomics: left and right post-fader (after gain, pan, mute and solo), and the
  input peak of the track's input channels.
* The 30 Hz telemetry carries `{ trackId: [left, right, input] }`. Only the values change, so the payload
  stays small even with many tracks.
* The UI holds peaks briefly and decays them, so the meters move smoothly.

**(M3) Clip indicators.** A track's indicator lights when its post-fader peak reaches 0 dBFS (1.0), and the
master's when its output does. Indicators stay lit until clicked. This is UI state, fed by the peaks.

**(M4) The master track is a project-level chain.** Recommended.

* `Project` gains `master`: a `Track` of kind `master`, with its own id, no clips and no entry in the track
  order.
* The graph is `Mix Input → [effects…] → Gain → Track Output`, edited with the same chain commands as
  track inserts (`insertNode` / `removeNode` / `reorderNode` with the master track's id as `trackId`).
* The compiler builds the master program after the tracks, with the summed mix as its input.
* Latency compensation already aligns the tracks. The master chain's latency shifts everything equally, so
  the reported latency includes it for the playhead.
* The metronome stays outside the master chain, so a limiter never ducks it.

**(M5) The master meter** in the top bar becomes the master track's output meter, post master fader and
inserts. Today it measures the device output, which is the same signal plus the metronome and preview.

## 3. UI decisions (answered by the user)

1. **Pan and meters on tracks:** in the track headers. Each header gets a pan control next to the gain and a
   stereo meter. Armed audio tracks show their input level instead. A knob on the meter sets the track's volume
   (since 2026-11-04): the meter's lanes are its track, on the same curve as the dock's fader.
2. **Pan control style:** a drag number showing "C", "L40" or "R20". Drag horizontally to pan; double-click
   resets to centre.
3. **Meters:** two thin horizontal bars (left and right) along the bottom of the header, green, then yellow,
   then red, with a clip dot at the right end. The colours are fixed to the scale (-60 to 0 dBFS: green up to
   -18 dB, yellow around -9 dB, red at 0 dB), and the level uncovers them rather than stretching them, so a colour
   always means the same level (revised after phase 12).
4. **Master track:** a "Master" header pinned below the track headers and always visible, with fader, meter and
   clip indicator. Selecting it shows its insert chain in the inspector.

## 4. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| M1 | Equal-power pan, unity at centre, balance behaviour for stereo | Yes |
| M2 | Post-fader peaks and input peaks per track in telemetry | Yes |
| M3 | Clip indicators held until clicked | Yes |
| M4 | Master track with its own insert chain, master fader and output | Yes |
| M5 | The top-bar meter shows the master output | Yes |

## 5. Implementation notes

* **Model.** `Track::pan` and `Project::master` (`TrackPtr`, may be null). `findTrack()` never returns the master
  track; `findTrackOrMaster()` does. `withMaster()` (Commands.h) adds the default master track. It is not a
  command: `ProjectManager::load()` applies it to new and opened projects outside the undo history, so files from
  before this step open with a master track. `projectfile::read()` stays faithful to the file.
* **Commands.** `setTrackPan { id, pan }` is new. `setTrackGain`, `setTrackPan` and the graph and chain commands
  accept the master track's id. Everything else (clips, rename, switches, colour, height, remove) only finds
  tracks, so the master track can't get clips or be removed. An instrument can't be inserted into it.
* **Nodes.** `dawg.mixInput` heads the master chain. It copies the context's `input`, which the engine points at
  the mix buffer for the master track.
* **Compiler.** The master track is planned like a track and shares the buffer slots. `CompiledEngine` gains
  `master`, `mixMemory` (two channels of one block) and `masterLatencySamples`. `latencySamples` is the largest
  track latency plus the master latency.
* **Engine.** `renderChunk` sums the tracks into the mix buffer, with fader and pan (`panGains()`), then runs the
  master track on it and adds its output to the device, also with fader and pan. Without a master track the
  tracks go straight to the device. Each `TrackState` holds peak-hold `peakLeft`, `peakRight` (after fader, pan,
  mute and solo) and `inputPeak` (the track's input channels, whether or not it monitors). The metronome and
  the explorer preview stay outside the master chain.
* **Telemetry.** `meters: { trackId: [left, right, input] }` for every track and the master track, read and
  reset by `MainComponent::trackMeters()` at 30 Hz.
* **UI.** `TrackHeaders` shows gain and pan (`DragNumber` with `axis="x"` and `resetValue={0}`), and a meter along
  the bottom of headers at least 36 px high. Armed audio tracks meter their input, in violet. `MasterHeader` sits
  in an extra row of the arrangement grid, and selecting it shows its inserts in the inspector. Clip indicators
  are `clipped` in the store, set from telemetry and cleared by clicking the dot. The top-bar meter shows the
  master track's output.
* **Tests.** tests/MixingTests.cpp: the pan law, pan commands, panned output, track and input meters, the master
  chain's rules, the master fader and pan, and the file round trip. PluginTests.cpp checks a master effect and
  its latency.
