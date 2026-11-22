# Design: voice-scope graph (phase 12)

Status: **implemented**. The owner decided the open questions on 2026-10-28 (section 9). Steps 0 to 6 are done
(section 7). Decisions V1–V16 are summarised in section 8.

This revision replaces the first proposal, which predated graph modulation (modulation.md). What changed, and why:

* The first proposal added `PortType::modulation`. It exists now (M1): a mono -1..1 signal in an audio buffer.
* It added voice-only variants of the filter and gain with their own modulation inputs. Every built-in effect, the
  synth, Faust and plugins now have the modulation pool (M2) with a depth and polarity per slot (M5, M6), so the
  ordinary nodes can run per voice and no variants are needed.
* It modulated at a 32-sample control rate with linear ramps. The engine now passes per-sample values to built-in
  nodes (`ProcessContext::modulatedParams`); the filter cutoff, the gain and the utility follow them.
* It said a per-voice envelope was not needed in v1. The LFO and ADSR exist now and gate or retrigger on the note
  events they receive, so in a voice graph, where each voice receives only its own note, they become a per-note
  envelope and a per-note retriggered LFO.
* Modulation writes the modulated value into the target node's parameters and detects manual or automated edits by
  comparing with what it last wrote, one state per slot. Per voice that breaks: voices need their own values, and
  each would read the others' writes as edits. The voice scope needs its own way to apply modulation (V12).
* `Node::reset()` and a reusable voice allocator were proposed; both now exist (step 0).
* The file format is at version 10 (steps 1 and 2), not 3; the voice graph is version 11 (V16).
* New tracks keep the MPE Synth; the voice graph is opt-in (V4). Plugin instruments get plain MIDI by default (V5).

## 1. Goals and non-goals

Goals:

* An **editable per-note graph** inside an instrument: oscillators, envelopes, LFOs, filters and Faust run once per
  sounding note, and their audio sums into the track's ordinary graph. With the oscillator node this is a modular
  synth inside the graph view.
* Per-note **pitch, pressure and timbre** (curves and live MPE input), velocity and gate drive nodes in that graph
  through the modulation pool, like an LFO does today.
* Polyphonic **Faust instruments** (one DSP instance per voice).
* The usual guarantees still hold: sample-accurate note starts, chase, sustain, choke, voice stealing, release
  tails, latency compensation, per-track offsets, automation, undo and project files.
* Existing projects keep working and sound the same.

Non-goals (later phases or after v1):

* Per-voice hosted plugins. Hosted instruments stay in the track graph and receive MIDI or MPE as they do today.
* Per-voice sends, groups or sidechains; the track graph still owns routing.
* Voice graphs on audio, group, return or master tracks, and nested voice graphs.
* Parallel voice rendering. Tracks stay parallel; an instrument renders its voices one after another in v1.
* Note links/glide, custom expression lanes, configurable polyphony.

## 2. What already exists

* `TrackGraph` with typed ports, free routing, latency compensation and the React Flow editor (phase8-node-graph.md).
* The sequencer turns clips into sample-accurate `NoteEvent`s; a `noteOn` carries a `CompiledNote*`, so voices
  evaluate curves directly (phase2-engine.md, D5).
* Graph modulation (modulation.md): the modulation port type, the 8-slot pool on every effect and instrument, the
  LFO and ADSR sources with note and audio-trigger inputs, polarity and depth, and per-sample values for built-in
  targets.
* `VoiceAllocator` (VoiceAllocator.h): 16 voices, note-offs, sustain, chokes, transport release and choke, live
  expression, and re-resolving notes after an edit. The MPE Synth uses it; its sound stays in the synth.
* `Node::reset()`: puts a node back in its prepared state on the audio thread. The LFO, ADSR, filter, gain and
  utility implement it.

## 3. Model

**(V1) A Voice Instrument node owns a nested voice graph.**

```cpp
struct GraphNode
{
    // ... existing fields ...
    std::shared_ptr<const VoiceGraph> voiceGraph; // only for dawg.voiceInstrument
};

struct VoiceGraph
{
    std::vector<GraphNode> nodes;
    std::vector<Connection> connections;
};
```

