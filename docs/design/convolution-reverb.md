# Design: the Convolution Reverb

Status: **implemented** (notes in section 8). The decisions are numbered I1–I15; the owner's answers are in section 7.

## 1. Goals

* A built-in convolution reverb (`dawg.convolutionReverb`): it convolves the signal with an impulse response (IR), a
  recording of a real room, plate, spring or cabinet, or any audio file. It is the reverb for real spaces, next to
  the algorithmic Loop Reverb (docs/design/loop-reverb.md), and the missing core effect the feature review listed.
* No added latency: the dry and wet paths are sample-aligned, and the node reports 0.
* IRs come from the project's audio pool, like a Sampler's samples, so a project stays self-contained. A file
  dragged from the sound explorer or picked in a dialog is copied into the pool first.
* Real-time safe: IRs are read, shaped, resampled and transformed off the audio thread, and nothing allocates, locks
  or waits in `process()`.
* Changing the IR, or any control that reshapes it, never clicks and never cuts the reverb's tail off.
* Long IRs at a modest, predictable CPU cost: a 10 s stereo IR at 48 kHz costs a few percent of one core.

Non-goals for v1:

* Voice graphs. A convolution per voice costs too much, so the node is for track graphs only (I13).
* Buffers below 128 samples with long IRs (I2): the transforms at each 4096-sample boundary come in one burst of
  about 2 ms for a 10 s IR.
* IRs longer than 20 s (I3).
* Modulating the IR's shape (Start, Length, Decay, Reverse, Stretch): each change rebuilds the IR (I6, I8).
* Recording or deconvolving IRs (a sweep generator), and an IR browser of its own: the sound explorer is the browser.

## 2. What it reuses

* **The audio pool and its import** (docs/design/sampler-midi-delay-limiter.md, section 4): `AudioLibrary::importFiles`
  copies WAV, AIFF and FLAC into the project's `Audio/` folder (other formats are decoded to WAV), `poolAudioFile` adds
  the entry, `AudioSource` reads it, and `AudioLibrary::missing` and `locateMissing` find and relink lost files. The
  Sampler's `sampler.import` and `sampler.importDialog` bridge calls are the model for the node's (I10).
* **Edits without a recompile**: a Sampler's zones reach it through `Node::setSampleMap` without recompiling the
  track (`publishSampleMaps`), and the audio thread takes new data through `SnapshotExchange` (`dawg/Realtime.h`). The
  IR takes the same path (I6, I11).
* **The effect plumbing**: `EffectNode` for bypass, `followParam` for 20 ms parameter ramps and per-sample
  modulation, the matched biquad designs in EffectNodes.cpp for the wet filters (I9), and the Loop Reverb's
  equal-power Mix (I9).
* **The panel**: `SampleWaveform`'s missing-file handling and the sound explorer's drag data (`PATHS_MIME`), the
  device card's live min-content width (docs/design/device-strip.md, D5) and the container query units on
  `.device-card-body`.

## 3. Decisions

**(I1) A new built-in effect node.** `dawg.convolutionReverb`, "Convolution Reverb", `NodeCategory::effect`, in
EffectNodes.cpp beside the other effects, whose filters and parameter helpers it shares, with the convolution engine,
the IR's preparation and the builder thread in `engine/src/Convolver.cpp` (`dawg/Convolver.h`): one stereo
audio input with modulation inputs, one stereo output, bypass through `EffectNode`. Track graphs only (I13).

**(I2) The engine: our own non-uniformly partitioned convolver.** Zero latency needs the IR's first samples to reach
the output in the same sample they arrive; long IRs need large FFT partitions to be cheap. The convolver splits the
IR into a direct head and three levels of uniformly partitioned FFT convolution (overlap-save with a frequency-domain
delay line, "UPOLS"), each level's partitions eight times the size of the previous:

| part | IR samples | partition | how | runs |
|------|------------|-----------|-----|------|
| head | 0 – 63 | — | direct FIR, 64 taps | every sample |
| level 1 | 64 – 511 | 64 | 7 partitions | every 64 samples |
| level 2 | 512 – 8191 | 512 | 15 partitions | every 512 samples |
| level 3 | 8192 – end | 4096 | as many as needed | spread over every 4096 samples |

