# Design: transparent ADAA (Vicanek) for the Saturator, Clipper and Waveshaper

Status: **implemented** (2026-11-05; notes in section 10). The owner chose to switch the three shapers to Vicanek's compensated ADAA after the
measurements in section 2. The decisions are numbered V1–V11 and summarised in section 9. The owner's answers are in
section 8.

## 1. Goals and non-goals

Goals:

* **Flat in the linear regime.** Where a curve is a straight line (quiet signals, the body of a clipped signal), the
  three shapers pass audio unchanged in level at every frequency up to Nyquist, at every oversampling choice. Today
  first-order ADAA's `(u + u₁) / 2` loses 11.7 dB at 20 kHz with oversampling Off and 2.0 dB at 2x (saturator.md, W7).
* **Closer to the continuous-time result.** Driven harmonics come out within about 0.5 dB of the exact
  continuous-time harmonics of the curve, instead of up to 2.2 dB low at Off.
* **The same anti-aliasing family.** Aliasing stays within about 2 dB of today's at every setting. The method is still
  first-order ADAA, with a closed-form antiderivative per curve.
* **Mix never combs.** The dry path keeps matching the wet path's linear response exactly.
* One implementation, shared by the three nodes.

Non-goals:

* Higher-order ADAA. Vicanek and Bilbao et al. note that it adds zeros in the band and needs more oversampling. The
  oversampling choices stay.
* Compensating the sub-sample delay. It stays below 1.5 samples at the shaper's rate and uncompensated, as today
  (V6).
* Keeping plain ADAA as an option (Q4).

## 2. Background and measurements

