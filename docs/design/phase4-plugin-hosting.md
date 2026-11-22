# Phase 4 design: plugin hosting

Status: **accepted** (implementation notes in section 12). Engine-side decisions are numbered P1–P10 and summarised in section 11.
UI decisions are in section 10.

## 1. Goals and non-goals

Goals:

* Load VST3 plugins (VST2 with `DAWG_ENABLE_VST2`, AU on macOS, LV2 on Linux) into a track's graph, as
  instruments or as effects.
* Drive instruments from the piano roll's notes, including curves and vibrato, over MPE.
* Open the plugin's own editor in a native window.
* Keep plugin state in the project model, so it survives undo/redo of graph edits and is saved with the
  project once saving exists.
* Align tracks of different latency at the master bus.

Non-goals for phase 4:

* Running plugins in a separate process. The architecture is single-process (decided in phase 1). A
  crashing plugin takes the app down. Sandboxing can come later behind the same node interface.
* Automation of plugin parameters (phase 11), sidechain and multi-bus layouts, multi-output instruments,
  CLAP (phase 11), MIDI 2.0.
* The node editor canvas (phase 8). Phase 4 needs a simpler way to put a plugin on a track (section 10).

## 2. Plugin node in the model

**(P1) One generic node type, `dawg.plugin`, with the plugin's identity and state stored on the node.**
Recommended. One registry type per installed plugin would put thousands of entries in the registry and make
a missing plugin an "unknown node type" error.

`GraphNode` gains an optional plugin block:

```cpp
struct PluginRef
{
    juce::String identifier;   // PluginDescription::createIdentifierString(), stable across rescans
    juce::String name;         // for display while missing, e.g. "Vital"
    juce::String format;       // "VST3", "VST", "AudioUnit", "LV2"
    bool isInstrument = false;
};

struct GraphNode
{
    ...
    std::optional<PluginRef> plugin;                         // set for dawg.plugin nodes
    std::shared_ptr<const juce::MemoryBlock> pluginState;    // opaque getStateInformation() blob, may be null
    MidiOutputSettings midi;                                 // section 5, instruments only
};
```

The state blob is shared between snapshots like every other model value, so undo does not copy it.
It is serialised as base64 in `toVar` and in patches. Patches carry it only when it changes (D3 already
keys nodes by id), so the UI rarely sees it. The UI never decodes it.

New commands:

* `addPlugin { trackId, identifier, position? }`: adds a `dawg.plugin` node and wires it in (see P2).
* `setPluginMidi { trackId, nodeId, mode?, bendRange? }`: changes the MIDI output settings (section 5).
* `removeNode` and the other graph commands work unchanged.

**(P2) Phase 4 wiring is a chain.** Recommended. Until the phase 8 node editor exists, each track's graph
is treated as `Sequencer → [instrument] → [effect …] → Gain → Track Output` (since phase 8 the Gain is optional,
phase8-node-graph.md, G4):

* Adding an instrument replaces the track's current instrument. That is the built-in MPE synth by default.
* Adding an effect appends it before `Gain`, or inserts it at a given index.

The commands rewrite the connections, so the model stays a general graph and phase 8 can edit it freely.

## 3. Plugin state and undo

Plugin parameters live inside the plugin, not in `GraphNode::params`. A plugin can have thousands of
parameters, and changes made in its own editor never pass through our commands.

**(P3) The live instance is the source of truth while it exists; the model's blob is refreshed at defined
moments.** Recommended. The app captures `getStateInformation()` from the live instance and writes it into the
current snapshot through a non-undoable `stamp`. A `stamp` replaces the current history entry's project
instead of pushing a new one, and it emits a normal patch. The moments are:

* Before a command that removes a plugin node, or a track containing one, so undo restores the plugin as it was.
* Before saving.
* When the plugin's editor window closes. This is cheap and keeps the blob reasonably fresh.

Undo of a graph edit then works naturally. Removing a plugin and undoing it creates a new instance from the
blob. The compiler's instance reuse (by `trackId, nodeId, type`) keeps the existing instance for every other
edit, so the plugin keeps its state and keeps sounding.

Parameter tweaks inside a plugin's editor are not undo steps in phase 4. That matches most DAWs, where
plugin-internal undo is the plugin's job.

## 4. Instantiation, threading and the audio callback

**(P4) Asynchronous loading with a placeholder.** Recommended. Creating a plugin instance can take seconds,
and VST3 requires the message thread. The compiler must not block on it.

* The `GraphCompiler` gets a `PluginHost` interface (implemented in the app, faked in tests). For a
  `dawg.plugin` node with no live instance, the compiler creates a `PluginNode` in the *loading* state and
  asks the host to instantiate the plugin with `createPluginInstanceAsync`.
