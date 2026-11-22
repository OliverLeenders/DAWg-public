# Design: Chorus, Flanger and Phaser

Status: **implemented** (notes in section 8); the owner's answers are in section 7. The decisions are numbered X1–X14.

## 1. Goals

* Three built-in modulation effects, `dawg.chorus`, `dawg.flanger` and `dawg.phaser`, so the most common modulation
  sounds no longer need a Faust script (`app/faust/examples/chorus.dsp`) or a patch.
* **True stereo**: stereo in, stereo out, each channel through its own delay lines or allpass chain, with the right
  channel's LFO a settable phase behind the left's (**Spread**). A mono input (left equal to right) comes out wide,
  because the two channels are modulated differently; Spread at 0° keeps it mono.
* Tempo sync for the LFO, as the LFO node and the Delay have it.
* Real-time safe: every buffer is sized in `prepare()`, nothing allocates, locks or waits in `process()`.
* No clicks or zipper noise when a control moves, and an output that stays bounded under any feedback setting.
* Output independent of the block size, so tests can compare runs exactly and exports match playback.
* Cheap enough to run in voice graphs (a few hundred multiply-adds per sample at most).

Non-goals for v1:

* Ensemble or "dimension" effects with many voices and random drift (Chorus has up to four voices, X5).
* Barber-pole (endlessly rising) flanging and phasing.
* Envelope-following sweeps: an ADSR or an envelope routed to Depth or Delay through modulation does that already.
* Bucket-brigade emulation (companding noise, clock aliasing). The delay lines are clean.

## 2. What it reuses

* **The effect plumbing** in EffectNodes.cpp: `EffectNode` for bypass, `stereoModPorts`, `followParam` for 20 ms
  ramps and per-sample modulation, the `differs` helper.
* **The Delay's interpolated read** (4-point Catmull-Rom behind the write position) and its soft write limit, which
  keeps a feedback loop from building up overshoot.
* **Tempo sync**: `lfoCycleBeats` and `lfoCycleChoices` (dawg/Modulation.h), the tempo from
  `transport->timeMap.tempoMap().bpmAt()` as the Delay reads it.
* **The panels**: `useNodeDrag`, `useNodeAnalysis` for live meters, `Knob`, `Section`, the `.segmented` control, the
  Delay panel's Sync button and note select, and the card's measured min-content width (panelMinWidth.ts).

## 3. Decisions

**(X1) Three nodes, one shared core.** `dawg.chorus` ("Chorus"), `dawg.flanger` ("Flanger") and `dawg.phaser`
("Phaser"), `NodeCategory::effect`, each with one stereo audio input with modulation inputs and one stereo output,
in EffectNodes.cpp beside the other effects. They are three nodes, not one with a mode switch, because their ranges,
defaults and panels differ and the user asked for three standard effects. What they share lives in a new header,
`engine/include/dawg/ModulationEffects.h`, so the tests can drive the pieces directly:

* `StereoLfo`: the phase accumulator, the shapes (X3), Spread and tempo sync (X4). All three nodes use it.
* `ModulatedDelay`: one channel's circular delay line with an interpolated read (X6). The Chorus and the Flanger use
  it; they differ only in how many taps they read, the ranges and the through-zero path.
* `AllpassStage`: a first-order allpass, which the Phaser chains (X10).

**(X2) Spread: the right LFO's phase offset.** 0–180°, in degrees. The right channel's LFO runs at the left's phase
plus Spread / 360. 0° is mono modulation (a mono input stays mono), 90° the classic stereo setting (the default for
all three), 180° opposite sweeps, the widest. Above 180° would only mirror the range with left and right swapped, so
it isn't offered. Spread is modulatable; it moves the right phase smoothly (it ramps like the other controls, X12).

Mono input needs nothing special: the left and right delay lines (or allpass chains) receive the same signal and are
read at different times, so the wet signal is decorrelated and the mix is wide. Nothing detects mono; the Utility
can still fold the result back.

**(X3) LFO shapes: Sine and Triangle.** A `shape` choice on each node, default Sine.

* **Sine**: smooth, the usual chorus and phaser sweep.
* **Triangle**: a linear sweep, the classic flanger shape; on a delay its constant slope gives a constant pitch
  offset up and down, a steadier chorus.