M. Vicanek, *Note on Alias Suppression in Digital Distortion* (2023, revised 2024,
<https://www.vicanek.de/articles/AADistortion.pdf>), refines Parker, Zavalishin and Le Bivic (DAFx 2016) in two steps.

1. **Delay the rectangular kernel by half a sample** (his eq. 4):

   ```
   y[n] = (F(m[n]) − F(u[n−1])) / (u[n] − u[n−1]) + (F(u[n−1]) − F(m[n−1])) / (u[n−1] − u[n−2]),
   m[n] = (u[n] + u[n−1]) / 2
   ```

   For `f(u) = u` this is `(u[n] + 6u[n−1] + u[n−2]) / 8`: −6 dB at Nyquist instead of a zero.
2. **Equalise it with two one-pole filters** (his eq. 6):

   ```
   y[n] = b · x[n] − a · y[n−1],  a = 1 / (3 + √8) ≈ 0.1716,  b = 1 + a
   ```

   One filter sits before the shaper and one after. Their poles sit on the 1-6-1 filter's zero inside the unit circle
   and at the mirror of the zero outside it, so the magnitude response is exactly flat. Vicanek calls the result
   "perfectly transparent in the linear regime".

The plugin the Saturator came from had step 1 without step 2. That's the version saturator.md section 4 turned down as
"not flat either". The full scheme wasn't considered then.

**Measured** with a Python model (numpy and scipy, double precision). The four variants run at the same processing
rate, followed by one near-ideal decimation filter (2049-tap Kaiser, β 16). Sampling is coherent, so every component
sits on one FFT bin. The variants are:

* **plain:** today's first-order ADAA.
* **vic:** step 1 only.
* **vic+eq:** steps 1 and 2.

Linear regime, at the 48 kHz output. Every value was confirmed with a −60 dBFS sine through tanh:

| Oversampling | Method | 10 kHz | 15 kHz | 20 kHz |
|---|---|---|---|---|
| Off | plain | −2.01 dB | −5.11 dB | −11.74 dB |
| Off | vic | −1.78 dB | −3.68 dB | −5.46 dB |
| 2x | plain | −0.47 dB | −1.09 dB | −2.01 dB |
| 4x | plain | −0.12 dB | −0.26 dB | −0.47 dB |
| any | vic+eq | 0.00 dB | 0.00 dB | 0.00 dB |

Driven sines. Alias power covers every non-harmonic bin up to 20 kHz, in dB relative to the fundamental (dBc). The
error is the worst of the in-band harmonics against the exact continuous-time harmonics:

| Case | Oversampling | plain: alias / error | vic+eq: alias / error |
|---|---|---|---|
| tanh, 7 kHz, +40 dB | Off | −23.0 dBc / −0.32 dB | −21.1 dBc / +0.16 dB |
| | 2x | −33.2 / −0.08 | −32.0 / +0.04 |
| | 4x | −47.5 / −0.02 | −47.1 / +0.01 |
| tanh, 2.5 kHz, +12 dB | Off | −54.0 / −2.15 | −51.5 / +0.34 |
| | 2x | −119.1 / −0.52 | −117.6 / +0.20 |
| hard clip, 7 kHz, +20 dB | Off | −23.7 / −0.33 | −21.5 / +0.16 |
| | 2x | −35.4 / −0.08 | −33.9 / +0.04 |
| | 4x | −61.3 / −0.02 | −59.7 / +0.01 |
| hard clip, 1.5 kHz, +6 dB | Off | −52.5 / −2.13 | −50.4 / +0.38 |
| | 2x | −73.8 / −0.51 | −73.0 / +0.27 |
| | 4x | −93.7 / −0.13 | −93.6 / +0.08 |

What this trades:

* **What it gains:** a flat response, and harmonics within 0.4 dB of the exact result.
* **What it costs:**
  * aliasing 1.5–2.5 dB higher with oversampling Off, and 0.1–1.6 dB higher at 2x and 4x. The filter after the shaper
    lifts the remaining aliases near Nyquist along with the treble;
  * about twice the antiderivative evaluations;
  * a slightly longer, frequency-dependent delay (V6).
* **Step 1 alone isn't worth it:** it aliases slightly more than plain ADAA and is only brighter with oversampling Off.

The model is a scratch script, not part of the repository. Step 1 of the implementation (section 7) reproduces these
cases as engine tests.

## 3. The method

**(V1) Vicanek's full scheme, in every shaper.** Per channel, at the shaper's (oversampled) rate:

1. `v = P_pre(u)`: the one-pole on the shaper's input `u` (V3).
2. The delayed-kernel ADAA of step 1 on `v`, with the node's curve `f` and antiderivative `F`.
3. `w = P_post(y)`: the one-pole on the shaper's output, before anything else the node does with it: its DC handling,
   tone, Output or ceiling.

The one-poles' DC gain is `b / (1 + a) = 1`, so a bias or DC offset passes them unchanged.

**(V2) One shared helper.** A `TransparentAdaa` per channel state and step function, in a header both EffectNodes.cpp
and the tests include. It has the curve and its antiderivative as callables, and it holds:

* `v₁`, the previous filtered input;
* `F(v₁)`;
* the second half-interval term of the previous sample, which is the next sample's second term in eq. 4;
* the two one-pole states.

Per sample it evaluates `F(m)` and `F(v)`; `F(v₁)` is cached. That is two antiderivatives against today's one.

The ill-conditioned case: when `|v − v₁|` is below the node's threshold (today's `illConditioned`), each half-interval
quotient is replaced by half the curve at that half's midpoint:

* `0.5 · f((3v₁ + v) / 4)` for the first half;
* `0.5 · f((v₁ + 3v) / 4)` for the second.

That is more accurate than Vicanek's suggested `½ f(v₁)`, and it reduces to his for equal inputs.

**(V3) Where the filters go in each node.** The input filter goes on `u`, after drive and bias, so the curve sees a
flat spectrum whatever the drive.

* **Saturator:** `u = g (x + b)`. `P_post` acts on the shaped value, before the division by `g`, the static DC term
  and the DC blocker (W3, W6).
* **Clipper:** `u = g x`. `P_post` comes before the ceiling clamp (C5). See Q1.
* **Waveshaper:** `u = g x + b`. `P_post` acts on the antialiased curve, before the bias's DC term is removed, the DC
  blocker and the tilt (S8).

A per-sample drive or bias ramp passes through `P_pre` like the signal does. That is a linear filter on a slow
control and changes nothing audible.

