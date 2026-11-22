# Design: split nodes (bands, mid/side, left/right)

Status: **accepted** (2026-11-13). The decisions are numbered SP1–SP12 and summarised in section 7; the owner's answers
are in section 8. It builds on phase8-node-graph.md, component-nodes.md and channel-strip.md (C8), and waits for
mono-ports.md: the stereo nodes' parts are mono ports (SP4, revised 2026-11-13).

## 1. Goals and non-goals

Goals:

* **Process parts of a signal apart, inside one track.** Compress the lows and leave the highs, saturate only the
  side, delay only the right channel, widen only the top end: split, process each part with any nodes (plugins too),
  and join again.
* **Built in, no scripting.** Today this takes a Faust script or a third-party multiband plugin.
* **Transparent when nothing is done.** Split and joined again with nothing in between, the signal keeps its level at
  every frequency (bands) or is unchanged sample for sample (mid/side, left/right).
* **Live.** Crossover frequencies can be moved, automated and modulated while playing, without clicks.
* **Ready-made.** "Multiband compressor" and "Mid/side" setups as components, so the device strip can hold them.

Non-goals:

* Implementing linear-phase crossovers now. They are designed for (SP11) and come later.
* Splits by other criteria: transient/sustain, spectral (tonal/noise).
* Surround.
* Effect plugins with several output buses (multi-output-instruments.md covers instruments only); this proposal is the
  built-in answer to most of what those would be used for.

## 2. What exists

* **Fan-out and fan-in in the graph.** An output port can feed any number of inputs, so a parallel chain is already
  possible; the **Mix** node sums up to 4 stereo inputs with a gain each.
* **Ports per node.** Nodes can have several output ports (the Sampler and instrument plugins have up to 16, named per
  port, multi-output-instruments.md).
* **The EQ already works in mid/side per band** (channel-strip.md, C8): M = (L + R) / 2, S = (L − R) / 2. That covers
  mid/side EQ, but nothing else can work on the mid or the side.
* **The Filter node** uses a TPT state-variable filter (`Svf` in EffectNodes.cpp), which modulates smoothly.
* **The Loop Reverb has a `Crossover`** (loopreverb/Dsp.h): two-band Linkwitz-Riley, but with fixed coefficients that
  are cleared whenever the frequency is set, so it can't be moved while playing.
* **Faust:** a script with more than 2 output channels gets more stereo ports ("Out 1", ...), and a component can wrap
  it. This works, but each user has to write the DSP.

## 3. How other DAWs do it

* **Bitwig:** *Multiband FX-2 / FX-3* and *Mid-Side FX* are containers: each band or the mid and the side is a chain of
  its own inside one device, summed at the end. Crossovers are Linkwitz-Riley, and the frequencies are modulatable.
* **Ableton:** *Multiband Dynamics* is one fixed device; for anything else, an *Audio Effect Rack* with parallel
  chains, each starting with an EQ that cuts out its band, which doesn't sum flat. EQ Eight works in mid/side, for EQ
  only.
* **FL Studio:** Patcher with a multiband splitter feeding separate chains.
* **Reaper:** JS splitters writing bands to extra track channels, picked up by later plugins' pin mappings.

