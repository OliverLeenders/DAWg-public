# Design: the Clipper

Status: **implemented** (notes in section 8). The owner's answers are in section 7. The decisions are numbered C1–C10.

## 1. Goals

* A built-in clipper (`dawg.clipper`): peaks above a ceiling are cut off, hard or with a rounded knee, for drums,
  bass, buses and the master. It is the missing core effect next to the Saturator, the Compressor and the Limiter.
* Low aliasing: first-order antiderivative anti-aliasing (ADAA) as in the Saturator, optionally on top of 2x or 4x
  oversampling.
* A ceiling that is a promise: at Mix 100 % and Output 0 dB no output sample exceeds it, with or without oversampling.
* Usable per voice in voice graphs, like every built-in effect.

Non-goals for v1:

* Inter-sample (true-peak) safety. The Clipper guarantees sample peaks at the ceiling; peaks between samples after
  reconstruction can still rise a little above it, and a true-peak limiter after it is the tool for that.
* Asymmetric clipping or a bias. The Saturator has the bias; a clipper is symmetric, so it adds no DC (C7).
* Second-order ADAA, multiband clipping, a clip-only "delta" listen.

## 2. What it reuses from the Saturator

The Clipper is the Saturator's structure with another curve (docs/design/saturator.md). It keeps:

* first-order ADAA in double, with the same ill-conditioned fallback (W4);
* the dry path through `(x + x₁) / 2`, so Mix never combs against ADAA's half-sample delay (W5);
* `juce::dsp::Oversampling` with half-band equiripple FIR filters and integer latency, Off / 2x / 4x, reported through
  `latencySamples()`, and `ParamInfo::changesLatency` so changing it recompiles the track (W7);
* the parameter smoothing and modulation of W8 (20 ms ramps, per-sample modulation smoothing);
* a static transfer curve from `effectResponse` for the panel (W9).

What differs: the curve and its antiderivative (C3, C4), the ceiling guarantee (C5), no bias and no DC blocker (C7),
and a clipping meter (C9).

## 3. Decisions

**(C1) A built-in effect node.** `dawg.clipper`, "Clipper", `NodeCategory::effect`, in EffectNodes.cpp after the
Saturator: one stereo audio input with modulation inputs, one stereo output, bypass through `EffectNode`. Built-in
effects are allowed in voice graphs (`nodetype::allowedInVoiceGraph`), so it can clip each voice.

**(C2) Parameters.**

| # | id | name | range | default | unit | modulatable |
|---|----|------|-------|---------|------|-------------|
| 0 | `drive` | Drive | −12..24 | 0 | dB | yes |
| 1 | `ceiling` | Ceiling | −24..0 | 0 | dB | yes |
| 2 | `knee` | Knee | 0..100 | 0 | % | yes |
| 3 | `output` | Output | −24..24 | 0 | dB | yes |
| 4 | `mix` | Mix | 0..100 | 100 | % | yes |
| 5 | `oversampling` | Oversampling | Off, 2x, 4x | 2x | | no (W7) |

Drive is the input gain into the clipper; Ceiling is the level it clips at, and so the output peak. Pushing Drive
into a fixed Ceiling is how a mastering clipper is used: the peak stays put and the loudness rises. Output is a plain
gain after everything, for gain matching; the ceiling guarantee is stated at 0 dB Output (C5). There is no link
between Output and Ceiling: with Drive there, a linked Output would only be a second way to do the same thing.

**(C3) The curve: a hard clip with a quadratic knee.** With ceiling `c` (linear) and knee `k` (0..1), the knee's
half-width is `w = k · c`, and with `t = c − w` and `a = |x|`:

```
h(a) = a                              a ≤ t          (untouched)
h(a) = a − (a − t)² / (4w)            t < a < c + w  (the knee)
h(a) = c                              a ≥ c + w      (clipped)
f(x) = sign(x) · h(|x|)
```

At `a = c + w` the knee reaches exactly `c` with slope 0, and at `a = t` it leaves the line with slope 1, so the curve
is smooth (C1) and never exceeds `c`. Knee 0 is the hard clip. Knee 100 % bends from the origin and reaches the
ceiling at twice its level. Below `t`, at any knee, the signal passes untouched, which is what a clipper needs on a
master: peaks are shaped, the body of the signal isn't.

Candidates weighed, all with closed-form antiderivatives:

