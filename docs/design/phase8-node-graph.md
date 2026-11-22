# Phase 8 design: node graph v1

Status: **implemented** (implementation notes in section 7). The decisions are numbered G1–G10 and summarised in section 6. The UI questions are
in section 5.

## 1. Goals and non-goals

Goals:

* A graph editor for the selected track's graph, and for the master track's.
* Free routing inside a track: parallel branches, splits and merges, several instruments layered on one sequencer.
* A first set of built-in effects, so that a graph is useful without plugins.
* The insert list in the inspector keeps working whenever a graph is still a simple chain.

Non-goals (later phases):

* Routing between tracks: sends, buses, sidechains (phase 13, as a project-level graph).
* Feedback loops. They stay rejected until an explicit feedback-delay node exists.
* Modulation ports and LFOs (later), automation (phase 10). Faust is phase 11, and the voice-scope graph is phase 12.
* Running branches on several threads.

## 2. What already exists

The engine has had real graphs since phase 2. Only the editing UI has been missing.

* `TrackGraph` holds nodes with positions, and connections between typed ports (audio, events).
* The commands `addNode`, `removeNode`, `connect`, `disconnect`, `setNodePosition`, `setNodeParam` and
  `setNodeBypass` exist. `connect` rejects cycles, mismatched port types, and a second connection into an
  event input.
* The compiler sorts the graph topologically and reuses buffers. It sums several connections into one audio
  input with a mix step. Where paths of different latency meet, it adds delays, so latency compensation inside
  a graph already works.
* The chain view (`findChain`, plus `trackChain` in the UI) recognises graphs that are simple chains. The
  inspector's insert list and `insertNode` / `reorderNode` depend on it.

## 3. Engine work

**(G1) Built-in effects use juce::dsp.** Recommended. Each effect is a `Node` with a `ParamBlock`, smoothed
parameters, and no allocation in `process()`:

| Type | Name | Parameters |
|---|---|---|
| `dawg.eq` | EQ | eight bands (HP, low shelf, four peaks, high shelf, LP): frequency, gain, Q, slope |
| `dawg.filter` | Filter | mode (low-pass, high-pass, band-pass, notch), cutoff, resonance, drive, slope (12 to 48 dB/octave, its stages in a Butterworth filter's proportions: -6 dB at the cutoff at resonance 0, and never above 0 dB) |
| `dawg.compressor` | Compressor | threshold, ratio, attack, release, makeup, mix |
| `dawg.utility` | Utility | gain, pan, width, invert phase per channel, mono |
| `dawg.mix` | Mix | four stereo inputs with a gain each (for explicit level control when merging) |

A plain merge needs no Mix node, because connecting two outputs to one input sums them. Splitting needs no
node either, because an output can feed several inputs.

**(G2) Parameters can be choices and toggles.** `ParamInfo` gains a kind (continuous, integer, choice with
labels, toggle), an optional unit, and a skew for frequency-like ranges. The UI draws the matching control.
Values stay floats in the model, so files and `setNodeParam` don't change.

**(G3) Delay and reverb come later** (the user's choice for v1). When they come, a tempo-synced delay reads the
tempo from `ProcessContext::transport`, so no new plumbing is needed.

**(G4) Graph commands stay as they are, with two additions:**

* `addNodes { trackId, nodes: [{ type, x, y, plugin? }], connections? }` adds several nodes in one step, for
  paste and duplicate. Connections refer to the new nodes by index.
* `removeNodes { trackId, nodeIds }` removes a selection in one step.

Fixed nodes (Sequencer, Audio Player, Mix Input, Track Output) still can't be added or removed. The track Gain
node becomes an ordinary, removable node in graph view. The chain view takes a gain before the output when there
is one, but does not need it (revised 2026-11-04): removing it joins its neighbours up, and the graph stays a chain.

**(G5) Plugins in graphs.** `addNode` accepts plugin types with a `plugin` argument (today only `insertNode`
does), so plugins can be placed anywhere. Several instruments can share the sequencer's events: an event
output may feed several inputs, while an event input still takes one connection.

**(G6) Chain and graph stay one model.** The inspector shows the insert list when `findChain` recognises the
graph. Otherwise it shows "Custom routing", the list of nodes, and an "Open graph" button. Clicking a node there
does what it does on a chain (revised after phase 12): its panel shows in the device dock, and a plugin's own
editor opens, or closes if it is open. Only rewiring needs the graph view. In a custom graph,
instruments come first, effects follow in signal-flow order (so an effect that feeds another is listed above
it), and LFOs and ADSRs have their own "Modulation" section instead of appearing as inserts. A custom graph is
never turned back into a chain automatically.

## 4. UI work

* **(G7) React Flow** (`@xyflow/react`, pinned to its latest stable version, as the dependency policy already
  plans) draws the canvas: pan, zoom, minimap, selection, and dragging connections. Nodes are our own React
  components. Connections are validated in the UI against port types and cycles before a command is sent; the
  engine still has the final say.
* **(G8) Node look.** A header with the node's colour by category (instrument, effect, utility, fixed), its
  name, and bypass, plus an open-editor button for plugins. Ports sit on the edges and are coloured by type
  (audio, events). Latency shows on the node when it isn't zero. A node's plugin status (loading, failed) is
  shown like in the inspector.
* **(G9) Edits are commands.** Moving a node is a merged gesture (one undo step per drag). Copy, paste,
  duplicate and delete work on the selection. Double-clicking a plugin node opens its editor.
* **(G10) Playing while editing.** Structural edits recompile as they do today. Instances survive, so tails and
  plugin state carry over. A connection change can click; v1 accepts that, and short crossfades come later.

## 5. UI decisions (answered by the user)

1. **Graph view:** a third centre view next to the arrangement and the piano roll, also in the Tab cycle. It shows
   the graph of the selected track, or of the master track.
   * Revised after phase 12: a graph belongs to one track, so the graph takes the place of the arrangement's
     timeline and the track headers stay beside it. Selecting another track in the headers (or the master's
     header) switches to that track's graph. The arrangement's time rows and lanes stay mounted while hidden, so
     its scroll and zoom survive the round trip.
   * A click on a node shows its panel in the device dock, opening the dock if it is hidden.
2. **Parameters:** nodes stay compact (name, ports, bypass). Selecting a node shows its parameters as controls in
   the inspector.
3. **Adding nodes:** both. Right-clicking the canvas opens a searchable menu at the cursor, and the Plugins tab
   in the right side bar also lists the built-in effects, which can be dragged onto the canvas.
4. **Built-in effects in v1:** EQ, Filter, Compressor, Utility and Mix. Delay and reverb come later.

## 6. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| G1 | Built-in EQ, filter, compressor, utility and mix nodes, on juce::dsp | Yes |
| G2 | Parameter kinds (choice, toggle, integer), units and skew | Yes |
| G3 | Delay and reverb later; tempo sync will come from the transport | Yes |
| G4 | `addNodes` / `removeNodes`; the track Gain node becomes removable | Yes |
| G5 | Plugins anywhere via `addNode`; event outputs can feed several instruments | Yes |
| G6 | One model: chain view when possible, "Custom routing" otherwise | Yes |
| G7 | React Flow for the canvas | Yes |
| G8 | Node look: category colour, typed port colours, latency and status badges | Yes |
| G9 | Edits are commands; drags merge into one undo step | Yes |
| G10 | Recompile on structural edits; no crossfades in v1 | Yes |

## 7. Implementation notes

* **Inserting a node into a cable** (2026-11-01): dragging a single node without connections over a cable highlights the
  cable, and dropping it there replaces A -> B with A -> node -> B, using the node's first input and output of the
  cable's kind (graph/insertNode.ts), as one undo step with the move. Audio and event cables only.
  A node dragged from the palette does the same (`addNode` with `insertInto`, since its id isn't known beforehand).
  There is no layout: the inserted node keeps its place unless it sits on A, and all nodes from B's position on move
  along by what clears it, with 40 units of padding, keeping their shape.
* **Taking a node out:** shaking it while dragging (three quick changes of direction) shows its cables gone and A -> B
  closed; the drop makes that one undo step with the move, and may insert it into another cable. What came into its
  first audio or event input goes on to what its first output of that kind fed; other cables just go.
* **Effects** live in engine/src/EffectNodes.cpp. They don't use juce::dsp after all: its IIR coefficients
  allocate, so the effects use small hand-written DSP instead. That is an RBJ biquad for the EQ, a
  topology-preserving state-variable filter (cutoff smoothed, coefficients every 16 samples), a compressor with a
  stereo-linked peak detector and a 6 dB soft knee, and `juce::SmoothedValue` for gains. A shared base handles
  bypass, which passes input 0 through.
* **Parameters** gain `ParamKind`, `unit`, `midValue` and `choices` (Node.h). Nodes gain a `NodeCategory`.
  `toVar (NodeDescriptor)` and the bridge method `nodes.types` describe all types to the UI. The MPE synth and
  gain parameters now carry units.
* **Commands.** `addNode` takes plugins (with `plugin`) and rejects instruments on non-instrument tracks.
  `addNodes` also takes `params` and `bypassed`, so pasted nodes keep their settings. Plugin state is not copied.
  `removeNode` removes the gain too (the graph then becomes a custom one). `removeNodes` doesn't rejoin
  neighbours. Event outputs could already feed several inputs.
* **Compiler.** Nodes that don't reach the track output now get an idle instance. They never run, but a plugin
  placed in the graph loads, shows its status and opens its editor before it is connected.
* **UI.**
  * ui/src/graph/: `GraphView` (React Flow 12.12.0 canvas), `GraphNodeView`, `validate.ts` (connection rules,
    mirroring the engine) and `params.ts`.
  * Parameter controls use a curve r^t that puts `mid` at the middle, close to a logarithmic scale for
    frequencies.
  * Node positions and selection are local while dragging. A drag ends in `setNodePosition` (several nodes:
    one batch).
  * Delete removes the selection as one `removeNodes` / `disconnect` batch. Pulling a cable off an input and
    letting go on empty canvas disconnects it, as in modular patching environments; dragging from an output only
    makes new cables.
    Ctrl+C, V and D copy, paste and duplicate within the view. Double-clicking a plugin node opens its editor.
  * The right-click menu reuses `PluginPicker` (kind `any`). The Plugins tab lists the built-in nodes first, and
    its rows can be dragged onto the canvas (`application/x-dawg-node`).
  * The inspector shows the selected node's parameters in graph view (`NodeInspector`), and "Custom routing"
    when `trackChain` (now as strict as the engine's `findChain`) finds no chain. The custom list groups
    instruments, effects in topological order, and modulation sources.
  * Built-in effects also appear in the chain inspector's effect picker.
* **Tests.**
  * tests/NodeGraphTests.cpp: the registry description, `addNodes` and `removeNodes`, gain removal, idle
    plugin instances, parallel branches summing, one sequencer driving two synths, and each effect (EQ boost
    and shelf, filter modes, the compressor curve and settled reduction, the utility, mix, bypass).
  * UI tests: the parameter curve, formatting, feedback-loop detection, and stricter chain detection.

## 8. Cable management (added 2026-11-02)

The owner found that cables ran behind nodes, and expected loops to clutter the view. Three things answer that:

* **Exit door**: an output node has a door button, which leaves the graph: a Voice Output goes back to the track's graph, and a Track Output to the arrangement. A component's output
  will go one level up the same way.
* **Focus** (`graph/focus.ts`): modulation cables stay faint until looked at, and hovering or selecting a node quietens everything not connected to it.
* **Layout** (`graph/layout.ts`): the Layout button runs ELK's layered layout, the Sugiyama method, through elkjs, loaded only when pressed. It reserves a lane for each long
  cable, orders columns to reduce crossings and knows where ports are. It returns routes, which are kept in memory beside the positions and hold while a cable's two
  nodes stay where they were put, and nothing has moved into their way.
* **Routing** (`graph/routing.ts`): any other cable is the usual curve, unless that would pass through a node, its own included. Then it runs around, along the nodes' edges, by a shortest
  path that counts each turn as extra length. This works from where the nodes are now, so it needs nothing saved in the project and holds after a move or on opening a project. It runs when
  a move ends, and a cable on a node being dragged is a curve until then. It doesn't give cables their own lanes as Layout does, so two may share a stretch, and a cable whose end is
  covered by another node stays a curve.
* **Feedback chips** (`feedbackChips`): a loop's Feedback node with one way in, one way out that leads back, and nothing modulating it, is drawn as a small chip on the cable going back,
  and ELK takes the cable through it as one, which runs back around the nodes. The node stays an ordinary node in the model, and its panel is unchanged. A cable to its free port
  makes it a full node again. Its input faces the node feeding it, and its output the other way, along whichever way those two nodes are further apart (`chipFacing`).