* `dawg.voiceInstrument`: one **events** input (from the sequencer), one **audio** output, the modulation pool, no
  parameters of its own in v1.
* Its voice graph has two fixed nodes, which cannot be deleted:
  * `dawg.voiceInput` — one **events** output: this voice's note (its note-on, note-off, choke and expression
    events, and the sustain pedal). It plays the role the sequencer plays in a track graph.
  * `dawg.voiceOutput` — one audio input. The instrument sums every voice's output.
* The default voice graph is a playable synth:
  `Voice Input → Note Expression`, `Note Expression.Pitch → Oscillator (pitch)`, `Voice Input → ADSR`,
  `ADSR → Gain (amplitude at 0 %, depth +100 %)`, `Oscillator → Gain → Voice Output`.
* Node ids stay project-wide; voice-graph nodes are addressed by the owning `voiceNodeId` plus their own `nodeId`,
  so id-keyed patches and undo records work unchanged.
* The graph is nested in the node, so duplicating, copying or undoing a Voice Instrument carries its voice graph.

**(V2) Per-note expression is a modulation source: the Note Expression node.**

* `dawg.noteExpression`: one events input, modulation outputs:

  | Output | Signal | Meaning |
  |---|---|---|
  | Pitch | (pitch − 60) / 240 | The note's pitch with its pitch curve and vibrato, or with live pitch (V3) |
  | Pressure | 0..1 | The pressure curve, or live pressure |
  | Timbre | 0..1 | The timbre curve, or live timbre |
  | Velocity | 0..1 | The note-on velocity, held for the note |
  | Gate | 0 or 1 | 1 while the note is held (or sustained), 0 after its note-off |

* In a voice graph it follows the voice's note. In a track graph it follows the most recent held note (a mono
  expression source for track effects); when that note ends, an older held one takes over at once. With no note
  held the values hold and the gate is 0; before the first note they rest at C4, no pressure and middle timbre.
* It has no parameters. It tracks notes with a `VoiceAllocator`, so sustain, live expression and edits to the
  notes behave as in the MPE Synth.
* Expression is evaluated every 32 samples and interpolated linearly in between, as the MPE Synth does.

**(V3) The oscillator gets its pitch only through its pitch parameter.**

* `dawg.oscillator` has no events input. Its `pitch` parameter is in semitones relative to C4 (MIDI 60), range
  −120..120, default 0. Unpatched, it plays C4 plus its coarse and fine tuning: a fixed-frequency source.