| curve | below the knee | reaches c | for a clipper |
|-------|---------------|-----------|---------------|
| quadratic knee (chosen) | exactly linear up to t | at c + w | one knob from hard to soft, curve stays under the hard clip |
| cubic x − (4/27) x³ / c² | bends from 0 | at 1.5 c | smooth (C∞ inside), but colours quiet signals; no hard setting |
| sine c · sin(x / c) | bends from 0 | at (π/2) c | like the cubic, with cos in its antiderivative |
| x / (1 + \|x\|) | bends from 0 | never | a saturator, not a clipper: no hard ceiling |
| variable knee between hard and cubic | linear to t | at c + w | what the quadratic knee already is, with a costlier formula |

The quadratic knee is recommended alone: one Knee control covers hard to soft, the untouched region matches what a
clipper is for, and its antiderivative is a cubic polynomial. The cubic is the best second choice if a Shape menu is
wanted later (it is the classic "soft clip" sound); see question 1.

*Since docs/design/vicanek-adaa.md, the shaper uses transparent ADAA instead: the curve and antiderivative below are unchanged, and the means are taken over
half-intervals with a one-pole filter before and after (V1–V5).*

**(C4) ADAA.** The antiderivative of `f` is even, `F(x) = H(|x|)`:

```
H(a) = a² / 2                                     a ≤ t
H(a) = a² / 2 − (a − t)³ / (12w)                  t < a < c + w
H(a) = H₁ + c · (a − c − w)                       a ≥ c + w,  H₁ = (c + w)² / 2 − 2w² / 3
```

With `w = 0` this is `x² / 2` inside and `c · |x| − c² / 2` outside, the hard clip's antiderivative. Per channel the
node keeps the previous shaper input `u₁ = g · x₁` and its `F`, and outputs, in double as in the Saturator,

```
(F(u) − F(u₁)) / (u − u₁)      when |u − u₁| ≥ 1e−5
f((u + u₁) / 2)                otherwise
```

The quotient is the mean of `f` over the segment from `u₁` to `u`, so it lies within `±c` up to rounding; the node
clamps it to `±c` to remove the rounding. When drive, ceiling or knee move, the stored `F(u₁)` was computed with the
old curve. The node therefore computes `F(u₁)` afresh with the current sample's curve (one extra `H` per sample),
so a moving ceiling never makes the quotient jump. That is a cost the Saturator doesn't pay, because its curve
doesn't change shape, only its input scaling.

Small signals see `(u + u₁) / 2`, ADAA's half-sample delay and gentle low-pass (the table in W7). As in the Saturator
the dry path matches it (C6) and the half-sample delay at the oversampled rate is not compensated; the oversampling's
whole-sample latency is reported.

The drive gain `g` multiplies the input before the curve and is not divided out afterwards: Drive is the input gain,
and the ceiling is in output terms.

*Since docs/design/vicanek-adaa.md, the shaper uses transparent ADAA instead: its output filter overshoots
the ceiling a little at the shaper's rate, so the peak guard runs at every oversampling choice, Off included, and
Off now has the guard's 1 ms of latency (V9).*

**(C5) The ceiling holds after oversampling: a peak guard, then a clamp.** At the shaper's rate ADAA keeps every
sample within `±c` (C4): each output is a mean of the curve, and no mean of values within `±c` leaves `±c`. The
downsampling filter has negative taps, so it rings on the clipped edges and the downsampled signal overshoots. Measured
without any correction, the overshoot reaches 4.5 dB at 4x on noise driven 24 dB into the ceiling, and less on gentle
material.

A clamp after downsampling, as first planned, keeps the ceiling but clips the ringing at the base rate, and that folds
back almost as much as no oversampling at all (section 8: 2x −24.9 dB and 4x −21.0 dB, against −24.0 dB for ADAA
alone). So with oversampling on, the downsampled signal first goes through a **peak guard**: a 1 ms lookahead gain
stage, linked across the channels. It is the Limiter's core (the moving average over the window of the sliding minimum
over the window of the gain each sample needs, `LookaheadGain`, shared with the Limiter), with the gain each sample
needs being `c / |y|` where it is above the ceiling. That gain reaches the needed value by the time the sample leaves
the delay, and it moves over two windows. A slowly moving gain adds sidebands close to the harmonics but folds nothing
back: 2x gives −36.4 dB and 4x −53.6 dB, close to the uncorrected −36.5 and −70.5 dB. Longer lookahead helps 4x a
little (2 ms −54.5 dB, 5 ms −58.0 dB) and isn't worth the latency. The guard adds `round(0.001 · rate)` samples to the
reported latency when oversampling is on (48 at 48 kHz). With oversampling Off there is no guard and no extra latency.

How others do it, for the record: KClip clamps at the session rate after oversampling and warns about the aliasing
that causes; StandardCLIP accepts peaks above its clip level; FabFilter's Pro-L 2 follows its fast stage with a second
stage that attenuates the overshoot smoothly, which is what the guard does. Correcting each peak with a band-limited
pulse (peak cancellation, as in radio transmitters) was tried and measured: it folds back like the clamp (2x −23.9 dB,
4x −20.3 dB), because any fast correction of a 7 kHz tone needs harmonics above Nyquist.