**(V4) Curves with a special closed form.** The Waveshaper's sine fold uses `(cos u₁ − cos u) / (u − u₁) =
sin m · sinc h`, which is well-conditioned everywhere (S4). Each half-interval has the same form with its own midpoint
and half-width, so it stays well-conditioned and needs no threshold.

**(V5) Curves that change while playing.** Clipper (C4) and Waveshaper rule: the previous input's antiderivative is
taken with the current sample's curve, so a moving ceiling, knee or drawn curve never makes the quotient jump.

* With eq. 4, that means `F(v₁)` and the previous second-half term `(F(v₁) − F(m₁)) / (v₁ − v₂)`.
* While a curve parameter ramps (the smoothers are active) or a shape crossfade runs (S10), the helper recomputes both
  with the current curve, at four antiderivatives per sample.
* Otherwise it uses its cache. A flag on the step call selects which.
* The Waveshaper's crossfade evaluates the outgoing curve from the same `v` history, which the recompute path already
  provides.

**(V6) Latency.** The linear response of the whole chain is `P² · (1 + 6z⁻¹ + z⁻²) / 8`. The `P²` cancels the
inner zero and mirrors the outer one, which leaves a first-order allpass, `(z⁻¹ + a) / (1 + a z⁻¹)`:

* Its group delay is `(1 − a) / (1 + a)` ≈ 0.71 samples at DC, rising to `(1 + a) / (1 − a)` ≈ 1.41 at Nyquist.
* The variation comes from the two one-poles alone. The ADAA stage itself is symmetric, so its phase is linear and its
  delay is exactly one sample at every frequency, as Vicanek states. Each one-pole adds −0.15 samples at DC and
  +0.21 at Nyquist.
* Today's `(u + u₁) / 2` gives 0.5 samples at every frequency.
* At 2x oversampling that is 0.35–0.71 base samples.
* As today it isn't compensated: it only matters against a parallel path in the same graph.

**(V7) The dry path through the same linear chain.** Today the dry signal goes through `(x + x₁) / 2`, so dry and wet
line up (W5, C6, S7). With V1 the dry signal goes through the same chain instead: the input one-pole, the
`(x + 6x₁ + x₂) / 8` FIR and the output one-pole.

* In the linear regime dry and wet are then identical, sample for sample, so Mix can't comb.
* It costs two one-poles and a 3-tap FIR per channel.
* The dry path isn't the shaper's input `u`, which carries drive and bias, so it has its own filter states.

## 4. What else changes

**(V8) Oversampling defaults stay at 2x.** Oversampling's other job, reducing aliasing, is unchanged. Off becomes
more attractive for voice graphs with many voices, since it no longer darkens the sound (Q2).

**(V9) The Clipper's ceiling** (Q1).

* At the shaper's rate, eq. 4's output is the sum of two half-interval means of the curve, each weighted ½. So, like
  today's, it never leaves `±c`. `P_post` then overshoots: its step response peaks at `b` ≈ 1.17 on the first sample.
* With oversampling on, the peak guard after downsampling (C5) already catches any overshoot, whatever its cause. Its
  measured cost has to be checked again with `P_post` in the chain.
* With oversampling Off there is no guard, only the final clamp, which would clip the overshoot naively.

**(V10) Tests.**

* **New, for each node at Off, 2x and 4x:**
  * a −60 dBFS sweep is flat within 0.05 dB up to 20 kHz;
  * at Mix 50 % the same sweep is flat too, so nothing combs;
  * driven harmonics match the exact continuous-time result within 0.5 dB (the tanh and hard clip cases of section
    2);
  * a bias's DC passes the one-poles unchanged;
  * the ill-conditioned fallback is continuous: a slow ramp through the threshold has no step;
  * curve changes during a ramp don't click (V5).
* **Existing aliasing tests:** their thresholds came from measurements of plain ADAA. They are measured again and set
  from the new figures, which may be up to 2.5 dB higher at Off (section 2). Each change is listed in the commit.
* **The Clipper's ceiling test** must still hold at every oversampling choice (V9).
* **The allocation test:** the helper allocates nothing.
* **Cost:** a CPU comparison in Release of each node before and after, at Off and 2x, recorded in the implementation
  notes.

**(V11) Docs.**

* saturator.md: section 4's note on Vicanek is corrected to say the plugin's version lacked the compensation, and W5
  and W7 point here.
* clipper.md: C4, C5 and C6 point here.
* waveshaper.md: S4, S7 and S9 point here.
* `docs/PLAN.md`: the Saturator, Clipper and Waveshaper entries say "transparent ADAA".

## 5. File format

No parameters change, so no migration is needed. Existing projects sound slightly brighter where they relied on
ADAA's treble loss. Only test projects exist.

## 6. Order relative to other work

This work changes the Waveshaper's inner loop, so it comes after the Waveshaper's symmetric drawn curve, which is
already merged.

## 7. Steps

1. **The helper and its tests.**
   * Deliverables: `TransparentAdaa` with the ill-conditioned fallback and the recompute path (V2, V5), plus
     standalone tests on `f(u) = u`, tanh and the hard clip that reproduce section 2's figures within 0.5 dB.
   * About 1 day.
2. **The three nodes.** In each node, the helper replaces its ADAA (V3, V4), and the dry path goes through the same
   chain (V7). The Clipper's Off case follows Q1. The node tests (V10) come with it. About 2 days.
3. **Thresholds, CPU figures and docs** (V10, V11). About half a day.

## 8. The owner's answers (2026-11-05)

* **Q1:** with oversampling Off the Clipper keeps the lookahead peak guard, as with oversampling on, so the ceiling stays
  clean. Step 2 measures its aliasing against the bare clamp, for the record.
* **Q2–Q4:** as recommended. Oversampling stays 2x by default, the helper computes in double, and plain ADAA isn't kept
  as an option.

The questions as asked:

1. **The Clipper's ceiling with oversampling Off** (V9):
   * (a) Keep the final clamp after `P_post`. **Recommended, if step 2 measures its aliasing cost as small**: the
     overshoot is short and only on hard edges.
   * (b) Also run the peak guard at Off. The ceiling is always clean, but it adds 1 ms of latency at Off, which matters
     in voice graphs.
   * (c) Leave out `P_post` in the Clipper at Off. The ceiling is exact without a guard, but the Clipper isn't flat at
     Off (−6 dB at Nyquist).
2. **Oversampling default:** keep 2x in all three (recommended, for aliasing), or make Off the default in voice graphs
   only.
3. **Rounding:** compute the helper in double, as today (recommended). Float would halve the precision near the
   ill-conditioned threshold.
4. **Plain ADAA as an option:** none (recommended, as decided: one method, no mode switch), or a hidden "Classic"
   choice to compare by ear.

## 9. Summary of decisions

| # | Decision | Status |
|---|----------|--------|
| V1 | Vicanek's delayed kernel plus both one-pole compensation filters in all three shapers | Owner |
| V2 | One shared `TransparentAdaa` helper; two antiderivatives per sample; quarter-point fallback | Recommended |
| V3 | Filters on `u` after drive and bias, and right after the shaper, before each node's own processing | Recommended |
| V4 | The sine fold keeps a well-conditioned closed form per half-interval | Recommended |
| V5 | Recompute the history's antiderivatives with the current curve while curves change | Recommended |
| V6 | Group delay 0.71–1.41 samples at the shaper's rate, uncompensated | Recommended |
| V7 | The dry path through the same linear chain, so Mix never combs | Recommended |
| V8 | Oversampling defaults stay at 2x | Owner |
| V9 | The Clipper's ceiling at Off: the lookahead peak guard runs at Off too | Owner |
| V10 | New flatness, Mix and harmonic tests; aliasing thresholds measured again | Recommended |
| V11 | Docs of the three nodes and the plan updated | Recommended |

## 10. Implementation notes

* **The helper** is `engine/include/dawg/TransparentAdaa.h` (namespace `dawg::adaa`). It offers:
  * `step()` for a curve that doesn't change, keeping `F(v₁)`: two antiderivatives per sample. The Saturator uses it.
  * `input()`, `shaped()` and `advance()` for a curve given as its mean over an interval, with or without
    recomputing the history (V5).
    * The Clipper always recomputes, since its ceiling and knee may move on any sample. That is four antiderivatives
      per sample, against two before.
    * The Waveshaper recomputes during a crossfade, and computes the outgoing curve from the same history.
  * `DryPath` for the dry signal, and `settle()`, which starts a history where a constant input has left it.
* **Where a fresh history starts.** The Waveshaper sets its history from the bias and the running curve at the first
  sample after a reset rather than at the reset itself. At reset time the drawn table may not be the one that will
  run, and a fresh history then gave a step on silence.
* **The Clipper at oversampling Off** runs the peak guard too, so it now reports the guard's lookahead as latency (48
  samples at 48 kHz). Measured on the aliasing test's 7 kHz sine at 18 dB drive (knee 0 %, then 50 %):

  | Clipper, Off | plain ADAA | transparent, final clamp only | transparent, peak guard (as built) |
  |---|---|---|---|
  | aliasing | −24.0 / −24.3 dBc | −19.8 / −19.8 dBc | −21.8 / −22.0 dBc |

* **Aliasing with each node's test signal** (7 kHz at 48 kHz, as in each node's design doc). Plain ADAA, then
  transparent ADAA:

  | Node, case | Off | 2x | 4x |
  |---|---|---|---|
  | Saturator, 40 dB | −28.0 / −24.6 | −47.4 / −45.0 | −92.8 / −90.6 |
  | Clipper, knee 0 % | −24.0 / −21.8 | −36.4 / −34.8 | −53.6 / −53.5 |
  | Waveshaper Soft | −28.5 / −25.7 | −42.6 / −41.1 | −65.0 / −64.4 |
  | Waveshaper Tube | −28.0 / −24.5 | −47.5 / −45.7 | −73.8 / −73.0 |
  | Waveshaper Fold | −8.4 / −3.3 | −29.4 / −30.1 | −49.7 / −49.8 |
  | Waveshaper Sine Fold | −6.7 / +5.9 | −75.8 / −73.9 | −91.5 / −90.1 |
  | Waveshaper Half Rectify | −23.5 / −20.8 | −40.8 / −39.6 | −57.4 / −57.0 |
  | Waveshaper Full Rectify | −8.7 / −7.1 | −29.4 / −29.1 | −42.4 / −42.4 |
  | Waveshaper Drawn | −21.0 / −18.3 | −33.4 / −32.6 | −48.0 / −51.2 |

  This node test measures to the base Nyquist (24 kHz), where the output filter lifts the remaining aliases most. With
  oversampling Off, aliasing is therefore 2–3.5 dB higher for most curves, more than section 2's 20 kHz figure. At 2x
  and 4x the methods are within 2.5 dB, and Fold and Drawn come out lower.

* **The folds at Off.** The Python model of section 2 reproduces the node's Fold and Sine Fold figures exactly, so
  they are a property of the method, not a fault of the code. Driven 12 dB into their folds, almost all the energy is
  far above Nyquist, and neither first-order scheme suppresses much of it without oversampling. Sine Fold's dBc
  figure also rests on a small fundamental near a Bessel null, which is why it swings so far.
* **Placement of the input filter checked.** In the model, both one-poles after the shaper alias more and miss the
  harmonics by more (+0.6 to +3.1 dB) than Vicanek's split, so the split stays.
* **Tests.**
  * `tests/TransparentAdaaTests.cpp` covers:
    * the helper against an independent reference;
    * the flat allpass, its two first taps and its DC delay;
    * the fallback near equal samples;
    * every node's flatness for quiet signals, at every oversampling choice, dry and mixed: within 0.05 dB up to
      20 kHz at Off, and 0.2 dB oversampled.
  * The ramp tests of the Clipper and the Waveshaper compare against the reference written out again in
    `tests/EffectTestUtils.h`.
  * The dry-path tests expect the linear chain.
  * Two aliasing bounds were loosened to the new figures, with the measurements in comments:
    * Saturator Off: 8 dB under naive, from 10;
    * Waveshaper Sine Fold Off: 0.5 dB under naive, from 3.
* **Not done:** a CPU comparison. By count, the Saturator evaluates two antiderivatives per sample instead of one,
  and the Clipper and the Waveshaper twice as many as before. Each also runs three one-poles and a 3-tap FIR per
  channel.
