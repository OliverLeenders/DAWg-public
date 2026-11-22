# Design: the Saturator

Status: **implemented** (notes in section 8). The owner's answers are in section 6. The decisions are numbered W1–W9.

## 1. Goals

* A built-in waveshaping distortion (`dawg.saturator`): tanh saturation from warm to heavy, with a **bias** that adds
  even harmonics, in track and voice graphs.
* Low aliasing without heavy oversampling: first-order antiderivative anti-aliasing (ADAA), optionally on top of 2x
  or 4x oversampling.
* Changing any control while audio plays never clicks, and the bias never leaves DC at the output.

Non-goals for v1: other curves (hard clip, arctan, x / (1 + |x|) all have closed-form antiderivatives and can follow
as a Shape choice), second-order ADAA, tone controls around the shaper (wire an EQ or Filter before and after).

## 2. Origin

The DSP follows the owner's ADAA plugin (`PluginDev/ADAA`, JUCE, GPLv3, the owner's own work), which implements
J. D. Parker, V. Zavalishin and E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using Continuous-Time
Convolution" (DAFx 2016). DAWg keeps the plugin's signal flow, its drive and bias ranges and its DC compensation, and
writes the node fresh: the plugin's code is not copied (section 4 lists why).

## 3. Decisions

**(W1) A built-in effect node.** `dawg.saturator`, "Saturator", `NodeCategory::effect`, in EffectNodes.cpp beside the
Filter: one stereo audio input with modulation inputs, one stereo output, bypass through `EffectNode`. Built-in
effects are allowed in voice graphs (`nodetype::allowedInVoiceGraph`), so it can saturate each voice.

**(W2) Parameters.**

| # | id | name | range | default | unit | modulatable |
|---|----|------|-------|---------|------|-------------|
| 0 | `drive` | Drive | 0..40 | 12 | dB | yes |
| 1 | `bias` | Bias | 0..100 | 0 | % | yes |
| 2 | `output` | Output | −24..24 | 0 | dB | yes |
| 3 | `mix` | Mix | 0..100 | 100 | % | yes |
| 4 | `oversampling` | Oversampling | Off, 2x, 4x | 2x | | no (W7) |

**(W3) The signal flow keeps the plugin's.** With drive `d` in dB and bias `p` in %:

```
g = 0.1 · 10^(d / 20)          0 dB is the plugin's drive 1, 40 dB its drive 100
b = exp(p / 10000) − 1         0 .. 0.01005, the plugin's exp(bias / 100) − 1 with its bias 0..1
u = g · (x + b)                the bias is added to the input, before the drive
y = (tanh_ADAA(u) − tanh(g · b)) / g
```

