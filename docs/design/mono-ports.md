# Design: mono audio ports

Status: **accepted** (2026-11-13). The decisions are numbered MP1–MP15 and summarised in section 9; the owner's
answers are in section 10. It builds on phase8-node-graph.md, component-nodes.md, multi-output-instruments.md and sidechain.md, and
the split nodes (split-nodes.md) are its first users.

## 1. Goals and non-goals

Goals:

* **A port can be mono.** Audio ports carry one or two channels, and a node that only has one signal (a mid, a side, a
  left, a right, a mono plugin bus, a one-channel Faust script) says so instead of putting it on both channels.
* **Mono stays mono down a chain.** An effect fed by a mono port runs on one channel, when it can, and its output is
  mono too, so a compressor on the Side of a mid/side split costs half.
* **True mono for plugins.** A plugin fed mono gets a mono input bus when it offers one, so it runs its mono algorithm
  (a mono-to-stereo reverb, a mono analog model) rather than its stereo one on two equal channels.
* **Mixing widths just works.** Any audio output can still feed any audio input; the conversion is fixed, simple,
  cheap and visible.
* **Nothing changes for stereo-only graphs.** No cost, no new conversions, no change in sound.

Non-goals:

* Surround or more than two channels on a port.
* Mono tracks. Track outputs, sends, Output to track nodes, meters on the mixer, export and freeze stay stereo; mono
  lives inside track graphs. Mono clips and device inputs come later, and the design leaves room for them (MP15).
* Switching width while playing. Width is worked out when the graph compiles.

## 2. What exists

* **Every audio port is stereo.** `PortType::audio` is "stereo audio" (Model.h); `AudioInput` and `AudioOutput` are
  two channel pointers (Node.h); every buffer slot holds two channels (`SlotViews`, GraphCompiler.cpp).
* **Mono is put on both channels wherever it comes in:** a mono device input ("the same channel twice",
  `ProcessContext::input`), a mono plugin output bus (PluginNode.cpp: "A mono bus plays on both sides"), the last
  port of a Faust script with an odd channel count (`outputChannel` repeats the last channel).
* **A mono plugin fed stereo hears the left channel only.** The plugin's main input bus keeps whatever layout the plugin
  starts with, and `process` copies `min(2, channels)` input channels, so a plugin whose main bus is mono gets the left
  and loses the right. This is a bug today, independent of this design; MP4 fixes it.
* **Modulation ports are mono already,** but they are control signals, not audio, and stay their own port type.
* **Fan-in sums:** several cables into one input are summed into a buffer by a mix step before the node runs.
* **Recompiles are cheap and common:** any graph edit recompiles the track off the audio thread, which is when widths
  would be worked out.

## 3. How other hosts do it

* **Reaper:** every track has up to 128 channels, and each plugin has a pin map choosing which channels it reads and
  writes. Fully general and famously hard to use.
* **Bitwig:** the grid has mono and stereo signals. A stereo cable into a mono input sums to mono, a mono cable into a
  stereo input feeds both sides. Mono ports are drawn differently. Plugins are stereo on the device chain.
* **Max/MSP, Pure Data, VCV Rack:** every cable is one channel (VCV adds polyphonic cables); stereo is two cables.
  Honest, but stereo work needs twice the wiring.
* **Logic, Cubase, Pro Tools:** mono and stereo tracks, chosen when the track is made, with plugins inserted in a
  mono, mono-to-stereo or stereo version to match. Width belongs to the track, not the port.

DAWg's graph is closest to Bitwig's grid: keep a cable per port, let a port be mono, and convert at the cable with
fixed rules. Plugins get what the hosts with mono tracks give them: a layout picked from the width of their input.

## 4. Model

**(MP1) An audio port has a width: mono or stereo.** `PortInfo` gets `channels` (1 or 2, default 2) beside its type.
It is not a new `PortType`: a mono and a stereo port are both audio and can always be connected. Three kinds of audio
port:

| Kind     | Width                                                   | Examples                                   |
|----------|---------------------------------------------------------|--------------------------------------------|
| Stereo   | Always 2                                                | Track Output, Sampler outputs, reverbs out |
| Mono     | Always 1                                                | Stereo Split's outputs, Stereo Merge's inputs, a mono plugin bus |
| Adaptive | 1 if every source feeding the node's main input is mono, else 2 | Most built-in effects, the Mix node, plugins (MP4) |