What they share: a **splitter whose outputs feed parallel chains, then a sum**. DAWg's graph already has chains and
sums; what it lacks is the splitter. In the graph this is a plain node with several outputs, and the container view
(Bitwig's) is what a component already gives.

## 4. Model

**(SP1) A Band Split node** (`dawg.bandSplit`): one stereo input, **2 to 4 stereo outputs**, one per band, named by
count:

| Bands | Ports                          |
|-------|--------------------------------|
| 2     | Low, High                      |
| 3     | Low, Mid, High                 |
| 4     | Low, Low mid, High mid, High   |

The number of bands is a setting of the node (`GraphNode::bands`, 3 by default), not a parameter: changing it changes
the ports, so it recompiles and is one undo step, and cables from ports that go away are removed in the same step. Four bands is the limit because the Mix node has 4 inputs, so a split always fits in one
Mix (question 1).

**(SP2) Linkwitz–Riley 24 dB/octave crossovers, built from TPT filters.** Each crossover is two cascaded second-order
Butterworth low-passes and high-passes (LR4) made of the same state-variable filter the Filter node uses, so moving a
frequency never clicks or rings, and modulation works at audio rate. At a crossover both bands are at −6 dB and in
phase, so the sum is flat. With 3 or 4 bands, the lower bands also pass through the all-pass of each higher crossover
(the standard compensation), so **the bands always sum to an all-pass of the input**: flat magnitude, with the phase
turn an analogue crossover has. Zero latency.

Parameters, automatable and modulatable as on any effect:

* **Crossover 1–3** (20 Hz–20 kHz, log). Defaults 120 Hz, 1 kHz, 6 kHz for 4 bands; 200 Hz and 2.5 kHz for 3; 1 kHz
  for 2. They are kept in order: one dragged past the next pushes it along, and while modulated each is clamped to at
  least a third of an octave above the one below.
* **Solo** (Off, or a band): the other outputs go silent, to hear one band while setting the frequencies. It is a
  listening aid and is saved like any parameter.

Only one slope: 24 dB/octave (question 2).

**(SP11) Linear phase, designed for now and built later.** A **Phase** choice parameter, *Minimum* (SP2, the default)
or *Linear*, marked `changesLatency` like the Saturator's oversampling: a change recompiles the track, which reads the
node's `latencySamples()` again, so delay compensation covers it. It is not automatable or modulatable for that reason.
*Linear* uses complementary FIR filters: each band is a linear-phase band-pass whose kernels sum to a pure delay, so
the bands sum to the input delayed by the latency, with no phase turn at all. The kernel length is fixed (so the
latency is too) and set by the lowest crossover's lowest allowed frequency; the kernels are built off the audio thread
and applied by FFT convolution. Moving a crossover in *Linear* builds new kernels and crossfades to them over one block
(no audio-rate modulation; modulation is read once per block). To keep this a drop-in later, the engine's Band Split
works through a **crossover interface** from the start (section 5): `prepare`, `setFrequencies`, `latency`, and
`process (input, bands, dry)`, with the minimum-phase LR4 bank as its first implementation. Solo and the Dry port sit
outside it, so they work in both modes; in *Linear* the Dry port is the input delayed by the latency.

**(SP3) Phase: don't mix the bands with the unsplit signal.** Because the sum is an all-pass, adding the bands back to
the dry input (another cable from before the split) combs. The Band Split's help text says so, and the ready-made
components (SP8) take their dry/wet *after* the bands are summed, against an all-pass-matched dry (the Band Split has
an extra **Dry** output port carrying the input through the same all-passes, shown only when wired). This keeps
parallel compression and dry/wet blends clean.

**(SP4) A Stereo Split node** (`dawg.stereoSplit`) with a **Mode** setting, *Mid/side* (the default) or *Left/right*:
one stereo input, two stereo outputs.

| Mode       | Ports       | Signals                                   |
|------------|-------------|-------------------------------------------|
| Mid/side   | Mid, Side   | M = (L + R) / 2, S = (L − R) / 2 (as C8)  |
| Left/right | Left, Right | L, R                                      |

Each output is a **mono port** (mono-ports.md, MP1), so the effects after it run in mono, and plugins get their mono
layout (MP4); a stereo input after it gets the part on both channels (MP2). The mode is a setting like the Band Split's `bands` (`GraphNode::mode`): it
renames the ports and recompiles, and since both modes have two ports, cables stay. The menu offers "Mid/side split"
and "Left/right split", which are the same node in either mode.

**(SP5) A Stereo Merge node** (`dawg.stereoMerge`) with the same **Mode** setting: two inputs named as the split's
outputs, each a **mono port**, one stereo output. A stereo cable into it is folded to mono, (L + R) / 2 (MP2), so a
part that a stereo effect made wide comes back as mono rather than dropped on one side, and the fold is marked on the
cable (MP9).

* *Mid/side:* L = M + S, R = M − S. Parameters **Mid** and **Side** gain (dB, −inf to +12), so the merge is also a
  width control.
* *Left/right:* L = Left, R = Right. Parameters **Left** and **Right** gain, the same range, so it also balances.

Split then Merge in the same mode, with nothing between, is the input exactly (bit-exact apart from rounding). The
gains keep their values when the mode changes, as the first and second gain.

The Merge is its own node because the Mix node sums; the matrix back to left and right is what makes mid/side work,
and for left/right, putting each part back on its own side.

**(SP6) Bands are joined with the Mix node.** No Band Merge: the Mix node at 0 dB on each input is exact, and its gains
are band levels for free.

**(SP7) Widths.** The Band Split is adaptive (MP1): fed mono, its bands and Dry port are mono. A mono source into a
Stereo Split gives a silent side, or equal Left and Right; nothing special.

## 5. Engine

* `BandSplitNode`, `StereoSplitNode` and `StereoMergeNode` in a new `SplitNodes.cpp`, registered with the effects. The
  Band Split's descriptor depends on `bands` and the stereo nodes' on `mode`, as the Sampler's depends on its zones'
  outputs.
* The Band Split runs a **crossover bank** behind an interface (SP11): `prepare (sampleRate, maxBlock, bands)`,
  `setFrequencies`, `latency()`, `process (input, bands, dry)`. The first implementation, `MinimumPhaseCrossovers`, is
  built from a small `LinkwitzRiley4` class (`setFrequency` with smoothing, `process(x) -> {low, high}`, and
  `allpass(x)`) on the Filter node's `Svf`, which moves to a shared header. The linear-phase bank comes later and
  plugs in here. The Loop Reverb keeps its own crossover until there is a reason to switch it.
* A band output that nothing is wired to is still computed (it is cheap), so Solo and the Dry port need no special
  cases. The step's output buffers already exist for every port.
* No latency in *Minimum* phase, no new compiler features: ports, cables, Mix, components, sends, freezing and latency
  compensation all work as for any node.

How it was built (steps 1 and 2):

* **Ports are fixed by index.** The Band Split always has five outputs, Low, Band 2, Band 3, High and Dry (index 4),
  and computes silence on the bands its count leaves out; the graph view hides those and names the rest after the count.
  So `bands` only needs `configure()`, a cable keeps its port when the count changes, and `setBandCount` removes the
  cables from the ports that go away. The stereo nodes' ports are named Mid and Side, and the graph view names them Left
  and Right in that mode.
* **Defaults per count:** the crossover parameters default to 3 bands' (200 Hz, 2.5 kHz, 6 kHz); `setBandCount` moves
  crossovers still at the old count's defaults to the new count's.
* **The crossover** (`dawg/Crossover.h`): `CrossoverBank` is the interface (`prepare (sampleRate, channels)`,
  `setFrequencies`, `latency`, `process (channel, input, bands, dry, numSamples)`), and `MinimumPhaseCrossovers` the
  first bank. It uses its own Butterworth TPT section rather than the Filter node's `Svf`, whose matched design does not
  make the low and high add up to an all-pass; a bilinear section does, exactly. The node updates the frequencies
  every 16 samples, smoothed, in order, a third of an octave apart and below 0.45 of the sample rate.
* **Phase** is there, with *Linear* running as *Minimum* until it is built.
* **Bypass:** a bypassed Band Split puts everything in the lowest band (and Dry), so its bands still add up to the
  input; a bypassed Stereo Split does what it always does, which loses nothing; a bypassed Merge joins at 0 dB.
* **Commands:** `setBandCount { trackId, nodeId, bands }` and `setStereoMode { trackId, nodeId, mode: "midSide" |
  "leftRight" }`, in any graph scope.

## 6. UI and file

* **Graph:** the Band Split's card shows a small frequency view with the bands in colours and the crossovers as handles
  to drag; its ports carry the same colours. The stereo nodes in *Mid/side* show M and S ports in the EQ's mid and side
  colours (orange and violet, C8), and in *Left/right* L and R.
* **Inspector:** the parameters as usual, the band count as a segmented control (2, 3, 4), Solo as buttons per band,
  Phase as a choice (*Linear* shown disabled until it exists), the stereo nodes' mode as a segmented control.
* **Node menu (Add node):** under Utility, "Band split", "Mid/side split", "Mid/side merge", "Left/right split",
  "Left/right merge".
* **Ready-made components (SP8)** under Components, insertable from the device strip like any component:
  * **Multiband compressor:** Band Split (3 bands) → a Compressor per band → Mix → dry/wet against the Dry port.
    Exposed: the crossovers, each band's threshold, ratio and makeup, and the mix.
  * **Mid/side:** Stereo Split → an empty chain on each → Stereo Merge, both in *Mid/side*, with the merge's gains
    exposed: a starting point to drop effects into from the graph.
* **File:** the new node types, `bands` (written only when not 3) and `mode` (written only when not mid/side).
  Nothing else changes.

**(SP8)** The two components ship as built-in component files, editable and saveable as copies like any component.

How it was built (steps 3 and 4): the graph card shows the bands on a frequency strip whose crossovers drag (one
dragged past the next pushes it along, the drag one undo step); ports are coloured per band, mid and side in the EQ's
colours, left and right apart. The inspector has the band count (2, 3, 4), Solo as a choice of the bands, Phase with
*Linear* disabled, and the stereo nodes' mode. The node menu offers "Band split", "Mid/side split", "Left/right split",
"Mid/side merge" and "Left/right merge" (`addNode` takes `mode` and `bands`); chain pickers and `insertNode` don't
offer them (SP9). The two components are factory patches (graph/patches.ts), as the Dub Delay and the others are,
offered wherever components are, the device strip included: **Multiband Compressor** (3 bands, a Compressor each, a
Mix; a second Mix blends in the Dry port, silent at first; exposed: both crossovers, each band's threshold, ratio and
makeup, and Dry) and **Mid/Side** (a split straight into a merge, the merge's gains exposed). The dock's generic panel
shows the Band Split's parameters, Phase's *Linear* included, which runs as *Minimum* until it is built.

**(SP9) In the device strip, only inside components.** A node with several outputs can't sit in the linear device chain,
so the strip offers the splits only inside components; in the graph they can go anywhere.

**(SP10) Tests:** bands summed by a Mix node equal an all-pass of the input (magnitude flat within 0.01 dB from 20 Hz to
20 kHz at 44.1, 48 and 96 kHz); each band is −6 dB at its crossover and below −40 dB two octaves into its neighbour; a
crossover swept across its range while a sine plays shows no step larger than the sine's own change; Solo; the Dry port
matches the summed bands; Split then Merge returns the input in both modes; Merge's gains in both modes; Left/right
puts each channel on its own output; changing the band count drops the right cables in one undo step; changing the
stereo mode keeps them; the components load and play. When *Linear* comes: the bands sum to the input delayed by the
reported latency, and a moved crossover crossfades without a click.

**(SP12) Left/right is a mode of the stereo nodes**, not a pair of nodes of its own: the two splits differ only in the
matrix, so one pair of nodes with a mode keeps the menu entries without doubling the code.

## 7. Decisions to review

| #    | Decision                                                                                      | Recommended |
|------|-----------------------------------------------------------------------------------------------|-------------|
| SP1  | A Band Split node with 2–4 band outputs; the count is a setting that changes the ports        | Yes |
| SP2  | LR4 crossovers from TPT filters, all-pass compensated; frequencies modulatable, kept in order | Yes |
| SP3  | A Dry port, all-pass matched, for blends; don't mix bands with the unsplit signal             | Yes |
| SP4  | A Stereo Split node, mid/side or left/right; each part a mono port                            | Yes |
| SP5  | A Stereo Merge node with a gain per part; mono inputs, stereo cables folded                   | Yes |
| SP6  | Bands joined by the existing Mix node; no Band Merge                                          | Yes |
| SP7  | The Band Split is adaptive; nothing special for mono sources                                  | Yes |
| SP8  | Built-in components: Multiband compressor and Mid/side                                        | Yes |
| SP9  | In the device strip only inside components                                                    | Yes |
| SP10 | The tests above                                                                               | Yes |
| SP11 | A Phase parameter (Minimum, Linear); a crossover interface now, linear phase built later      | Yes |
| SP12 | Left/right is a mode of the stereo nodes                                                      | Yes |

## 8. Owner's answers (2026-11-13)

1. **How many bands?** 4.
2. **Slopes:** only 24 dB/octave.
3. **Linear phase:** yes, as a mode, but built later; design for it now (SP11).
4. **Mid and Side ports:** first each signal on both channels; then mono ports, designed in mono-ports.md.
5. **Components:** Multiband compressor and Mid/side.
6. **Added:** a way to split a stereo signal into left and right (SP4, SP5, SP12).

## 9. Steps

0. Mono ports (mono-ports.md), or at least its step 1.
1. The crossover class and the Band Split node, with the tests (no UI beyond the generic node card). **Done.**
2. Stereo Split and Merge, both modes. **Done.**
3. Graph and inspector UI: the frequency view, the band count, Solo, Phase, the stereo mode, port colours. **Done.**
4. The built-in components. **Done.**
5. The linear-phase crossover bank. **Done.** `LinearPhaseCrossovers` (engine/src/Crossover.cpp): each band but the
   highest is a linear-phase FIR with its Linkwitz-Riley counterpart's magnitude (designed on a grid twice the kernel's
   length, Blackman-windowed around its centre), applied by uniformly partitioned FFT convolution in 512-sample
   blocks; the highest band is the delayed input minus the others, so the sum is a pure delay to the last bit. Kernels
   are about 0.17 s at any rate (8191 taps at 48 kHz), so the latency is 512 + 4095 samples there, about 96 ms. A
   thread of the bank's own builds new kernels when the crossovers move (the audio thread can't wake it, so it looks
   every 10 ms); the audio thread switches at a block boundary and crossfades over that block. The first kernels are
   built when *Linear* is chosen, so playback starts split. The inspector's *Linear* button is enabled.
