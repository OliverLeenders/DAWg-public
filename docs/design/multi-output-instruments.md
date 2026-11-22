# Design: multi-output instruments

Status: **accepted; steps 1 to 4 implemented** (section 9). The decisions are numbered MO1–MO11 and summarised in section 7; the open
questions are in section 8. It builds on groups-and-sends.md (R1–R13) and sidechain.md (K1–K8).

## 1. Goals and non-goals

Goals:

* **One instrument, several mixer strips.** A drum kit in one Sampler sends its kick, snare and hats to strips of their
  own, each with its own inserts, fader, pan, meter, mute, solo, sends and automation. The notes stay in one clip on one
  track.
* **Any instrument.** The Sampler (by zone), hosted plugins with several output buses (VST3 and CLAP: Kontakt,
  drum machines), and any point of a track's graph.
* **One step.** "Each pad to its own track" sets everything up, and one undo takes it back.
* Latency compensation, mute and solo, stems and the project file work for these strips as they do for tracks.

Non-goals:

* Voice graphs: a Voice Instrument's voices sum into one output, as now. Its track graph can still split after it.
* Surround and buses wider than stereo. A mono plugin output is played on both sides.
* MIDI outputs to other tracks.

## 2. What exists

* **A track's graph has one sink, Track Output** (`dawg.trackOutput`). Everything a track sounds goes through it to its
  fader and pan, then to its group or the master.
* **The bus graph** (groups-and-sends.md, R6): tracks, groups and returns are its nodes, main outputs and sends its edges.
  It is sorted once per compile (K3), each group and return gets a mix buffer, and latency is aligned at every bus input
  (R7).
* **Sends with a target node** (R13, K2) already carry a signal from one track into a named node on another.
* **Nodes can have several output ports** (the graph and its cables handle that already). But the Sampler has one stereo
  output, and `PluginNode` plays only the first two channels of a plugin's outputs, whatever buses it has.

## 3. How other DAWs do it

* **Logic, Cubase:** activating an instrument's extra outputs creates **aux channels** in the mixer, fed by them. The
  instrument's own channel plays the main output.
* **Ableton:** an audio track picks "Drum Rack, Out 3/4" as its **input**; the source chooses nothing.
* **Bitwig:** a drum machine's pads, or a plugin's outputs, appear as **chains** with their own mixer strips under the
  instrument's track.
* **Reaper:** track channels, routed by receives; flexible, and hard to follow.

What they share: the extra outputs end up as **strips that belong to the instrument's track**, created for you. The
Logic and Bitwig way fits DAWg best, because DAWg's routing already lives in the graph and in tracks.

## 4. Model

**(MO1) Instruments get extra output ports.** The first port stays **Main**; extra ones are **Out 2** to **Out 16**,
stereo.

* **Sampler:** a zone gets an **output** field (1 = Main, the default; 2 to 16). The node has as many ports as the
  highest output any zone uses, so a Sampler that doesn't use them looks as it does today.
* **Plugins:** each output bus after the main one becomes a port, named as the plugin names it ("Kick", "3/4"), up to
  16. Plugins usually start with those buses off; the node's menu gets **Use all outputs**, which turns them on (as
  `enableSidechain` turns on an input bus). A mono bus is a port that plays on both sides.

**(MO2) An output that goes nowhere stays in Main.** If nothing is wired to Out 2, the instrument mixes it into Main
itself. Moving a zone to Out 2 never silences it; it leaves Main only once Out 2 is wired somewhere. (The alternative,
silence until wired, is how plugins behave in Logic, and surprises people.)

**(MO3) A new graph node, "Output to track", takes a signal out of the graph.** `dawg.busOutput` has one stereo input
and one setting: the target track. Wired from Out 2, it sends that output to the target. It can also be wired from any
other point of the graph (a pre-effects copy, say). It belongs to track graphs only, not voice graphs. The engine treats it
as one more edge in the bus graph, as it treats a send (MO7).

**(MO4) Outputs land on output tracks.** A new track kind, `output`:

* **Like a return**, it has no clips, and its graph is `Mix Input → [effects…] → Gain → Track Output`. It has its own
  fader, pan, meter, sends and automation.
* **It belongs to an instrument track:** `Track::source` names it. It starts directly under that track, in its group,
  and from there is placed like any track: it can be moved anywhere and grouped apart from the rest of the instrument
  (Kick and Snare in a "Shells" group, say). It plays into whatever group holds it, or the master. Moving the
  instrument leaves it where it is; removing or duplicating the instrument takes it along (MO11).
* **It is fed only by Output to track nodes,** from its own instrument's graph. (Returns and groups could be allowed as
  targets later; they aren't needed for this.)

**(MO5) "Each pad to its own track" and "Output tracks for all outputs".**

* **Sampler**, in the ⋯ menu and a pad's menu: for each pad (each key with zones, or each zone in Zones view), make an
  output track named after it, set its zones' output, add an Output to track node and wire it. One command,
  `splitSamplerOutputs`, so one undo.
* **Plugins**, in the node's menu: after **Use all outputs**, make one output track per active extra bus, named after
  the bus.