Levels 1 and 2 start one partition in (IR sample `P`): their first output sample is needed `P` samples after the
input block it depends on was complete, which is exactly when the level runs, so they add no latency
(W. G. Gardner, "Efficient convolution without input-output delay", JAES 1995; F. Wefers, *Partitioned convolution
algorithms for real-time auralization*, 2015). Level 3 starts two partitions in (sample 8192), so its output for a
block is needed a whole block after its input block is complete: its multiply-adds are spread in 64 equal steps over
that block, one per 64 samples, and only its transforms happen at the boundary (time-distributed partitioned
convolution, as in Wefers, chapter 5). As first designed, with level 3 starting at 4096 and all its work at the
boundary, the slowest 256-sample block took 6.3 ms for a 10 s stereo IR and 13.5 ms in true stereo, longer than the
block lasts (5.3 ms); spread, it takes about 2 ms. Each level keeps the spectra of its last input blocks (the delay line)
and multiplies them with the IR's partition spectra; the IR's spectra are computed once when it is built (I4). FFTs
are `juce::dsp::FFT` real transforms of size `2P`.

The work happens at fixed sample counts from the node's start, not per `process()` call, so the output is
bit-identical for any sequence of block sizes, as the Loop Reverb's is, and tests can compare runs exactly.

Measured (the hidden benchmark in tests/ConvolverTests.cpp, Release, 48 kHz, 256-sample blocks, a 10 s IR): 6.5 %
of one core in stereo and about 10 % in true stereo; the slowest block, the one with the 4096-sample boundary's
transforms, about 2 ms (2–3.6 ms in true stereo, varying from run to run). At 128-sample buffers (2.7 ms) that is
tight for long true-stereo IRs, which is the non-goal above; a faster FFT than JUCE's fallback, or level 2's work
spread as level 3's is, would be the next step.

Why not `juce::dsp::Convolution` with `NonUniform` partitioning, which was the first candidate:

* It has two levels only: a zero-latency head that runs an FFT of twice the *maximum* block size on every call,
  whatever the call's length, and one tail level.
* Loading an IR builds a new engine with an empty history and crossfades to it over 50 ms, so every change of a
  trim or decay control would cut the tail off. Our levels keep their input history across IR changes (I6).
* Its loading functions must be called from the audio thread, each instance starts its own loader thread unless a
  shared queue is passed, and it resamples IRs with `ResamplingAudioSource` (linear interpolation behind a gentle
  low-pass), which aliases when a 96 kHz IR is brought down to 48 kHz.
* Its output depends on the block sizes it is called with.

Our engine is about 400 lines and is tested against a direct convolution (section 5).

**(I3) Mono, stereo and their channels.** A mono IR is convolved with each input channel. A stereo IR's left channel
convolves the left input and its right channel the right input: the usual "stereo IR" convention, which is what most
free and commercial stereo IRs are recorded for. The input spectra are computed once per input channel and shared
by every IR channel that reads them, so true stereo (four IR channels: left to left, left to right, right to left,
right to right) doubles only the multiply-adds, not the FFTs, and v1 has it (question 3). A four-channel file is
true stereo in that order (the convention of the common true-stereo libraries); files with three or more than four
channels use their first two as a stereo IR. `AudioSource` reads two channels for playback, so it gets
`readAllChannels`, which reads every channel of the file into a buffer, for the builder. IRs are capped at 20 s after Stretch; a longer file is cut there, with
the same short fade-out as Length (I8), and the panel says so.

**(I4) The IR is built on a background thread.** Building an IR, in this order:

1. read the pool file at its own rate with all its channels (`AudioSource::readAllChannels`; files up to 30 s are
   already in memory), or generate the default IR (I10);
2. cut it to Start and Length (I8), at the file's rate;
3. apply the Decay envelope and the fades (I8), measured from Start;
4. reverse it, if Reverse is on, so a reversed IR swells into its loudest part;
5. resample it to the device rate, with Stretch folded into the ratio (I8), by a windowed-sinc resampler (Kaiser
   window, 32 zero crossings each side, cutoff at 0.95 of the lower Nyquist): a 96 kHz IR keeps no alias above
   24 kHz when brought down to 48 kHz;
6. scale it (I5);
7. split it into the head and partitions and transform the partitions.