* A loading or missing `PluginNode` passes audio through for effects and outputs silence for instruments.
  It also keeps the model's state blob untouched, so a project with a missing plugin round-trips without
  losing that plugin's settings.
* When the instance is ready, the host calls `setStateInformation` with the blob (if any), then
  `prepareToPlay`, attaches the instance to the `PluginNode`, and recompiles. Latency may have changed.
* On failure, the node stays *missing* and the error is reported to the UI (section 9).

`PluginNode` wraps `juce::AudioPluginInstance`:

* Ports: an instrument has `events in → audio out`. An effect has `audio in → audio out`; if it
  `acceptsMidi()`, it also gets an `events in` port, unused in phase 4. All buses are stereo main only, and
  the node requests that layout. Plugins that refuse a stereo layout are loaded with their default layout
  and adapted: mono is duplicated, extra channels are dropped.
* `process()` builds the block's `MidiBuffer` (section 5) and calls `processBlock`.
* The `MidiBuffer` and a scratch `AudioBuffer` are preallocated in `prepare()`. `MidiBuffer` needs an
  `ensureSize` big enough for a dense block: 16 channels × (bend + pressure + CC74) per 32-sample step.

**(P5) Allocation guard exemption around plugin calls.** Recommended. Plugins are third-party code and some
allocate in `processBlock`. The Debug allocation guard would report violations we cannot fix. `PluginNode`
wraps the call in a `ScopedAllowAllocation`, which counts plugin allocations separately and shows them as a
per-plugin warning in telemetry instead of a DAWg violation.

Threading rules:

* Instance creation, `prepareToPlay`, `setStateInformation`, `getStateInformation` and editor creation
  happen on the message thread.
* `processBlock` runs on the audio thread.
* JUCE's `AudioPluginInstance` already serialises state calls against processing for VST3 via the
  plugin's own locks. For safety, `setStateInformation` on an instance that is already in a published
  engine goes through `suspendProcessing(true)` / `(false)`.
* Instances are destroyed on the message thread. `SnapshotExchange` already guarantees the last reference
  is dropped there.

## 5. Notes to MPE MIDI

**(P6) Conversion lives inside `PluginNode`, not in a separate node.** Recommended. The node consumes
`NoteEvent`s, which carry the `CompiledNote` with its curves and vibrato (D5), and generates MIDI for the
block. A separate converter node would need a MIDI port type that nothing else uses yet. Phase 8 can split
it out if MIDI effects become a thing.

**MPE mode (default):**

* **Zone setup.** A lower zone uses the master channel 1 and member channels 2–16. On `prepare()` and after
  every transport start, the node sends:
  * the MPE Configuration Message (RPN 6, 15 members) on channel 1;
  * pitch-bend sensitivity (RPN 0) on every member channel.
* **Channel allocation.** Each note gets the least recently released free member channel. With more than
  15 sounding notes, the oldest note is released (note off) and its channel reused. Releases still in
  their tail keep their channel until a new note needs it; the least recent release is reused first.
* **Note on.** The node sends, at the note's sample offset and in this order:
  1. pitch bend, pressure and CC74 at their values at note start;
  2. the note on, with note number = `round(basePitch)` and velocity from the note.

  Fractional base pitches (microtonal) are carried by the bend.
* **Expression.**
  * Pitch bend = `basePitch + pitchCurve + vibrato − noteNumber`, scaled by the bend range (default
    ±48 semitones, 14-bit).
  * Pressure: channel pressure, 7-bit.
  * Timbre: CC74, 7-bit.

  Values are evaluated every 32 samples (about 0.7 ms at 48 kHz, matching the built-in synth's sub-block
  rate). A message is sent only when its quantised value changed. Bends beyond the range are clamped.
* **Note off.** The node sends the release velocity. `choke` and `chokeAll` send note off immediately, plus
  CC120 (all sound off) on members for `chokeAll`. `releaseAll` sends note offs for all sounding notes.

**Fallback mode for non-MPE plugins (`mode: "midi"`):**

* Everything goes on channel 1. Note on and off use the rounded pitch and velocity.
* Pressure goes out as polyphonic aftertouch.
* Pitch curves, vibrato and CC74 are dropped, since they would bend every note at once. Live pitch (a keyboard's
  wheel) goes out as channel pitch bend over the node's bend range.
* This mode is per node.

