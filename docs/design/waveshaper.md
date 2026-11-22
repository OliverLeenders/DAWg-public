# Design: the Waveshaper

Status: **implemented** (notes in section 8). The owner's answers are in section 7. The decisions are numbered S1–S12.

## 1. Goals

* A built-in waveshaping distortion with a choice of curves (`dawg.waveshaper`): wavefolding, a sine fold, an
  asymmetric tube-like curve, rectifiers, a soft curve, and a curve drawn on the panel. It is the effect for
  sounds the Saturator (tanh) and the Clipper (hard clip with a knee) can't make: folds, octave-up rectification and
  arbitrary transfer curves, on synth voices as much as on tracks.
* Drive, a bias that moves the operating point, a tone tilt after the shaper, Output and Mix.
* Low aliasing: first-order antiderivative anti-aliasing (ADAA) for every curve, with a closed-form antiderivative
  for each (the drawn curve's piecewise), on top of 2x, 4x or 8x oversampling.
* Changing any control while audio plays never clicks, the shape choice and the drawn curve included, and the shaper
  leaves no DC at the output.
* Usable per voice in voice graphs, like every built-in effect.

Non-goals for v1:

* Second-order ADAA. It would help the folding curves most (S5), but it needs the antiderivative's antiderivative
  and a second ill-conditioned case; oversampling covers the gap.
* A ceiling promise. The Clipper owns that (its peak guard); the Waveshaper's output can exceed 0 dBFS, as the
  Saturator's can at high Output.
* Pre-emphasis (a tilt before the shaper and its inverse after), multiband shaping, curve morphing between shapes,
  more than one drawn curve per node.
* A hard clip and tanh as shapes. The Clipper and the Saturator are those, with their own features (the ceiling and
  meter, the tuned bias).

## 2. What it reuses

The Waveshaper is the Saturator's and the Clipper's structure with a set of curves (docs/design/saturator.md,
docs/design/clipper.md). It keeps:

* first-order ADAA in double, with the ill-conditioned fallback to the curve at the midpoint (W4, C4), and the
  Clipper's rule that the previous input's antiderivative is computed again with the current sample's curve, so a
  moving bias never makes the quotient jump;
* the dry path through `(x + x₁) / 2` at the shaper's rate, so Mix never combs against ADAA's half-sample delay
  (W5, C6);
* the DC blocker on the wet signal at 5 Hz (W6);
* `juce::dsp::Oversampling` with half-band equiripple FIR filters and integer latency, reported through
  `latencySamples()`, and `ParamInfo::changesLatency`, so changing it recompiles the track (W7);
* `followParam`: 20 ms ramps when set or automated, per-sample modulation smoothing (W8);
* a static transfer curve from `effectResponse` for the panel (W9, C10), and factory presets;
* for the drawn curve, the LFO's curve model (`Curve`, `CurvePoint` with linear, hold, power and free segments), its
  publication to the audio thread (`SnapshotExchange`) and its editing helpers (`ui/src/pianoroll/curveEditing.ts`,
  as in `ui/src/graph/LfoShapeEditor.tsx`).

What is new: the curves and their antiderivatives (S3, S4), the drawn curve (S6), a bipolar bias in the shaper's
units (S7), the tone tilt (S8), 8x oversampling (S9) and the crossfade between shapes (S10).

## 3. Decisions

**(S1) A new built-in effect node.** `dawg.waveshaper`, "Waveshaper", `NodeCategory::effect`, in EffectNodes.cpp after
the Clipper: one stereo audio input with modulation inputs, one stereo output, bypass through `EffectNode`. Built-in
effects are allowed in voice graphs (`nodetype::allowedInVoiceGraph`).

Why not a Shape choice on the Saturator instead (question 1):

* The Saturator's controls are tuned for tanh: Drive divides the output by its gain so small signals keep unity gain
  (W3), and Bias is a tiny offset (0 to 0.01) added before the drive, tuned for the second harmonic of quiet signals.
  Folds and rectifiers need the opposite: Drive as plain input gain (the number of folds is the point), and a bias of
  up to ±1 in the shaper's units, which moves the fold points. One node would need per-shape meanings for its knobs.
* The drawn curve brings model data (the curve), a curve editor in the panel and a crossfade on edits; the Saturator
  would carry all of that for its one-knob use.
* The Saturator stays the cheap, simple, well-measured choice (unity gain, no tone, 2x), and its tests and presets
  stay as they are.

The cost is a second node with similar plumbing; the shared pieces (`followParam`, the oversampling choices, the ADAA
pattern) are already shared helpers, and the Waveshaper adds a shared `adaa` step both could use.

**(S2) Parameters.**

| # | id | name | range | default | unit | modulatable |
|---|----|------|-------|---------|------|-------------|
| 0 | `shape` | Shape | Soft, Tube, Fold, Sine Fold, Half Rectify, Full Rectify, Drawn | Drawn | | no (choice) |
| 1 | `drive` | Drive | −12..36 | 6 | dB | yes |
| 2 | `bias` | Bias | −100..100 | 0 | % | yes |
| 3 | `tone` | Tone | −12..12 | 0 | dB | yes |
| 4 | `output` | Output | −24..24 | 0 | dB | yes |
| 5 | `mix` | Mix | 0..100 | 100 | % | yes |
| 6 | `oversampling` | Oversampling | Off, 2x, 4x, 8x | 2x | | no (W7) |
| 7 | `symmetric` | Symmetric | off, on | on | | no (toggle) |

Shape is a choice and so can be automated but not modulated (`acceptsModulation`); changing it crossfades (S10).
Drawn is the default shape, at the owner's request (section 7): a new Waveshaper opens on the curve editor.
Symmetric applies to the drawn curve only (S6).

**(S3) The curves.** With `g` the drive's gain and `b` the bias (S7), the shaper's input is `u = g · x + b`. Every
curve has slope 1 at 0 where that makes sense, and output within ±1 except Tube's negative half (down to −2):

| shape | f(u) | character |
|-------|------|-----------|
| Soft | `u / (1 + \|u\|)` | gentler and more open than tanh; never reaches ±1, so loud signals keep some dynamics |
| Tube | `1 − e^(−u)` for u ≥ 0, `2 (e^(u/2) − 1)` for u < 0 | asymmetric: the positive half saturates at 1, the negative half later and softer at −2, so even harmonics without a bias; smooth (C1) at 0 |
| Fold | `1 − \|((u + 1) mod 4) − 2\|` | triangle wavefolder: the line up to ±1, then folds back, every 2 units of input; the classic west-coast fold |
| Sine Fold | `sin u` | folds smoothly: the peak is at π/2, then it folds back; fewer high harmonics than Fold for the same drive |
| Half Rectify | `u / (1 + u)` for u > 0, 0 otherwise | the negative half removed, the positive half soft-limited at 1 |
| Full Rectify | `\|u\| / (1 + \|u\|)` | the negative half flipped up: an octave up, soft-limited at 1 |
| Drawn | the drawn curve (S6) | anything, from subtle to steps |

Drive is the input gain into the curve, as on the Clipper, and isn't divided out afterwards: on the folds it sets how
many folds there are, which is the point of them. Output makes up level.

The rectifiers are soft-limited so that Drive does something. A plain `|u|` is homogeneous: driving it is only a
gain. With the limit, quiet signals are rectified cleanly and loud ones are squashed as well.

Candidates left out:

| curve | why not |
|-------|---------|
| tanh | the Saturator |
| hard clip | the Clipper (with a knee, a ceiling promise and a meter) |
| arctan `(2/π) atan(πu/2)` | between tanh and Soft; too close to both to earn a slot |
| cubic `u − u³/3` | a soft clip like tanh with a hard edge at ±1; the drawn curve can make it |
| Chebyshev polynomials (a chosen harmonic) | only exact for a full-scale sine; unbounded beyond ±1 |
| a bit crusher / staircase | a quantiser, not a waveshaper in the usual sense, and ADAA of steps sounds like a slope; the drawn curve with hold segments covers it |

*Since docs/design/vicanek-adaa.md, the shaper uses transparent ADAA instead: the antiderivatives below are unchanged, and the means are taken
over half-intervals with a one-pole filter before and after; during a shape crossfade the history is taken again with
each curve (V1–V5).*

**(S4) ADAA, closed form for each curve.** For each curve its antiderivative `F` (with `F(0) = 0`):

| shape | F(u) |
|-------|------|
| Soft | `\|u\| − ln(1 + \|u\|)` (with `log1p`) |
| Tube | `u + e^(−u) − 1` for u ≥ 0, `4 e^(u/2) − 2u − 4` for u < 0 |
| Fold | with `p = (u + 1) mod 4`: `G(p) + ½`, where `G(p) = p²/2 − p` for p < 2 and `G(p) = 3(p − 2) − (p² − 4)/2` for p ≥ 2 |
| Sine Fold | `1 − cos u` |
| Half Rectify | `u − ln(1 + u)` for u > 0, 0 otherwise |
| Full Rectify | `sign(u) · (\|u\| − ln(1 + \|u\|))` |
| Drawn | piecewise quadratic over the table's linear segments (S6) |

Fold's `F` is periodic (a triangle has zero mean over a period), so it stays bounded however far the drive pushes;
`mod` is computed as `p − 4 · floor(p / 4)` in double. Per channel the node keeps the previous `u` and outputs, in
double,

```
(F(u) − F(u₁)) / (u − u₁)      when |u − u₁| ≥ 1e−5
f((u + u₁) / 2)                otherwise
```

Sine Fold needs no fallback: `(cos u₁ − cos u) / (u − u₁) = sin(m) · sin(h) / h` with `m = (u + u₁)/2` and
`h = (u − u₁)/2`, which is exact and well-conditioned everywhere with `sin(h)/h → 1`. The node uses that form.

When the bias moves, the stored `F(u₁)` would belong to the old curve offset, but because the state is `u₁` itself
(bias included), the curve doesn't change shape and nothing jumps; only the drawn curve's table and the shape choice
change the curve, and S10 crossfades those.

**(S5) Where first-order ADAA isn't enough, and how oversampling covers it.** First-order ADAA is the shaper's output
averaged over each sample period, with the input interpolated linearly in between. That box average is a low-pass on
the shaped signal (a sinc in frequency): it takes about 12 dB more off the aliases from a kink in the curve (a jump
in slope, whose harmonics fall at 12 dB per octave, now at 18) and about 6 dB more off smooth curves' harmonics.
Measured on the Clipper's hard clip it gives about 12 dB (C, section 8).

That's enough where a curve has few kinks per cycle (Soft and Tube have none; the rectifiers have one at zero,
and the Clipper's numbers apply). It isn't enough for Fold and Sine Fold at high drive. Each fold adds a kink (Fold)
or a turning point (Sine Fold) per input cycle, and at 24 dB drive a full-scale sine folds about eight times per half
cycle: the shaped signal's bandwidth grows with the drive, and most of it is far above Nyquist, where the box average
only takes a little off. Two things help:

* Oversampling: 2x, 4x and 8x lift Nyquist, so more of the fold harmonics fit below it, and the half-band filters
  remove what lands above the base rate's Nyquist. For the folds, 4x or 8x is the useful setting; 8x is new here
  (S9).
* ADAA degrades gracefully: when a single sample spans several folds, the quotient is the mean over them, which tends
  to zero for Fold and Sine Fold, so the output loses the fold's top octaves rather than turning into noise.

The aliasing test (section 5) measures each curve at Off, 2x, 4x and 8x and records the numbers in section 8.

**(S6) The drawn curve.** The panel draws the curve `f(u)` over `u` from −1 to 1, values −1 to 1, with the LFO
editor's points and segments (linear, hold, power, free Bezier). Beyond ±1 the curve holds its end values, so a loud
or driven input is clipped at whatever the ends are. A new node's curve is the identity line (two points), and an
empty curve means that too.

*Symmetric and asymmetric (added 2026-11-05, section 7).* Symmetric, the default, plays only the curve's positive
half, mirrored through the origin for negative inputs: `f(−u) = −f(u)`, an odd curve, so it adds no DC and only odd
harmonics (until Bias moves the operating point). The editor then shows only the positive quadrant, `u` and values
from 0 to 1, filling the plot, and pins the half's first point at the origin (0, 0), which every odd curve passes
through. Asymmetric plays and shows the whole curve.

The stored curve is always the whole curve. Symmetric, the editor stores what it draws together with its mirror image,
so the stored curve is the curve that plays. Switching to asymmetric is one undo step: Symmetric off, and the curve
rewritten as its positive half and that half's mirror image, with a control point at (0, 0), so it sounds the same
right after the switch and the negative half can then be drawn on its own. Switching back to symmetric plays the
positive half of whatever is drawn. Linear, hold and free (Bezier) segments mirror exactly: a free segment's tension
point `(hx, hy)` becomes `(1 − hx, 1 − hy)`, and a hold after a sloped segment gets a second point one tick (1/7680 of
the input range) later for its step. Power segments, which the editor doesn't make, mirror approximately (their
tension negated).