A single shared worker thread ("IR builder") runs the builds for every node, newest request per node first; a request
that is superseded before it starts is dropped. The node starts a build when its file, its sample rate or one of the
shaping parameters changes. A 10 s stereo IR at 96 kHz takes about 50–100 ms to build (the resampler dominates). The
result is an immutable kernel: the head taps, the partition spectra per level and channel, the shaped IR's envelope
for the panel, and the file's estimated decay time (I12).

**(I5) Level: the IR is normalised by default.** IRs come at very different levels: a cabinet IR peaks near full
scale, a hall recorded from a distance is quiet, and a long IR's energy adds up. With **Normalise** on (the default),
the IR is scaled so that the energy reaching each output, `Σ h²` summed over the IR channels feeding that output
and averaged over the two outputs, is 1 at the device rate, measured on the
whole file before Start, Length and Decay. Then white noise comes out of the wet path at the dry level, whatever the
file, and shortening or decaying the IR makes the reverb quieter, as it should. With Normalise off the file is used as
it is, with the gain that keeps a resampled IR's response the same (`file rate / device rate`), so a unit impulse at
the file's rate returns the file. The wet path's **Gain** (I9) trims the level either way.

**(I6) Swapping IRs: a crossfade that keeps the tail.** A new kernel reaches the audio thread through a
`ProgramExchange` (like `SnapshotExchange`, but the audio thread keeps a program until it has faded out and then
retires it). At the next 4096-sample boundary, where all levels are at a boundary together, the new kernel starts
preparing: level 3 sums its first block over that cycle, as it does for the playing kernel (I2). At the boundary
after, it crossfades linearly from the old kernel's output to the new one's over 4096 samples. An edit is heard
85–170 ms later at 48 kHz and has faded in after 255 ms; a newer one that arrives meanwhile replaces one that hasn't
started preparing. During the fade both kernels run on the same input history, so the new IR
sounds as if it had always been there: the tail of what was played before carries on through the new IR. Only the
multiply-adds and inverse FFTs are doubled while fading, never the forward FFTs. The retired kernel goes back to the
message thread to be freed.

The delay lines hold as many level-3 partitions as the longest kernel the node has needed so far, with a quarter
more room. A kernel that needs more (a longer file, or more Stretch) comes with a larger history built on the
builder thread; when it starts preparing, the audio thread copies the old history's newest state into it (the head, levels 1 and
2, and up to 2 MB per channel of level-3 spectra, about 64 partitions or 5.5 s at 48 kHz), so even then the tail
carries on, apart from the far end of a very long one. Edits of Start, Length, Decay and Reverse never need more.

**(I7) Pre-delay: a delay line in front of the convolver.** 0 to 250 ms, default 0. A change moves the read tap with a
20 ms crossfade between the old and new tap positions; as it sits before the convolver, the reverb's tail is never
cut and nothing glides in pitch. It is automatable but not modulatable (moving it continuously would be a pitch
effect). Pre-delay in the IR itself (zeros in front) would cost partitions and a rebuild per change.

**(I8) Shaping the IR.** These parameters rebuild the IR (I4), so they are neither modulatable nor automatable (I14):

* **Start** (0–1000 ms, default 0): cuts the beginning off, to skip silence before the direct sound or to remove the
  direct sound and keep the room. A 1 ms fade-in follows the cut.
* **Length** (1–100 %, default 100 %): how much of the IR after Start is kept. A shortened IR ends with a fade-out
  over the last 10 % of what is kept (at least 5 ms), so it never stops abruptly.
* **Decay** (10–100 %, default 100 %): shortens the reverb time smoothly instead of cutting it. The builder estimates
  the IR's decay time `T` (I12), and multiplies the IR by `e^(−a t)` with `a` chosen so the result decays in
  `Decay × T`: `a = (ln 1000 / T) · (1 / Decay − 1)`, which adds the missing slope to the IR's own. At 100 % the IR
  is untouched.
* **Reverse** (off by default): plays the IR backwards, after Start and Length, for reverse-reverb swells.
* **Stretch** (50–200 %, default 100 %): resamples the IR as if the room were larger or smaller: longer and darker,
  or shorter and brighter. It changes the resampling ratio of step 5 of I4, so it costs nothing extra. Whether v1 has
  it is question 4.

**(I9) The wet path and Mix.** After the convolver, on the wet signal only, in this order:

* **Low cut** (20–1000 Hz, default 20 Hz) and **High cut** (1–20 kHz, default 20 kHz): second-order Butterworth
  high-pass and low-pass, the matched designs the EQ uses, 12 dB per octave. At the end of its range each is out of
  the path (it keeps running, so moving it in starts from its settled state), so the defaults leave the IR exactly as
  it is.
  Reverbs are usually cleaned up exactly here; these work in real time and are modulatable, unlike reshaping the IR.
* **Width** (0–200 %, default 100 %): mid/side on the wet signal; 0 % is mono, 100 % the IR as recorded, 200 % the
  side doubled.
* **Gain** (−24 to +12 dB, default 0): the wet level.
* **Mix** (0–100 %, default 25 %): dry and wet at equal power, as on the Loop Reverb; on a return track the user sets
  it to 100 %.

All of these, and Mix, ramp over 20 ms through `followParam` and follow modulation per sample.

**(I10) Where IRs come from: the pool.** The node's model data is the pool file it uses, a new
`GraphNode::fileId` (0 for none), saved with the node. The file is an ordinary pool entry, so Save As, Collect and
missing-file handling cover it with no new code.

* **Commands**: `setNodeFile { trackId, nodeId, file: { path, name, sampleRate, channels, frames } }` pools the file
  (`poolAudioFile`) and sets `fileId`, or `{ fileId }` points at a file already in the pool, or `{ fileId: 0 }`
  removes it. One undo step, "Load impulse response".
* **Bridge**: `convolution.import { trackId, nodeId, paths }` (a drop from the sound explorer) and
  `convolution.importDialog { trackId, nodeId }` copy the file into the project's `Audio/` folder with
  `AudioLibrary::importFiles`, as the Sampler's calls do, then apply `setNodeFile`. A drop of several files takes the
  first.
* **Missing files**: the wet path is silent (Mix still passes the dry signal), and the panel shows "Missing file"
  with the existing Locate action, as the Sampler's zones do.
* **No file**: the node uses a default IR that DAWg generates (question 1): stereo noise, different per channel
  from a fixed seed, decaying by 60 dB in 2 s, through a one-pole low-pass whose cutoff falls from 12 kHz to 3 kHz
  over the tail, as air and walls absorb highs, with a 5 ms onset ramp. It is built at the device rate, so it needs
  no resampling, and every control shapes it as it would a file. The panel names it "Default room".
* **Bundled IRs**: none in v1. Recorded IRs carry licences (the OpenAIR library's are mostly CC BY or CC BY-SA, some
  non-commercial), and bundling them in an AGPL program needs each one's terms checked and its attribution shown.
  IRs we generate ourselves have no such problem (the default IR, below).

**(I11) Getting the file to the node.** The graph compiler hands the node its file as it hands a Sampler its zones:
a new `Node::setAudioFile (std::shared_ptr<const AudioSource>)`, called when the node is compiled and again when only
`fileId` changed, without recompiling (`sameStructure` ignores `fileId`, as it ignores zones). The node compares the
source with the one it has and starts a build (I4) only if it changed. A relinked file changes the pool entry's path,
which recompiles as before, and the node then gets the new source.

**(I12) Panel meters.** `panelMeters()` returns `{ envelope, seconds, decay, pending, missing, truncated }`:

* `envelope`: the whole file's envelope, at most 256 points per channel, in dB (the RMS over each point's span, as the
  louder channel), computed once per file by the builder. The panel draws the trim, decay and fade over it, so moving
  Start or Length doesn't move the picture.
* `seconds`: the file's length in seconds; `decay`: its estimated decay time `T` in seconds (from the Schroeder
  backward integral, the slope between −5 and −35 dB extrapolated to −60 dB, i.e. T30; for IRs that never fall
  35 dB, from −5 to −25 dB, T20; 0 when not estimable, and Decay then does nothing).
* `pending` while a build is under way, `missing` for a missing file, `truncated` when the file was cut at 20 s.

**(I13) Not in voice graphs, no latency.** `nodetype::allowedInVoiceGraph` excludes `dawg.convolutionReverb`: a voice
graph runs up to 16 instances per Voice Instrument, each with its own delay lines and bursts. `latencySamples()` is
0, and the tests show it.