Dividing by `g` keeps the small-signal gain at unity, so Drive changes how much is saturated rather than how loud it
gets (loud signals do get quieter as Drive rises: the shaper's output never exceeds 1 / g; Output makes that up).
Subtracting `tanh(g · b) / g` removes the static DC the bias causes.

**The bias range stays as tuned.** Because `b` is added before the drive, the balance of even to odd harmonics depends
on the input level, hardly on Drive. Measured with a 1 kHz sine at full bias (static curve, no ADAA), H2 relative
to H3:

| sine level | 0 dB drive | 20 dB | 34 dB | 40 dB |
|------------|-----------|-------|-------|-------|
| 0 dBFS | −24.4 dB | −25.5 dB | −29.6 dB | −30.2 dB |
| −12 dBFS | −12.4 dB | −12.5 dB | −14.0 dB | −15.9 dB |
| −20 dBFS | −4.4 dB | −4.4 dB | −4.7 dB | −5.4 dB |
| −30 dBFS | +5.6 dB | +5.6 dB | +5.6 dB | +5.7 dB |

So at full bias the second harmonic is level with the third for signals around −20 to −25 dBFS, which is where the
range was tuned. The shaper's derivative at the bias point, 1 − tanh²(g · b), is at least 0.99, so the bias needs no
gain correction of its own.

**(W4) ADAA computed exactly, in double.** The antiderivative of tanh is log cosh, computed in the overflow-free form
`|u| + log1p(exp(−2|u|)) − ln 2`. Per channel the node keeps the previous `u`, and outputs

```
(F(u) − F(u₁)) / (u − u₁)      when |u − u₁| ≥ 1e−5
tanh((u + u₁) / 2)             otherwise
```

in double: in float the difference of two log cosh values cancels badly. No lookup tables (the plugin's linear table
turns tanh into a staircase, and both of its tables misbehave outside their range). ADAA is always on: it is the
point of the node.

Because the state holds the previous `u`, which already includes the previous drive and bias, smoothly moving drive
and bias need no special handling.

*Since docs/design/vicanek-adaa.md, the shaper uses transparent ADAA instead: its linear response is a flat allpass rather than
`(u + u₁) / 2`, and the dry path goes through that same chain (V1, V7). What follows describes the original
design.*

**(W5) The dry path matches ADAA's linear response.** For small signals first-order ADAA outputs `(u + u₁) / 2`: a half-
sample delay and a gentle low-pass. The dry signal for Mix goes through the same `(x + x₁) / 2`, so for small signals
dry and wet are identical and Mix never combs. Mix is applied in the oversampled domain, before downsampling.

**(W6) DC blocker on the wet signal.** The static term (W3) removes the bias's DC for silence, but a biased shaper
also rectifies: its DC follows the level. A first-order DC blocker at 5 Hz on the wet signal only, at the oversampled
rate before Mix (W5), removes it. The dry signal keeps its own DC, as other effects do.

*Since docs/design/vicanek-adaa.md, the shaper uses transparent ADAA instead: nothing is lost at 20 kHz at any choice, so the table in W7 no longer
applies. 2x stays the default for aliasing (V8).*

**(W7) Oversampling.** `juce::dsp::Oversampling` with `filterHalfBandFIREquiripple` and integer latency
(`setUsingIntegerLatency`), one or two stages, so the delay is linear-phase and whole samples. `latencySamples()`
reports it, so the graph compensates for it. Bypassed it adds none, and the compiler counts none.

Why 2x by default: first-order ADAA's `(u + u₁) / 2` takes highs off even when nothing saturates. At 48 kHz:

| oversampling | at 10 kHz | at 15 kHz | at 20 kHz |
|--------------|-----------|-----------|-----------|
| Off | −2.0 dB | −5.1 dB | −11.7 dB |
| 2x | −0.5 dB | −1.1 dB | −2.0 dB |
| 4x | −0.1 dB | −0.3 dB | −0.5 dB |

The oversampling filters add their own small passband loss. Off is for voice graphs with many voices, where per-voice
oversampling is costly and darkening is often welcome.

Latency is read when the graph compiles (`GraphCompiler`), so the oversampling choice must recompile the track's
graph when it changes. Until now a parameter change never recompiled. New: `ParamInfo::changesLatency`. A parameter
flagged with it is not modulatable and not automatable, and a change to it (from `setNodeParam`, MIDI learn or undo)
recompiles the project. Without a recompile, the alternative would be to report the 4x latency for every choice,
which costs latency for nothing.

ADAA's half-sample delay at the oversampled rate (a quarter of a base sample at 2x) isn't compensated: it is below
one sample and only matters against a parallel path in the same graph, where it is −0.5 dB at 20 kHz at 2x.

**(W8) Smoothing and modulation as in the Filter and Utility.** Drive, bias, output and mix ramp over 20 ms
(`smoothingSeconds`) when set or automated, and follow modulation per sample with the short modulation smoothing
(`modulationSmoothingCoefficient`). `g`, `b`, `1 / g` and the static DC term are computed per sample from the
smoothed values, at the base rate and held across the oversampled samples of one base sample.

**(W9) Panel: the transfer curve.** `effectResponse` gets `dawg.saturator`: the static curve
`(tanh(g · (x + b)) − tanh(g · b)) / g` for x in −1..1, scaled by Output, drawn as y over x with both axes −1..1 and
a diagonal for reference, so the bias shows as the curve's asymmetry. The device dock shows it above the controls, as
the Filter shows its response. A dot marks the input level on the curve, as on the Compressor: the node's meters give
`input`, the input's peak before Drive, held and falling with a 50 ms time constant.

## 4. Not taken from the plugin

* Shared state: its ADAA history and DC filter are statics, shared by every instance. DAWg's state is per node and per
  channel (per voice in voice graphs).
* Lookup tables: the linear table outputs 0 for inputs beyond its ±10 range (F goes flat, so the ADAA quotient is 0),
  and the cubic table reads out of bounds there; both are rebuilt by every new instance while others play.
* The plugin's "transparent" ADAA: its small-signal response is (x[n] + 6x[n−1] + x[n−2]) / 8, −5.5 dB at 20 kHz at
  48 kHz, and it ignores the table choice. It is only the first half of M. Vicanek's scheme, without the two
  compensation filters that make it flat. The full scheme was adopted later for all three shapers
  (docs/design/vicanek-adaa.md).
* No anti-aliasing as the default, unsmoothed drive and bias (the makeup gain jumps), mode choices outside the saved
  state, and its oversampling, which is never switched on or initialised.

## 5. Tests (tests/SaturatorTests.cpp)

* Aliasing: a 7 kHz sine at 40 dB drive at 48 kHz; the energy outside the harmonics (folded back) is clearly lower
  than a naive tanh's with oversampling Off, and lower still at 2x; the thresholds come from a first measurement.
* The bias table in W3, within 1 dB, at Off and 4x oversampling (the shaper, not the anti-aliasing, sets it).
* No DC: a biased sine at several levels leaves less than −80 dBFS of DC after the DC blocker settles.
* Unity small-signal gain at 1 kHz for every Drive; Mix at 0 % equals the dry path's (x + x₁) / 2.
* No clicks: Drive and Bias swept over their range in one block; the output's sample-to-sample difference stays
  below a bound set by the input's.
* Robustness: inputs of ±1000 and denormals give finite output; `reset()` clears the history.
* Latency: `latencySamples()` matches the measured delay of an impulse at Off (0), 2x and 4x; changing Oversampling
  recompiles and keeps the track aligned.
* Voice graphs: a Saturator in a voice graph keeps separate state per voice.

## 6. Owner's answers (2026-10-28)

1. **Bias range:** 0..100 %, as in the plugin; no negative bias.
2. **Default oversampling:** 2x everywhere, voice graphs included.
3. **Default drive:** 12 dB.
4. **Gain:** Output only, no auto gain.

## 7. Steps

1. Engine: the node (W1–W8) and its tests. **Done**, see section 8.
2. `ParamInfo::changesLatency` and the recompile on `setParam` (W7), with its test. **Done**, see section 8.
3. Panel: `effectResponse` (W9), the node's entry in the UI and the device dock's curve. **Done**, see section 8.
4. PLAN.md and this document's status. **Done.**

## 8. Implementation notes

* Node (EffectNodes.cpp, `SaturatorNode`): parameters 0–4 as in W2. The engine now links `juce_dsp` for
  `juce::dsp::Oversampling` (2x and 4x instances, both prepared, so switching never allocates).
* The oversampling choice is read in `prepare()` and `configure()` and taken over by the audio thread at its next
  block, with the ADAA history and the oversampler cleared. `latencySamples()` reports the choice of the last
  `configure()`.
* Recompiling (GraphCompiler.cpp): `update()` compiles when `sameLatencyParams()` finds a `changesLatency` parameter
  that moved, in track and voice graphs; otherwise parameter changes still go through `applyNonStructural()`. The
  node switches oversampling when `configure()` runs during that compilation, a block or so before the new engine
  with the new delay compensation is installed; switching is rare, so that short misalignment is accepted.
* `ParamInfo::acceptsAutomation()` is false for such parameters: `addAutomationLane` refuses them, the node type's
  JSON says `automatable: false`, and the arrangement's lane menu leaves them out. `acceptsModulation()` is false too.
* `effectResponse` returns the static curve with 201 points and values rounded to 1e−4 (`toArray` got a precision
  argument; the dB curves keep two decimals).
* Measured aliasing (the test's 7 kHz sine at 40 dB drive, power outside the harmonics relative to the
  fundamental): naive tanh −15.8 dB, ADAA −28.0 dB, ADAA at 2x −47.4 dB, at 4x −92.8 dB.
* Panel (ui/src/dock/SaturatorPanel.tsx, `NODE_TYPES.saturator`): the transfer curve in a square plot with both axes
  −1..1, grid lines at ±0.5 and 0 and a dashed unity line; the curve is clipped to the plot when Output lifts it past
  ±1. Oversampling is a segmented control above it; Drive and Bias (Shape) and Output and Mix are knobs. Node menus
  come from the registry, so the node needed no other UI entry.
* Tests: tests/SaturatorTests.cpp (including the recompile and the automation refusal), plus the Saturator in the registry test (NodeGraphTests.cpp) and in the
  reset-equals-fresh table (ModulationTests.cpp).
