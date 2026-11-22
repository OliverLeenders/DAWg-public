# Design: parameter links

Status: **accepted and built** (2026-11-15). The owner accepted every recommendation, the decisions in section 4
(L1–L16) and the recommended answers in section 6, on 2026-11-15. Section 9 says how it was built and what differs.

## 1. What this is

A **link** makes one parameter follow another through a mapping: when the source moves, for whatever reason (a drag,
MIDI, automation, modulation, the plugin's own editor), the target takes `map(source)`. Sources and targets can be
built-in node parameters or plugin parameters, on the same node graph or not.

The motivating case is **emphasis and de-emphasis**: two EQs around some processing (a saturator, a compressor, a tape
plugin), the second mirroring the first with every band gain inverted and every frequency, Q, type and switch the
same, so the pair cancels apart from what the processing in between did.

```
 audio ─▶ [EQ A  +6 dB @ 3 kHz] ─▶ [Saturator] ─▶ [EQ B  −6 dB @ 3 kHz] ─▶
              │                                       ▲
              └──── link: gain → −gain, freq → freq, Q → Q, type → type ─┘
```

Other mappings: identity, inverted, an offset, a scale, and a custom range or curve.

The owner feels macros cover this "to a degree". Section 2 says how far, and section 3 why a link is a different
thing from a modulation, though it can share parts of it.

## 2. What exists today

### 2.1 Macros (macros-and-modulation-column.md, MC5–MC16)

* A macro mapping is a **modulation slot that names its knob**: `ModulationSlot::macro` (`engine/include/dawg/Model.h:460`,
  the field at about line 485). It has a target, a signed depth −1..1 and an active flag, and nothing else: no range, no
  curve, no offset (`Model.h:460`–`496`).
* `mapMacro` (`engine/src/Commands.cpp:5275`) reuses the node's slot for that parameter and knob, or takes a free one
  of the node's **eight** modulation ports (`maxModulationInputs`, modulation.md M2), and fails when all are used.
* A slot is **relative**: the engine writes `modulatedParamValue (info, base, amount)` (`engine/src/AudioEngine.cpp:1285`,
  `writeModulation`), where the base is the parameter's own value, re-adopted whenever the parameter moved since
  modulation last wrote it (`modulationBase`, `AudioEngine.cpp:1267`).
* The compiler turns a macro slot into a cable from the Macros device after components are flattened (`wireMacros`,
  `engine/src/GraphCompiler.cpp:1927`), and gives each voice a follower (`followMacros`,
  `engine/src/ModulationNodes.cpp:795`).

What that means for the EQ pair:

* **Opposite polarity: yes.** Depth is signed, so one knob can map EQ A's band gain at +0.5 and EQ B's at −0.5. With
  the knob's neutral at Centre, a full turn swings ±1 × 0.5 × the gain's 48 dB range (`engine/src/EffectNodes.cpp:350`,
  −24..+24 dB), so A goes +24 dB and B −24 dB. Frequencies follow in octaves (MC1), so mapping both frequencies at the
  same depth moves them together.
* **Per-target range or inversion: only as a signed depth.** There's no minimum and maximum, offset or curve per
  target; component-nodes.md (N6, question 2) already noted that a real macro "needs a range and curve for each
  mapping".
* **It mirrors movements, not values.** Each target moves around its own base. The pair only cancels if both bases
  are already mirrored (A at +g, B at −g) and the macro is the only thing that moves them.
* **Grabbing a target breaks the pair.** A drag on EQ B's gain is a manual change, so `modulationBase` adopts it as B's
  new base and the macro goes on adding around it; A doesn't move. Automating A's gain doesn't touch B. Dragging A's
  curve on the EQ panel isn't caught by Map mode at all (macros-and-modulation-column.md, section 3).
* **It doesn't scale to a whole device.** Mirroring an EQ means about 30 parameters per pair (eight bands' gain,
  frequency, Q, type, slope and switches: `eqBands`, `EffectNodes.cpp:447`), but a node has eight modulation ports,
  and choice and toggle parameters can't be modulated at all (`ParamInfo::acceptsModulation`,
  `engine/include/dawg/Node.h:205`).

