# Design: component nodes (phase 17)

Status: **built**: all of section 7 is done. The decisions are numbered N1–N10 and listed in section 8, and section 9
holds the owner's answers. The idea is the owner's (2026-11-02): a node in the graph view that has inputs and outputs and
can be opened.

## 1. What this is, in plain terms

A **component** is a box around part of a graph. From outside it's one node with the inputs and outputs you gave it, and some knobs. Open
it, and there's the graph inside, with an Input node and an Output node where its ports come in and go out.

```
 Outside:                                 Inside "Dub Echo":

 Sequencer ─► Vital ─► ┌──────────┐       Input ─► Mix ─► Feedback ─► Filter ─► Saturator ─┬─► Output
                       │ Dub Echo │ ─►            ▲                                        │
                       │ ○ Time   │               └────────────────────────────────────────┘
                       │ ○ Dark   │
                       └──────────┘
```

Voice Instruments already look like this: a node that opens onto a graph. The difference is what the box means. A Voice Instrument's graph
runs **once per note**. A component's runs **once, wherever the component is**. It's for organising, naming and reusing, and doesn't change
the sound: grouping nodes into a component, or ungrouping, leaves the audio exactly as it was.

## 2. Use cases

* **Tidying a big graph.** Twenty nodes of a drum bus become four named boxes: Transients, Tone, Glue, Room.
* **A patch you can reuse.** Build the dub echo from the feedback design once, save it, drop it on any track. This goes
  well with feedback routing, whose patches are several nodes that only make sense together.
* **Your own effect or instrument, with your own knobs.** Show Time and Dark on the box, and hide the rest. On a chain, it's one slot in the
  inspector, with its knobs in the bottom dock like any device.
* **Inside a voice graph.** A "Plucked String" component holding the Karplus-Strong loop, used three times with different tunings
  and mixed: a part of a synth voice, treated as one thing.
* **Parallel processing.** A component with two outputs, dry and wet, or one with a key input for a sidechain.
* **Sharing.** Saved components are the natural form for factory patches and, later, for sharing with others.

## 3. Model

**(N1) A component owns a nested graph, like a Voice Instrument.** `dawg.component`, with a `TrackGraph`-like graph nested in the node, which
therefore comes along when it is copied, duplicated or undone (V1). Node ids stay project-wide, so patches, undo, automation lanes, key sends
and telemetry all go on addressing inner nodes by id.

```cpp
struct GraphNode
{
    // ... existing fields ...
    std::shared_ptr<const ComponentGraph> component; // only for dawg.component
};

struct ComponentPort { PortType type; juce::String name; };

struct ComponentGraph
{
    TrackGraph graph;
    std::vector<ComponentPort> inputs;
    std::vector<ComponentPort> outputs;
    std::vector<ExposedParam> exposed;
};
```

Inside are two fixed nodes that can't be deleted, `dawg.componentInput` and `dawg.componentOutput`, with an output or an input for each port.

**(N2) Ports are per node, and any type.** A component can have audio, events and modulation ports, added and named from inside: drag a cable onto a
spare port on the Input or Output node, and a new one appears, typed by what was connected. Removing a port removes its cables, inside and
out.

This is the largest change to what exists: today a node type has one fixed port list (`NodeDescriptor`), and both `connect` and the UI read
it. It needs "the ports of this node" in place of "the ports of this type", in the engine and in the UI. Everything else asks through that one
function.

**(N3) Scope follows from where it is.** A component in a track graph may hold what a track graph may, and one in a voice graph what a voice
graph may (V6): so no plugins in a voice graph, whatever the depth. Neither may hold what there can only be one of: Sequencer, Mix Input, Track
Output, Voice Input, Voice Output. Components can hold components, up to 8 levels. A Voice Instrument can be in a component, and a component in
a voice graph, and a track-scope component holding a Voice Instrument can't then be moved into a voice graph.

A component with an event input and an audio output counts as an instrument, and one with one audio input and output as an effect, so that the
inspector's chain (G6) shows it as a slot.

## 4. How it runs

**(N4) The compiler flattens components away.** Before planning, each component is replaced by its contents, and each cable to a port becomes
cables straight to whatever is on the other side. By the time the planner runs, there are no components, and so:

* A component costs nothing at run time: not one extra step, buffer or copy.
* **Grouping can't change the sound**, which is a promise worth having, and can be tested by nulling.
* Latency compensation, sends, key inputs and modulation all work across the box's edge without new code.
* **A feedback loop can cross the edge**, because after flattening there's no edge. The region (feedback-routing.md, R2) is found on the
  flat graph.
* Voice graphs get this for nothing, since a voice program is planned by the same planner (V8).

The alternative, running a component as a real node with its own step list as the Voice Instrument does, would be a sub-graph with its own
latency and buffers. It would add a copy per port and would need extra work for latency across the edge, and loops couldn't pass through without
making the component's whole contents a region. It offers nothing in return, since a component has no per-note state to hold.

The flat graph's edge cases:

* **Several cables into a port and several out** multiply: each source connects to each target. Audio and modulation inputs sum already. For events, a
  source that goes to two inputs is fine, and two sources into one event port stay refused, checked at the port, as today.
* **A port with nothing connected outside** is silent inside, and **an output with nothing inside** is silent outside.
* **A wire straight from Input to Output** in a component flattens to a plain cable.
* **Instances are cached by node id** across compilations (as now), so editing a component's insides leaves the rest of it playing, with its state.
* **Checking for cycles** happens on the flat graph: `connect` flattens the outermost graph of the track or voice to see whether a cable closes a
  loop, including one through several boxes, and the Feedback rule applies as usual.

**(N5) Bypassing a component** passes its first audio input to its first audio output if it has both, and otherwise silences its outputs, as for
other nodes. Flattening handles this by wiring across and leaving out the contents, with instances kept so that plugins stay loaded.

## 5. Knobs

**(N6) Exposed parameters.** In a component, right-click any parameter of an inner node, and choose *Show on component*. The component then shows
that parameter under a name you can change, in the graph node and the dock.

```cpp
struct ExposedParam { ObjectId nodeId; juce::String param; juce::String name; };
```

An exposed knob **is** the inner parameter, not a copy: moving it sets the inner node's value by the ordinary command, so undo, automation lanes
and modulation all act on the inner node as they do now, and there's no new engine work. Exposed parameters can be plugin parameters in
a track graph.

A macro, meaning one knob moving several parameters over chosen ranges, is what'd come next. It needs real mapping and curves, so it stays
out of the first version; a modulation source inside can do some of it.

## 6. UI

**(N7) Opening one works as a Voice Instrument does.** A button on the node and a double-click go in, and a breadcrumb shows where you are: `Track 1 ›
Dub Echo › Tone`. `ui.enterVoiceGraph` becomes a path of nested graphs. Commands' `voiceNodeId` scope (`GraphScope`) becomes the same path, or the
owning graph found from the node's id, which commands can already do for voice nodes.

**(N8) Making and undoing one.**

* **Group:** select nodes, and press Ctrl+G. A component replaces them, and the cables crossing the edge of the selection become ports, named
  after what they connected. The result sounds identical.
* **Ungroup:** Ctrl+Shift+G puts the contents back. Exposed parameters are forgotten.
* **New empty component** in the node palette, with one audio input and output, as a place to start.

The whole group or ungroup is one undo step.

**(N9) Saving and reusing.** *Save component* adds it to the browser's Plugins panel, under Components, in the app's settings like the node presets
(humanise-and-presets.md), as a JSON of the nested graph. Dropping it in gets new ids throughout. Plugin state is saved, since a component without it
wouldn't be what you saved, and a missing plugin shows as it does in a project. Each copy is independent.

**Linked copies** (edit one, and all change) would be good, and clips have them already, but they raise hard questions: what's shared, what's a per-copy
override, and what happens to their state. They're left for later, and the model doesn't rule them out.

**(N10) File.** A project format version step for the nested graph and ports. No migration is needed, as no old project has a component.

Everything else that walks a track's nodes has to walk into components too, as it does for voice graphs: stamping plugin state, plugin status,
the key input list, automation targets, MIDI learn and id remapping on copy. One shared "all the nodes of a track" function does that, and the
voice graph's walks should move onto it. This is easily missed, so it gets its own test.

## 7. Steps

Feedback first (PLAN.md): it's smaller and stands alone, and components then have something good to hold.