The LFO is computed per sample from a double-precision phase: `sin(2πφ)` and a triangle `1 − 4·|φ − 0.5|` shifted to
start at 0, both from −1 to 1, starting at 0 rising. The triangle's corners are where a delay's slope reverses: the
pitch steps from up to down there, but the read position stays continuous, so the waveform never jumps. Other shapes
are question 3.

**(X4) Rate and tempo sync.** `sync` (toggle, default off), `rate` (0.01–10 Hz, mid 1 Hz) and `cycle`
(`lfoCycleChoices`, default 1/1 for the Chorus and the Phaser, 2/1 for the Flanger). While synced, the cycle's length
in seconds comes from the tempo at the block's start, as the Delay's times do.

* **Free**: the phase runs on from wherever it was; reset (an export pass, a newly prepared node) starts it at 0.
* **Synced, playing**: the LFO follows the song position, `φ = beats / cycleBeats` (mod 1), so a sweep lands on the
  same beat on every playback and in every export, and two synced effects stay in step. On a jump (start of
  playback, a seek, a loop wrap) the phase doesn't jump with it: it slews to the song position with a time constant
  of 50 ms (the increment is corrected by the phase error each sample), because a jump in the LFO is a jump in the
  delay, which clicks. Whether this is wanted is question 2.
* **Synced, stopped**: the LFO runs on at the tempo's rate, so a monitored input still moves.

The rate itself (the phase increment) is ramped over 20 ms, so turning Rate never jumps the phase.

**(X5) The Chorus.** Up to four voices per channel; each voice is a tap on the channel's delay line, its LFO a
quarter (or a third, or half) of a cycle from the others: voice `k` of `V` at `φ + k / V`. The right channel's
voices are offset by `Spread / 360 / V`, Spread's share of the gap to the next left voice: an offset of a whole
`1 / V` would land each right voice on a left one, so 180° puts them halfway between, the widest, at any count. The voices' sum is scaled by `1 / √V`, so more voices make the chorus thicker without
getting much louder. Each tap's delay is

    d(t) = Delay · (1 + Depth · m(t))        m in −1..1

so Depth is the sweep as a fraction of Delay; at 100 % it sweeps from 0 to twice Delay (clamped to the minimum read
distance, X6). The pitch deviates by the delay's slope, `Delay · Depth · 2π · Rate` at its peak: with Delay 15 ms,
Depth 15 % and Rate 0.8 Hz (the defaults) about ±1.1 %, or ±20 cents, a gentle chorus. **Feedback** (0–80 %, default 0) feeds the voices' sum back into the line, for
the metallic, resonant choruses; it is limited lower than the Flanger's because four taps add up.

**(X6) Delay-line interpolation: 4-point Catmull-Rom, the Delay's.** A modulated delay's read position moves every
sample, so the interpolator's frequency response is heard as it changes:

* **Linear** interpolation is a low-pass whose depth depends on the fractional position: at a half-sample position
  it is −2 dB at 10 kHz (48 kHz), at a whole sample 0 dB. On a sweep this modulates the highs at the sweep's rate, and
  inside the flanger's feedback loop the loss compounds per pass.
* **Allpass (Thiran)** interpolation is flat in magnitude but has a state that rings when the delay moves quickly, the
  wrong choice for a delay that never stops moving.
* **4-point Catmull-Rom** (cubic Hermite), which the Delay already uses: within about 0.5 dB to 10 kHz at any
  position, four reads and a few multiplies per tap. Interpolating at higher order (6-point Lagrange, windowed sinc)
  would cost more for a difference that's inaudible next to the effect.

The read sits at least 2 samples behind the write position (the 4-point kernel reads one sample on either side),
which is the minimum delay of every tap: 0.04 ms at 48 kHz. Lines are sized in `prepare()` for the longest delay the
node's ranges allow (twice the maximum Delay, plus the through-zero reference, plus the interpolation margin) and
are written with the Delay's soft limit (±4) so a feedback loop can't build up beyond it.