*Model.* `GraphNode::modulationShape` already holds the LFO's cycle as a `Curve` with times over
`0..modulationShapeTicks` and values −1..1. The Waveshaper uses the same field, times mapped linearly to `u` from −1
to 1. `setModulationShape` accepts a Waveshaper node too, and anchors its curve at both ends with the end points'
own values (the LFO's anchors loop to the first value instead). Project files and pasting already carry the field
(`ProjectFile.cpp`, `Commands.cpp`); the paste check widens from the LFO to both types. Renaming the field to `shape`
is left out: it would touch the LFO, the project file and the UI for a name.

*Engine.* `configure()` runs on the message thread. When the curve differs from the last one published, it samples
the curve with `evaluateCurve` at 257 points over −1..1 (256 linear segments, step `h = 1/128`) and computes the
running integral at each point (trapezoids, exact for linear segments). It samples a second table from the positive
half mirrored (`y(u) = −f(−u)` below 0), so the Symmetric switch needs no work on the audio thread. The two tables
(each 2 × 257 doubles, about 4 KB) are published together through a `SnapshotExchange`, as the LFO publishes its
shape, and the audio thread plays the one Symmetric chooses. The audio thread copies a new snapshot into
its own table on `acquire()` (the exchange's pointer is only valid until the next acquire), so it can keep the old
table for the crossfade (S10) without holding a snapshot the message thread may free.

*ADAA.* The table is a piecewise-linear `f`, so its antiderivative is piecewise quadratic and exact: with `k` the
segment and `t` the position in it,

```
f(u) = y[k] + t · (y[k+1] − y[k])
F(u) = I[k] + h · (y[k] · t + (y[k+1] − y[k]) · t² / 2)
beyond the ends: F(u) = I[256] + y[256] · (u − 1),  F(u) = I[0] + y[0] · (u + 1)    (I[0] = F(−1))
```

so the drawn curve gets the same first-order ADAA as the built-in curves in both modes (the mirrored table is a
piecewise-linear curve like any other), and its kinks behave as S5 says. 256
segments make a smooth drawn curve exact to well under 0.01 % of full scale; a hold segment (a step) becomes a ramp
1/128 wide, which ADAA would turn into a slope anyway.

*Why not the curve's own segments in closed form.* Linear and hold segments would be exact, but power segments are
`t^k`-style easings and free segments are Bezier curves parameterised in `t`, not in `u`; integrating them in `u`
needs a cubic solve per sample. Sampling to a table is exact for linear segments at the table's resolution and costs
a lookup per sample for any segment type.

**(S7) Bias, and DC.** Bias moves the operating point: `u = g · x + b` with `b` from −1 to 1 (Bias −100..100 %),
added after the drive, in the shaper's units, so the same Bias puts the rest point at the same place on the curve at
any Drive. On the folds it moves the fold points (the "symmetry" control of hardware folders); on Soft it adds even
harmonics like the Saturator's bias, much more strongly.

The static offset `f(b)` is subtracted from the wet signal, so silence stays silent at any Bias and Shape, and a
first-order DC blocker at 5 Hz on the wet signal (as W6) removes the DC that the asymmetric curves (Tube, the
rectifiers, a bias, an asymmetric drawn curve) produce from signal. The rectifiers' DC is large (a full-rectified
sine has a mean of 0.64 of its peak), so the blocker's settling is part of their sound: a 5 Hz blocker settles in
about 30 ms.

**(S8) Tone: a tilt after the shaper, on the wet signal.** A first-order tilt: the wet signal is split with a one-pole
low-pass at 800 Hz into its low part and the rest (`high = wet − low`), and recombined as
`low · 10^(−T/40) + high · 10^(T/40)`, so Tone `T` from −12 to 12 dB tilts the spectrum by `T` from the lows to the
highs around 800 Hz, and at 0 dB it is exactly the input. It runs at the shaper's rate, before Mix, so the dry signal
is untouched; the gains are smoothed and modulatable, and the filter's coefficient is fixed per rate, so moving Tone
never clicks.

After the shaper because that's where distortion needs a tone control: to tame the fizz of folds and rectifiers, or to
brighten a dark soft curve. Before the shaper it would choose which frequencies distort (question 3); an EQ or Filter
node before the Waveshaper does that today.

*Since docs/design/vicanek-adaa.md, the shaper uses transparent ADAA instead: the −2 dB at 20 kHz below no longer applies, and 2x
stays the default for aliasing (V8).*

**(S9) Oversampling: Off, 2x, 4x, 8x, default 2x.** The Saturator's and Clipper's choices plus 8x (three half-band
stages), because the folds need it (S5). 8x costs about twice 4x and adds its latency (reported, compensated). 2x by
default for the same reasons as the Saturator (W7: ADAA's `(u + u₁) / 2` takes highs off, −2 dB at 20 kHz at 2x) and
because many voices in a voice graph each pay for it; the Fold presets choose 4x or 8x (question 4).

**(S10) Changing the shape or the drawn curve crossfades.** When Shape changes (from the panel, automation or undo),
a new drawn curve arrives, or Symmetric switches, the node computes the ADAA output of both the old and the new curve from the same `u` and
`u₁` for 20 ms and crossfades linearly. Both are mean values of their curves over the same segment, so the fade is
smooth. After the fade only the new curve runs. A change during a fade waits until it ends, at most 20 ms, and then
fades from where it ended. Starting a new fade at once would drop the quieter of the two curves while it is still
audible: the first version did, and the click test caught it once Symmetric switched a curve that isn't odd.

*Since docs/design/vicanek-adaa.md, the shaper uses transparent ADAA instead: the dry signal goes through the shaped path's linear
chain (V7).*

**(S11) Mix and smoothing as in the Saturator.** The dry signal is `(x + x₁) / 2` at the shaper's rate, Mix crossfades
dry and the toned wet before downsampling, and Output is applied after downsampling. Mix 0 % is the input passed
through (before Drive). Drive, Bias, Tone, Output and Mix ramp over 20 ms when set or automated and follow modulation
per sample; `g`, `b`, `f(b)` and the tone gains are computed per sample at the base rate and held across the
oversampled samples of one base sample. Oversampling is taken over by the audio thread at its next block, with the
history, the DC blocker, the tone filter and the oversamplers cleared, as the Saturator does.

**(S12) Panel.** Like the Clipper's, without the meter: the static transfer curve, output over input from −1 to 1 before
Drive, with a diagonal for reference and the output axis widened when the curve goes beyond ±1 (`outputRange`, as
the Saturator and Clipper do). Above it, the Shape as a drop-down and Oversampling as a segmented control. Drive, Bias
and Tone are the Shape section's knobs, Output and Mix the Output section's. `effectResponse` gets `dawg.waveshaper`
with 201 points; for Drawn it returns the identity, because the panel shows the drawn curve itself (below). A dot
marks the input level on the curve, as on the Saturator; with Drawn it sits at the shaper's input `u = g x + b` on the
drawn curve, from the meters' `shaperInput` and `shaped` (the running curve at `u`), mirrored into the positive
quadrant when symmetric.

With Shape on Drawn, the plot shows the drawn curve `f(u)` instead and is the curve editor: the LFO editor's gestures
(Alt-click adds a point, drag moves, double-click deletes, drag a segment's handle to bend it, double-click the handle
to straighten). Symmetric, it shows `u` and values from 0 to 1 (S6); asymmetric, both from −1 to 1. Below it, a
Symmetric / Asymmetric switch and a row of curve presets: Identity, Soft, Fold and Steps, defined as positive halves
and mirrored, and in the asymmetric mode also Half. A light band marks the range of `u` a full-scale input reaches at
the current Drive and Bias (`b ± g` clipped to the plot; symmetric, from 0 to `|b| + g`), so it's clear which part of
the curve is in use. The editor dispatches `setModulationShape` as the LFO's does.

The plot is a square in the panel's main column, as large as the column's width and height allow; the controls are in
their own column beside it, so nothing overlaps it at any card width or dock height. The card's minimum width is set
so the square is at least 120 px next to the knob sections, and checked at the dock's minimum and default heights.

## 4. Presets

Factory presets in `ui/src/dock/presets.ts`:

| name | shape | drive | bias | tone | oversampling | curve |
|------|-------|-------|------|------|--------------|-------|
| Gentle Warmth | Soft | 6 | 0 | −2 | 2x | |
| Tube Drive | Tube | 12 | 0 | −3 | 2x | |
| West Coast Fold | Fold | 18 | 0 | −4 | 4x | |
| Asymmetric Fold | Fold | 15 | 40 | −3 | 4x | |
| Sine Folder | Sine Fold | 18 | 0 | 0 | 4x | |
| Octave Up | Full Rectify | 6 | 0 | −3 | 2x | |
| Half-wave Buzz | Half Rectify | 12 | 20 | 0 | 2x | |
| Drawn Steps | Drawn, symmetric | 0 | 0 | −3 | 4x | a staircase of hold segments, a step of 0 around the origin |
| Drawn Half-wave | Drawn, asymmetric | 3 | 0 | −2 | 2x | the Half curve: a quarter of the negative half kept |

The Drawn preset sets the curve with `setModulationShape`, as the LFO presets set their shapes.

## 5. Tests (tests/WaveshaperTests.cpp)

* ADAA matches the analytic result on ramps: for every shape, a linear ramp from −4 to 4 (several folds) with
  oversampling Off, Tone 0 and Mix 100; each output equals `(F(u) − F(u₁)) / (u − u₁) − f(b)` worked out
  independently in the test (before the DC blocker, which the test accounts for by measuring with the blocker's
  input, or by checking against the blocker applied to the expected values). The drawn curve with a known
  piecewise-linear curve is checked the same way.
* Each antiderivative's derivative equals its curve (numerically, at many points, both sides of every branch), and
  each `F` is continuous at the branch points.
* Aliasing: a 7 kHz sine at 0 dBFS at 48 kHz for each shape at a drive where it distorts strongly; the energy outside
  the harmonics is clearly lower than the naive shaper's with oversampling Off, and lower again at 2x, 4x and 8x.
  Thresholds from a first measurement, recorded in section 8.
* DC: silence gives exact silence at any Bias and Shape; a sine through Full Rectify and through Tube with Bias leaves
  less than −80 dBFS of DC once the blocker settles.
* Mix and bypass: Mix 0 % equals the dry path `(x + x₁) / 2`; bypassed, the output equals the input with no latency.
* Tone: at 0 dB a small signal through Soft passes with the curve's gain; at +12 dB a high tone is louder than a low
  one by close to 12 dB (within the first-order tilt's tolerance).
* No clicks: Drive, Bias and Tone swept over their range in one block with a loud sine; Shape switched through every
  choice, new drawn curves published (also during fades), and Symmetric switched on a curve that isn't odd while a
  sine plays; the sample-to-sample difference stays below a bound set
  by the input's.
* No allocation in `process()` (`AllocationGuard`), at every oversampling choice, with modulation, with a new drawn
  curve arriving.
* Latency: `latencySamples()` matches the measured delay of an impulse at Off, 2x, 4x and 8x.
* Robustness: ±1000 and denormal inputs give finite output for every shape; `reset()` clears the history; instances
  keep their own state.
* Symmetric: every ADAA, aliasing, DC and robustness test runs the drawn curve both ways; the symmetric table is the
  positive half mirrored, and a reset node plays the mode it is set to.
* Commands: `setModulationShape` on a Waveshaper anchors the curve at both ends with its own values, and is refused
  on other node types except the LFO.
* The registry test (NodeGraphTests.cpp), the reset-equals-fresh table (ModulationTests.cpp) and `effectResponse`.
* UI (ui/src/dock/WaveshaperPanel.test.ts): the drive range band (both modes), the mapping between the editor and `u`,
  the mirroring (the whole curve equals the half and its image off the points, beyond the ends too, with a point at
  the origin, for the presets and for bent and held segments), the positive half and the pinned origin, the drawn
  curve presets, the identity for a new node, the layout helper that sizes the plot, and the card's minimum width.

## 6. Summary

| | Waveshaper |
|---|---|
| Node | `dawg.waveshaper`, effect, track and voice graphs (S1) |
| Curves | Soft, Tube, Fold, Sine Fold, Half Rectify, Full Rectify, Drawn (the default, symmetric by default) (S3, S6) |
| Anti-aliasing | first-order ADAA in double with a closed-form antiderivative per curve, piecewise for the drawn curve (S4); Off / 2x / 4x / 8x oversampling for the folds (S5, S9) |
| Controls | Drive (input gain), Bias (operating point, ±1), Tone (tilt after the shaper), Output, Mix |
| DC | static offset removed, 5 Hz blocker on the wet signal (S7) |
| Changes | 20 ms ramps; Shape, the drawn curve and Symmetric crossfade over 20 ms (S10) |
| Latency | the oversampler's, reported and compensated; ADAA's half sample not compensated |
| Panel | transfer curve; with Drawn, a curve editor with the used range marked, the positive quadrant when symmetric (S12) |

## 7. Owner's answers (2026-11-05)

1. **New node or a Shape on the Saturator:** a new node, `dawg.waveshaper` (S1); the Saturator is unchanged.
2. **The curve set:** Soft, Tube, Fold, Sine Fold, Half Rectify, Full Rectify and Drawn, the drawn curve in v1 (S3,
   S6).
3. **Tone:** a tilt after the shaper (S8).
4. **Oversampling:** Off / 2x / 4x / 8x, 2x by default (S9).
5. **Symmetric drawing (requested later on 2026-11-05):** the drawn curve gets a symmetric mode, the default, in which
   only the positive half is drawn and shown, scaled to the whole plot; switching to asymmetric puts a control point
   at (0, 0) and keeps the mirrored points, so the sound doesn't change. Drawn becomes the default shape (S2, S6).

## 8. Implementation notes

* Node (EffectNodes.cpp, `WaveshaperNode`): parameters as in S2. The curves, their antiderivatives and the ADAA step
  are free functions in `dawg::waveshaper` (declared in BuiltinNodes.h), so the tests check them directly. The drawn
  curve's table is `waveshaper::DrawnTable`.
* The previous shaper input starts at the bias after a reset or an oversampling switch, so silence is silent from the
  first sample at any Bias.
* The drawn curve: `configure()` publishes a new table only when the curve changed, and the audio thread copies it into
  one of two tables; a new table goes into the slot no audible drawn curve uses, so a change during a fade never
  overwrites the curve fading out. `reset()` takes the newest table without a fade.
* `setModulationShape` accepts a Waveshaper and anchors its curve's ends with their own values; pasting carries the
  curve. Saving a user preset of a Waveshaper keeps its drawn curve, as for an LFO.
* Measured aliasing (the test's 7 kHz sine at 0 dBFS at 48 kHz; power outside the harmonics relative to the
  fundamental, for Full Rectify relative to the second harmonic, since it has no fundamental):

  | shape (drive) | naive | ADAA, Off | 2x | 4x | 8x |
  |---|---|---|---|---|---|
  | Soft (18 dB) | −16.2 dB | −28.5 dB | −42.6 dB | −65.0 dB | −88.2 dB |
  | Tube (18 dB) | −17.2 dB | −28.0 dB | −47.5 dB | −73.8 dB | −88.2 dB |
  | Fold (12 dB) | +6.0 dB | −8.4 dB | −29.4 dB | −49.7 dB | −64.5 dB |
  | Sine Fold (12 dB) | +6.9 dB | −6.7 dB | −75.8 dB | −91.5 dB | −90.1 dB (float floor) |
  | Half Rectify (18 dB) | −13.4 dB | −23.5 dB | −40.8 dB | −57.4 dB | −71.5 dB |
  | Full Rectify (18 dB) | −0.8 dB | −8.7 dB | −29.4 dB | −42.4 dB | −55.7 dB |
  | Drawn, the test's curve (18 dB) | −8.9 dB | −21.0 dB | −33.4 dB | −48.0 dB | −72.3 dB |

  As S5 expects, ADAA alone gains 10–14 dB, and the folds and the full rectifier need 4x or 8x. Sine Fold at 12 dB
  is a sine of a sine, whose harmonics fall off quickly (Bessel functions), so 2x already removes most of its aliasing.
* Panel (ui/src/dock/WaveshaperPanel.tsx, with the editor in WaveshaperCurveEditor.tsx and the helpers in
  waveshaper.ts): the Shape drop-down and Oversampling wrap onto two rows in a narrow card; the plot is the largest
  square in its box, drawn by the canvas, with the editor's SVG laid over the same square. The card is 460 px wide by
  default, 380 px at least, which leaves the plot about 150 px wide beside the knobs.
* Presets: the nine of section 4, in ui/src/dock/presets.ts; the two drawn ones carry their curves.
* Symmetric (section 7, question 5): the `symmetric` toggle is parameter 7. The node publishes both tables with each
  curve and swaps them at the start of a block, with the crossfade. The UI's mirroring is `mirrored()` and
  `positiveHalf()` in ui/src/dock/waveshaper.ts; the switch to asymmetric is a `batch` of the parameter and the curve.
* Crossfades (S10): a change that arrives during a fade waits for it to end.
* Tests: tests/WaveshaperTests.cpp, the Waveshaper in the registry test (NodeGraphTests.cpp) and the
  reset-equals-fresh table (ModulationTests.cpp), and ui/src/dock/WaveshaperPanel.test.ts. The allocation test is
  skipped in Release builds, where the allocation guard is compiled out.