**(I14) A parameter flag for "not automatable".** `ParamInfo` gets `automatable` (default true), and
`acceptsAutomation()` returns `automatable && ! changesLatency`. The shaping parameters (I8) and Normalise set it to
false and `modulatable` to false: automation runs on the audio thread, which can't start a build. Their changes come
from the panel and undo, which call `configure()`, which compares them with the values of the last build and starts a
new one.

**(I15) Export waits for builds.** An offline export renders with the live instances right after a change could have
started a build. `renderExportPasses` first waits until the IR builder is idle (`ConvolutionReverb::waitForBuilds()`),
so an export never renders without the IR it should have. Live playback doesn't wait: right after a project opens the
node is dry until its IR is ready, usually well under a second.

### Parameters

| # | id | name | range | default | unit | modulatable | automatable |
|---|----|------|-------|---------|------|-------------|-------------|
| 0 | `predelay` | Pre-delay | 0..250 | 0 | ms | no | yes |
| 1 | `start` | Start | 0..1000 | 0 | ms | no | no |
| 2 | `length` | Length | 1..100 | 100 | % | no | no |
| 3 | `decay` | Decay | 10..100 | 100 | % | no | no |
| 4 | `reverse` | Reverse | off, on | off | | no | no |
| 5 | `stretch` | Stretch | 50..200 | 100 | % | no | no |
| 6 | `normalise` | Normalise | off, on | on | | no | no |
| 7 | `lowCut` | Low cut | 20..1000 | 20 | Hz | yes | yes |
| 8 | `highCut` | High cut | 1000..20000 | 20000 | Hz | yes | yes |
| 9 | `width` | Width | 0..200 | 100 | % | yes | yes |
| 10 | `gain` | Gain | −24..12 | 0 | dB | yes | yes |
| 11 | `mix` | Mix | 0..100 | 25 | % | yes | yes |

### The panel (I-panel)

`ui/src/dock/ConvolutionReverbPanel.tsx`:

* **Header row**: the IR's name ("Default room" for the generated one, or "Missing file" with a Locate button), a
  Load button that opens the dialog, a button back to the default room, and the reverb time readout. The graph and
  header are a drop target for files from the sound explorer, highlighted while a file is dragged over them.
* **Graph**: the file's envelope in dB (72 dB below its peak to the peak) over the file's time, filled and dim. The
  cut parts (before Start, after Start + Length) are shaded; over the kept part, the resulting envelope after Decay
  and the fade, in the accent colour; Reverse mirrors it within the kept part. The axis is the file's own time, so
  trimming never moves the picture; Pre-delay and Stretch show in their knobs and in the reverb time readout
  ("RT 2.4 s", the decay time after Decay and Stretch). "Building…" before the first build, "The file is missing" for a
  missing file.
* **Controls**, in two rows of sections so none is out of view: Impulse (Start, Length, Decay, Stretch) and Space
  (Pre-delay, Width); Shape (Reverse, Normalise), Tone (Low cut, High cut) and Output (Gain, Mix).
* **Layout**: the graph takes the space left beside the controls and gives way in width down to a floor (160 px); the
  controls never overlap it, because the panel's min-content width is the controls' width plus the graph's floor,
  which the card measures (D5). The controls are in two rows; when the card's body is too short for two rows of knobs
  (under 176 px, a dock of about 230 px or less), a container query on `.device-card-body` puts them in one row, and
  the card grows to that row's width. The graph's height is the body's less the header, never a fixed aspect ratio,
  so it can't grow under the controls.