**(X7) The Flanger.** One tap per channel on its delay line, with the same formula as the Chorus (X5) over shorter
ranges: **Delay** 0.1–10 ms (default 2 ms), **Depth** 0–100 % (default 70 %). **Feedback** −95 % to +95 % (default
50 %) feeds the tap back into the line: positive feedback sharpens the peaks between the notches, negative feedback
moves them by half a comb spacing and gives the hollower sound. A linear sweep of the delay is the classic tape and
bucket-brigade shape: the notches race upwards as the delay nears its minimum (the "jet" at Depth 100 %).

**(X8) Through-zero flanging.** A `throughZero` toggle (default off; whether v1 has it is question 1). Tape flanging
sweeps one tape against another, so the delay *difference* passes through zero and the notches cancel the whole
spectrum for a moment. A delay line can't read ahead of its input, so the dry path is delayed instead: with
Through-zero on, the dry signal and the feedback both go through a fixed reference delay of 5 ms, and the swept tap
sits at

    d(t) = 5 ms + min(Delay · Depth, 5 ms) · m(t)

so it crosses the reference twice per cycle. The node reports 5 ms of latency while it is on, so the track's delay
compensation keeps it in time; the toggle is `changesLatency`, and therefore neither automatable nor modulatable (as
the Saturator's Oversampling). The sweep's amplitude is capped at the reference, so the tap never reads closer than
the minimum.

**(X9) Mix: linear.** `out = dry · (1 − Mix) + wet · Mix`, 0–100 %, default 50 % for all three. A flanger's and a
phaser's notches are only complete when dry and wet are equal in level, which a linear mix gives at 50 % and an
equal-power mix (the reverbs') never does. In through-zero mode the dry is the delayed reference.

**(X10) The Phaser.** A chain of first-order allpass filters per channel, all at the same swept frequency, its output
mixed with the dry signal: each pair of stages adds one notch.

* **Stages**: 2, 4, 6, 8, 10 or 12 (a choice, default 4), giving 1 to 6 notches. All 12 stages always run (a few
  multiply-adds each), and the output and feedback are taken after stage N; changing N crossfades from the old
  tap to the new one over 20 ms, so it doesn't click. Not modulatable (a choice), but automatable.
* **Centre**: 50 Hz–8 kHz (default 700 Hz, logarithmic knob, mid 700 Hz), the allpass frequency at the LFO's zero.
* **Depth**: 0–100 % (default 60 %): the sweep in octaves, ±4 octaves at 100 %, `f(t) = Centre · 2^(4 · Depth · m(t))`,
  clamped to 20 Hz – 0.45 × the sample rate. An exponential sweep moves the notches evenly in pitch, as the ear
  hears them.
* **Feedback**: −95 % to +95 % (default 40 %), from the last used stage back to the chain's input, through a one-sample
  delay (the classic structure). Positive and negative feedback emphasise the peaks or the notches in different
  places; the loop's gain is at most 0.95 since an allpass has unit gain, and the written value is soft-limited as in
  X6.

Each stage is `y = a·x + x₁ − a·y₁` with `a = (tan(πf/fs) − 1) / (tan(πf/fs) + 1)`; the coefficient is computed once
per sample per channel (one `tan`) and shared by the channel's stages.

**(X11) Per-channel state, shared LFO.** Each node has one `StereoLfo` (left and right phases), two delay lines or
two allpass chains, and no cross-channel paths: left feeds back into left, right into right. Cross-feedback (left
into right) is a non-goal for v1.

**(X12) Smoothing.** Every continuous parameter (Rate, Delay, Depth, Feedback, Spread, Centre, Mix) goes through
`followParam`: a 20 ms ramp when set or automated, per-sample following (1 ms) when modulated. A moving Delay or
Depth changes the delay continuously, which is a brief pitch glide, never a jump. Rate ramps the increment (X4), so
the LFO's phase stays continuous. Spread ramps the right channel's offset. Shape changes crossfade from the old
shape's value to the new one over 20 ms. Stages crossfade (X10). Sync and Cycle are steps by nature; a cycle change
only changes the increment (and, while playing, the slewed target phase), so it doesn't click either.

**(X13) Voice graphs, latency, reset.** All three are allowed in voice graphs: a Chorus at 40 ms maximum Delay holds
about 8 KB per channel at 48 kHz, and the CPU cost per instance is small. `latencySamples()` is 0, except the
Flanger's 5 ms in through-zero mode. `reset()` clears the lines, the allpass states and the feedback, and puts the LFO
at phase 0 (as a fresh node), so reset equals fresh and the voice graph table in ModulationTests.cpp covers them.

**(X14) Panel meters.** `panelMeters()` returns `{ phase }`, the left LFO's phase (0–1), so the panel can mark where
the sweep is. Nothing else is needed: the sweep's curves are drawn from the parameters with the engine's formulas.

### Parameters

Chorus (`dawg.chorus`):

| # | id | name | range | default | unit | modulatable |
|---|----|------|-------|---------|------|-------------|
| 0 | `sync` | Tempo sync | off, on | off | | no |
| 1 | `rate` | Rate | 0.01..10 | 0.8 | Hz (mid 1) | yes |
| 2 | `cycle` | Cycle | lfoCycleChoices | 1/1 | | no |
| 3 | `shape` | Shape | Sine, Triangle | Sine | | no |
| 4 | `spread` | Spread | 0..180 | 90 | ° | yes |
| 5 | `delay` | Delay | 5..40 | 15 | ms | yes |
| 6 | `depth` | Depth | 0..100 | 15 | % | yes |
| 7 | `voices` | Voices | 1..4 | 2 | | no |
| 8 | `feedback` | Feedback | 0..80 | 0 | % | yes |
| 9 | `mix` | Mix | 0..100 | 50 | % | yes |

Flanger (`dawg.flanger`): `sync`, `rate` (default 0.2 Hz), `cycle` (2/1), `shape` (Triangle), `spread` (90°), then
`delay` (0.1..10 ms, 2 ms), `depth` (0..100 %, 70 %), `feedback` (−95..95 %, 50 %), `throughZero` (off, on; off;
changes latency, not automatable or modulatable), `mix` (50 %).

Phaser (`dawg.phaser`): `sync`, `rate` (default 0.4 Hz), `cycle` (1/1), `shape` (Sine), `spread` (90°), then
`stages` (2, 4, 6, 8, 10, 12; 4), `centre` (50..8000 Hz, 700 Hz, mid 700), `depth` (0..100 %, 60 %), `feedback`
(−95..95 %, 40 %), `mix` (50 %).

The LFO's parameters come first and in the same order on all three, so a panel helper and the tests can treat them
alike.

### The panels (X-panel)

`ui/src/dock/ModulationEffectPanels.tsx` (one panel component, exported for each of the three types) and
`modulationEffects.ts` (the formulas, tested in `modulationEffects.test.ts`):

* **Header row**: Sync (button), the Cycle select when synced, and the Shape as a segmented control; the Flanger
  adds a "Through-zero" button.
* **Graph**: the sweep over one LFO cycle, time to the right: the delay in ms (Chorus, Flanger) or the allpass
  frequency on a log axis (Phaser), the left channel's curve in the accent colour and the right's dimmer, offset by
  Spread, so Spread, Depth and Shape are visible at a glance; the Chorus draws each voice. A vertical line marks the
  current phase from the meters. Through-zero draws the reference delay as a dashed line.
* **Controls**: sections LFO (Rate or nothing when synced, Spread), and Delay (Delay, Depth, Voices) or Sweep (Stages,
  Centre, Depth), and Output (Feedback, Mix). Voices and Stages are small segmented controls inside their section.
* **Layout**: as the Clipper's: the graph in `panel-main` takes the width left beside `panel-sections` down to a
  floor, its height the body's less the header, so the card's measured min-content width keeps the controls off it
  at every dock height and card width.
* **Card widths** (`deviceWidths.ts`): initial 560 px, minimum 440 px (the two-row layout measured 436 px in the
  app); in a short dock the one-row layout is wider, and the card grows to its measured min-content width.
* **Presets** (`presets.ts`): Chorus "Subtle" (defaults), "Wide" (Spread 180°, 3 voices), "Vibrato" (Mix 100 %,
  Spread 0°, Depth 40 %, Rate 5 Hz, 1 voice), "Resonant" (Feedback 60 %, Delay 8 ms); Flanger "Classic"
  (defaults), "Jet" (Depth 100 %, Feedback 85 %, Rate 0.1 Hz), "Hollow" (Feedback −70 %), "Tape" (Through-zero,
  Feedback 0 %, Rate 0.15 Hz); Phaser "Gentle" (defaults), "Deep" (8 stages, Feedback 70 %), "Swirl" (12 stages,
  Rate 0.15 Hz, Spread 180°), "Synced" (sync, 1/4).

## 4. Signal flow

```
Chorus, per channel:
in ─┬────────────────────────────────────────────────────────────► dry ─┐
    └─► (+) ─► delay line ─► voices' taps (LFO + k/V) ─► Σ / √V ─► wet ─┴─ linear Mix ─► out
         ▲                                              │
         └──────────────── feedback ────────────────────┘

Flanger, per channel (through-zero: dry and feedback via the 5 ms reference tap):
in ─┬───────────────────────────────────────────► dry ─┐
    └─► (+) ─► delay line ─► swept tap ──────────► wet ─┴─ linear Mix ─► out
         ▲                     │
         └──── feedback ───────┘

Phaser, per channel:
in ─┬─────────────────────────────────────────────────────────► dry ─┐
    └─► (+) ─► allpass × 12 (one swept a) ─► tap after N (crossfaded) ─► wet ─┴─ linear Mix ─► out
         ▲                                              │
         └────────── z⁻¹ · feedback ────────────────────┘
```

## 5. Tests (tests/ModulationEffectsTests.cpp)

* **Descriptors**: types, categories, ports, parameter order and defaults, voice graphs allowed, Through-zero
  `changesLatency`.
* **Stereo**: a stereo input with different signals per channel: each output channel depends only on its own input
  (feeding silence into one side leaves that output silent).
* **Mono widening**: a mono input at Spread 90° gives left and right outputs that differ (correlation below a bound),
  and at Spread 0° outputs that are equal sample for sample.
* **L/R phase offset**: through `StereoLfo` directly, the right phase is the left's plus Spread / 360. Through the
  nodes: with Mix 100 % and Feedback 0, a click train's echoes give each channel's delay over time (Chorus,
  Flanger), and the right channel's delay curve equals the left's shifted by Spread / 360 of a cycle; for the Phaser,
  the right output on a mono input equals the left output of a second Phaser whose LFO starts Spread later.
* **LFO**: shapes' values and ranges, the free rate, the synced rate at a tempo, following the song position while
  playing and slewing on a jump without a step larger than a bound.
* **Interpolation**: a constant fractional delay returns a sine within 0.5 dB at 10 kHz, and a moving delay leaves
  a sine's level within a bound.
* **Delay and Depth**: Depth 0 gives the dry signal delayed by Delay; the Flanger's comb notch sits at `1 / (2 d)`.
* **Through-zero**: the latency is reported, and at the zero crossing the wet signal equals the delayed dry (Mix 50 %,
  Feedback 0), so the notches leave the spectrum entirely.
* **Phaser**: at Depth 0, N stages place N / 2 notches at the analytic frequencies; changing Stages doesn't click.
* **Bounded under feedback**: full-scale white noise and a full-scale sine at the worst settings (feedback at its
  limits, depth 100 %, fastest rate, every stage count) for 10 s stay below a fixed bound, never NaN or infinite.
* **Block-size independence**: the same input in blocks of 1, 17, 64, 333 and 4096 samples gives bit-identical output
  for each node, free and synced.
* **No allocation** in `process()` (`AllocationGuard`), including parameter and shape changes, in Debug.
* **Smoothing**: a jump of each continuous parameter while a sine plays gives no sample-to-sample step above a bound.
* **Mix and bypass**: Mix 0 % is the dry signal, bypassed is the input, Mix 100 % only the wet.
* **Reset equals fresh**, through the voice graph table in ModulationTests.cpp.
* **UI** (`ui/src/dock/modulationEffects.test.ts`): the sweep formulas against the engine's (the same cases), the
  Spread offset, the phaser's frequency clamp.

## 6. Summary

| | Chorus | Flanger | Phaser |
|---|---|---|---|
| Node | `dawg.chorus` | `dawg.flanger` | `dawg.phaser` |
| Core | delay line, 1–4 taps per channel | delay line, 1 tap per channel | 2–12 first-order allpasses per channel |
| Sweep | Delay · (1 + Depth · m), 5–40 ms | Delay · (1 + Depth · m), 0.1–10 ms; or ±5 ms through zero | Centre · 2^(±4 oct · Depth · m) |
| Feedback | 0–80 % | ±95 % | ±95 % |
| LFO | Sine, Triangle; Hz or tempo sync; Spread 0–180° | same | same |
| Interpolation | 4-point Catmull-Rom | 4-point Catmull-Rom | — |
| Latency | none | none; 5 ms through zero | none |

## 7. Owner's answers (2026-11-08)

1. **Through-zero:** yes, as a toggle, off by default (X8).
2. **Synced LFO:** follows the song position while playing (X4).
3. **Shapes:** Sine and Triangle only (X3).
4. **Voices and voice graphs:** up to four voices, default two; all three allowed in voice graphs (X5, X13).

The questions as asked:


1. **Through-zero flanging in v1?** It needs the dry signal delayed by a 5 ms reference, so the Flanger reports 5 ms of
   latency while it's on (compensated on the track like any other latency), and its sweep is capped at ±5 ms around
   the reference (X8). Recommended: yes, as a toggle, off by default.
2. **A synced LFO while playing**: (a) follow the song position, so every playback and export sweeps the same way on
   the same beat, slewing over 50 ms after a jump instead of clicking (X4); or (b) only take its rate from the tempo
   and run free like an unsynced one. Recommended: (a).
3. **LFO shapes**: Sine and Triangle only (X3), or also a smooth random shape (a slowly wandering, band-limited noise)
   for a less regular chorus? Recommended: Sine and Triangle in v1; random later, with the LFO node's own shapes.
4. **Chorus voices**: up to four taps per channel (X5), or keep v1 to one tap per channel, as a classic two-channel
   stereo chorus? Recommended: up to four, default two.
5. **Voice graphs**: allow all three (X13)? Recommended: yes.

## 8. Implementation notes

* **Files.** `engine/include/dawg/ModulationEffects.h`: `lfoValue`, `phaseDifference`, `StereoLfo`, `DelayLine`,
  `allpassCoefficient` and `AllpassStage`. The nodes are `ChorusNode`, `FlangerNode` and `PhaserNode` in EffectNodes.cpp,
  on a shared `ModulationEffectNode` that ramps every continuous parameter (`followParam`) and works out the LFO's
  per-sample increment and song-position target for each block. The node types are `nodetype::chorus`, `flanger` and
  `phaser`.
* **The Chorus's Spread** (X5) is a share of the gap between voices, `Spread / 360 / V`: as first written, a whole
  `Spread / 360` put two voices at 180° exactly on the left voices, so the two channels were identical.
* **The LFO** advances before each sample is read. It takes a song position at once only if nothing has run since a
  reset (a fresh node, an export pass); once it has run free, a song position that appears (playback starting) is
  slewed to (X4).
* **Units.** The Spread's unit is the degree sign; `toVar` now reads parameter units as UTF-8, and the UI formats "°"
  without a space, as it does "%".
* **Through-zero** follows the Clipper's pattern for a latency-changing choice: `configure()` stores it, the compiler
  reads `latencySamples()` after the recompile, and the audio thread switches at its next block, clearing the delay
  lines.
* **Tests.** `tests/ModulationEffectsTests.cpp`: descriptors, the LFO's shapes, offset and slew, per-channel
  independence, mono widening and Spread 0, the right channel equal to a second node's left one started Spread later
  in the song (all three, 45°, 90° and 180°), block-size independence (bit-identical free, within 1e-5 synced), bounded
  output under every feedback extreme for 10 s, no allocation (Debug only), delays and comb notches at Depth 0,
  through-zero latency and exact delayed output at the crossing, the Phaser's analytic notch frequencies, click-free
  control changes, Mix 0 and bypass, the phase meter. Reset equals fresh is in the table in ModulationTests.cpp.
* **Checked in the running app** (a separate dev instance, 2026-11-08): the three nodes inserted on a track, each card
  at dock heights of 140, 180, 220, 300 and 520 px with card widths asked for of 300, 560 and 1200 px: no control over
  the graph or outside the body, and no scrolling; synced (the Cycle select) and through-zero (the dashed reference)
  drawn. The sections needed `min-width: max-content`: `.panel-section`'s `min-width: 0` let the grid's min-content
  width collapse, so at first the card didn't grow for the one-row layout. Not checked by hand: listening.