1. **Ports per node (N2),** done: the engine's commands ask `portsOf (node)` and the UI asks `infoOf`, which Faust nodes already needed. The compiler keeps reading its
   instances' descriptors, as flattening leaves it none without one. No visible change, except that a compiled Faust node's extra ports can now be cabled and keyed, as the engine
   already ran them.
2. **Model and flattening (N1, N4),** done: `flattenComponents`, and the file format with its checks. A port is known by its position, and a port of the Component Input or Output
   is the same position on the component. A component made by a test nulls against the same graph without it. Commands can't reach inside yet, and key sends to a node in a component wait for step 3.
3. **All-node walks (N10)** and commands' scope (N7), done: `forEachNode` is the one walk over a track's nodes. A command's `voiceNodeId` names a Voice Instrument or component at any
   depth, or the node it names finds its own graph, so no path is needed. What a component may hold, its depth and loops across its edges are checked once, after any edit of a track's
   graphs (`requireValidComponents`), so a paste can't get past them.
4. **UI,** done: open, breadcrumb, the Input and Output with a spare port each, adding, renaming and removing ports, and the exit door going up one level. A dragged cable is
   `connectOrCloseLoop`, as only the engine can tell whether it closes a loop through boxes. A component's own name waits for step 6.
5. **Group and ungroup (N8),** done, with the null test. A port is made for each outside output a selection hears and each inner output that's heard, so an output feeding two nodes
   stays one cable to the box. A bypassed component can't be ungrouped, as what's inside would start to sound.
6. **Exposed parameters (N6)** in the node and the dock, done, with a component's name. A pin beside each parameter in the inspector shows it on the component directly holding its
   node; that takes the place of the right-click, as the inspector lists every node's parameters while a dock panel may be graphical. A plugin's parameters aren't offered yet, as the UI
   only knows their ranges from the plugin. A component's slot now opens its dock panel, which has an Open button, as a device's does.
7. **Save and reuse (N9),** and factory components, including feedback's patches, done. A saved component is one file in `Components` in the app's data folder, not the settings file,
   as plugin state can be megabytes, with that state inline. The list leaves out what's in them, which is read when one is dropped in. Dub Delay, Resonator and Flanger are components with
   their main controls on them; Plucked String stays a Voice Instrument. Those with one audio input and output are offered as chain inserts too.

Tests: nulling grouped against ungrouped across latency, sends, key inputs, modulation and a loop crossing the edge; nesting to the limit; a track-scope
component refused in a voice graph, at any depth; copying gets new ids at every depth; plugin state saved from inside a component; file round trip;
undo of group and ungroup.

## 8. Decisions

| # | Decision |
|---|---|
| N1 | A component owns a nested graph, with fixed Input and Output nodes |
| N2 | Ports are per node, named by the user, of any type |
| N3 | Allowed contents follow the scope it sits in; nesting up to 8 levels |
| N4 | The compiler flattens components, so they cost nothing and can't change the sound |
| N5 | Bypass wires first audio input to first output |
| N6 | Exposed parameters are the inner parameters themselves; macros later |
| N7 | Opening generalises the voice graph's path |
| N8 | Group and ungroup from a selection, as one undo step |
| N9 | Saved in the browser, with plugin state; copies are independent; linked copies later |
| N10 | One format step; one shared walk over a track's nodes |

## 9. The owner's answers

The owner accepted every recommendation below on 2026-11-02.

1. **The name.** "Component" is your word, and clear to programmers. "Group" clashes with group tracks. Others use "Rack" (Ableton), "Combinator"
   (Reason) and "Patcher" (Bitwig, Max). Recommended: Component.
2. **Macros in the first version (N6)?** They make a saved component feel like a real device, but need a range and curve for each mapping. Recommended: exposed
   parameters first, as they're nearly free, with macros straight after if you miss them.
3. **Linked copies (N9)?** Recommended: later.
4. **Should a voice-graph component be able to have its own polyphony or "run once" part?** Recommended: no. That's what the Voice Instrument is for.
5. **Does a saved component keep plugin state (N9)?** Recommended: yes. Licensing isn't a concern, as the plugin must be installed anyway, but states
   can be large, and a Kontakt state with samples may be megabytes in the settings file; saved components may want a folder of their own for that reason.