Last, the signal is clamped to `±c`: it catches float rounding behind the guard and, at Mix below 100 % with
oversampling Off, the dry signal's peaks, without ADAA. So the ceiling holds at any Mix, as the owner chose
(question 2). Output, after the clamp, scales the ceiling with it: the peak is `ceiling + output` in dB.

*Since docs/design/vicanek-adaa.md, the shaper uses transparent ADAA instead: the dry signal goes through the shaped path's linear chain
(V7).*

**(C6) Mix.** As in the Saturator: the dry signal goes through `(x + x₁) / 2` at the shaper's rate, and Mix crossfades
dry and wet before downsampling, so for signals below the knee dry and wet are identical and Mix never combs. The dry
signal is the input before Drive, so Mix 0 % is the input passed through, not the driven input.

**(C7) DC.** The curve is odd, so it rectifies nothing and needs no DC blocker. A DC offset at the input is treated as
signal: it is driven and clipped like any other level (a constant input `x` comes out as `f(g · x)`), and silence
stays silence.

**(C8) Smoothing and modulation as in the Saturator (W8).** Drive, Ceiling, Knee, Output and Mix ramp over 20 ms when
set or automated and follow modulation per sample. `g`, `c` and `w` are computed per sample from the smoothed values
at the base rate and held across the oversampled samples of one base sample. Oversampling is chosen in `prepare()` and
`configure()` and taken over by the audio thread at its next block, with the history and oversamplers cleared,
exactly as the Saturator does.

**(C9) A clipping meter.** `meters()` returns `{ over }`: how far the driven input's peak went above the ceiling in dB,
0 when nothing clipped. The audio thread keeps a peak with a release of 20 dB per second (so a single clipped sample
stays visible for at least a panel frame) in an atomic, as the Limiter keeps its reduction. Measured on the driven
input at the shaper's rate, before ADAA, so it reads the same at every oversampling choice.

**(C10) Panel.** Like the Saturator's: the static transfer curve, output over input with the input axis −1..1 (before
Drive, so Drive moves the knee inwards) and the output axis −1..1, the ceiling drawn as dashed lines at
`±ceiling · output`, and a diagonal for reference. Beside the curve, a vertical meter of `over` (0 to 12 dB) with a
readout ("Clip 2.4 dB", or "—" when nothing clips) that lights in the accent colour while clipping. Oversampling is a
segmented control above, as on the Saturator; Drive, Ceiling and Knee are the Clip section's knobs, Output and Mix the
Output section's. `effectResponse` gets `dawg.clipper` with 201 points. A dot marks the input level on the curve, as
on the Compressor and the Saturator (`input` in the meters, beside `over`).

## 4. Presets

Factory presets in `ui/src/dock/presets.ts`, as the Saturator has: "Transparent Peaks" (drive 3, ceiling −0.3, knee 0,
4x), "Drum Bus Snap" (drive 6, ceiling −1, knee 20, 2x), "Soft Clip Warmth" (drive 6, ceiling −3, knee 100, 2x),
"Master Clipper" (drive 2, ceiling −0.1, knee 10, 4x), "Per-voice Grit" (drive 12, ceiling −6, knee 40, Off).

## 5. Tests (tests/ClipperTests.cpp)

* ADAA matches the analytic result on ramps: a linear ramp from −3 to 3 through a hard clip and a knee of 50 %, with
  oversampling Off; each output equals `(F(u) − F(u₁)) / (u − u₁)` worked out independently, and on the flat parts
  it is the ceiling exactly.
* Aliasing: a 7 kHz sine at 0 dBFS with Drive 18 dB at 48 kHz, hard and with a knee; the energy outside the
  harmonics is clearly lower than a naive clip's with oversampling Off, and lower again at 2x and 4x (thresholds from a
  first measurement, recorded in section 8).
* The ceiling: sines, noise and square waves driven 24 dB into ceilings of 0, −6 and −18 dB, at every oversampling
  choice, Mix of 100 % and 50 % and knees of 0 and 60 %: no output sample above the ceiling (in float). A separate
  test shows the downsampling filter overshooting a clipped sine, so the guard is needed.