* **Back:** removing an output track sets its zones back to Main, and removes its Output to track node.

**(MO6) Mute and solo follow the instrument.** Output tracks behave like members of their instrument track:

* Muting the instrument mutes its output tracks; muting an output track mutes only it.
* Soloing the instrument solos its outputs too, and keeps the groups that hold them heard. Soloing an output track
  (or a group holding it) plays only that output; the instrument's graph still runs, but its Main is silent.
* The instrument's graph runs whenever any of its strips is audible.

## 5. Engine

**(MO7) Output to track is an edge in the bus graph.**

* The compiler finds each Output to track node, and adds an edge from its track to the target in the sorted order (K3).
  An output track always comes after its instrument, so there is no new kind of cycle.
* While a track's graph runs, the node copies its input into a per-node buffer. After the graph, the mixer adds that
  buffer to the target's mix buffer, as it does with a send.
* **Latency:** the compiler computes the latency at each sink of the graph, not only at Track Output (an effect after Out 2
  delays only Out 2). Each edge carries its own latency into R7's alignment, so the kick on its own strip stays in time with
  the rest of the kit.
* **Ports with no cable (MO2):** the compiler tells the Sampler and the plugin node which extra ports are wired. The node
  mixes the others into Main. This is a flag per port, set at compile time, so it's free on the audio thread.

**(MO8) The Sampler renders per output.** A voice already knows its zone. It writes into its zone's output buffer
instead of always into Main. With no extra outputs in use, this is today's path.

## 6. UI and file

**(MO9) Output tracks are marked as the instrument's.** An output track has an icon before its name and an empty lane.
While it sits right under its instrument (after only the instrument's other output tracks, in the same group) a connector
hangs it off the instrument; anywhere else its header names the instrument.

**(MO10) Where outputs are chosen.**

* **Sampler:** an **Output** field in the zone's Sound section (Main, 2…16, or the output track's name), the zone list
  showing it next to the layer, and the menus of MO5.
* **Graph view:** Out 2… ports on the node; Output to track nodes show their target track's name and colour.

**(MO11) Project file and commands.**

* `SampleZone.output` (1 by default); `Track.source` on output tracks; `dawg.busOutput` nodes with a `target`.
* New commands: `addTrack { kind: "output", source }`, `setNodeTarget`, `splitSamplerOutputs`,
  `setPluginOutputs { all }`, `splitPluginOutputs`.
* Removing an instrument track removes its output tracks, wherever they are (the UI says so: "and its 4 output
  tracks"). `duplicateTrack` duplicates them and points the copies' nodes at the copies; copies of output tracks that
  were outside the duplicated block go right after the copy of their instrument.
* **Stems:** each output track is a stem of its own.

## 7. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| MO1 | Extra output ports, Main plus Out 2–16, stereo; Sampler by zone, plugins by bus | Yes |
| MO2 | An unwired output stays in Main | Yes |
| MO3 | An "Output to track" graph node takes any signal to a track | Yes |
| MO4 | A new `output` track kind, belonging to the instrument's track | Yes |
| MO5 | One-step "Each pad to its own track", and output tracks for a plugin's outputs | Yes |
| MO6 | Mute and solo follow the instrument; soloing an output plays only it | Yes |
| MO7 | Output to track is a bus-graph edge, latency per sink | Yes |
| MO8 | The Sampler writes each voice to its zone's output | Yes |
| MO9 | Output tracks marked as the instrument's; a connector while right under it | Yes |
| MO10 | Output field per zone; ports and target names in the graph | Yes |
| MO11 | File fields and commands as listed; output tracks are stems | Yes |
| MO12 | A track with output tracks is only a header; its Main has an output track of its own (added 2026-11-15) | Yes |

## 8. Questions for the owner

1. **Output tracks, or any track?** MO4 adds an `output` kind that belongs to its instrument. The alternative is to
   let Output to track nodes target returns or groups, with no new kind. That is more flexible but loses "these strips
   are the kit's" (moving, duplicating and removing them together).
2. **Unwired outputs:** stay in Main (MO2, recommended), or silent until wired?
3. **How many outputs:** 16 stereo (recommended; Kontakt offers 16 stereo, as do most drum plugins), or more?
4. **Sampler granularity:** an output per zone (recommended), or per pad/key only?
5. **Order of work.** Proposed:
   1. engine: the output track kind, the Output to track node, the bus-graph edge and latency;
   2. the Sampler's zone outputs and "Each pad to its own track";
   3. the UI (track headers, mixer, the zone field);
   4. plugin output buses;
   5. stems.

   The steps after the first could be reordered if plugins matter more than the Sampler.

## 9. The owner's answers, and what was built

The owner took every recommendation, with these answers (2026-11-13):

1. **Output tracks** (MO4), not Output to track nodes aimed at returns or groups.
2. **Unwired outputs stay in Main** (MO2).
3. **16 stereo outputs** (MO1).
4. **An output per zone:** each zone chooses its output, Main by default.
5. A node in the graph **doesn't show all 16 outputs**: a Sampler shows Main and up to the highest output a zone
   plays from or a cable leaves.

Built (steps 1 to 3):

* `TrackKind::output` with `Track::source`. A new output track goes right after its track (past its other output
  tracks there), in its group; after that it is placed like any track (see the revision below). Removing the track
  removes its output tracks, and a duplicate's Output to track nodes feed the duplicates of the output tracks.
* `dawg.busOutput` (Output to track) with `GraphNode::target`; `setNodeTarget` (with `newTrack` to make the track in
  the same step) and `splitSamplerOutputs`. Removing an output track removes the nodes that fed it and puts zones whose
  output then leads nowhere back on Main.
* The compiler plans every sink of a track graph: Track Output and each Output to track. Latency is worked out in one
  pass over the tracks in dependency order (members, senders and instruments before what they feed); an Output to track
  reaches its output track with the latency at that node, delayed where it has to be.
* The mixer adds each Output to track to its output track's bus whatever the track's fader and solo, unless the track
  is muted; a stem export always feeds them, so each output track is a stem. An output track is heard when its
  instrument is soloed.
* The Sampler: `SampleZone::output`, 16 output ports, and a bit per wired port handed to the node
  (`ProcessContext::wiredOutputs`); a zone whose output isn't wired plays in Main.
* UI: output tracks with an icon, a connector while right under their instrument and its name elsewhere, no arm
  button, the bus inspector; deleting an
  instrument with output tracks asks first. The Sampler's Sound section has an **Output** choice (named after the output
  tracks, with **Own output track**), and the ⋯ menu **Each pad to its own track** (Pads) or **Each zone to its own
  track** (Zones), for the zones still on Main. The Output to track node shows a picker of the track's output tracks.