An adaptive node's outputs follow its main input: mono in, mono out. The descriptor says which ports are adaptive
(`PortInfo::channels = 0`), and the node is told its width before it runs (MP6). A node whose output is wide by its
nature (a reverb, a chorus, a ping-pong delay, the Utility's width or pan) has a mono or adaptive input and a stereo
output: mono in, stereo out.

**(MP2) Converting at the cable.** Any audio output can feed any audio input:

* **Mono into stereo:** the signal on both channels at full level (no pan law), as DAWg does with mono today. A mono
  source in the centre then sounds exactly as it did when it was two equal channels.
* **Stereo into mono:** the average, (L + R) / 2: the mid. Equal channels come out unchanged; a wide signal loses its
  side. This is the same fold the Stereo Merge uses (split-nodes.md, SP5).
* **Several cables into one input:** summed as now. Into a mono input, stereo sources are folded first, then all are
  summed; into a stereo input, mono sources count on both channels.

Folding is the one place a signal changes, so it is never hidden: a cable that folds is drawn with a marker (MP9), and
its tooltip says "Folded to mono: (L + R) / 2".

**(MP3) Widths are worked out when the graph compiles.** The compiler visits the nodes in order, as it already does,
and gives every adaptive port the width its sources have. Feedback loops (feedback-routing.md) are worked out by
treating the Feedback node's output as stereo when anything in the loop is stereo. A width that changes (a cable
added from a stereo source into a chain that was mono) is a recompile like any other edit, and the nodes that change
width are prepared again, off the audio thread.

## 5. Plugins

**(MP4) A plugin's main bus follows its input.** The plugin node is adaptive. When the graph compiles, it tries
layouts in order and keeps the first the plugin accepts:

| Fed by | Layouts tried (input → output)                        |
|--------|-------------------------------------------------------|
| Mono   | mono → mono, mono → stereo, stereo → stereo           |
| Stereo | stereo → stereo, mono → mono, mono → stereo           |

The plugin's output port takes the width of the output bus it got. A stereo-fed plugin that only offers mono gets the
fold (MP2), so it hears both channels instead of only the left (the bug in section 2). A mono-fed plugin that only
offers stereo gets the mono signal on both channels, as today.

Some plugins sound different in mono and stereo. A plugin node gets a **Channels** setting, *Auto* (the table above),
*Mono → stereo* (for reverbs and delays that should spread a mono input), or *Stereo* (as today, always stereo → stereo),
in the plugin's menu and inspector. It is a node setting: changing it changes the layout and recompiles.

Changing a plugin's layout means releasing and preparing it again. That happens only when its input width or its
setting changes, never while playing, and the plugin's state (its parameters and chunk) is kept.

How it is built: the plugin is asked once, when it is attached, which of the three main layouts it accepts (a plugin in
a helper process answers over its pipe, so it is not asked again on every compile). While the graph compiles, the
node answers its output's width for a given input width (`Node::outputWidth`, which changes nothing), and once the
widths settle it takes that layout (`Node::useWidth`), before the compiler reads its latency. While a plugin loads, its
output has its input's width, and the recompile after it loads picks the real layout. The setting is the node's
`pluginChannels` (`setPluginChannels { trackId, nodeId, channels: "auto" | "monoToStereo" | "stereo" }`), saved as
`channels`.

Sidechain inputs keep their own rule (sidechain.md, K7: stereo, else mono). Extra output buses of instruments
(multi-output-instruments.md) become mono ports when the plugin makes them mono, instead of being put on both sides.

**(MP5) Faust scripts get the width they declare.** Channels go to ports in pairs as now, but a script with an odd
count gets a mono last port instead of repeating its last channel. A script with one input and one output has mono
ports, and so a mono effect in Faust costs half.

## 6. Engine

**(MP6) A node learns its width from its step.** Each compiled step carries the width of its node's adaptive ports
(`CompiledStep::channels`), and the engine passes it as `ProcessContext::channels`, 1 or 2. It is not set on the node
(a `setChannels` call was the first idea): while a new compiled engine replaces the old one, both share the node
instances, and the old one may still run a block with the old width. A node without adaptive ports always sees 2.

**Buffers keep two channels per slot.** Memory is not the point and the engine's slot allocator stays as it is. A mono
port uses channel 0 of its slot:

* A mono output's `AudioOutput` is `{ channel 0, scratch }`, so a node that writes both channels by mistake does no harm.
* A mono output read by a stereo input is given as `{ channel 0, channel 0 }`: mono into stereo (MP2) costs nothing.
* A stereo output read by a mono input goes through a **fold step**, a mix step that averages into channel 0 of a slot
  of its own. With several sources, the existing mix step does the fold while it sums.