* Card width in `deviceWidths.ts`: default 760 px, minimum 640 px (the two-row layout's measured minimum).
* **Presets**, which only set parameters (the IR stays): "Natural" (defaults), "Tight Room" (Length 40 %, Decay
  60 %, Low cut 150 Hz), "Dark Hall" (High cut 5 kHz, Pre-delay 30 ms, Mix 30 %), "Reverse Swell" (Reverse, Length
  60 %, Mix 40 %), "Wide Send" (Mix 100 %, Width 150 %, Low cut 200 Hz).

## 4. Signal flow

```
in ─┬──────────────────────────────────────────────────────────────────────────► dry ─┐
    └─► pre-delay ─► convolver (head + 3 levels; old/new kernel crossfade) ─► low cut ─►     equal-power Mix ─► out
                                                       high cut ─► width ─► gain ─► wet ─┘
```

## 5. Tests (tests/ConvolutionReverbTests.cpp, tests/ConvolverTests.cpp)

* **Convolver against a direct reference**: random IRs of lengths 1, 63, 64, 65, 511, 512, 4096, 4097 and 20 000
  samples, mono and stereo, against a direct double-precision convolution of random input; maximum error below 1e−5
  relative to the signal's peak.
* **A unit impulse returns the IR**: Normalise off, the file at the device rate, Mix 100 %, filters out of the way:
  the output is the IR, sample for sample (within float rounding), starting at the impulse's sample.
* **No added latency**: the impulse's response starts at the impulse's sample, and `latencySamples()` is 0.
* **Block-size independence**: the same input in blocks of 1, 17, 64, 333 and 4096 samples gives bit-identical
  output.
* **Mix and bypass**: Mix 0 % gives the dry signal; Mix 100 % only the wet; bypassed, the output is the input.
* **Start, Length and Reverse**: the impulse response equals the trimmed, faded or reversed IR.
* **Decay**: an IR of exponentially decaying noise with T = 2 s, at Decay 50 %, measures T between 0.9 and 1.1 s;
  the decay estimate itself is within 5 % on synthetic IRs.
* **Pre-delay**: the response starts Pre-delay later; changing it while a sine plays gives no step larger than the
  signal's own sample-to-sample change.
* **Swap without a click**: a sine through IR A, then IR B is published mid-block; the output's sample-to-sample
  difference stays within a bound set by the steady states', and the tail of a burst played before the swap
  continues through B (energy after the fade above a bound) when the history is kept.
* **Resampling**: a 96 kHz IR (a decaying 1 kHz sine) at a 48 kHz device: the response has the same frequency and
  duration; a 44.1 kHz IR at 48 kHz likewise; no energy above 20 kHz from a 96 kHz IR holding a 30 kHz tone.
* **Normalise**: white noise through two IRs of very different levels comes out at the same RMS within 1 dB.
* **No allocation** in `process()` (`AllocationGuard`), including across a swap and a delay-line growth, in Debug
  (the guard is compiled out in Release).
* **Missing file and no file**: the wet path is silent and the dry passes.
* **Model and files**: `setNodeFile` pools the file (once per path), points at a pool file or none, and refuses other
  nodes and unknown files; saving and loading keeps `fileId` and the pool entry; the compiler hands the node its file,
  and a changed file without a recompile; the descriptor (effect, not allowed in voice graphs, which parameters
  modulate and automate); reset silences the tail. The reset-equals-fresh table (ModulationTests.cpp) is for voice
  graphs, which this node isn't allowed in.
* **UI** (`ui/src/dock/ConvolutionReverbPanel.test.ts`): the envelope-to-path helper, the trim and decay overlay maths
  (the same formula as I8), the time axis with pre-delay, and the decay readout.

## 6. Summary

| | Convolution Reverb |
|---|---|
| Node | `dawg.convolutionReverb`, effect, track graphs only |
| Engine | our own: a 64-tap direct head and three FFT levels (64, 512, 4096, the last spread over time), zero latency, bit-identical for any block sizes (I2) |
| IR | a pool file (`GraphNode::fileId`), imported like a Sampler's samples, or a generated default; mono, stereo or true stereo, up to 20 s (I3, I10) |
| Building | on a shared background thread: trim, reverse, decay, windowed-sinc resampling with Stretch, normalising, FFTs (I4) |
| Swap | prepared over one 4096-sample cycle, then a 4096-sample crossfade on the shared input history: the tail carries on (I6) |
| Controls | Pre-delay; Start, Length, Decay, Reverse, Stretch, Normalise (rebuild); Low and High cut, Width, Gain, Mix (real time) |
| Latency | none |
| Panel | the file's dB envelope with the trim and decay drawn over it, name, Load, drop target, decay time |

## 7. Owner's answers (2026-11-08)

1. **No IR:** (b), the generated default IR (I10).
2. **Normalise:** on by default (I5).
3. **True stereo:** yes, in v1 (I3).
4. **Stretch:** kept (I8).
5. **Swap timing:** not asked. Spreading level 3 (I2) added a cycle of preparation, so an edit is heard after
   85–170 ms and has faded in after 255 ms at 48 kHz (I6).

The questions as asked:

1. **A new node without an IR.** (a) It passes the dry signal and the panel asks for a file. (b) It starts with a
   built-in IR that DAWg generates (a stereo decaying noise tail of about 2 s with a gentle high-frequency roll-off,
   no file and no licence), so it reverberates at once; a loaded file replaces it. (c) We also generate and ship a
   handful of such IRs as files (room, hall, plate-like) for the sound explorer. Recommended: (b).
2. **Normalise default.** On (recommended: every IR at a usable level) or off (the file as it is).
3. **True stereo.** Support four-channel IRs (LL, LR, RL, RR) in v1, at twice the multiply-adds? It needs the IR read
   with all its channels, beside `AudioSource`, which reads two. Recommended: not in v1.
4. **Stretch.** Keep it (cheap, and a creative control), or leave it out to keep the panel smaller? Recommended: keep.
5. **Swap timing.** The crossfade starts at the next 4096-sample boundary and lasts 4096 samples, so an edit is heard
   within 85–170 ms at 48 kHz. Acceptable, or should edits be faster at the cost of a level-3 recomputation?
   Recommended: acceptable.

## 8. Implementation notes

* **Files.** `engine/include/dawg/Convolver.h` and `engine/src/Convolver.cpp`: the convolver, `History`, `Kernel`,
  `ProgramExchange`, the IR's preparation (`defaultImpulse`, `estimateDecay`, `analyse`, `resample`, `shapeImpulse`,
  `makeKernel`) and the `Builder` thread. The node is `ConvolutionReverbNode` in EffectNodes.cpp. `GraphNode::fileId`,
  `Node::setAudioFile`, `nodetype::usesAudioFile`, `ParamInfo::automatable`, `AudioSource::readAllChannels`, the
  `setNodeFile` command, and in the app `convolution.import`, `convolution.importDialog` and the export's wait
  (`waitForConvolutionBuilds`).