**Revised (2026-11-13): output tracks are placed freely.** As first built, an output track was pinned under its
instrument and in the instrument's group, so Kick and Snare couldn't be grouped apart from the other outputs. Now an
output track only starts there: it moves and groups like any track (`normaliseGroups` and `subtreeEnd` no longer treat it
specially, and Ctrl+G takes it). What stays tied to the instrument: removing and duplicating (MO11), mute and solo (MO6,
with a group holding an output track heard while the instrument is soloed), and Output to track nodes, which still feed
only the instrument's own output tracks. Tracks placed "after" a track (a new output track, a MIDI track from an audio
track) go past the output tracks still right under it.

**Plugin output buses (step 4).** An instrument plugin node has Main and Out 2 to Out 16, one per output bus after the
main one (`maxPluginOutputs`); the graph shows Main, the buses that are on (named as the plugin names them; CLAP ports
by their names) and any port a cable leaves. `GraphNode::allOutputs` ("Use all outputs" in the device menu,
`setPluginOutputs`) turns every bus on, stereo where the plugin takes it, else mono, else as it declares it; off turns
them off again, and a plugin's own default is kept until the setting is first used. The layout changes under
`suspendProcessing` and the plugin is prepared again; VST3, CLAP and plugins in the helper process all take it through
JUCE's bus layouts. Each bus plays into its port while that is wired and into Main otherwise (MO2); a mono bus plays on
both sides. **Each output to its own track** (`splitPluginOutputs`) turns the buses on and gives every bus not yet wired
an output track named after it, in one undo step. The UI reads the buses from the node status (`outputs: [{ name, on
}]`).

**Freezing** a track renders what each of its Output to track nodes sends too, in the same pass, and its output tracks
play those renders while it is frozen (docs/design/audio-features.md, AU15).

**Revised (2026-11-15): a track with output tracks is only a header (MO12).** As built first, the instrument's own line
still played its Main, with its own fader, pan, meter and sends, so a kit with two outputs looked like three strips of
which the first was also an output. Now:

* A track with output tracks plays nothing on its own line: its fader, pan and (non-key) sends play no part, and its
  header shows no fader, pan or meter. The mixer shows no strip for it. Mute and solo still act on all its outputs
  (MO6), and key sends still run.
* What reaches its Track Output (its Main) plays on a **Main output track** (`Track::mainOutput`, named "{track}
  Main"), as one more Output to track with the latency at the Track Output. So a plugin with Main and Out 2 split takes
  three lines: the header, Main and Out 2.
* The Main output track is made by the command that gives a track its first output tracks, or that makes its Main play
  something again while it has them (a zone set back to Main, a new zone), as the first output track. A Sampler whose
  every pad went to its own track has nothing in Main and gets no Main line: two pads, three lines. A Main output track
  that is removed while the Main still plays stays removed (that Main is then silent).
* Removing every output track makes the track an ordinary one again. A frozen header's render plays on its Main
  output track. Projects made before this have no Main output tracks, so their headers' Mains are silent until one is
  made (no backwards compatibility yet).
* A header takes no new audio effects (insertNode refuses them, and the device strip, inspector and arrangement drop
  say so): they would only play in Main, or nowhere when there is no Main line. Effects go on the output tracks. MIDI
  effects and the instrument still go on the header. When a track becomes a header and gets a Main output track, its
  audio effects move there in place of that track's default ones, with their automation lanes, MIDI mappings, links
  and the modulation sources that only modulate them; their macro slots go, as the Macros device stays on the header.
  A header without a Main line keeps its effects, which play nothing.

Not yet: effect plugins keep one output.
