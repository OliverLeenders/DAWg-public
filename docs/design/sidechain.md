# Design: sidechain inputs

Status: **implemented** (notes in section 5). The last part of phase 13's routing. The decisions are numbered K1–K8; the owner's answers are in
section 4.

## 1. Goals

* A node can be **keyed** by another signal: the compressor ducks the bass under the kick.
* Keys come from **other tracks** (through sends, as groups-and-sends.md R13 planned) or from **inside the track**
  (wired in the graph view).
* **Hosted plugins with a sidechain input** (a VST3 aux input bus) can be keyed the same way.

Non-goals: keying by MIDI, several sidechain inputs per node, keys from the master.

## 2. Decisions

**(K1) Keyable nodes have a second audio input, "Key".** The compressor's inputs become `Audio` and `Key`; effect
plugins get `Audio` and `Sidechain`. A node is keyable when its second input port is audio. An unconnected key input
is silent.

**(K2) Keys between tracks are sends with a target node.** `Send { target, targetNode, … }` already exists for this
(R13): a send whose `targetNode` names a keyable node on another track feeds that node's key. It keeps the send's
level, on/off and pre/post-fader tap, and is **pre-fader by default**, so the kick's fader and mute don't change how
it ducks the bass. Key sends run whatever the source's mute, solo or stem export, since they don't make sound. A track
can key a node once; several tracks keying one node are summed.

**(K3) One order for all tracks, no loops.** The mixing order becomes one topological order over all tracks: members
before their group, senders before returns, and key sources before the track they key. `addSend` refuses a key that
would close a loop (a group keying a member's compressor, or two tracks keying each other) and a key to a node on the
sending track (wire it in the graph instead). A file holding a loop keeps the send, but it doesn't play.

**(K4) Key buses.** Tracks render one after another into shared buffer slots, so each keyed node gets a key bus, as
groups and returns have buses: its key sends add into it, and the node's key input reads it. A key wired inside the
track wins over key sends.

**(K5) The key is delayed to meet the node's signal.** A key leaves its source at the source's output, before
alignment (`reach`); the keyed node's input sits at its track's input plus the latency before the node. The key is
delayed by the difference. When the source is slower than that path, the key arrives late by the difference: the
engine doesn't delay the keyed track for it, and the compressor panel says how late.

**(K6) The compressor's detector.** A `Detector` choice (Internal, Key), a high-pass on the detector (`keyFilter` on
or off, `keyFreq` 20–500 Hz, matched 12 dB/octave), and `Listen`, which outputs the detector signal instead of the
compressed audio. Picking a key source switches the detector to Key; Internal switches it back.

**(K7) Plugins with a sidechain bus.** When a plugin has a second input bus, DAWg enables it as stereo and feeds it
the key input; the plugin decides what to do with it (its own sidechain switch). Plugins without one ignore the key.
The node's status says whether the plugin has a sidechain, and the UI only offers keys for those.

**(K8) The UI.**

* The compressor panel gets a **Key** selector (Internal, or a track) and a Key section with the high-pass and
  Listen. Choosing a track makes the key send; choosing Internal removes it.
* A plugin with a sidechain gets the same Key selector in the dock's header.
* The source track's **Sends** list shows key sends ("→ Bass · Compressor key") with level, on/off and pre/post, and
  its Add send menu offers the keyable nodes on other tracks.
* The compressor's history graph draws the key level as a second line while keyed.

## 3. Steps

1. Model and commands: key inputs (K1), key sends and loop checks (K2, K3), removing a keyed node removes its key
   sends; the compressor's detector parameters (K6).
2. Engine: the order (K3), key buses (K4), key delays (K5), the detector, plugin sidechain buses (K7); meters for the
   key level.
3. UI (K8).

## 4. Owner's answers

* **Choosing a key:** both the compressor's Key selector and the source track's Sends menu, as one send.
* **Key tap:** pre-fader by default.
* **Key options:** detector high-pass and Listen.
* **Plugins:** plugin sidechain buses in this round.

## 5. Implementation notes

* Model and commands (Commands.cpp): `isKeyable()` (the node's second input is audio), `runsBefore()` for loops
  through membership and sends, `addKeySend()` behind `addSend` with a `targetNode`, and `removeKeySendsTo()` when a
  keyed node is removed. `normaliseGroups()` keeps key sends whose node exists.
* Compiler (GraphCompiler.cpp): the planner records free key inputs (`PlannedTrack::keyInputs`); key sends are
  validated into `keyTargets`; one depth-first order over `runsAfter` replaces the group-then-return order; each key
  gets a key bus and a delay of `arrival[target] + inputLatency - reach[source]`, the node told its lateness
  (`Node::setKeyLateSamples`). `CompiledSend::key` marks key sends, which the audio engine runs for every track.
* An internally wired key makes the track's graph a custom one (the inspector's chain view needs a simple chain).
* Compressor (EffectNodes.cpp): parameters 6–9 (`detector`, `keyFilter`, `keyFreq`, `listen`); its history slots
  are [input, output, reduction, key], and `meters()` adds `keyLateMs`.
* Plugins (PluginNode.cpp): `enableSidechain()` on attach, the key copied into the sidechain bus's channels, and
  `hasSidechain` in the node status.
* UI: KeySelector.tsx (the selector, `keySendsTo`, `keyableNodes`), the compressor's Key section, the plugin's
  selector in the dock header, key sends and "Key →" entries in SendsInspector.tsx, and the key line in the history.
* Tests (tests/SidechainTests.cpp): the commands, ducking from a muted source placed after the keyed track,
  pre/post-fader keys, the detector's high-pass and Listen, and a plugin's sidechain bus.

