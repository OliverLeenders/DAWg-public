# Design: Gate and Expander

Status: **implemented** (notes in section 8); the owner's answers are in section 7. The decisions are numbered N1–N14.

## 1. Goals

* Two built-in downward dynamics processors next to the Compressor: a **Gate** (`dawg.gate`), which mutes or
  attenuates the signal while it is below a threshold, and an **Expander** (`dawg.expander`), which turns quiet
  passages down by a ratio. Drum spill, amp hiss and room noise between phrases no longer need a plugin.
* A **sidechain** like the Compressor's (docs/design/sidechain.md): a Key input, a key from another track through a
  key send, a filter on what the detector hears, and Listen.
* Hysteresis, hold and optional lookahead on the Gate, so it opens on the first transient and doesn't chatter on a
  decaying note.
* No clicks: the gain moves along ramps, never in steps, except where the user asks for the fastest attack.
* Real-time safe (every buffer sized in `prepare()`), block-size independent (sample-exact state machines), cheap
  enough for voice graphs.
* A panel that reads like the Compressor's: the transfer curve with handles, the level history, a gain-reduction
  meter and knobs.

These are separate from the ADSR's gate, which is a modulation trigger, not an audio processor.

Non-goals for v1:

* Upward expansion, and the Gate's "flip" (ducking) mode. A ducker is a Compressor keyed from another track.
* A gate that emits a trigger or MIDI note when it opens (gate-to-MIDI, drum replacement).
* Frequency-selective gating (a gate acting on one band only).
* A Mix control. Parallel gating is unusual; a Mix node does it.

## 2. What it reuses

* **The effect plumbing** in EffectNodes.cpp: `EffectNode` for bypass, `followParam` for 20 ms ramps, `differs`,
  `gainToDb` / `dbToGain`, the `Biquad` with its matched high-pass and low-pass designs (`setPass`).
* **The sidechain** (sidechain.md, K1–K6): the `keyedModPorts` (Audio, Key), the `Detector` choice, key sends and
  key buses, `setKeyLateSamples`. `isKeyable` already accepts any built-in node whose second input is audio, so the
  key routing in the engine needs no change.
* **The Compressor's meters**: the 10 ms slot history and `meters()` (N12), moved into a shared helper.
* **The panels**: `useNodeDrag`, `useResponse`, `useNodeAnalysis`, `Knob`, `Section`, `KeySelector`, `drawHandle`,
  `CompressorHistory` and the GR meter, and the card's measured min-content width (`panelMinWidth.ts`).

## 3. Decisions

**(N1) Two nodes, one shared core.** `dawg.gate` ("Gate") and `dawg.expander` ("Expander"), `NodeCategory::effect`,
with the Compressor's ports: a stereo Audio input, a stereo Key input (with modulation inputs) and a stereo output.
They are two nodes rather than one with a mode, because they differ in parameters (the Gate's hysteresis against the
Expander's ratio and knee), in their gain computer (a state machine against a static curve with ballistics) and in
their panels, and a Gate and an Expander are what users look for in the device list. What they share lives in one
base class in EffectNodes.cpp, `DownwardDynamicsNode`: the detector (N3), the key filters and Listen (N4), the
lookahead delay (N5), hold, and the meters (N12). The subclasses only turn a detector level into a target gain.

**(N2) Sharing with the Compressor: the meters and the key path, not the gain computer.** The Compressor's history
and `meters()` move into a `DynamicsMeters` helper that the Compressor, Gate and Expander all use, so the three
panels get the same data. The key filter, as a small `KeyFilter` (high-pass and low-pass biquads with on/off), is
shared too, and the Compressor gets the same high-pass and low-pass (question 3): its `keyFilter` and `keyFreq`
become `keyHp`, `keyHpFreq`, `keyLp` and `keyLpFreq` with the ranges of N4. The Compressor's gain computer (soft knee,
one-pole attack and release on the reduction) stays its own; the Expander's ballistics follow the same form (N8).

**(N3) The detector: stereo-linked, Peak or RMS.** One level for both channels, so the stereo image never shifts
while the gain moves: the larger of the two channels' detector levels. A `detection` choice per node:

* **Peak** (the Gate's default): the rectified key, `max(|L|, |R|)`, through a peak follower with an instant attack
  and a 10 ms release (a one-pole decay). Instant attack is what a gate needs to open on the first transient; the
  decay bridges the zero crossings of a sustained note, and Hold (N7) bridges the rest.
* **RMS** (the Expander's default): the mean square of each channel through a one-pole with a 10 ms time constant,
  then the larger channel's square root. Smoother, closer to loudness, the natural choice for gentle expansion.

The detector level is computed per sample in dB. The history draws it as the key line while keyed; the level dot,
as on the Compressor, is the audio's input and output peak.

**(N4) The key path: Detector, high-pass, low-pass, Listen.** As the Compressor (K6), plus a low-pass:

* `detector`: Internal or Sidechain (the Key input). Choosing a key source in the panel switches it to Sidechain.
* `keyHp` on/off and `keyHpFreq` 20 Hz–5 kHz (default 100 Hz), `keyLp` on/off and `keyLpFreq` 100 Hz–20 kHz (default
  8 kHz): 12 dB/octave matched designs on what the detector hears, not on the audio. A kick gate with the low-pass
  at 150 Hz ignores the hi-hat spill; a snare gate with the high-pass at 200 Hz ignores the kick.
* `listen`: outputs the filtered key instead of the processed audio, to tune the filters.

Wider filter ranges than the Compressor's 20–500 Hz, because a gate's key filter is used to pick out one drum.

**(N5) Lookahead: a choice, with its latency reported.** `lookahead`: Off, 0.5, 1, 2, 5 or 10 ms (default Off). The
audio goes through a delay of that length while the detector reads the undelayed key, so the gain starts moving
before the transient arrives: with a lookahead at least as long as the attack, the Gate opens fully by the time the
first sample of the hit plays, and no transient is cut. The node reports the lookahead as its latency, so the track's
delay compensation keeps it in time. Like the Saturator's Oversampling, the choice is `changesLatency`: a change
recompiles the graph, and it is neither automatable nor modulatable. That is why it is a choice and not a knob: a
continuous control would recompile on every step of a drag. Listen plays the key undelayed (it is a monitoring aid).
The delay line is sized in `prepare()` for 10 ms; changing the lookahead resets it (it recompiles anyway).

**(N6) The Gate's thresholds and hysteresis.** `threshold` (−80..0 dB, default −40 dB) is where the gate **opens**;
`hysteresis` (0..24 dB, default 6 dB) puts the **close** threshold that far below it. The gate opens when the
detector level rises above the threshold and starts closing only when it falls below the close threshold, so a
signal hovering around one level doesn't chatter. At 0 dB hysteresis both are the same.

**(N7) Hold.** `hold` (0..500 ms, default 20 ms): once the level falls below the close threshold, the gate stays
fully open (Gate) or keeps its gain (Expander) for the hold time before releasing. A level back above the close
threshold during the hold restarts it. Hold is counted in samples, so it is exact and independent of the block size.

**(N8) Gain ballistics.**

* **Gate**: a state machine (closed, opening, open, holding, closing) drives the gain, in exact ramps rather than
  one-poles, so Attack and Release mean what they say:
  * **Attack** (0.01..100 ms, default 0.5 ms, log knob): opening ramps the gain **linearly in amplitude** from the
    floor to 1 in the attack time. A linear amplitude ramp lets a transient through at once; a ramp in dB would hold
    the first milliseconds near the floor and soften the hit.
  * **Release** (1..2000 ms, default 100 ms, log knob): closing ramps the gain **linearly in dB** from 0 dB to the
    floor (N9) in the release time: an exponential decay, which sounds like a natural fade.
  * A reopen during the release ramps up from where the gain is, at the attack's rate. The gain never steps; the
    shortest attack (0.01 ms, under one sample at 48 kHz) is the only way to get a hard edge, and the user asks for
    it.
* **Expander**: the static curve (N10) gives a target gain change in dB, and the gain follows it as the Compressor's
  does: a one-pole in dB, with the attack's time constant when the gain rises (opening) and the release's when it
  falls, and Hold delaying the start of a fall. Attack 0.01..100 ms (default 1 ms), Release 1..2000 ms (default
  150 ms).

The gain is applied per sample, both channels by the same gain (N3).

**(N9) The range (floor).** `range` (−80..0 dB): how far the gain goes down. The Gate's default is −80 dB, which
means fully closed (gain 0; the knob shows "−∞" at its end); a range of −10 dB to −20 dB gives the gentler gating
used on drum overheads and dialogue. On the Expander (default −40 dB) the range caps the curve's gain change, so a
2:1 expander doesn't push the noise floor to silence.

**(N10) The Expander's curve: ratio and a soft knee.** `ratio` 1..20 (default 2, i.e. 1:2, shown "1:2"): below the
threshold, each dB the input falls costs `ratio − 1` more dB of gain:

    gainChange = max(range, (ratio − 1) · min(0, level − threshold))

with the Compressor's 6 dB soft knee around the threshold (the same quadratic blend), so the curve has no corner
there. At ratio 20 the Expander is close to a gate without hysteresis. `threshold` −80..0 dB, default −40 dB. Only
the Expander has a ratio: a gate's curve is a step.

**(N11) Smoothing of the controls.** The thresholds, hysteresis, ratio and range move the target, which the
ballistics already smooth; Attack, Release and Hold set rates. None needs its own ramp, as the Compressor's don't.
They are modulatable (modulation is read per block, like the Compressor's). The key filter frequencies recompute
the biquads when they move (as the Compressor's high-pass). Toggles and choices are steps; turning a key filter on
switches the detector's input, which the detector's own envelope smooths.

**(N12) Meters.** `DynamicsMeters` (N2), as the Compressor's `meters()`: `{ history, reduction, input, output,
keyLateMs }` with a [input, output, reduction, key] quadruple per 10 ms slot, plus `open` on the Gate (whether it is
open or holding at the last slot) for the panel's open light. Reduction is the gain change in dB (0 or negative,
at most the range; −80 when closed).

**(N13) Voice graphs, latency, reset.** Both nodes are allowed in voice graphs (a gate per voice is cheap: a few
multiplies, a 10 ms delay line). `latencySamples()` is the lookahead. `reset()` closes the Gate (gain at the floor,
detector at silence) and puts the Expander at its resting gain, the gain for silence, so a fresh node and a reset
node behave alike, and the first notes open the gate through its attack as they would after a pause.

**(N14) The panels.** One component for both types in `ui/src/dock/GatePanel.tsx`, the Compressor panel's layout:

* **Transfer curve** (input on x, output on y, −80..+6 dB): the Gate draws its step, with the open threshold and the
  close threshold as two verticals joined by arrows (the hysteresis loop); the Expander draws its curve with the knee
  and the range's flat floor. Handles: **T** (threshold, drag sideways), **H** on the Gate (close threshold, drag
  sideways, sets the hysteresis), **R** on the Expander (ratio, drag the curve's lower end up or down), and **F**
  (range, the floor, drag up or down; at 0 dB in, where the floor's output is the range). The detector level is the dot on the curve.
* **History** beside it (CompressorHistory: input, output, key while keyed, the reduction) with the threshold line,
  and the Gate's close threshold as a second, dashed line. The GR meter in the history's place when narrow.
* **An open light** on the Gate in the readout (lit while open or holding).
* **Sections**: Timing (Attack, Hold, Release), Detector (Peak/RMS segmented, Lookahead select), and Sidechain
  (Source via KeySelector, HP and LP buttons with their frequency knobs, Listen), the Compressor's Sidechain column.
* **Layout and widths**: the Compressor's `useCurveLayout` (full, meter, compact), reporting its min-content width
  to the card, so no control covers the curve at any dock height or card width. `deviceWidths.ts` gets entries for
  both types, measured in the app.
* **Presets** (`presets.ts`): Gate "Default", "Kick" (threshold −30 dB, attack 0.05 ms, hold 30 ms, release 80 ms,
  key LP 150 Hz, lookahead 1 ms), "Snare" (threshold −28 dB, key HP 200 Hz, hold 40 ms, release 120 ms, lookahead
  1 ms), "Toms" (threshold −35 dB, hold 80 ms, release 300 ms, key LP 300 Hz), "Gentle" (range −12 dB, hysteresis
  4 dB, release 250 ms); Expander "Default", "Noise" (threshold −50 dB, ratio 2, range −20 dB, release 200 ms),
  "Vocal" (threshold −45 dB, ratio 1.5, range −12 dB, RMS), "Tight" (ratio 4, range −40 dB, release 80 ms).

The UI's sidechain code generalises from "the Compressor" to "a node with a `detector` parameter": `KeySelector`
switches the detector for any such node, and the Sends list's "add key send" does the same. The engine's error
"Keys go to a compressor or a plugin with a sidechain input" becomes "to a keyable node".

### Parameters

Gate (`dawg.gate`):

| # | id | name | range | default | unit | notes |
|---|----|------|-------|---------|------|-------|
| 0 | `threshold` | Threshold | −80..0 | −40 | dB | open threshold |
| 1 | `hysteresis` | Hysteresis | 0..24 | 6 | dB | close = threshold − hysteresis |
| 2 | `range` | Range | −80..0 | −80 | dB | −80 is fully closed |
| 3 | `attack` | Attack | 0.01..100 | 0.5 | ms | log, mid 1 |
| 4 | `hold` | Hold | 0..500 | 20 | ms | mid 50 |
| 5 | `release` | Release | 1..2000 | 100 | ms | log, mid 100 |
| 6 | `lookahead` | Lookahead | Off, 0.5, 1, 2, 5, 10 ms | Off | | changes latency |
| 7 | `detection` | Detection | Peak, RMS | Peak | | |
| 8 | `detector` | Detector | Internal, Sidechain | Internal | | |
| 9 | `keyHp` | Sidechain HP on | off, on | off | | |
| 10 | `keyHpFreq` | Sidechain HP | 20..5000 | 100 | Hz | log, mid 200 |
| 11 | `keyLp` | Sidechain LP on | off, on | off | | |
| 12 | `keyLpFreq` | Sidechain LP | 100..20000 | 8000 | Hz | log, mid 2000 |
| 13 | `listen` | Listen | off, on | off | | |

Expander (`dawg.expander`): `threshold` (−80..0 dB, −40), `ratio` (1..20, 2, shown 1:n), `range` (−80..0 dB, −40),
`attack` (0.01..100 ms, 1), `hold` (0..500 ms, 10), `release` (1..2000 ms, 150), then `lookahead`, `detection`
(default RMS), `detector`, `keyHp`, `keyHpFreq`, `keyLp`, `keyLpFreq`, `listen` as the Gate's. The shared parameters
come in the same order from `range` on (the Gate's `hysteresis` sits where the Expander's `ratio` does), so the base
class reads them by the same indices.

## 4. Signal flow

```
audio ──────────────────────────► lookahead delay ──► × gain ──► out (Listen: the filtered key)
                                                         ▲
key (Internal: audio) ─► HP? ─► LP? ─► detector (Peak/RMS, linked) ─► gain computer ─► ballistics (hold)
                                                         Gate: state machine, ramps; Expander: curve, one-pole
```

## 5. Tests (tests/GateExpanderTests.cpp)

* **Descriptors**: types, ports (keyable), parameter order and defaults, Lookahead `changesLatency`, voice graphs.
* **Thresholds and hysteresis**: a tone stepping between levels opens the Gate above the threshold, keeps it open
  between the close and open thresholds, and closes it below the close threshold; at 0 dB hysteresis both coincide.
* **Hold**: after the level drops, the gain stays at 1 for the hold time (to the sample, plus the detector's decay),
  and a level above the close threshold during the hold restarts it.
* **Range**: closed, the output is the input times the range's gain; at −80 dB, silence.
* **Expander ratio**: steady tones below the threshold come out with `(ratio − 1)` dB of reduction per dB, within
  the knee's tolerance; the range caps it.
* **Attack and release timing**: the Gate's gain reaches 1 exactly `attack` after opening and the floor exactly
  `release` after the hold ends; the Expander's one-pole reaches 63 % in its time constant.
* **Lookahead**: the reported latency, and the gain fully open at the transient's first sample when lookahead ≥
  attack.
* **Stereo linking**: a signal in the left channel only opens the gate for both channels by the same gain.
* **Sidechain**: keyed, a silent audio input's gate follows the key; the key filters reject a tone outside their
  band; Listen outputs the filtered key.
* **Block-size independence**: blocks of 1, 17, 64, 333 and 4096 samples give bit-identical output, with
  lookahead on and off.
* **No allocation** in `process()` (`AllocationGuard`), in Debug.
* **No clicks**: opening and closing on a sine at the default attack and release give no sample-to-sample step above
  a bound.
* **Reset equals fresh**, through the voice-graph table in ModulationTests.cpp.
* **UI**: the curve formulas against the engine's response, and the presets' values in range (presets.test.ts).

## 6. Summary

| | Gate | Expander |
|---|---|---|
| Node | `dawg.gate` | `dawg.expander` |
| Gain computer | open/close state machine with hysteresis | 1:ratio below threshold, 6 dB soft knee |
| Ballistics | exact ramps: attack linear in amplitude, release linear in dB | one-pole in dB, as the Compressor |
| Floor | Range, −80 dB = closed | Range caps the reduction |
| Hold | yes | yes |
| Detector | Peak (default) or RMS, stereo-linked | RMS (default) or Peak |
| Sidechain | Key input, HP and LP, Listen | same |
| Lookahead | Off–10 ms, reported as latency | same |

## 7. Owner's answers (2026-11-08)

1. **Two nodes**, Gate and Expander, on one shared core (N1).
2. **Lookahead as a choice** of fixed values, reported as latency (N5).
3. **The Compressor gets the same key filters now** (N2, N4).
4. **Exact ramps** for the Gate (N8).

The questions as asked:


1. **Two nodes or one?** Two nodes, Gate and Expander, on one shared core (N1), or one "Gate / Expander" node with a
   mode switch? Recommended: two nodes.
2. **Lookahead**: a choice of fixed values (Off, 0.5, 1, 2, 5, 10 ms) that adds that much latency and recompiles
   the graph when changed (N5); a continuous knob instead (recompiles while dragging); or no lookahead in v1?
   Recommended: the choice.
3. **The Compressor's key filter**: the Gate and Expander get a high-pass and a low-pass with wider ranges (N4).
   Give the Compressor the same now (its `keyFreq` range changes; no compatibility to keep), or leave it as it is?
   Recommended: leave it for now, a small follow-up.
4. **Gate ballistics**: exact ramps (attack linear in amplitude, release linear in dB, N8), or one-poles like the
   Compressor's? Recommended: exact ramps, which make Attack and Release mean "time to fully open or close".

## 8. Implementation notes

* **Files.** The nodes are `GateNode` and `ExpanderNode` in EffectNodes.cpp, on `DownwardDynamicsNode` (detector,
  key path, lookahead, hold, meters), with `KeyFilter` and `DynamicsMeters` shared with `CompressorNode`. The types
  are `nodetype::gate` and `nodetype::expander`; `expanderGainChangeDb` (BuiltinNodes.h) exposes the curve for the
  tests. Tests in tests/GateExpanderTests.cpp, and both types in the reset-equals-fresh table (ModulationTests.cpp).
* **The Compressor's key path** is now `detector`, `keyHp`, `keyHpFreq` (20 Hz–5 kHz), `keyLp`, `keyLpFreq`
  (100 Hz–20 kHz), `listen`: `keyFilter` and `keyFreq` are gone (question 3).
* **Curves in the panel.** The panel draws both curves from `ui/src/dock/dynamicsCurves.ts` (tested against the
  engine's cases in `dynamicsCurves.test.ts`) rather than a `nodes.response` round trip: the Gate's is a step and the
  Expander's one formula.
* **Ranges at the bottom.** The Gate's −80 dB is silence (gain 0) as N9 says; the Expander's −80 dB is −80 dB.
* **Lookahead** is read when the graph is configured or prepared, as the Saturator's Oversampling: `reset()` keeps
  the compiled value, so the latency the compiler read and the delay always agree.
* **The panel** is `GatePanel` for both types, on the Compressor's layout (`dynamicsLayout.ts`, moved out of
  CompressorPanel.tsx) and its Sidechain column (`SidechainSection.tsx`, now with HP, LP and Listen). The Detector
  section (Peak/RMS, Lookahead) sits under Timing, beside it when the dock is too short for both. `KeySelector`'s
  `hasKeyDetector` names the nodes whose Detector a key source switches (Compressor, Gate, Expander).
* **F at 0 dB in.** First placed left of the close threshold, the F handle could only reach a shallow range before
  leaving the plot; at 0 dB in, its height is the range itself. The Gate draws its floor on, dashed, past the
  threshold to reach it.
* **The history's canvas** now shrinks with the dock (`.compressor-history .panel-canvas`): it kept its drawn height
  when the dock got shorter, which scrolled the Compressor's card too.
* **App check** (own Vite on 5291, CDP 9241, own WebView2 profile): docks 140–520 px, cards 300, 640 and 1200 px,
  key filters off and on; no control overlaps the curve or leaves the card. Below about 230 px the curve's 120 px floor
  and its readout scroll the body a little, as the Compressor's do. The handles were dragged and set their parameters.
  Not checked with live audio (the meters are the Compressor's, tested in the engine).
