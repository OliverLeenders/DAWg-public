# Phase 10 design: automation

Status: **implemented** (implementation notes in section 7). The decisions are numbered A1–A8 and summarised in section 6. The UI questions are
in section 5.

## 1. Goals and non-goals

Goals:

* Automate track gain and pan (also the master track's), node parameters (built-in and Faust later), and plugin
  parameters.
* Draw and edit automation in the arrangement. Record it from DAWg's controls, from plugin editors and from MIDI
  learn mappings, in Read, Touch, Latch and Write modes.
* Store the controller data that phase 9 left out: the sustain pedal and other CCs recorded with notes, played
  back to the built-in synth and plugins.

Non-goals:

* Modulators (LFOs, envelopes) and modulation ports in the graph. These come later, in the node graph.
* Tempo and time-signature automation. That is the tempo map of phase 13.
* Automation that moves with clips (Cubase's "automation follows events"). Possibly later.

## 2. Model

**(A1) Automation lives on tracks, as one lane per parameter.** Recommended.

* `Track::automation` is a list of lanes. Each lane has:
  * a **target**: track `gain` or `pan`; a node parameter (`nodeId` plus parameter id); or a plugin parameter
    (`nodeId` plus the plugin's parameter id string, which stays stable across plugin versions, with its name
    cached for display);
  * **points** on the timeline, in absolute ticks: `[tick, value, shape, tension]`, the same shapes as
    expression curves;
  * `active`: whether the lane plays (switching it off keeps the points);
  * the lane's height in the arrangement.
* **Values are normalised to 0..1** across the parameter's range. Node parameters use their `midValue` curve, the
  same mapping the inspector sliders use, so drawing feels like moving the control. Plugin parameters are already
  normalised. Gain uses the fader curve from MIDI learn (0 dB at about 72 %), and pan maps 0..1 to left..right.
* Commands:
  * `addAutomationLane { trackId, target }`, `removeAutomationLane`, `setAutomationLane { trackId, laneId,
    active?, height? }`;
  * `setAutomationPoints { trackId, laneId, points }` replaces a lane's points (like `setCurve`);
  * `writeAutomation { trackId, laneId, from, to, points }` replaces a time range (used by recording and by range
    edits).
* Removing a node removes its lanes. Removing a track takes its lanes with it.

**(A2) Clip controller lanes.** `NoteClip` gains `controllers`: a map from controller to curve, in content ticks
(they move and loop with the clip, unlike automation). Controllers are CC 0–127, pitch bend and channel
pressure. Sustain (CC64) is a step curve. Recording stores CC64 and any other controllers it receives on the
recorded clip. The phase 9 decoder already turns them into events.

## 3. Engine

**(A3) Automation is evaluated per block, then smoothed.** Recommended.

* The compiler turns each active lane into a compiled curve (sorted points, with the value mapping baked in) and
  hands it to its target.
* Each block, the engine evaluates every lane at the block start, transport-aligned: tracks run ahead by the
  look-ahead, so their lanes are evaluated at the track transport.
  * **Track gain and pan:** evaluated at the block start and end, then ramped per sample in `mixTrack`, so fades
    stay smooth.
  * **Node parameters:** written to the node's `ParamBlock`. Built-in nodes already smooth their parameters.
  * **Plugin parameters:** set with `AudioProcessorParameter::setValue` before `processBlock`. The JUCE VST3
    host forwards that as a parameter change at offset 0. Sample-accurate plugin automation (VST3 parameter
    queues with offsets) comes later.
* Block sizes are at most 512 samples (about 10 ms), which is fine for mixing automation. If finer timing is
  needed, the engine can split blocks at 32-sample control points the same way it splits at loop ends.

**(A4) Playback overrides the manual value.** While a lane is active and the transport plays (or is stopped at a
position), the parameter follows the lane. The control shows the automated value, and the UI reads it from
telemetry for visible lanes. The model keeps the manual value. It applies again when the lane is switched off,
or for parameters without a lane.

**(A5) Clip controllers play through the sequencer.** It emits `sustain` and `controller` events (from phase 9)
whenever a clip's controller curve changes value, at control rate and on value changes only. Chasing works
like notes: after a jump, the current pedal state and CC values are sent.

## 4. Recording

**(A6) Automation modes per track: Off, Read, Touch, Latch, Write.**

* **Read:** play the lanes.
* **Touch:** while a control is held, its values are written; on release, playback continues with the existing
  points.
* **Latch:** like Touch, but writing continues after release until the transport stops.
* **Write:** every active lane of the track is overwritten while playing, with the controls' current values.
* Writing works whether or not the transport is recording. Nothing is written while stopped.

**(A7) Sources of a written value:** DAWg's controls (headers, inspector sliders, node parameters); plugin
editors, through `audioProcessorParameterChangeGestureBegin` / `End` and `audioProcessorParameterChanged` (the
gesture is the "touch"); and MIDI learn mappings (a controller movement is a touch that ends after 500 ms of
rest, as in phase 9). Writing to a parameter without a lane creates the lane.

**(A8) Written data is thinned.**

* Values are sampled at control rate on the audio thread and pushed to a queue, the way phase 9 recorded MIDI.
  The message thread simplifies them (Ramer–Douglas–Peucker, as in note recording) and applies one
  `writeAutomation` per touch.
* One pass (play to stop) is one undo step, labelled "Write automation".
* Loop playback writes into the same range again: Touch and Latch replace what they cross.

## 5. UI decisions (answered by the user)

1. **Lanes:** under each track in the arrangement. An arrow in the header expands the track, showing one lane
   per automated parameter below it (Cubase-style). Each lane has a small header (parameter name, active toggle,
   remove), and a "+" adds a lane through a parameter picker (track, nodes, plugins).
2. **Mode:** per track. A small button in the track header cycles Off, R, T, L and W.
3. **Clip controllers:** extra lane tabs in the piano roll next to velocity, pressure and timbre (Sustain, Mod
   wheel, plus any other controller in the clip, and a "+" for more). The data belongs to the clip and moves and
   loops with it.
4. **Shortcut:** the right-click menu of track gain and pan and of node parameters (where MIDI learn already is)
   also offers "Show automation", which creates or reveals the parameter's lane.

## 6. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| A1 | Track lanes with absolute-tick points, normalised values, stable plugin parameter ids | Yes |
| A2 | Clip controller curves (CC, pitch bend, pressure; sustain as steps) in content ticks | Yes |
| A3 | Per-block evaluation; ramped gain and pan; ParamBlock writes; plugin `setValue` before each block | Yes |
| A4 | Active lanes override the manual value; the controls show automated values | Yes |
| A5 | The sequencer plays clip controllers, with chasing | Yes |
| A6 | Off / Read / Touch / Latch / Write modes per track | Yes |
| A7 | Write sources: DAWg controls, plugin editor gestures, MIDI learn; writing creates the lane | Yes |
| A8 | Control-rate capture, simplified, one undo step per pass | Yes |

## 7. Implementation notes

* **Model.**
  * `AutomationTarget`, `AutomationLane` and `AutomationMode`, with `Track::automation`, `automationMode` and
    `automationExpanded`. Lanes are serialised keyed by id, plus an `automationOrder`.
  * `NoteClip::controllers` holds the clip controllers, keyed 0–129. Linked clips share controllers like notes.
  * `curveFromVar` (Commands.h) parses any 0..1 curve. Removing a node removes its lanes.
  * `addClip` takes `controllers`.
* **Values.**
  * `paramFromNormalised` / `paramToNormalised` (Node.h) are the same mid-skewed curve as the UI sliders. MIDI
    learn uses them too.
  * Gain uses `faderGain` (2·p², 0 dB at about 0.71). Pan maps 0..1 to −1..+1.
* **Engine.**
  * The compiler turns active lanes into `CompiledLane`s on their `CompiledTrack`. It resolves the node
    instance, the parameter index, or the plugin parameter index (`PluginNode::parameterIndex`).
  * `LaneState` (last played value, overridden) is shared across compilations by lane id. Lane edits count as
    structural changes, so they recompile.
  * `AudioEngine` evaluates each track's lanes per chunk on the track transport (the look-ahead):
    * gain and pan ramp per sample in `mixTrack`;
    * node parameters are written to the `ParamBlock`;
    * plugin parameters use `setAutomatedParameter` (`setValue` before the block).
  * Mode Off, or an overridden lane, leaves the manual value. The plugin host now recompiles after every plugin
    load, so plugin lanes resolve.
* **Clip controllers.** `TrackSequence` compiles `ControllerSegment`s, one per clip pass (`forEachPass` in
  Model.h). The sequencer sends `sustain` and `controller` events: the value at the chunk start, then every point
  inside the chunk, only on a 7-bit (pitch bend 14-bit) change. A start, jump or loop resends; leaving the clips
  lifts the pedal; stopping lifts it too.
* **Recording.**
  * AutomationControl.cpp keeps a capture per touched parameter. Touches come from:
    * the UI (`automation.touch` from header drags and inspector sliders);
    * plugin editors (`PluginHost` gesture callbacks);
    * MIDI learn, where a touch ends after 500 ms of rest.
  * Write mode captures every active lane of the track while playing.
  * Captures sample the manual value at the 30 Hz timer, at the playhead. A loop wrap starts a new run.
  * At transport stop, the runs are simplified (Ramer–Douglas–Peucker, 0.004) and applied as one "Write
    automation" batch: `writeAutomation` per run, or `addAutomationLane` with points for a new lane.
  * Note recording now stores sustain and other controllers in the clip (`NoteRecorder::controllers`).
* **UI.**
  * `layout.ts` gives each lane its automation rows (`fullHeight`, `automationAt`). `drawAutomation` draws the
    curves; `useAutomationEditing` adds points (Alt+click), moves and box-selects them, bends segments with free
    tension handles inside the segment rectangle, and deletes points (double-click or Delete).
  * The track header has the expand arrow, "+", which opens `AutomationPicker` (track, node and plugin
    parameters), and the mode button (Off / R / T / L / W). `AutomationHeader` gives each lane its name, current
    value, active toggle, remove button and resize handle.
  * Controls follow played values (telemetry `automation`).
  * The MIDI-learn menu gets "Show automation".
  * In the piano roll, the lane tabs gain Sustain, Mod wheel, every controller the clip has and "+" (any CC).
    `controllers.ts` handles drawing and editing (Alt+click or a double-click adds a point; dragging empty space box-selects);
    pedal points are steps snapped to 0 and 1.
* **Master track lanes** (polish pass): the pinned master row grows with its automation rows (up to 320 px).
  It has its own canvas and editing, and its header has the expand arrow, "+" and the mode button.
* **Not done in v1:** sample-accurate plugin automation.
* **Tests.**
  * tests/AutomationTests.cpp: lane commands and their rules, the file round trip, gain and pan lanes with the
    modes and overriding, a node parameter lane, sustain from a clip curve, recorded controllers.
  * UI tests: the fader curve and clip passes.