* A sink (the Track Output, an Output to track) hands its slot to the mixer, which reads both channels, so a mono
  source into a sink is copied to both channels by a mix step first. Only graphs whose end is mono pay for it, once.
* A node's analyser (the device panel's tap) shows a mono output on both sides.

**Built-in nodes, adaptive at first** (they run one channel when mono): Gain, Filter, EQ (a mono signal is all mid:
its bands on both channels and on the mid filter it, those on the side do nothing), Compressor, Gate, Expander
(their key input stays stereo; Listen on a mono step hears the key folded), Limiter, Saturator, Clipper, Bitcrusher,
Waveshaper, Mix, Feedback, and the delay-compensation nodes the compiler adds. **Adaptive in, stereo out:** Loop
Reverb, Convolution Reverb, Chorus, Flanger, Phaser, Delay, Utility. **Stereo only, as now:** instruments, the Audio
Player (until mono clips, step 7), Track Output, Output to track, sends. Component Input and Output nodes take their port's width (MP11). Any
node not on these lists stays stereo, and a mono source feeding it is
put on both channels (MP2), so nothing breaks while nodes are converted one by one.

**(MP15) Sources can decide their own width, for mono clips and inputs later.** The width pass does not read a node's
widths from descriptors everywhere: one function of the planner (`sourceWidth`) answers the width of a source's
output, and the descriptor's width is only its default answer. Plugins and Faust scripts answer through their
instances (`Node::outputWidth`, step 3); when mono clips come, the Audio Player answers there too. Later, the Audio Player answers mono when every clip it plays is from a mono file (any
stereo clip, or crossfaded takes of mixed widths, make it stereo), and an audio track's monitored input answers mono
for a mono device input, so `ProcessContext::input` would carry one channel. A clip added or a file replaced already
recompiles the track, so the width follows without anything new. Freeze renders stay stereo.

**(MP7) Width is fixed per compile.** A step's width never changes while it runs; no branches on width in the audio
thread beyond the node's own loop over its channels.

