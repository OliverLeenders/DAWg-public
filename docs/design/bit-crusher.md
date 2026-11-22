# Design: the Bit Crusher

Status: **implemented** (notes in section 8); the owner's answers are in section 7. The decisions are numbered B1–B14.

## 1. Goals

* A built-in lo-fi effect, `dawg.bitcrusher` ("Bit Crusher"), with the two classic sections in one node: a
  **bit-depth reducer** (a quantiser with fewer levels) and a **sample-rate reducer** (a sample-and-hold at a lower
  rate). Old samplers, 8-bit consoles, telephone and walkie-talkie sounds, and the harsh digital fuzz of a 1-bit signal.
* Both sections sweep smoothly: fractional bit depths and per-sample rate changes, so automating or modulating them
  never zippers or clicks.
* A rate that can follow the played note in a voice graph, so the aliasing it makes stays in tune.
* Exact timing: a fractional ratio (a rate of 19.2 kHz at 48 kHz holds 2, 3, 2, 3 ... samples) keeps its pattern over
  any length of audio, independent of the block size.
* With every section at its "off" end, the node passes its input through unchanged, bit for bit.
* Real-time safe, cheap, allowed in voice graphs.
* A panel that shows what the controls do: the staircase transfer curve and a held, quantised test wave.

Non-goals for v1:

* Anti-aliasing of the quantiser (ADAA, oversampling). The Saturator, Clipper and Waveshaper suppress aliasing because
  it is a fault there; here it is most of the sound. ADAA of a staircase would turn each step into a slope (the
  Waveshaper's doc rejects a staircase shape for this reason), and oversampling would remove the fold-back that
  defines a sample-rate reducer. The "clean" options are filters instead (B7).
* Emulations of particular hardware (a 12-bit sampler's converters, filters and companding). The controls here can get
  close; a device model is a different effect.
* μ-law or A-law companding (non-uniform steps). A possible later choice for the quantiser.
* Band-limited (BLEP) steps at fractional instants: question 4.

## 2. What it reuses

* **The effect plumbing** in EffectNodes.cpp: `EffectNode` for bypass, `followParam` for 20 ms ramps and per-sample
  modulation, `dbToGain`, `differs`, and the `Biquad` with its matched low-pass (`setPass`) for the filters (B7).
* **The DC blocker** of the Saturator and Waveshaper (a one-pole high-pass at 5 Hz), for the DC that bias, mid-rise
  quantising and asymmetric clipping leave (B10).
* **Pitch tracking** as the Oscillator does it: a semitone parameter spanning −120..120 follows the Note Expression's
  Pitch output exactly at 100 % depth (`pitchSignalRange`, phase12-voice-scope.md, V3).
* **The panels**: `Knob`, `Section`, the segmented buttons, `useNodeDrag`, the card's measured min-content width
  (`panelMinWidth.ts`), presets in `presets.ts`, widths in `deviceWidths.ts`.

## 3. Decisions

**(B1) One node with both sections.** `dawg.bitcrusher`, "Bit Crusher", `NodeCategory::effect`, in EffectNodes.cpp
after the Waveshaper: one stereo audio input with modulation inputs, one stereo output, bypass through `EffectNode`.
One node rather than a "Bit Crusher" and a "Downsampler" (question 1), because:

* The two sections are almost always used together, and every crusher users know has both.
* Each section has an "off" end (B3, B5), so either alone is one knob turn away; a second node would add nothing a
  turned-off section doesn't.
* They share Drive, Mix, Output, the DC blocker and one panel, and the order between them matters (B2), which one node
  fixes and two nodes would leave to the user to get wrong.

**(B2) Signal flow.** Like a converter: the hold samples the signal, then the quantiser measures it.

```
in ─► × drive ─► clip (B9) ─► AA filter? (B7) ─► sample-and-hold (B5, B6) ─► + dither? ─► quantise (B3, B4)
   ─► smooth filter? (B7) ─► DC blocker (B10) ─► mix with dry (B11) ─► × output ─► out
```

Quantising after the hold means the quantiser (and the dither) runs once per held sample, so dither noise is held too,
as in a real converter at that rate, and the held value doesn't flicker between two levels.

**(B3) Bit depth: continuous, with "off" at the top.** `bits`, 1..24, default 8, a continuous parameter (question 2).
With `M = 2^(bits − 1)` levels per unit (a real number for fractional bits), the step is `Δ = 1 / M`, so 8 bits steps
by 1/128 and 1 bit by 1. A fractional depth lies between its neighbours: 4.5 bits has a step between 4 bits' and 5
bits', and moving the knob slides the step size smoothly instead of jumping between powers of two. This is what makes
the depth automatable and modulatable without zipper noise (B13).

At exactly 24 bits the quantiser is off: the signal passes unquantised (24-bit steps are at −144 dB anyway). That is
what lets the node be transparent (B12).

The panel shows the depth with one decimal, and the number of levels beside it. A "Stepped" option (whole bits only)
isn't a separate mode: the knob can snap with a modifier, and presets use whole bits.

**(B4) The quantiser: mid-tread or mid-rise.** `quantiser`, a choice, default Mid-tread (question 3):

* **Mid-tread**: levels at `k Δ`, with 0 among them: `y = clamp(round(x M), −K, K) / M`, `K = floor(M)`. Silence stays
  silent, quiet signals fall into the zero step and drop out (the gated, breaking-up sound of low bit depths). At
  whole bits it has `2^bits + 1` levels from −1 to 1, symmetric: 1 bit gives −1, 0 and 1. (A two's-complement
  converter has `2^bits` levels and one more step below zero than above; symmetric is the better-sounding choice and
  the one most crushers make.)
* **Mid-rise**: levels at `(k + ½) Δ`, with a boundary at 0: `y = (floor(x M) + ½) / M`, clamped to the levels inside
  ±1. Never silent: a quiet signal becomes a square wave of ±Δ/2 around zero, the buzzy sound of 1 to 3 bits; 1 bit
  is ±½, a pure sign function. At whole bits it has exactly `2^bits` levels.

**(B5) Rate reduction: a sample-and-hold at a target rate in Hz.** `rate`, 20 Hz..192 kHz, logarithmic (middle
2 kHz), default 11 kHz: the rate at which the hold takes a new sample. A rate in Hz rather than a ratio (question 5),
because a ratio sounds different at 44.1 and 48 kHz and the rate in Hz is what users think in ("11 kHz",
"telephone"). The panel shows the ratio to the session rate beside it ("÷ 4.36").

A **phase accumulator** in double runs at `rate / sampleRate` per sample; when it passes 1 it wraps (subtracting 1,
not resetting, so the fractional part carries over) and the hold takes the current input sample. With the rate at or
above the session's sample rate, the hold takes every sample: off, transparent. The top of the knob (192 kHz) is above
every supported sample rate and shows "Full".

**(B6) Fractional rates: exact over long runs, without jitter.** The phase accumulator makes fractional ratios work:
at a ratio of 2.5, holds of 2 and 3 samples alternate, and a ratio of 4.36 gives 4s and 5s in the proportion that
averages 4.36. Three properties, all tested:

* **No drift**: after N samples the hold has taken `floor(N · rate / sampleRate)` (± 1) samples, for N in the tens of
  millions. The accumulator holds only the fractional phase (0..1) in double, so its rounding error stays near 1e−16
  per step and never builds up into a missed or extra hold.
* **No jitter** in the sense of randomness: every hold is `floor(ratio)` or `ceil(ratio)` samples long, in a fixed
  periodic pattern. (Deliberate jitter is B8.)
* **Block-size independence**: the phase carries across blocks; the pattern doesn't depend on where blocks start.

The steps happen on whole samples: a fractional ratio's step times are rounded to the sample grid, which adds the
pattern's own sidebands (the classic sound of a 4.36 ratio). Placing steps at their exact fractional instants with
band-limited corrections is question 4.

**(B7) Clean or lo-fi: two optional filters.** Both off by default (lo-fi):

* **AA** (`antiAlias`, toggle): a low-pass before the hold at 0.45 × the rate, 24 dB/octave (two matched 12 dB/octave
  biquads, Butterworth). The hold then folds back much less: a clean sample-rate conversion's loss of highs instead of
  metallic inharmonic aliasing.
* **Smooth** (`smooth`, toggle): the same low-pass after the quantiser, a reconstruction filter. It rounds the
  staircase's corners off, removing the images above half the rate (and the quantiser's harsh high harmonics with
  them): the dull, warm sound of an old sampler's output stage.

Both together are the "clean" character (a band-limited low-rate resampler); neither is the hard digital "lo-fi" one.
The filters follow the rate: their coefficients are recomputed when the smoothed rate moves, every 32 samples at most,
from the current value (cheap; at the 20 ms ramp the steps between updates are inaudible). At the full rate (the
rate at or above the sample rate) the filters are off, and they start from silence when they come back on. The filters
add no latency.

**(B8) Jitter.** `jitter`, 0..100 %, default 0: each hold's length is varied randomly, by up to ±½ of the period at
100 %, with a uniform random offset added to the accumulator's increment at each wrap. It smears the aliasing into
noise, like a cheap converter's unstable clock. The random generator is the engine's deterministic one (a fixed seed,
restored by `reset()`), so renders are repeatable and reset equals fresh. No slow "drift" control: an LFO on the rate
does that, and does it better.

**(B9) Drive and clipping.** `drive`, −24..+24 dB, default 0: gain before everything, so quiet material can use more
of the levels (the quantiser's steps are fixed relative to full scale) and loud material hits the quantiser's limits.
`clip`, a choice, Hard or Soft, default Hard: Hard is the quantiser's own limit at ±1 (a converter's); Soft puts a
`tanh` before the hold, so overdriving rounds off before it crunches (question 6). No anti-aliasing on the clip: the
quantiser after it aliases far more.

**(B10) Bias and DC.** `bias`, −100..100 %, default 0: an offset before the quantiser, in units of half a step
(so its meaning doesn't change with the depth): at 100 % a mid-tread quantiser behaves like a mid-rise one for small
signals, and in between quiet signals break up asymmetrically, a gritty, more "broken" sound. A DC blocker at 5 Hz
after the quantiser (and the smooth filter) removes the offset this leaves, and the DC of mid-rise quantising and
asymmetric clipping, so the node never outputs DC. It is off when the quantiser is off, so transparency holds.

**(B11) Mix and Output.** `mix`, 0..100 %, default 100; `output`, −24..+24 dB, default 0. The dry signal is the node's
input, undelayed: nothing in the wet path has latency (B7's filters are minimum-phase IIR filters, so mixing them with
the dry signal combs a little at partial Mix, as any filter's would, which is acceptable for an effect used mostly at
100 %).

**(B12) Transparency.** With `bits` at 24, `rate` at or above the sample rate, `drive` 0 dB, `clip` Hard, both filters
off, `bias` and `jitter` 0 and Mix 100 %, the output equals the input bit for bit: each stage skips itself at its off
setting (the quantiser and its clamp, the DC blocker, the hold, the gains when exactly 0 dB). Hard clip at ±1 belongs to
the quantiser, so with the quantiser off nothing clips. A test checks this on a full-scale-plus signal.

**(B13) Smoothing.** Continuous parameters follow `followParam`: 20 ms ramps when set or automated, the per-sample
modulation smoothing when modulated. `bits` is smoothed in bits, so a sweep changes the step size exponentially, as
the ear hears it. `rate` is smoothed in **semitones** (its logarithm), so a sweep is even in pitch; per sample its
phase increment follows. `drive`, `output`, `mix`, `bias` and `jitter` ramp in their own units. The choices and toggles
are steps: switching the quantiser or turning a filter on can click once, as on the Waveshaper's Symmetric, which is
acceptable for controls that aren't automated in practice (the filters' state is cleared when they are turned on, so
they don't start with stale state).

**(B14) Note-tracking rate, voice graphs.** `rateMode`, a choice, Hz or Note, default Hz (question 7):

* **Hz**: the rate is `rate`.
* **Note**: the rate is a pitch, `note`, −120..+120 semitones relative to C4 (261.63 Hz), default +24 (C6, 1046.5 Hz),
  the Oscillator's convention: modulating `note` from a Note Expression's Pitch at 100 % makes the rate follow the
  played note exactly, so in a voice graph each voice's hold runs at a multiple of its own pitch and the aliasing
  lands on the note's harmonics: tuned, chiptune-like crunch rather than inharmonic noise. Outside a voice graph the
  knob tunes it by hand.

The node is allowed in voice graphs (a phase, a few multiplies, four biquads when filtered). `reset()` puts the phase
at 1 (so the first sample is taken at once), clears the held values, filters and DC blocker and reseeds the jitter, so
a reset node and a fresh one behave alike. No latency.

### Parameters

| # | id | name | range | default | unit | notes |
|---|----|------|-------|---------|------|-------|
| 0 | `bits` | Bits | 1..24 | 8 | bits | continuous; 24 is off |
| 1 | `quantiser` | Quantiser | Mid-tread, Mid-rise | Mid-tread | | |
| 2 | `rateMode` | Rate mode | Hz, Note | Hz | | |
| 3 | `rate` | Rate | 20..192000 | 11000 | Hz | log, mid 2000; ≥ sample rate is off ("Full") |
| 4 | `note` | Note | −120..120 | 24 | st | from C4; Pitch-trackable |
| 5 | `jitter` | Jitter | 0..100 | 0 | % | |
| 6 | `antiAlias` | AA filter | off, on | off | | |
| 7 | `smooth` | Smooth | off, on | off | | |
| 8 | `dither` | Dither | Off, TPDF | Off | | question 3 |
| 9 | `drive` | Drive | −24..24 | 0 | dB | |
| 10 | `clip` | Clip | Hard, Soft | Hard | | |
| 11 | `bias` | Bias | −100..100 | 0 | % | of half a step |
| 12 | `mix` | Mix | 0..100 | 100 | % | |
| 13 | `output` | Output | −24..24 | 0 | dB | |

**Dither** (B4, question 3): TPDF, the sum of two uniform randoms of ±½ step, added before the quantiser. It turns the
quantiser's distortion of quiet signals into steady noise: at low depths a "cleaner", hissier crush. No noise shaping
in v1: shaping pushes noise up where the reduced rate's images already are, and its purpose (hiding noise in a
high-resolution signal) doesn't apply at 4 to 8 bits.

## 4. The panel

`ui/src/dock/BitCrusherPanel.tsx`, the Waveshaper's and Gate's layout:

* **The graph**, two views side by side when wide, one above the other when tall, the scope alone when narrow:
  * **Transfer**: input −1..1 on x, output on y, the quantiser's staircase (with drive, clip and bias) over the
    identity's faint diagonal. At 24 bits it is the diagonal.
  * **Scope**: one cycle of a test sine (a fixed fraction of the rate, so the steps are visible at any rate), the
    dry wave faint, the held and quantised wave in the accent colour, the smooth filter's effect not drawn (it would
    need the engine's response; the label says "Smooth on").
  Both are computed in TypeScript from the same formulas as the engine (`bitCrusherCurves.ts`, tested against the
  engine's numbers like `dynamicsCurves.ts`), so the graph redraws as knobs move without a round trip.
* **Sections**: Bits (Bits, Quantiser segmented, Dither, Bias), Rate (Hz/Note segmented, Rate or Note, Jitter, AA and
  Smooth buttons), Output (Drive, Clip segmented, Mix, Output).
* **Readouts**: the levels count ("8.0 bits · 257 levels") and the ratio ("11.0 kHz · ÷ 4.36").
* **Layout**: the card's min-content width is reported (`panelMinWidth.ts`), the graph has a floor height, and no
  control ever covers the graph at any dock height or card width; `deviceWidths.ts` gets a measured width.
* **Presets** (`presets.ts`): "Default"; "8-bit" (8 bits, 22 kHz); "Console" (4 bits, Mid-rise, 16 kHz, Note off);
  "Telephone" (12 bits, 8 kHz, AA and Smooth); "Old sampler" (12 bits, 26 kHz, Smooth); "1-bit fuzz" (1 bit,
  Mid-rise, Full rate, Drive +12 dB, Soft); "Broken" (5 bits, 6 kHz, jitter 60 %, bias 50 %); "Tuned" (Note mode,
  +24 st, 6 bits).

## 5. Tests (tests/BitCrusherTests.cpp)

* **Descriptor**: type, ports, parameter order and defaults, allowed in voice graphs, no latency.
* **Step count**: a slow full-scale ramp through the node (rate Full) gives `2^bits + 1` distinct levels mid-tread and
  `2^bits` mid-rise at 1, 2, 4 and 8 bits, spaced exactly `1 / 2^(bits − 1)`; at fractional bits the count lies between
  its neighbours' and the spacing is `2^(1 − bits)`.
* **Hold length**: at integer ratios (rate = sampleRate / 4) every hold is 4 samples; at 2.5, holds alternate 2 and 3;
  over 20 million samples at a fractional ratio the number of holds is `floor(N / ratio)` ± 1 and every hold is
  `floor` or `ceil` of the ratio long.
* **Transparency** (B12): default-off settings pass a noisy signal with peaks above 1 through bit for bit.
* **Stereo**: different signals in the two channels stay separate and use the same hold instants.
* **Note mode**: `note` = 0 holds at 261.63 Hz; `note` = 12 at twice that.
* **Jitter**: on, hold lengths vary within ±½ period; the same settings render the same output twice (seeded).
* **DC**: mid-rise on silence gives no DC after the blocker settles.
* **Block-size independence**: blocks of 1, 17, 64, 333 and 4096 samples give bit-identical output, with jitter, both
  filters and a moving rate.
* **No allocation** in `process()` (`AllocationGuard`), in Debug.
* **Reset equals fresh**, through the voice-graph table in ModulationTests.cpp.
* **UI**: the curve formulas against the engine's quantiser (`bitCrusherCurves.test.ts`) and the presets' values in
  range (presets.test.ts).

## 6. Summary

| | |
|---|---|
| Node | `dawg.bitcrusher`, one node, both sections |
| Bits | 1..24 continuous, 24 = off; mid-tread or mid-rise; TPDF dither optional |
| Rate | 20 Hz..192 kHz (≥ sample rate = off), or a note from C4 for key tracking |
| Hold | double phase accumulator, exact fractional patterns, steps on whole samples |
| Clean vs lo-fi | optional AA pre-filter and Smooth post-filter, 24 dB/oct at 0.45 × rate |
| Extras | Jitter, Drive with Hard/Soft clip, Bias in half-steps, DC blocker, Mix, Output |
| Anti-aliasing | none on the quantiser (the aliasing is the effect) |
| Voice graphs | yes; no latency |

## 7. Owner's answers (2026-11-08)

1. **One node** with both sections (B1).
2. **Continuous** bit depth (B3); **both quantisers** and **TPDF dither**, no noise shaping (B4, question 3).
3. **Steps on whole samples** (B6); no BLEP in v1.
4. **Hz with a Note mode**, and the **AA and Smooth toggles** (B5, B7, B14; questions 5, 7 and 8).

Question 6 (Drive, Clip and Bias) wasn't asked separately; the recommendation (keep them) stands.

The questions as asked:


1. **One node or two?** One "Bit Crusher" with both sections (B1), or a "Bit Crusher" and a separate "Downsampler"?
   Recommended: one node.
2. **Bit depth**: continuous with fractional bits (B3), so sweeps and modulation are smooth; or stepped, whole bits
   only? Recommended: continuous.
3. **Quantiser and dither**: both mid-tread (default) and mid-rise as a choice (B4), plus TPDF dither as an option and
   no noise shaping? Or fewer choices (mid-tread only, no dither)? Recommended: both quantisers, TPDF, no shaping.
4. **Step timing at fractional ratios**: steps on whole samples (B6; cheap, the classic sound, the pattern's sidebands
   included); or steps at their exact fractional instants with band-limited (polyBLEP) corrections and the input read
   by interpolation at that instant (cleaner, a sample of latency, more code)? Recommended: whole samples in v1.
5. **Rate units**: a rate in Hz (B5), shown with its ratio; or a ratio (÷ 1..÷ 64) that depends on the session rate?
   Recommended: Hz.
6. **Drive, clip and bias**: Drive with a Hard/Soft clip choice and a Bias in half-steps (B9, B10)? Or leave them out
   for a smaller node (Mix and Output only)? Recommended: keep them, they are cheap and make the low depths usable.
7. **Note tracking**: a Hz/Note rate mode with a Pitch-trackable `note` parameter (B14)? Or Hz only, with key tracking
   left to modulating the rate? (Modulation adds to a parameter linearly, so tracking a Hz rate from Pitch wouldn't be
   in tune.) Recommended: the Note mode.
8. **Filters**: the AA and Smooth toggles at a fixed 0.45 × rate and 24 dB/octave (B7); or a single "Clean / Lo-fi"
   character switch; or a Smooth with its own cutoff knob? Recommended: the two toggles.

## 8. Implementation notes

* **Files.** `BitCrusherNode` in EffectNodes.cpp after the Waveshaper; `bitcrusher::quantise` and
  `bitcrusher::noteRate` (BuiltinNodes.h) for the tests; `nodetype::bitcrusher`. Tests in tests/BitCrusherTests.cpp,
  and the node twice in the reset-equals-fresh table (ModulationTests.cpp), in Hz and Note modes.
* **The phase's tolerance.** A whole ratio's increment is often inexact in binary (4800 Hz at 48 kHz is 0.1, and ten
  of them add up to just under 1), which made such a hold last one sample longer now and then. The hold takes a sample
  once the phase is within 1e−9 of 1; a test checks ratios 3, 4, 7 and 10.
* **Exact increments.** The rate ramps in its logarithm; once the ramp has settled, the increment is the parameter's
  own value over the sample rate rather than `exp2` of the logarithm, so a settled whole ratio is exact.
* **Quantising at the hold.** The quantiser, the bias and the dither run once per held sample (B2), with the depth at
  that sample; between holds the output is the held level through the Smooth filter and the DC blocker.
* **The DC blocker** always runs, but only a quantising node (Bits below 24) uses its output, so the node is bit-exact
  transparent at the off settings and the blocker carries on without a jump when the quantiser comes back.
* **Dither and jitter** share one `juce::Random`, seeded in `reset()`: renders repeat, and blocks don't matter, since
  the draws happen in sample order.
* **The panel** is `BitCrusherPanel.tsx`, with its curves in `bitCrusher.ts` (tested against the engine's cases in
  `bitCrusher.test.ts`). Rate mode, AA and Smooth sit above the graph, on one row; the Quantiser and the Clip are
  menus in knob-sized slots (`ChoiceSlot`), and Dither a toggle in the Bits section (`SlotToggle`). The sections are a two-row grid beside the graph, one row in a short dock, as
  the modulation effects'.