* **Compiler.** `TrackPlanner::withFiles` hands each Convolution Reverb its file when the track compiles;
  `publishSampleMaps` hands over a changed `fileId` without a recompile. A newly pooled file changes the pool, which
  recompiles as any pool change does.
* **Swaps.** The audio thread holds up to three programs in three slots: the current one, one preparing (level 3's
  first block, I6) and one fading out; a program that arrives meanwhile waits as the pending one (an older pending one
  is retired unseen). The very first program starts at once, from silence.
  A program built for another rate is silent. The node rebuilds when prepared at a new rate; prepared at the same
  rate (each export pass) it keeps its program and clears its history.
* **Filters at the ends of their ranges** are out of the path (I9), so the defaults leave the IR exactly as built; the
  unit-impulse tests depend on that.
* **Tests.** `tests/ConvolverTests.cpp` (the engine: a direct convolution for mono, stereo and true stereo at IR lengths
  around every partition boundary, a unit impulse, bit-identical output for block sizes 1, 17, 64, 333, 4096 and a mix,
  the swap's crossfade matching the two IRs' convolutions exactly and continuing B's tail, a history growth, the rate
  check, resampling, the decay estimate, shaping and the default IR) and `tests/ConvolutionReverbTests.cpp` (the node:
  descriptor flags, unit impulse and latency, Mix and bypass, the response equal to the shaped IR for Start, Length,
  Decay, Reverse and Stretch, pre-delay and its click-free move, swaps of shape and file without a click, a 96 kHz IR at
  48 kHz, Normalise, a missing file and the default IR, reset, no allocation, and the command, pooling, saving and
  compiler hand-over). `ui/src/dock/ConvolutionReverbPanel.test.ts` tests the panel's helpers in
  `ui/src/dock/convolution.ts`. The allocation test runs only in Debug builds, where the guard is compiled in.
* **Checked in the running app** (a separate dev instance, 2026-11-08): the node added to a track, a 96 kHz stereo file
  and a four-channel true-stereo file loaded with `convolution.import` and copied into the project's `Audio/` folder,
  the envelope, trim and decay drawn, the reverb time read out, and the card at dock heights of 140, 180, 220, 300 and
  520 px with card widths asked for of 300, 760 and 1200 px: no control over the graph or outside the body, and no
  scrolling. Not checked by hand: dragging a file from the sound explorer (the drop handler is the Sampler's), the file
  dialog, Locate for a missing file, and listening.