**Delay compensation, latency, modulation, freezing:** unchanged. Latency does not depend on width (except through a
plugin's own report after a new layout, which the recompile reads). Modulation ports stay mono control ports. Freeze
renders the track output, which is stereo.

## 7. UI

**(MP8) Mono ports look different.** In the graph, a stereo port is a filled circle and a mono port a ring, and a
cable from a mono port is drawn thinner. An adaptive port takes the look of the width it got in the last compile. Port
tooltips say "Mono" or "Stereo".

**(MP9) Folding is visible.** A cable that folds stereo to mono gets a small marker where it enters the port, with the
tooltip from MP2. There is no warning: folding is often what is wanted (a stereo source into a mid/side merge), it only
must not be a surprise.

**(MP10) Meters show the width.** A node's meter on a mono port is one bar. The mixer is unchanged (tracks are stereo).

How it is built: after each compilation the app sends `dawg_widths` (and answers `nodes.widths`): the widths every
port of a node with a mono port got, and its inputs that fold, by "track:node"; a node not listed is all stereo. Node
types describe each port's declared width (`channels`: 1, 2, or 0 for adaptive), which the graph shows for nodes that
did not compile. The graph view draws a mono port as a ring and its cables thinner, puts an orange bar at an input
that folds (its tooltip is MP2's), and names the width in each audio port's tooltip. Inside components and voice
graphs ports show as declared until step 5. No device panel has a two-bar level meter for a node today, so MP10 needs
nothing yet; the analyser shows a mono output on both sides. The Channels setting is in an effect plugin's menu and in
the inspector.

The device strip needs nothing new: a chain stays a chain, and a mono chain inside a component shows its meters as
mono.

## 8. Components and file

**(MP11) Component ports have a width.** `ComponentPort` gets `channels`: stereo (default), mono or adaptive, chosen in
the component's port editor. Inside, the port behaves as a node port of that kind, so a "Mono bass" component can take
mono in and give stereo out.

How it is built: a component costs nothing because flattening turns its ports into junctions, cables straight through.
When the compiler flattens (`flattenComponents (graph, wireLoop, true)`), a mono or stereo audio port becomes an
internal node instead (`dawg.internal.monoPort`, `dawg.internal.stereoPort`, registered but not user-editable), which
copies its channels through, so the usual rules fold a stereo signal into a mono port and read a mono one on both sides
of a stereo port. An adaptive port stays a junction and costs nothing. Its id is negative and the same on every
compilation, and says whose port it is, so the graph view can show a mono port's fold on the component (an input) or
on its Output node (an output). The command is `setComponentPortChannels { trackId, nodeId, side, index, channels }`
(2, 1 or 0); `channels` is saved only when not stereo. In the graph, an Input or Output node's audio port has a small
S, M or A button that steps through the widths. Widths inside components now show in the graph; inside a voice graph
ports show as declared.

**(MP12) File.** Built-in ports' widths come from their descriptors and are not saved. Saved: a plugin node's
`channels` setting (only when not Auto) and a component port's `channels` (only when not stereo). Projects without them
load as before.

**(MP13) Behaviour changes for existing projects:** a stereo-fed plugin whose main bus is mono now hears both channels
folded instead of the left only, and mono plugins after mono sources now run in mono. Both are intended; there are only
test projects so far, so no compatibility mode.

**(MP14) Tests:**

* Mono into stereo gives equal channels at full level; stereo into mono gives (L + R) / 2; several mixed cables into
  mono and into stereo sum as MP2 says.
* A chain of adaptive nodes after a mono source runs mono (each step's width is 1), and sounds as the same chain run
  stereo on equal channels.
* A stereo cable added into the chain recompiles it to stereo; removing it, back to mono.
* Feedback loops: a loop with a stereo node is stereo throughout; an all-mono loop is mono.
* Plugins: the layout table with a test processor accepting each set of layouts; the Channels setting; a mono-only
  plugin fed stereo hears both channels; state is kept across a layout change.
* Faust: a 1-in/1-out script has mono ports; a 3-output script has a stereo and a mono port.
* Components: mono, stereo and adaptive ports.
* A stereo-only graph compiles to the same steps as before (no fold steps, every width 2).
* No allocation in the audio thread with mono ports (the allocation guard).

## 9. Decisions to review

| #    | Decision                                                                                          | Recommended |
|------|---------------------------------------------------------------------------------------------------|-------------|
| MP1  | Audio ports have a width (mono, stereo, adaptive), not a new port type                            | Yes |
| MP2  | Mono into stereo: both channels at full level. Stereo into mono: (L + R) / 2                      | Yes |
| MP3  | Widths worked out at compile; loops stereo if anything in them is                                 | Yes |
| MP4  | Plugins adaptive, layouts tried in order; a Channels setting (Auto, Mono → stereo, Stereo)        | Yes |
| MP5  | Faust scripts with an odd channel count get a mono last port                                      | Yes |
| MP6  | Width per step, in ProcessContext; slots stay two channels; mono into stereo free; fold steps    | Yes |
| MP7  | Width fixed per compile                                                                           | Yes |
| MP8  | Mono ports drawn as rings, mono cables thinner                                                    | Yes |
| MP9  | Folding cables marked, with a tooltip, no warning                                                 | Yes |
| MP10 | Meters on mono ports show one bar                                                                 | Yes |
| MP11 | Component ports have a width                                                                      | Yes |
| MP12 | Only the plugin setting and component port widths are saved                                      | Yes |
| MP13 | Mono-only plugins fed stereo now hear the fold, not the left; no compatibility mode               | Yes |
| MP14 | The tests above                                                                                   | Yes |
| MP15 | The width pass asks each source for its width, so mono clips and inputs can come later           | Yes |

## 10. Owner's answers (2026-11-13)

1. **Mono into stereo level:** both channels at full level.
2. **Plugins on Auto, fed mono:** mono → mono first.
3. **Built-in nodes:** all of the list in section 6.
4. **Mono sources:** later, but designed for now (MP15).

## 11. Steps

1. Ports with a width, the compiler's width pass (with `sourceWidth` as the one place sources answer, MP15), mono
   views, fold steps and step widths; Mix, Feedback, Gain and the delay-compensation nodes adaptive; the tests for MP2,
   MP3, MP6. **Done.**
2. The other built-in nodes from section 6. **Done.**
3. Plugins: the layout negotiation and the Channels setting; Faust's odd ports; mono output buses. **Done.**
4. UI: port and cable looks, fold markers, mono meters, the Channels setting. **Done.**
5. Component port widths. **Done.**
6. Then the split nodes (split-nodes.md) with mono ports. **Done.**
7. Mono clips and device inputs (MP15). **Done.** The Audio Player's output is adaptive with no inputs: the compiler
   asks `playerWidth()`, which answers mono when every clip on the track (or the one file a player is given) is a mono
   file and the input, if one is set, is mono; a track with neither stays stereo. A mono player writes one channel and
   hears a mono input. Switching the input between mono, stereo and none recompiles; moving it to another channel of
   the same width doesn't.