* Pitch tracking is a patch: Note Expression's Pitch output into a modulation slot on `pitch` at 100 % depth. The
  Pitch output is scaled for exactly that (modulation adds depth × signal × the parameter's 240-semitone range),
  so it tracks the note exactly, and an LFO on the same parameter at 1 % depth is a ±2.4 semitone vibrato.
* The oscillator follows `pitch` per sample (`modulatedParams`) without smoothing, so pitch curves and bends are
  exact. Coarse (±48 semitones) and fine (±100 cents) are ordinary parameters.

**(V4) The voice graph is opt-in; new tracks keep the MPE Synth.**

* New instrument tracks still get `Sequencer → MPE Synth → Gain → Track Output`.
* The MPE Synth's inspector gets "Convert to voice graph": `convertToVoiceGraph { trackId, nodeId }` turns it into
  a Voice Instrument with the default voice graph, keeping its id, position and connections, as one undo step. The
  synth's parameters go, with their modulation inputs, automation lanes and MIDI mappings.
* No automatic migration: a saved `dawg.mpeSynth` stays one.

**(V5) Plugin instruments get plain MIDI by default.** A new plugin instrument node sends plain MIDI on channel 1;
MPE (one member channel per note) is opt-in in its MIDI settings. Each mode has its own default bend range: ±2
semitones for plain MIDI, ±48 for MPE. Switching the mode moves the range to the new mode's default; the range can
then be changed in both modes. Plain MIDI uses it for live pitch (a keyboard's wheel) and sends no range message,
as before. Files keep what they saved; a plugin node saved without MIDI settings still reads as MPE at ±48. Plain
MIDI used to ignore the stored range and always bend ±2, so format version 9 (`migrate8To9`) sets plain MIDI nodes
to ±2.

**(V6) Voice nodes.** Allowed in a voice graph: every built-in node except the ones that are about tracks, and Faust:

| Allowed | Rejected |
|---|---|
| Oscillator, Note Expression, LFO, ADSR, Filter, EQ, Compressor, Utility, Gain, Mix, Loop Reverb, Faust | Hosted plugins, Sequencer, Audio Player, Mix Input, Track Output, MPE Synth, Voice Instrument |

The two fixed nodes appear only in voice graphs. The compressor's key input is internal only in a voice graph (no
key sends into a voice).

**(V7) Polyphony is fixed at 16 voices** (`VoiceAllocator::numVoices`). The node reports its sounding voices; the
telemetry carries them as `voices` by `"<trackId>:<nodeId>"`, and the graph view shows them on the node.

## 4. Compiler and runtime

**(V8) Compile a voice program once, instantiate it per voice.**

* The track planner recognises `dawg.voiceInstrument` and plans its voice graph with the same topological,
  buffer-allocation and latency machinery as a track graph (`TrackPlanner` without a voice compiler), into a
  `CompiledVoice` (CompiledEngine.h): one `CompiledVoiceSlot` per voice slot with its step list, arena, event
  buffers and the Voice Output's buffer, and the voice graph's latency. The Voice Input runs no step: it is the event
  buffer the instrument writes the voice's events into. Voice-graph nodes that don't reach the Voice Output get no
  instance (a voice graph holds no plugins to load early).
* The voice graph is planned once per slot. Node instances are cached by `(track, node, type, delay, voice)`, so
  each of the 16 slots has its own instances and arena: oscillator phases, filter states and Faust DSPs are per
  voice. The compiled step carries the `CompiledVoice`, and the engine hands it to the node in
  `ProcessContext::voice`.
* All voices share one latency; the track planner adds it to the Voice Instrument's latency, so master alignment,
  negative offsets and sends behave as before. The voice steps run through the same `runSteps()` as a track's.

**(V9) The Voice Instrument node allocates voices and runs them.**

* `VoiceInstrumentNode` is a real `Node`, cached across compilations, with a `VoiceAllocator`. Its compiled
  `CompiledVoice` reaches it through the processing context (`ProcessContext::voice`).
* Each block it routes the incoming events to the voices: each voice gets its own `EventBuffer` with only its
  note's events (and the sustain pedal), so the LFO, ADSR and Note Expression inside see one note.
* Each active voice runs the voice program over the whole block and its Voice Output is summed into the node's
  output. On the engine's track threads (`ProcessContext::parallel`, docs/design/phase2-engine.md, 5.6), the playing
  voices render at the same time, each into a buffer of its own, and the buffers are summed in slot order: the same
  sum, to the bit, as rendering them one after another. A slot freed during its block is freed once all slots are
  done, since the allocator is shared.
* **Stealing**: the oldest releasing or choking voice, else the oldest held voice (`StealPolicy::releasedFirst`; the
  MPE Synth keeps `StealPolicy::oldest`). The stolen voice fades out over 5 ms in its own slot, the slot's nodes are
  reset (`Node::reset()`), and the new note starts in it right after the fade. The note starts up to 5 ms late only
  when every voice is busy; its events wait in the slot's queue until then, and may carry into the next block.
* A new note in a free voice resets that slot's nodes first, so it starts from a clean state. A note that starts
  while the sustain pedal is down gets a pedal-down event first, so its envelope sustains.
* Each slot renders its block in pieces, split only where its own note starts or its fade ends; events reach the
  voice's nodes at their sample offsets within the piece.
* **End of a voice**: a released voice runs until its output has been below −90 dB for 50 ms after the note-off; after
  a safety tail of 10 s it fades out like a stolen voice. A choked voice (a choke, a seek, a removed note) fades out
  over 5 ms. Either way the slot is then freed.
* Seeks and transport stops already send `chokeAll` and `releaseAll`; they reach every sounding voice's nodes too.
* The node's `meters()` report the sounding voices as `{ voices }`.

**(V10) Recompiles keep the voices.** Parameter changes stay in place. Structural changes recompile the voice
program; instances are reused by slot, so phases, filter states and Faust instances survive an edit that keeps the
node. Active voices and their note ids belong to the cached `VoiceInstrumentNode`, so a recompile does not restart
them.

**(V11) Parameters.** Every voice slot's instance of a node gets the same parameter values, set from the model on
every compilation and on every in-place parameter change, as for a track node. Automation of a voice node writes all
16 instances: its compiled lane holds the other slots' instances in `CompiledLane::voiceNodes`. A lane's target names
the voice node by its id, like a track node (ids are unique in the project), and lives on the track.

**(V12) Modulation per voice.** A voice node's modulation slots compile as in a track graph, once per voice slot:
each slot has its own instances, its own `ModulationState` per modulation slot (keyed by voice) and its own per-sample
buffers. Each voice computes its modulated value from its own sources and writes it to its own instance, so the usual
base detection (M5) works per voice: no voice reads another's writes as an edit. Plugin targets cannot occur (V6).

**(V13) Sends, keys and metering are unchanged.** The Voice Instrument is the track's source, like the MPE Synth.

## 5. Nodes

**(V14) The oscillator.** Sine; saw and pulse band-limited with polyBLEP, triangle with polyBLAMP; white noise.

| Parameter | Range | Notes |
|---|---|---|
| Waveform | sine, triangle, saw, pulse, noise | choice |
| Pitch | −120..120 st, 0 = C4 | modulation target for pitch tracking (V3) |
| Coarse | −48..48 st | |
| Fine | −100..100 cents | |
| Pulse width | 1..99 % | pulse only; modulatable (PWM) |
| Level | −60..0 dB, default −12 | modulatable, smoothed over 1 ms |
| Unison | 1..8 voices | detuned copies inside the node |
| Detune | 0..100 cents | spread of the unison voices |
| Spread | 0..100 % | stereo spread of the unison voices |
| Phase | 0..360° | start phase; further unison copies are offset by the golden ratio |
| Random phase | 0..100 %, default 0 | moves each copy's start by up to this share of a cycle, anew at every start, so unison attacks don't all chirp alike |
| Free phase | toggle | `reset()` leaves the phases running; they start at random phases |

The output is stereo (spread); the copies are mixed at equal power (1/√n). `reset()` sets the phases, unless the
phase is free. Pitch, fine, pulse width and level follow per-sample modulation; pitch and fine without smoothing.

Aliasing, measured as all alias energy against all harmonic energy: saw −35 dB at 430 Hz and −28 dB at 1730 Hz
(a naive saw: −13 dB at 1730 Hz); pulse −30 to −38 dB; triangle −55 to −73 dB. Two-point polyBLEP is the usual
trade-off for a virtual-analogue oscillator; oversampling can come later if high notes need it.

**(V15) Polyphonic Faust.** A Faust node in a voice graph has one `FaustNode`, with its own DSP, per voice slot; all
run the script's newest program, and a script edit crossfades in every slot. `FaustNode::reset()` clears the DSP's
state (`instanceClear()`) when a note starts in its slot. Faust parameters stay block-rate; the per-sample values are
not used. Note expression reaches a script through its sliders: the example `Voice synth`
(app/faust/examples/voice-synth.dsp) has a `pitch` slider over −120..120 semitones, like the Oscillator's (V3), and a
`gate` slider, for Note Expression's Pitch and Gate at 100 %. A voice piece starts at the note-on, so the gate opens
on the note's sample; it closes within a block of the note-off.

The UI learns a voice Faust node's ports and parameters from its status (`plugins.status`), like a track node's: the
program its slots run, or, for a node that doesn't reach the Voice Output and so has no instance, its script's
program, which the status asks the compiler for. Its bargraphs show the largest value over its voices.

**CPU.** 16 held voices, rendered offline in Release on the development machine (the hidden `[benchmark]` test): the
default voice graph takes 6 % of real time; the Voice synth script 30 % with libfaust's interpreter, 3.1–3.6 % with its
LLVM backend, the default (phase11-faust-panels.md, section 7).

## 6. UI, commands and file

**UI (agreed: double-click and breadcrumb).** The Graph view shows the track graph. A Voice Instrument node shows
its sounding voices and an "Edit voice graph" button; double-clicking it switches the same canvas to its voice graph
(`voiceScope` in the UI store), with a breadcrumb `Track › Voice Instrument` whose track name leads back. The add menu
offers only the allowed nodes (V6) and no plugins; Voice Input and Voice Output cannot be deleted. The inspector,
device dock, modulation slot editing and the automation picker (`Voice Instrument › Oscillator`) work on voice nodes:
they find a node by id in the track's graph or its voice graphs (`graph/voice.ts`). The MPE Synth's slot menu and
inspector offer "Convert to voice graph", which then shows the voice graph; a Voice Instrument's slot menu, inspector
and dock offer "Edit voice graph". The Voice Instrument is in the instrument picker and the add menu. Copy and paste
carry a Voice Instrument's voice graph. The UI never edits a second model.

**Commands** take an optional `voiceNodeId`: `addNode`/`addNodes`, `removeNode`/`removeNodes`, `connect`/`disconnect`,
`setNodeParam`, `setNodeBypass`, `setNodePosition`, `setNodeCode`, `setModulation`, `setModulationShape`. The voice
graph is edited copy-on-write inside its owner (`updateGraph` in Commands.cpp). `addNode` of a Voice Instrument
creates the default voice graph; removing one forgets the automation and MIDI mappings of its voice nodes too;
`duplicateTrack` gives the copies new ids. New: `convertToVoiceGraph`. The chain tools (`insertNode`, `reorderNode`)
and `setPluginMidi` refuse a `voiceNodeId`; `insertNode` of a Voice Instrument gives it the default voice graph.
Without a `voiceNodeId`, the node a command names (`nodeId`, `fromNode`, the first of `nodeIds`) decides the graph,
so callers that only know a node id (the inspector's controls, MIDI learn, automation writing) work on voice nodes
too; `addNode`/`addNodes` name no node and need it. A pasted Voice Instrument (`addNodes` with a `voice` object in the
model's form) brings its voice graph, checked like a file's (`projectfile::voiceGraphFromVar`) and given new ids.
Automation targets name voice nodes by id (V11).

**(V16) Project format version 11.** A Voice Instrument serialises a `voice` object with id-keyed `nodes` and
`connections`. `migrate10To11` is the identity: older files are valid as they are. Reading checks a voice graph as
strictly as a track graph: a node type that can't run per voice (V6), a missing or doubled Voice Input or Voice
Output, or a Voice Input or Output in a track graph makes the file invalid. (Old projects are test projects; nothing
needs repairing.)

## 7. Steps

Each step is its own branch-sized piece with tests, approved before it starts.

0. **Preparation** ✅ (branch `voice-prep`): the LFO starts at its set phase; `Node::reset()`; the gain and utility
   follow per-sample modulation; `VoiceAllocator` taken out of the MPE Synth (bit-identical output).
1. **Plugin MIDI default** ✅ (V5): plain MIDI for new plugin instruments, a bend range per mode, format 9.
2. **Oscillator and Note Expression** ✅ in the track graph (V2, V3, V14). Both are useful on their own: a mono
   synth from Sequencer → Note Expression → Oscillator → Gain, with an ADSR. The graph view's add menu offers both;
   Note Expression counts as a modulation source in the inspector. No device panel yet: the dock shows knobs.
3. **Voice graph model, commands and file** ✅ (V1, V6, V16): nested graph, fixed nodes, default voice graph,
   `voiceNodeId` in commands, `convertToVoiceGraph`, format 11, validation, undo and duplicate tests. The Voice
   Instrument compiles as a silent node and stays out of the add menu until steps 4 and 5.
4. **Voice compiler and runtime** ✅ (V8–V12): voice program, per-slot instances and arenas, per-voice events, steal
   policy and fade, voice end, per-voice modulation, latency. Engine tests: pitch, independent voices (two notes
   together play exactly what each plays alone), chase, sustain, seek choke, stealing without clicks, release tails,
   recompiles that keep voices, parameters in every slot, per-slot compilation and latency, no allocation. The Voice
   Instrument plays, but stays out of the add menu until step 5; until then only the `convertToVoiceGraph` command
   makes one.
5. **UI** ✅: scope navigation and breadcrumb, voice badge, add-menu filtering, fixed-node protection, conversion and
   edit buttons, inspector, dock and automation on voice nodes, paste with the voice graph; on the engine side
   voice-node automation in every slot, node-id scope inference and `insertNode` of a Voice Instrument. Checked in the
   app: convert, edit a voice node from the inspector, the add menu, play a chord (3 voices, back to 0 after stop).
   An unconnected Oscillator in a track graph still makes the track "custom routing" (the chain rules count every
   audio node, as the engine's `findChain()` does).
6. **Polyphonic Faust and polish** ✅ (V15): per-voice Faust instances cleared per note, status and bargraphs for
   voice Faust nodes, the Voice synth example, the voice count in the telemetry (step 5), a CPU benchmark. Engine test:
   two overlapping Faust voices play exactly what each plays alone, and end after their release. Checked in the app:
   the Voice synth added in a voice graph shows its script's ports, and a three-note chord plays in 3 voices that end
   after the release.

## 8. Decisions

| # | Decision | Status |
|---|---|---|
| V1 | A Voice Instrument node owns a nested voice graph with fixed Voice Input (events) and Voice Output | Agreed |
| V2 | Per-note expression is the Note Expression source; mono last-note in track graphs | Agreed by owner |
| V3 | The oscillator's pitch comes only through its pitch parameter, patched from Note Expression | Agreed by owner |
| V4 | Voice graph opt-in; new tracks keep the MPE Synth; a convert command | Agreed by owner |
| V5 | Plugin instruments default to plain MIDI; MPE opt-in | Agreed by owner |
| V6 | All built-ins except track-level nodes, plus Faust; no plugins | Agreed by owner |
| V7 | 16 voices, fixed | Agreed |
| V8 | One voice program, instantiated per voice slot | Agreed |
| V9 | The instrument allocates; per-voice event buffers; steal released first with a 5 ms fade | Agreed by owner |
| V10 | Recompiles keep instances and voices | Agreed |
| V11 | Parameters shared by all slots, set from the model | Agreed |
| V12 | Modulation applied per voice around the model value | Agreed |
| V13 | Sends, keys and metering stay track-scope | Agreed |
| V14 | Oscillator with basic waveforms and unison | Agreed by owner |
| V15 | Polyphonic Faust, block-rate parameters | Agreed |
| V16 | Format 11, identity migration | Agreed |

## 9. Owner decisions and open questions

Decided on 2026-10-28:

1. Default for new instruments: plugin instruments default to plain MIDI; new tracks keep the MPE Synth; the voice
   graph is opt-in (V4, V5).
2. Expression: a Note Expression modulation source rather than fixed expression ports (V2).
3. Voice scope contents: all built-ins and Faust (V6).
4. Navigation: double-click and breadcrumb (section 6).
5. Oscillator pitch: only through its pitch input (V3).
6. Stealing: released voices first, with a 5 ms fade (V9).
7. Oscillator: basic waveforms plus unison in v1 (V14).
8. Bend range: plain MIDI defaults to ±2 semitones, MPE keeps ±48 (V5).

## 10. Testing plan

* **Model/commands:** nested add/remove/connect/disconnect, id uniqueness, copy/duplicate, undo, conversion, removal
  of a Voice Instrument, fixed-node protection, rejected node types, automation targets in voice scope.
* **File:** version 11 round trip, version 10 files unchanged, malformed nested graphs repaired, id remapping on paste.
* **Compiler:** voice program order, one instance per slot, latency, parallel branches, per-voice modulation
  buffers, track offsets, sends and sidechains still aligned.
* **Audio:** one note starts one voice; two notes are independent; pitch/pressure/timbre change only their voice;
  chase mid-note; sustain; choke and seek; stealing without clicks; release tails end; live expression; an ADSR and
  a retriggered LFO per note; no allocation in `process()`.
* **UI:** scope navigation, add-menu filtering, connection validation, fixed nodes, parameter, modulation and
  automation editing, copy/paste.