So macros give a one-knob, two-polarity remote control, not a link.

### 2.2 Automation and touch

* A lane targets a node parameter by `{ kind, nodeId, param }` (`AutomationTarget`, `Model.h:1321`; lane at
  `Model.h:1358`), values normalised 0..1. Lanes live on the track (`Track::automation`, `Model.h:1547`).
* The audio thread plays every lane once per chunk, before the steps run (`automate`, `AudioEngine.cpp:49`): it sets
  `lane.node->params()` and each voice instance's (`CompiledLane::voiceNodes`), or a plugin's parameter through
  `setAutomatedParameter` / `automateByEvents` (sample-accurate for CLAP, `AudioEngine.cpp:33`).
* Touch: DAWg's controls call `automation.touch` (`ui/src/automation/touch.ts:6`, handled at
  `app/src/AutomationControl.cpp:78`); plugin editors report gestures through the host's listener
  (`app/src/PluginHost.cpp:285`–`295`, routed at `AutomationControl.cpp:116`); MIDI learn touches for half a second.
  A lane being written is marked `overridden` so its parameter keeps its manual value (`syncAutomationOverrides`,
  `AutomationControl.cpp:168`).
* Nothing ties two lanes together: automation of A never moves B.

### 2.3 Exposed parameters and MIDI learn

* `ExposedParam { nodeId, param, name }` (`Model.h:1153`, command at `Commands.cpp:5044`) shows an inner parameter on
  its component. It **is** the inner parameter, so it's an alias, not a mapping: one value, no transform
  (component-nodes.md, N6).
* `MidiMapping { device, channel, controller, trackId, nodeId, param }` (`Model.h:1719`) has no range or curve: a CC
  sets the parameter along its knob's curve (`paramValue`, `app/src/MidiControl.cpp:24`) through an ordinary
  `setNodeParam` with a gesture id (`applyMidiMapping`, `MidiControl.cpp:162`). It looks the parameter up in the node's
  built-in descriptor, so a plugin parameter can't be MIDI-learned this way.
* One CC can feed several mappings, so MIDI learn can already drive two parameters at once, both in the same direction
  and only while the controller moves.

### 2.4 Modulation sources (modulation.md, M1–M7)

* LFO, ADSR, Note Expression and the Macros device output a modulation signal (−1..1) on a modulation port; a slot on
  the target adds `source × depth`, scaled by the target's range, octaves or knob travel (MC1–MC3), around the base.