Defaults: a new node gets MIDI mode with a ±2 semitone bend range (docs/design/phase12-voice-scope.md, V5);
MPE plugins are switched to MPE, which defaults to ±48. Switching the mode moves the bend range to the new mode's
default. Users change the range (e.g. ±24 for MPE synths that ignore RPN 0) via `setPluginMidi`. In MIDI mode the
range scales live pitch (a keyboard's wheel) into channel pitch bend; no range message is sent. Before format
version 9, MIDI mode always used ±2 whatever the stored range.

**Live audition** (negative note ids) goes through the same path, so auditioning in the piano roll plays the
plugin.

## 6. Latency alignment

**(P7) Delay tracks to the longest track latency at the master bus.** Recommended. The compiler already
reports each `CompiledTrack`'s latency. It now:

* takes the maximum over all tracks;
* gives each track a delay line of `max − trackLatency` before its gain and mix into the master;
* shifts the sequencer's note scheduling earlier by the same compensation, so notes still line up with the
  playhead and the metronome.

The metronome is delayed by `max` too. The playhead shown in the UI is the audible position (the engine
reports `position − max`).

A plugin that changes its latency (`AudioProcessorListener::audioProcessorChanged` with `latencyChanged`)
triggers a recompile on the message thread.

## 7. Editor windows

**(P8) Native top-level windows owned by the app.** Recommended, and required: web content cannot host a
native plugin view.

* A `PluginWindows` manager in `app/` keeps at most one window per plugin node.
* A window is opened through the bridge with `plugin.openEditor { trackId, nodeId }` and closed with
  `plugin.closeEditor`.
* A window closes automatically when its node disappears from the model. That includes undo of an add.
* The window title is `Track name — Plugin name`.
* Window position is remembered per node, in app settings rather than in the project.
* Plugins without an editor get JUCE's `GenericAudioProcessorEditor`.

How these windows behave relative to the main window is a UI question (section 10).

## 8. Plugin scanning and browsing

Scanning already exists (out-of-process, per file). Phase 4 adds:

* `plugins.get` returns, per plugin:
  * `identifier`, `name`, `manufacturer`, `format`, `category`, `isInstrument`;
  * `numInputs` and `numOutputs`.
* Instantiation looks plugins up by identifier in the `KnownPluginList`. If the identifier is unknown, the
  plugin is missing and stays a placeholder (P4).
* Rescanning does not touch loaded instances.

## 9. Errors and telemetry

* Load failures and missing plugins are reported to the UI as a per-node status (`loading`, `ready`,
  `missing`, `failed: <message>`) in a `dawg_nodes` event. The status is not stored in the model, because
  it is runtime state.
* Telemetry gains per-plugin allocation counts (P5) and, per track, the compensated latency.

## 10. UI decisions (answered by the user)

1. **Insert UI: inspector in the track list.** The selected track expands in the left track list (Cubase
   inspector style), showing:
   * an instrument slot;
   * numbered insert slots;
   * a "+ add effect" slot.

   Each slot has bypass and remove buttons. Clicking a slot's name opens the plugin's editor. Effects are
   dragged up or down to reorder them, with a line showing where the effect will land; the context menu's
   Move up and Move down do the same without dragging. The piano roll keeps its full width.
2. **Picker: searchable popup.** Clicking an empty slot (or the instrument slot) opens a type-to-search
   popup, filtered to instruments or effects, showing format and vendor. It is keyboard driven.
3. **Editor windows: floating above DAWg.** There is one window per plugin, and several can be open. They
   stay above the main window but not above other apps, and hide when DAWg minimises.
4. **Settings and errors: slot context menu and badge.**
   * Right-clicking a slot offers MPE or MIDI mode, the bend range, bypass and remove.
   * Missing and failed plugins show a warning badge and a greyed name, with a tooltip giving the reason.

Bypass needs one more model field: `GraphNode::bypassed`. A bypassed plugin passes audio through, or outputs
silence if it is an instrument, and is not processed. The command is `setNodeBypass { trackId, nodeId,
bypassed }`.

## 11. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| P1 | One `dawg.plugin` node type; identity, state blob and MIDI settings stored on the node | Yes |
| P2 | Chain wiring (instrument + inserts) until the phase 8 node editor | Yes |
| P3 | Live instance is the source of truth; blob refreshed by non-undoable stamps at removal, save and editor close | Yes |
| P4 | Asynchronous instantiation with a placeholder node for loading and missing plugins | Yes |
| P5 | Allocation guard exempts plugin calls and reports them per plugin | Yes |
| P6 | Notes-to-MPE conversion inside the plugin node: lower zone, ±48 bend, 32-sample rate, MIDI fallback mode | Yes |
| P7 | Master-level latency alignment with sequencer pre-roll | Yes |
| P8 | Native editor windows managed by the app, one per node | Yes |
| P9 | In-process hosting; no sandbox in phase 4 | Yes |
| P10 | Project save/load comes in phase 5 (project workflow). Phase 4 adds a temporary JSON save/open (`.dawg.json`) so plugin state round-trips can be tested | Yes |

---

## 12. Implementation notes (phase 4 as built)

Where the code differs from the decisions above, and why:

* **Two node types instead of one (P1).** Ports are static per node descriptor, and an instrument
  (`events in → audio out`) and an effect (`audio in → audio out`) need different ports. So there are
  `dawg.pluginInstrument` and `dawg.pluginEffect` (`engine/include/dawg/PluginNode.h`). Effects have no
  event input yet.
* **`insertNode` instead of `addPlugin` (P2).** One command inserts any chain member:
  * plugins, which need `plugin`;
  * the built-in MPE synth, so a track can get it back after a plugin replaced it.

  `reorderNode` moves effects. `removeNode` reconnects the neighbours when it removes a chain member. The
  gain node cannot be removed. The chain logic is `findChain()` in `engine/src/Commands.cpp`, mirrored in
  `ui/src/components/chain.ts`.
* **Dragging reorders only a simple chain.** The inspector's and mixer's slots are draggable only where
  `trackChain()` found a chain; a graph with custom routing shows a read-only list and is edited in the graph
  view. A drop before slot *i* passes `i - 1` to `reorderNode` when the effect came from above, because the
  command removes the effect first; `reorderIndex()` in `ui/src/components/chain.ts` does that arithmetic for
  both places.
* **Plugin state is not sent to the UI (P1, P3).**
  * `toVar` leaves the state blob out, so patches never carry it.
  * The project file adds it as base64 (`engine/src/ProjectFile.cpp`).
  * `Session::amend()` is the non-undoable stamp.
  * `withPluginState()` builds the amended project.
* **Removed plugins stay alive for undo (P3).** The compiler keeps up to 16 removed plugin instances
  (`GraphCompiler::maxRetiredPlugins`). Undoing a removal then brings back the same live instance, with
  state that was never stamped. The instance is prepared again on the way back, which resets voices that
  were cut off. Stamps still happen before removals, so the model is correct once an instance drops out
  of that list.
* **Plugins survive device changes (P4).** On a spec change, the compiler re-prepares plugin nodes
  (`Node::keepAcrossSpecChange()`) instead of recreating them. `PluginNode::process()` try-locks the
  processor's callback lock and skips the plugin while it is suspended. That makes re-preparing and
  setting state safe while an older engine still references the node.
* **Bypass.**
  * A bypassed effect is skipped.
  * A bypassed instrument still runs and is muted, so it never misses a note-off.
  * Bypassing does not keep the plugin's latency, so a bypassed latent effect shifts its track until
    re-enabled.
* **Allocation counting (P5).** `ScopedAllowAllocation` counts plugin allocations in a separate global
  counter (`exemptAllocationCount()`, telemetry `pluginAllocations`), not per plugin.
* **Latency alignment (P7).** Tracks see the transport `CompiledEngine::latencySamples` ahead: tick range
  and `startSample` are both shifted. Their outputs are delayed to the slowest track. The metronome and the
  playhead stay unshifted.
* **Editor windows (P8).** `app/src/PluginWindows.cpp`:
  * Windows are always-on-top while DAWg is the foreground process, normal otherwise, and hidden while the
    main window is minimised.
  * Positions are stored per plugin identifier in the app settings.
  * Closing a window stamps the plugin's state.
  * Windows of removed nodes close before the engine is updated.
* **Status events.** `dawg_nodes` carries `{ status, error, latency, hasSidechain, editorOpen }` per
  `"<trackId>:<nodeId>"`. It is emitted only when that status changes, not after every command. The status
  does not ask the plugin whether it has an editor: JUCE's VST3 `hasEditor()` creates and destroys a view to
  answer, which takes around half a second for Spitfire's BBCSO and froze the app for seconds while a project's
  plugins loaded. A ready plugin always shows the editor button; `PluginWindows` opens the native editor when
  there is one and falls back to JUCE's generic parameter editor otherwise.
* **Temporary save/open (P10).** `project.save { saveAs }` and `project.open` use native file dialogs and
  are bound to Ctrl+S, Ctrl+Shift+S and Ctrl+O. There are no menu entries yet. Opening a file resets the
  compiler, because ids may repeat with a different meaning. It also calls `Session::reset()`, which
  clears history but keeps the revision counting, so the UI gets a normal patch.