* DC and silence: silence gives exact silence; a constant input gives `f(g · x)` once settled.
* Mix and bypass: Mix 0 % equals the dry path `(x + x₁) / 2`; bypassed, output equals input with zero latency.
* Small signals below the knee pass at unity gain at every Drive (with Drive's gain), through ADAA's `(x + x₁) / 2`.
* No clicks: Ceiling and Knee swept over their range in one block with a loud sine; the sample-to-sample difference
  stays below a bound set by the input's.
* No allocation in `process()` (`AllocationGuard`), at every oversampling choice, with modulation.
* Latency: `latencySamples()` matches the measured delay of an impulse at Off, 2x and 4x.
* Meter: `over` reads the drive above the ceiling for a full-scale sine, and 0 for a quiet one.
* Robustness: ±1000 and denormal inputs give finite output; `reset()` clears the history; instances keep their own
  state.
* The registry test (NodeGraphTests.cpp), the reset-equals-fresh table (ModulationTests.cpp) and the panel curve.
* UI (ui/src/dock/ClipperPanel.test.ts): the meter's scaling and readout helpers.

## 6. Summary

| | Clipper |
|---|---|
| Node | `dawg.clipper`, effect, track and voice graphs |
| Curve | hard clip with a quadratic knee, 0 % hard to 100 % soft (C3) |
| Anti-aliasing | first-order ADAA in double, closed-form antiderivative (C4), plus Off / 2x / 4x oversampling |
| Ceiling | no sample above it at Output 0 dB, any Mix, any oversampling: a 1 ms peak guard after downsampling, then a clamp (C5) |
| Latency | the oversampler's plus 1 ms of guard when oversampled, reported and compensated; ADAA's half sample not compensated |
| DC | none added, no DC blocker (C7) |
| Panel | transfer curve with the ceiling, a clipping meter (C9, C10) |

## 7. Owner's answers (2026-11-04)

1. **Shape:** one Knee control, the quadratic knee of C3; no Shape menu.
2. **Ceiling with Mix:** the final clamp acts on the mixed signal, so the ceiling holds at any Mix (C5).
3. **Gain staging:** Drive, Ceiling and a free Output, no link (C2).
4. **Default oversampling:** 2x, as on the Saturator.
5. **The clamp's aliasing (after measuring, 2026-11-04):** the owner asked for other fixes to be researched before
   falling back to a "Strict ceiling" toggle. The peak guard of C5 keeps the ceiling and most of the oversampling's
   benefit, so there is no toggle.

## 8. Implementation notes

* Node (EffectNodes.cpp, `ClipperNode`): parameters as in C2, the curve and antiderivative of C3 and C4. The
  antiderivative's knee branch is skipped when `w` is 0, so a hard clip never divides by zero. `LookaheadGain` is the
  Limiter's former core, moved out unchanged; the Limiter now uses it too, and its tests pass as before. The
  parameter-following loop the Saturator had is a shared `followParam`, and the two nodes share the oversampling
  choices.
* The previous input's antiderivative is computed again with the current sample's curve (C4), so the state is only
  the previous `u` and `x` per channel.
* Measured aliasing (the test's 7 kHz sine at 0 dBFS, Drive 18 dB, ceiling 0 dB, at 48 kHz; power outside the
  harmonics relative to the fundamental):

  | | hard clip | knee 50 % |
  |---|---|---|
  | naive | −11.7 dB | −12.0 dB |
  | ADAA, Off | −24.0 dB | −24.3 dB |
  | ADAA, 2x, guard and clamp | −36.4 dB | −37.3 dB |
  | ADAA, 4x, guard and clamp | −53.6 dB | −54.3 dB |
  | for comparison, 2x / 4x with a plain clamp | −24.9 / −21.0 dB | −25.0 / −21.0 dB |
  | for comparison, 2x / 4x with no correction | −36.5 / −70.5 dB | −37.3 / −76.4 dB |

* The oversampling filters' gain at DC is 0.9998, so a constant input comes out 0.02 % low when oversampled.
* Panel (ui/src/dock/ClipperPanel.tsx): the curve in a square plot as on the Saturator, the ceiling as dashed lines
  (accent-coloured while clipping), and a meter of `over` to the plot's right with 3, 6 and 9 dB ticks, plus the
  readout. The canvas fills the column beside the knobs (an aspect ratio let it grow under them in a tall dock); the plot and meter are drawn in its top left with their own borders. Its default width in the device strip is 440 px (minimum 380).
* Presets: the five of section 4, in ui/src/dock/presets.ts.
* Tests: tests/ClipperTests.cpp, with tests/EffectTestUtils.h, which now holds the FFT and runner helpers the
  Saturator's tests had. Also the Clipper in the registry test (NodeGraphTests.cpp) and the reset-equals-fresh table
  (ModulationTests.cpp), and ui/src/dock/ClipperPanel.test.ts. The allocation test is skipped in Release builds,
  where the allocation guard is compiled out.