* Applied once per block just before the target node runs, and per sample for the parameters that read
  `ProcessContext::modulatedParams` (the filter's cutoff, the gain, the utility).
* Cables only connect nodes in one graph; macro slots are the one way to reach a component or voice graph without a
  cable (MC7).
* There is no source that reads a parameter. A "parameter follower" source would let the existing slots do relative
  links, but not absolute ones (section 3, option C).

### 2.5 How the engine sets parameters

* **Built-in.** The model holds the value (`GraphNode::params`, `Model.h:876`). `setNodeParam` clamps and stores it
  (`Commands.cpp:5412`); when only values changed the compiler keeps the instances and copies them into each node's
  `ParamBlock` (`sameStructure` at `GraphCompiler.cpp:2059`, `applyParams` at `GraphCompiler.cpp:323`). `ParamBlock`
  is an array of atomics written by the message thread and read by the audio thread (`Node.h:711`). Automation and
  modulation write the same block from the audio thread.
* **Plugins.** The model holds no values: the live instance is the source of truth and its state is an opaque blob
  (`GraphNode::pluginState`). A parameter is addressed by its stable id and resolved to an index on the message thread
  (`PluginNode::parameterIndex`, `engine/include/dawg/PluginNode.h:78`); the audio thread reads and writes by index
  (`parameterValue`, `setAutomatedParameter`, `setModulatedParameter`, `modulateParameter`, `automateParameter`:
  `PluginNode.h:86`–`147`). `plugin.parameters` lists them for the UI (`AutomationControl.cpp:86`).
* **Out of process.** The `RemoteInstance` keeps a copy of every value: a write on this side marks it dirty for the
  next block (`parameterSet`, `engine/src/RemotePlugin.cpp:1619`), and the helper reports the plugin's own changes and
  gestures back (`RemotePlugin.cpp:800`–`814`, `valueFromPlugin` and `gestureFromPlugin` at about 1627–1650). So reads
  and writes from the audio thread work the same in and out of process, one block apart at most.
* **Value changes from a plugin's editor aren't observed.** `PluginHost::audioProcessorParameterChanged` is empty
  (`PluginHost.cpp:280`); only gestures are. The audio thread can still see the new value by reading it.
* **Rescan.** A CLAP plugin that adds or removes parameters after it was prepared is replaced by a new instance made
  from its state (`ParameterListSource`, `engine/include/dawg/ClapPlugin.h:79`); automation of a parameter that went
  stays in the model, unused, and resolves again if it comes back.

### 2.6 Threading

Independent tracks render in parallel on a worker pool, one block in flight (`AudioEngine::WorkerPool`,
`AudioEngine.cpp:284`). Within a track the steps run in plan order. Tracks run ahead of the playhead by their latency
and are aligned at the master (`CompiledEngine.h:577`–`600`).

## 3. Options

All three keep the link itself in the project model; they differ in who carries the value across.

### Option A: links in the model, propagated by commands

`setNodeParam` on a source also writes `map(value)` to each target, in the same command and so the same undo step.

* Simple; no engine work; the saved project always holds mirrored values; undo is free.
* Misses everything that doesn't go through a command: automation, modulation, a plugin's own editor (plugin values
  aren't in the model at all, 2.5). Automate EQ A's gain and B stays put: the motivating case fails as soon as
  anything moves.

### Option B: links in the model, followed by the engine (recommended)

The model holds `ParamLink` records. The compiler resolves each to source and target addresses (a `ParamBlock`
index, or a plugin index, like `CompiledLane` does). On the audio thread, just before the target node runs, the
engine reads the source's **effective** value (after automation and modulation) and writes `map(source)` into the
target, before the target's own modulation is applied. The command layer adds what the engine can't do: it keeps the
target's model value mirrored for parameters that need a recompile, and redirects edits of a target to its source.

* Covers every way a source can move, in and out of process, built-in or plugin.
* Same-block in the common case: EQ A runs before EQ B in the plan, so B reads A's value of this block. Both EQs smooth
  their parameter changes the same way, so they also stay in step while moving.
* Needs compiler and engine work (one more pass over a short list per block, no allocation), and rules for
  driven parameters (one driver, no cycles, lanes on a driven parameter don't play).

### Option C: a link as a graph node

A "Parameter follower" modulation source that outputs a parameter's value as a signal, wired by cable to the target's
modulation port, with a new "absolute" slot mode that replaces instead of adds.

* Visible as cables in the graph view, ordered by the planner like any source.
* A node has eight modulation ports; an EQ mirror needs about thirty. Choice and toggle parameters can't be
  modulation targets. Cables don't cross graphs or tracks. A cable per parameter is noise in the graph view. It would
  stretch the modulation machinery (relative by design: M5) into something it isn't.

### Recommendation

**Option B**, sharing what modulation already has: the parameter addressing of `CompiledLane` and
`CompiledModulation`, the base handling of `ModulationState`, `modulatedParamValue`'s scales for the mapping's units
(MC1–MC3), and the macro's "Map mode" for the UI. Links stay their own records rather than a kind of modulation slot,
because they're absolute, not pooled per node, and can reach choices, toggles, other tracks and plugin editors.

## 4. Decisions

### The model

**(L1) A link is a project-level record.**

```cpp
struct ParamAddress
{
    ObjectId trackId = 0; // the track (or master) that holds the node
    ObjectId nodeId = 0;  // any depth: components, voice graphs (node ids are project-wide)
    juce::String param;   // built-in id, or the plugin's stable parameter id
};

struct ParamLink
{
    ObjectId id = 0;
    ParamAddress source;
    ParamAddress target;
    LinkMapping mapping;
    bool active = true;
};

// Project::links
std::vector<ParamLink> links;
```

It lives on the project, not a track, because links may cross tracks (L10). Saved as `links` in the project file;
there are only test projects, so no migration.

**(L2) A mapping is one of a few kinds, in the parameter's own units where it has them.**

```cpp
enum class LinkMappingKind { identity, inverted, offset, scaled, range };

struct LinkMapping
{
    LinkMappingKind kind = LinkMappingKind::identity;
    double amount = 0.0;   // offset: added; scaled: factor (around `centre`)
    double centre = 0.0;   // inverted and scaled: the value they turn around (default: the range's centre)
    double inMin = 0.0, inMax = 1.0, outMin = 0.0, outMax = 1.0; // range, normalised
    Curve curve;           // range: optional shape between them (empty is a straight line)
};
```

* **Identity**: `y = x`.
* **Inverted**: `y = 2c − x` around a centre `c`. For EQ gain (−24..+24 dB, centre 0) that's `−gain`. The default
  centre is the range's centre in units, which mirrors any symmetric range; for a plugin it's 0.5 normalised.
* **Offset**: `y = x + amount`. **Scaled**: `y = c + amount × (x − c)`; `amount` −1 is inverted.
* **Range**: the normalised source over `inMin..inMax` goes to `outMin..outMax` along `curve`. Inverting a range is
  `outMin > outMax`. This is the one kind that works between any two parameters, plugins included.

The unit-based kinds use the parameter's modulation scale (MC1–MC3): on a frequency, an offset is in octaves and a scale
acts on log frequency, so "an octave above" stays an octave everywhere. They need both ends to share the unit
(`ParamInfo::unit`, same scale); otherwise only identity in normalised terms and range are offered. Choice and toggle
targets take identity only (a choice index copied; a choice with a different count is rejected). The result is
clamped to the target's range.

**(L3) One driver per parameter, no cycles.** A parameter can be the target of at most one link. Links may chain
(A → B → C) and fan out (A → B, A → C). `addLink` rejects a link that would close a cycle, or a second driver, with a
`CommandError`. So the links form a forest, evaluated root first, and no feedback is possible in the engine.

**(L4) Two-way feel, one-way truth.** A link is one-way in the engine. Editing a driven parameter from DAWg's controls
or MIDI learn is redirected to the root of its chain: `setNodeParam` on a target with an invertible mapping (all
kinds but a range with a non-monotonic curve) sets the source to `inverse(value)` instead, in the same gesture. So
dragging EQ B's gain up drags EQ A's down, and both stay mirrored. A non-invertible link makes its target read-only
in the UI (shown as driven, L13).

**(L5) Whole-device links are a set of parameter links.** For two nodes of the same type, *Link device ▸* creates one
link per linkable parameter in one command (`linkNodes`), with a preset: *Mirror* (identity everywhere) or, for an EQ,
*Mirror, gains inverted* (inverted on every `…Gain`, identity elsewhere). The UI groups links by node pair, so a
mirrored EQ shows as one entry with its exceptions. There's no separate "node link" record: a single parameter of the
pair can be unlinked or remapped later, and parameter links survive the same way whatever made them.

### The engine

**(L6) The engine follows the source's effective value.** A compiled link (`CompiledLink`, alongside `CompiledLane`)
holds the source and target as a `ParamBlock` index on a node (with the target's voice instances, as
`CompiledLane::voiceNodes`) or a plugin index, and its mapping. Just before a target node runs, after `automate()` and
before the target's modulation is written, the engine evaluates the links into that node in chain order: read the
source (`params().get` or `PluginNode::parameterValue`), map, write (`params().set`, or `setAutomatedParameter` /
`automateParameter` for a plugin). The target's modulation slots then add around the linked value, as
`modulationBase` adopts it. So a link follows drags, automation, MIDI, the source's modulation and the source plugin's
editor alike. No allocation and no locks: the list is built by the compiler.

**(L7) Block rate first.** A link writes once per block. Built-in nodes smooth the change over the same time on both
ends, so a mirrored pair moves in step. A source read per sample (`ProcessContext::modulatedParams`, M5) is followed at
block rate in v1; a target that reads per-sample values gets them filled from the source's (mapped) in a later step,
if a test shows the difference matters.

**(L8) Latency between source and target.** When there is processing with latency between the source and the target
on the same path (a lookahead limiter, a linear-phase plugin), the target hears audio that went through the source
that many samples ago. A link delays the source's values by the plan's latency between the two nodes, in blocks
(rounded), so a moving emphasis curve meets its own audio at the de-emphasis. Links between nodes on different paths
use no delay.

**(L9) Lanes on a driven parameter don't play.** The compiler leaves out a lane whose target is driven, and the UI
shows the lane as overridden by the link. Writing automation while a driven parameter is touched writes the root
source's lane instead (L4 applies to touches as it does to edits). The source of truth when either side is automated is
always the source.

**(L10) Across tracks, one block late.** Tracks render in parallel (2.6), so a target can't wait for another track's
source without new scheduling dependencies. A cross-track link reads a value the source's track published at the end
of the previous block (an atomic per link, written once the source node has run). That's deterministic and costs a
block of lag; tracks' latency alignment (they run ahead by different amounts) makes exact sync across tracks
impossible anyway, so the UI says "follows one block later" on such links.

**(L11) Components and voice graphs.** Components are flattened before compilation, so a link to or from a node inside
one needs nothing more. A target in a voice graph is written in every voice slot (as lanes do, phase12-voice-scope.md,
V11). A source in a voice graph is rejected: there's no one value to read across voices.

**(L12) Plugin parameters that come and go.** A link keeps its parameter id. While a plugin is loading, failed,
crashed or lacks the parameter after a rescan (2.5), the compiled link is unresolved and does nothing, and the UI
shows it as broken; when the parameter returns, the next compilation resolves it again. As with automation, nothing
is deleted. Removing a node removes its links (a post-pass after each graph edit, like `pruneMacroSlots`,
`Commands.cpp:3724`).

### The commands

**(L13) The model mirrors what it can.** After each edit, a post-pass sets every built-in target's model value to
`map(source)` where the source is built-in (plugin sources have no model value). So the saved project, the inspector
and any parameter that needs a recompile (`ParamInfo::automatable` false, `Node.h:167`, or one that changes the
node's latency) are right without the engine. The engine's write (L6) and the model's agree for static values.

**(L14) Commands.**

* `addLink { source, target, mapping }`, `setLink { id, mapping?, active? }`, `removeLink { id }`.
* `linkNodes { source: {trackId, nodeId}, target: {trackId, nodeId}, preset }` for L5.
* Undo works as for any command. A redirected edit (L4) is one step that changes the source and, by L13, the targets.
* Duplicating a track or copying nodes copies the links whose both ends are in the copy, remapped to the new ids;
  links to outside parameters are not copied.

### The UI

**(L15) Making links.** Right-click any parameter (the same `MidiLearnable` menu as *Map to Macro*): *Link to…*
enters Link mode, like the macros' Map mode: the next parameter moved or clicked becomes the target, with identity.
For two nodes of the same type, the node's menu has *Link device ▸ Mirror* and, for an EQ, *Mirror, gains inverted*:
pick the node, then click the other one in the strip or graph view.

**(L16) Seeing links.** A linked parameter shows a small chain mark by its knob: on a source it lists its targets; on
a target the knob follows its source and its tooltip says "Linked to EQ A · Band 3 gain (inverted)". Hovering a mark
highlights the other end wherever it's shown, as hovering a macro chip does (MC13). A *Links* chip in the modulation
column (MC12) lists the track's links, grouped by node pair, each with its mapping (kind picker, amount, a small
curve editor for range) and a remove button. The graph view draws a link between two nodes on the shown level as a
thin dashed line (not a cable: it has no port), toggled with the other overlays. A link whose other end is on
another track or level shows only the mark.

## 5. How it relates to macros

* **Macros stay what they are**: a modulation source with relative depth, eight knobs, MIDI and automation. Their
  machinery isn't a good base for links (pooled ports, relative only, no choices: section 2.1).
* **A macro knob can be a link source.** It's an ordinary node parameter (MC10), so linking it to targets with the
  *range* kind gives what component-nodes.md N6 asked for: one knob moving several parameters over chosen ranges and
  curves, absolutely, inverted where wanted. That's the "proper macro" without changing the Macros device.
* **Later, a shared mapping type.** Once `LinkMapping` exists, a modulation slot could take a range and curve too, if
  relative macro mappings miss them. Not part of this proposal.
* The two can coexist on one target: a link sets its value, a macro or LFO modulates around it (L6).

## 6. Questions for the owner

The owner accepted every recommended answer below on 2026-11-15.

1. **Option B, engine-followed links?** It's the only option that keeps the EQ pair mirrored under automation,
   modulation and plugin editors. Recommended: yes.
2. **Should editing a driven parameter redirect to its source (L4), or should targets be read-only?** Redirecting
   makes a mirrored pair feel like one device from either end. Recommended: redirect when the mapping can be inverted,
   read-only otherwise.
3. **Latency-compensated links (L8)?** Needed only when there's latency between the two ends and the source moves;
   it's a small ring buffer per link. Recommended: yes, block-granular, from the start, as emphasis around a lookahead
   limiter is a real case.
4. **A driven plugin parameter grabbed in the plugin's own editor.** The engine overwrites it next block, so it snaps
   back. Redirecting it (L4) means noticing the change on the audio thread (as `modulationBase` does) and posting an
   edit of the source to the message thread. Recommended: snap back in v1, with the gesture shown as a hint ("driven
   by a link"); redirect later if it's missed.
5. **Cross-track links one block late (L10)**, rather than making the target track wait for the source track?
   Recommended: yes; the lag is a block, and track latency alignment already makes cross-track sync inexact.
6. **Choice and toggle parameters by identity only (L2)?** Enough for EQ band types and switches. Recommended: yes.
7. **Whole-device links as sets of parameter links (L5)**, rather than a node-level link record? A node-level record
   would follow parameters a node gains later (a Faust script's new controls), but adds a second kind of link to
   every rule. Recommended: parameter links only.
8. **Should *Mirror, gains inverted* exist only for the EQ, or for any node whose parameters have "gain" in their
   unit dB and a symmetric range?** Recommended: by that rule (symmetric dB ranges), so the Channel Strip's EQ and
   Band Split levels get it too.

## 7. Implementation plan

Each step builds and passes its tests before the next.

1. **Model and file.** `ParamAddress`, `LinkMapping`, `ParamLink`, `Project::links`; read and write in `ProjectFile`;
   the JSON for the UI. *Tests* (ProjectFileTests): round trip of every mapping kind; a link to a missing node loads
   and is pruned.
2. **Mapping functions.** `mapLinkValue (info, mapping, x)` and `invertLinkValue` (Node.h, beside
   `modulatedParamValue`), units and normalised, octaves for frequencies, clamping. *Tests* (new LinkTests): each kind
   on a linear, a skewed and a frequency parameter; inverse of inverse is identity; range with `outMin > outMax`.
3. **Commands.** `addLink`, `setLink`, `removeLink`, `linkNodes`; one-driver and cycle checks (L3); removing a node
   prunes its links (L12); the model-mirror post-pass (L13); redirect in `setNodeParam` (L4); duplicate and copy
   (L14). *Tests* (CommandTests, LinkTests): cycle and second driver rejected; `linkNodes` on two EQs with the
   inverted-gains preset makes the right set; dragging B's gain moves A's, in one undo step; undo restores both.
4. **Compiler and engine.** `CompiledLink` resolution (built-in index, plugin index, voice instances), evaluation just
   before the target's modulation (L6), lanes on driven parameters left out (L9). *Tests* (LinkTests, render tests):
   an automated source gain drives the target's gain the same block; an LFO on the source moves the target; a voice
   graph target follows in every voice; the **emphasis null test**: white noise through EQ A (+9 dB at 3 kHz) and EQ B
   linked inverted, with a gain of 0 dB between them, comes out within −80 dB of the input, with A's gain automated
   from −12 to +12 dB during the render; with no allocation in the audio callback (`AllocationGuard`).
5. **Plugins.** Source and target on plugin parameters, in and out of process; unresolved while loading or after a
   rescan, resolved again (L12). *Tests* (PluginTests, RemotePluginTests, ClapTests with the test plugins): a plugin
   target follows a built-in source; a rescan that drops and restores the parameter.
6. **Latency and tracks.** The latency delay (L8) and the cross-track published value (L10). *Tests*: a link across a
   node with 512 samples of reported latency lags by that much, rounded to blocks; a cross-track link lags one block.
7. **UI.** Protocol types and store; *Link to…* in `MidiLearnable`'s menu and Link mode in `useParam` and
   `useNodeDrag`; *Link device ▸* in the node menu; chain marks, hover highlight and tooltips (L16); the Links chip and
   editor in the modulation column; dashed links in the graph view. *Tests* (vitest): Link mode picks the target;
   grouping by node pair; a driven knob shows its source's mapped value. Check by hand in a dev instance: link two EQs
   by *Mirror, gains inverted*, drag either curve, automate A, look at B.
8. **Docs.** PLAN.md entry; this document's status and a "notes from building it" section; modulation.md and the
   macros document point here for absolute mappings.

## 8. Limits

* Block-rate following (L7): a fast LFO on the source of a parameter that isn't smoothed can step at large buffer
  sizes on the target, as modulation does.
* Cancellation is only as exact as the processing in between is linear; a saturator between the EQs is the point of
  emphasis, so the pair won't null there, by design.
* Plugin-to-plugin "inverted" works in normalised terms (around 0.5): exact only for a plugin whose gain knob is
  linear and symmetric in dB. Others take a range with a curve.

## 9. Notes from building it

* **Where it lives.** `ParamAddress`, `LinkMapping` and `ParamLink` are in `Model.h`, `Project::links` in the file as
  `links` (no format bump: only test projects exist). The arithmetic is `engine/src/ParamLinks.cpp`
  (`mapLinkValue`, `invertLinkValue`, `linkDomain`), copied for the UI in `ui/src/dock/links.ts` so a following knob
  shows what the engine plays. The commands are `addLink`, `setLink`, `removeLink` and `linkNodes`; `setNodeParam`
  sends an edit of a target to the root of its chain (L4), and `applyCommand` prunes and mirrors after every command
  (L12, L13). Duplicating a track copies the links between its own nodes (L14).
* **The engine.** `GraphCompiler::CompileRun::addLinks` resolves each link after the tracks are planned and puts it on
  its target's step (`CompiledStep::links`), or on the track (`CompiledTrack::links`) for a target without a step of
  the track's own, such as a voice graph's node. `runStep` applies a step's links just before its modulation, so the
  target's modulation adds to the linked value; a track's own links run after its automation. A link whose source is
  on another track reads a value the audio thread publishes once every track has run (`CompiledEngine::linkSources`,
  `publishLinks`), so it follows a block late whatever order the workers ran in (L10). A lane on a driven parameter
  is left out by `compileLanes` (L9), and a touch on one writes its root's lane (`touchAutomation`).
* **Same block.** A source before its target in the plan is read after its own automation and modulation of the same
  block, so the test that automates EQ A's bell from -12 to +12 dB with EQ B linked inverted gives sample for sample
  the output of the same pair with B automated by hand with the mirrored lane.
* **The EQ didn't null exactly** (fixed in section 10). The EQ's bells, shelves and tilts are matched designs (vicanek-adaa.md and the EQ's
  `setMatched`), whose zeros are fitted to the analogue magnitude rather than mapped, so a bell at -g isn't the exact
  inverse of the one at +g: a ±9 dB bell at 3 kHz leaves noise about 34 dB down, not the -80 dB section 7 hoped for.
  The link does its part (the test compares with the hand-mirrored pair); exact cancellation would need an EQ mode
  whose cut is the boost's reciprocal. That is a question about the EQ, not the links, and is left for the owner.
* **Latency.** `PlannedTrack::inputLatencies` records what reaches each node; a link whose source feeds its target
  along the track's cables follows late by the difference (L8), kept in a 64-entry history on the link's own clock.
  The test puts the Saturator (oversampled, with latency) between the EQs and checks the delay equals its latency.
* **Plugin sources** are read with `PluginNode::parameterValue`, which is the plugin's value: a CLAP plugin's
  modulation apart from its value (clap-hosting.md, C9) isn't followed. A plugin target is only written when the
  mapped value differs from what it has, so a plugin isn't sent the same value every block.
* **The UI.** Right-click a knob: *Link to another parameter…* starts Link mode (the next knob moved follows it, Escape
  ends it), *Stop following …* and *Unlink …* remove links. A device's menu offers *Mirror on …* for every other
  built-in device of its type in the project, and *gains inverted* when it has levels in dB with a symmetric range.
  A knob that drives or follows another has a tick under it in the link colour (`--link`, a new token), its tooltip
  says what it follows or drives, and hovering it marks the other ends. The modulation column has a *Links* chip
  (counting pairs of devices) that opens the track's links grouped by pair; a pair's plain same-to-same links fold
  into a count, so a mirrored EQ shows its eight inverted gains.
* **Not built.** The graph view's dashed lines between linked nodes (L16); Link mode from a panel's graph (an EQ
  curve's handles go through `useNodeDrag`, which only knows Map mode); copying links when nodes are copied and
  pasted (the clipboard carries nodes only); the driven value shown live on a knob whose source is a plugin. The
  device menu names two devices of one type on one track alike ("Vocal · EQ").
* **Tests.** `tests/LinkTests.cpp` (mappings, commands, undo, duplicating a track, the file, the engine, latency, a
  cross-track link, no allocation in Debug) and `ui/src/dock/links.test.ts`. Checked in a dev instance: the device
  menu's Mirror, the Links editor folded, an edit of the follower moving the source, Link mode on two Saturator knobs
  and the hover marks. Not checked by ear, nor with a plugin at either end.

## 10. Cuts that mirror boosts (2026-11-15)

The EQ's matched designs (section 9) place an analogue prototype's poles by impulse invariance but fit its zeros to
the magnitude at three frequencies. The analogue cut is the boost's reciprocal (its poles and zeros swapped), but
the digital cut isn't, as the boost's fitted zeros become the cut's mapped poles. A mirrored pair nulled to about
-34 dB. The owner chose a mode over replacing the design.

* **(L17) The EQ's Cuts setting.** `cuts` is a choice parameter, *Matched* (0, the default) or *Mirror boosts* (1),
  in the panel's right-click menu beside *Add band*. With *Mirror boosts*, a bell, shelf or tilt with a negative
  gain is built as the boost by as much and turned upside down (B / A becomes A / B, normalised by the new a0).
  Boosts are the same in both modes. The matched numerator has its zeros inside the unit circle, so the reciprocal
  is stable. High-pass, low-pass and notch bands have no gain and are the same.
* **(L18) Mirroring turns it on.** `linkNodes` with *gains inverted* sets the source's `cuts` to *Mirror boosts* when its
  device has the parameter; the link copies it to the target, as it copies every parameter. It stays an ordinary
  parameter: switching it back by hand on the source switches both.
* **(L19) The biquad runs as direct form I in double precision.** Two things kept the reciprocal pair from nulling
  after the design was fixed. Float coefficients and state lost too much in a low shelf at 120 Hz, whose poles sit
  close to z = 1 (-77 dB). And transposed direct form II keeps state that depends on the coefficients, so while
  both EQs' coefficients moved under automation the cut didn't undo the boost's transient (-61 dB). Direct form I
  keeps the signal's own history, so the reciprocal section is the boost's algebraic inverse sample for sample,
  even while both change. This applies to every user of the `Biquad` class (the EQ, the Bit Crusher's
  filters, the Convolution Reverb's low and high cut, and the dynamics' sidechain key filter),
  which cost a little more and lose less at low frequencies.
* **Results** (`LinkTests`): a static pair of bells, a shelf and a tilt, one at 18 kHz with a Q of 10, cancels to below
  -100 dB (about -128 dB); with the emphasis automated from -12 to +12 dB, the linked pair cancels to about
  -135 dB. With *Matched*, the same static pair stays above -60 dB. Several bands moving at once could in principle
  leave a trace, since an exact time-varying inverse would run the sections in the opposite order; with one moving
  band per pair, as in the test, there is none.
