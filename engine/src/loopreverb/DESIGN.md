# DESIGN: DAWg's Loop Reverb (clean-room implementation)

Written by the clean side (an AI implementer, see records/DECLARATION.md) from SPEC.md alone (1.0, then 1.1 to 1.6,
and the 1.7 draft), between the handover commit and this document's last commit, on 2026-10-26, (1.5, 1.6)
2026-10-27 and (1.7) 2026-11-16. Every decision has a reason: a calculation, a measurement with the harness (section 4), or a reference from SPEC.md, section 9. Section 6
records how the design changed and why.

References, as cited below:

* **[S62]** M. R. Schroeder, "Natural sounding artificial reverberation", JAES, 1962.
* **[M79]** J. A. Moorer, "About this reverberation business", Computer Music Journal, 1979.
* **[G71]** M. A. Gerzon, "Synthetic stereo reverberation", Studio Sound, 1971–72.
* **[D97]** J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", JAES, 1997.
* **[JC91]** J.-M. Jot and A. Chaigne, "Digital delay networks for designing artificial reverberators", AES
  Convention, 1991.
* **[PASP]** J. O. Smith III, *Physical Audio Signal Processing*, chapters on artificial reverberation (and its
  sections on delay-line interpolation).
* **[TXT]** textbook digital filters and measurement (bilinear transform, first-order shelves, one-pole smoothers,
  Lagrange interpolation, the RBJ cookbook bandpass, Schroeder backward integration for decay curves) and
  pseudo-random number generation (SplitMix64, Voss–McCartney pink noise).

No third-party code is used; all code in node/ is written for this project (AGPL-3.0-or-later, SPEC 7.8).

## T. Revision for SPEC 1.7 (draft, 2026-11-16)

Worked from SPEC.md as found in the workspace on 2026-11-16, marked "1.7 draft, not yet approved" (SHA-256
de78391f46925ecc1b47122dba16d2fc31b7c1c0d1a366ce91b5f6fde58e60aa; no row in records/MONITOR.md yet). SPEC 1.7 asks
for two things, with the sound unchanged (every earlier requirement, A21 included):
* less CPU: at most 1.5 % of a core at the defaults and 2.5 % at any setting, with MSVC as well (7.6, A11);
* almost none in silence: once the input has been 0 and the output below −120 dBFS for 1 s, at most 0.1 % until
  input returns, and nothing audible changes (7.9, A25).

### T.1 Does the design as a whole still fit?

A21 fixes the output at the defaults to 100 dB, and in a recursive network with a 13 s tail even a different rounding
order grows to about −88 dB (the MSVC build against g++'s recorded samples, 1.6's acceptance). So the choice is not
between architectures but between ways of computing the same one, operation for operation. The alternatives:

| Option | For | Against | Verdict |
|---|---|---|---|
| **Compute the same network faster** (the same operations in the same order, organised for the compiler and the vector units) | the output stays bit-identical with each compiler; nothing to re-tune or re-listen | needs care: every expression kept as it was, no fused multiply-add | **chosen** (T.2) |
| Fewer lines (8 instead of 16) | roughly half the tail's cost | a different sound: fails A21, and A14's smooth highs were reached with 16 (P.2 rejected fewer) | rejected |
| Cheaper chorus interpolation (third order) | about a third of the chorus's reads | 0.7 dB less at 8 kHz over 0.1–1 s at the defaults (S.3), close to A23's 1 dB | rejected |
| Process the network in sub-blocks shorter than its shortest delay (contiguous reads per line) | turns 64 interleaved memory streams into bursts | only helps if memory is the limit; measured, it isn't (T.2) | rejected |
| **Silence: sleep, then restart as prepared** | costs nothing while asleep; the output after the silence is exactly a fresh node's (A25) and doesn't depend on how long the silence was | the modulation's phases restart after a silence; question 7 | **chosen** (T.3) |
| Silence: sleep, but keep the modulation's phases and the control grid moving | literally "what it would have been" (7.9) | still costs per control tick, and the output differs from a fresh node's by far more than 120 dB at Modulation above 0 % (A25) | rejected |
| Silence: a cheaper mode while the tail is quiet but not yet below −120 dBFS | could help long tails | anything cheaper is a different network: audible at −60 dBFS (the first pad corner 10 s after the noise) | rejected |

### T.2 CPU (7.6, A11)

**Where the time went** (a sampling profiler written for this, in build/, reading the instruction pointer every
millisecond; the MSVC build with debug information):
* At the defaults with MSVC: the per-line absorption loop 23 %, the lines' reads 16 %, the rest of the tail 32 %
  (allpass sections, rotation network, output sums), the input tone shelves 4 %, the early taps 4 %. MSVC ran every
  per-line loop one line at a time: it can't prove that the arrays don't overlap.
* With the chorus at 100 %: the fifth-order reads (conversions, coefficients, zero-initialised arrays) dominated.
* Not memory: capping every buffer at 1024 samples (wrong output, same instructions) changed the cost by 3 %.
* g++ at −O3 (CMake's Release) already vectorised most of the loops; under MSVC the same code took 1.7–1.9 times as
  long. MSVC decides A11 (7.6 names it).

**The changes** (all bit-identical to 1e26190's output, with g++ and with MSVC, each against its own build: hashes of
12 settings × noise and impulse, with blocks of 480, 1, 512 and 77, and 600 blocks of random control changes):
* **Delay banks.** The sixteen lines, and the sixteen in-loop allpass delays, are each one buffer with one write
  position (they were always pushed together); each line starts a cache line further on, so their writes fall in
  different cache sets.
* **Whole delays once.** At rest every delay is a whole number of samples (1.7). They are now converted to integers
  when the delays settle, not 48 times per sample.
* **Reads first, then arithmetic over all lines.** The lines' reads go into arrays; absorption, the allpass sections,
  the output terms, the rotation network and the injection then run four lines at a time.
* **node/Simd.h:** four floats with addition, subtraction, multiplication and a sign flip only, as SSE2 (every x86-64
  CPU has it), NEON (ARM64), or plain scalar code, chosen by `#if` platform guards (sdk/CODING_GUIDELINES.md). Each
  lane is rounded exactly as the scalar expression is; no fused multiply-add. Where a lane swap replaces a scalar
  partner (the rotation network's strides 1 and 2), c b − s a is computed as c b + (−s) a, and −x y as −(x y): the
  same numbers exactly.
* **The chorus's fifth-order reads** compute the coefficients of four lines at a time and truncate with the vector
  conversion (the same value as the scalar conversion for these positive delays).

**Measured** (60 s of noise, best of three; this machine was heavily loaded by other processes during the session,
so the times are given as ratios to 1e26190's, both measured back to back, in thread cycles):

| Setting | MSVC | g++ −O3 |
|---|---|---|
| Defaults | 0.62 | 0.71 |
| Feedback, Size, Diffusion, Mix 100 % | 0.63 | 0.74 |
| The same with Modulation and Width 100 %, Damp and Thin 50 % | 0.58 | 0.66 |
| Every continuous control at its maximum (Early Timing aside), Pre-delay 250 ms | 0.59 | 0.62 |
| Blocks of 480 instead of 512 | the same ratios within 3 % | |

From 1.6's acceptance (MSVC, the DAWg side's machine: about 1.1 s at the defaults, 1.6–1.85 s with the chorus on),
that predicts about 0.68 s at the defaults (limit 0.9 s) and 0.93–1.07 s at the costliest settings (limit 1.5 s). The
absolute times on this machine (2.7 s at the defaults with g++) say nothing: the same build took 1.03 s on
2026-10-27. **The DAWg side measures A11 on its own machine**; the test now takes the best of three, with both limits.

### T.3 Silence (7.9, A25)

* **Sleeping.** Every sample the node counts how long the input has been exactly 0 and the output below −120 dBFS
  (1e−6, both channels). When the count reaches 1 s, the node clears every buffer and filter state, outputs exact
  zeros and only scans its input (eight samples at a time). The output criterion is the spec's: at Mix 0 % the
  output is the dry signal only, so the node may sleep while its tail would still be there; turning Mix up during
  the silence then brings no old tail back.
* **Waking.** At the first sample that isn't 0 the node starts again as prepare() leaves it: targets taken from the
  parameters with no glide, modulation phases and the control grid from the start (restart()). Sleeping and waking
  are counted sample by sample, and a block is split where either happens, so the output doesn't depend on the
  block sizes (7.3). The restart is part of the design whether or not the node sleeps, which makes 7.9 ("what it would
  have been") and A25 ("a fresh node's output") the same thing; question 7 asks the DAWg side to confirm.
* **Measured** (A25's test: 10 s of pink noise, 60 s of silence, noise again; g++):

  | Setting | Below −120 dBFS after the noise | Asleep | 20–60 s of the silence |
  |---|---|---|---|
  | Defaults | 2.8 s | 3.8 s | 0.03–0.09 % of a core |
  | Second pad corner | 14.1 s | 15.1 s | 0.03–0.09 % |
  | Every control's maximum (Mute on) | at once | 1 s | 0.03–0.09 % |
  | First pad corner | 24.4 s | 25.4 s | 0.68 % |
  | Every maximum but Mute | 24.5 s | 25.5 s | 1.0 % |

  Asleep, a 512-sample block takes about 3 µs here (0.03 %; the higher figures are wall time on the loaded machine).
  After the silence, and after silence at the start, the output is **bit-identical** to a fresh node's for the same
  noise, at every setting. Blocks of 1, 64, 480, 4096 and random sizes from 1 to 4096 give identical output.
  60 s of silence from the start costs 3–4 % of the cost of 60 s of noise (the first second runs before it sleeps).
* **A21:** where the accepted output's tail would go on below −120 dBFS for more than a second, it is now 0
  (Feedback 0 % in the hash set; none of A21's recorded cases). The test still passes, 116 dB below the output with
  g++.

### T.4 Where A11 and A25 stand

* **A11:** bit-identical output at about 60 % of 1e26190's cost under MSVC; predicted within both limits, with about
  25 % margin at the defaults. Not measured on an idle machine here.
* **A25:** passes at the defaults, the second pad corner and every maximum, read as 20 s into the silence. **The
  first pad corner can't pass as worded**: its tail is still about −54 dBFS 10 s after the noise and only falls below
  −120 dBFS after 24.4 s, so 7.9 lets the node sleep from 25.4 s and the window from 20 s costs 0.68 %. Sleeping
  earlier would cut a tail that is still audible. The test checks the cost where 7.9 lets the node sleep within 20 s,
  and reports the rest; question 7 asks how A25's window is meant.

## S. Revision for SPEC 1.6 (2026-10-27)

SPEC 1.6 asks for two controls, each keeping the accepted sound (c9ba0bf) at its default (A21, to 100 dB):
* width beyond the natural width, up to a fully decorrelated tail (5.10, A22);
* a modulation control, from a still tail to a lush chorus (5.12, A23);
* A24 by listening.

### S.1 Does the design as a whole still fit?

A21 sets the frame: at the new controls' defaults the output must be the accepted one to 100 dB. Any change to the
network, its lengths, its mixing or its absorption changes that output by far more than 100 dB allows. So the real
choice is not which architecture, but how to add two mechanisms that are exact no-ops at their defaults. The network
also suits both additions:
* its output stage already sets the tail's correlation per band (1.9), so going wider is one more step of the same
  law;
* it has sixteen independent delay lines, the classic place for modulation that becomes chorus [D97], [PASP].

| Option | For | Against | Verdict |
|---|---|---|---|
| **Width: extend the existing control** (1.5's 0–100 % becomes 0–75 %; 75–100 % is "very wide") | one knob from narrow through natural to very wide; at 0–75 % the exact 1.5 sound | saved projects need converting (new = 0.75 × old); the default moves to 75 % | **chosen** |
| Width: a second control (an extra width, 0 % = 1.5) | no conversion of saved projects | two knobs for one quality, and a meaningless combination (Width 0 % with the extra at 100 %); the very wide end only makes sense on top of 1.5's widest | rejected |
| Very wide: decorrelate the early part too (fewer cross taps, larger time differences) | a wider early image | the early part's lean (a panned source keeps its side) and its coherence are what 5.10 keeps; time differences beyond 0.7 ms sound disorienting (question 3) | rejected: the early part stays at 1.5's widest |
| Very wide: lower the bass correlation too | wider bass | 5.10: the low end less wide and solid; A22 wants at least 0.3 at 63–125 Hz | rejected: below 150 Hz it stays 0.92 |
| **Modulation below the default: fade the accepted movement in** (the in-loop allpass angle's depth × m / 25 %) | 0 % is time-invariant; the default is exact | none | **chosen** |
| Modulation above the default: deepen the allpass angle modulation | already there, lossless | it moves phase, not delay: the pitch it gives in hertz is about the same at every frequency, so in cents it is ∝ 1/f (110 Hz would move 16 times as much as 1760 Hz); A23 needs at least 3 cents at 440 Hz and at most 8 at 110 Hz | rejected |
| A chorus on the tail's output (a modulated delay per channel) | simple, no effect on the decay | moves the whole tail together: vibrato, not chorus (5.12: "never ... vibrato-like") | rejected |
| **Modulated line delays inside the network** [D97], [PASP] | each pass shifts a path's pitch by the same number of cents at every frequency; paths through different lines move independently, so a held note spreads into a chorus | interpolation in the loop loses highs; the response's fine structure changes with the delays | **chosen**, with fifth-order interpolation (S.3) |

### S.2 Width (5.10, A22)

* **Scale.** Width keeps its id, name and range. 1.5's value v maps to **0.75 × v** (exact for every value; 0, 50
  and 100 % become 0, 37.5 and 75 %). Up to 75 % everything is computed from w / 75 % exactly as 1.5 computed it
  from v / 100 %, so these settings give 1.5's output bit for bit (S.4). **DAWg converts saved projects with
  new = 0.75 × old.** The default moves from 100 % to 75 %, the same sound.
* **Beyond 75 %.** With x = (w − 75 %) / 25 %, the tail's correlation above the 150 Hz crossover goes from 0.25 (1.5's
  widest) linearly to 0 at 100 %. At 0 the mid and side gains are both 1, so the network's two outputs pass as they
  are: two sums of disjoint lines, uncorrelated by construction. Below the crossover the correlation stays 0.92.
  The early part stays at 1.5's widest: each source's own reflections around its side, leaning at least 6 dB to it,
  the two sides balanced.
* **Measured** (seed 1, Width 100 %, from 100 ms):
  * correlation per octave, 63 Hz–8 kHz: 0.81, 0.53, 0.22, 0.08, −0.03, −0.06, −0.03, −0.02. A22: −0.1 to 0.1 from
    500 Hz to 4 kHz, at most 0.3 at 250 Hz, at least 0.3 at 63 and 125 Hz;
  * mono sum −0.04 dB against the sum of the channels' energies (limit −3 dB);
  * left − right −0.04 dB (limit 1.5 dB);
  * a hard-left source's early part leans left by 7.8 dB (limit 3 dB);
  * at 1, 2 and 4 kHz the correlation falls by 0.05 for every 5 % from 75 to 100 % (0.22 → −0.03 at 1 kHz).
  * Seeds 2–8 at 100 %: 500 Hz–4 kHz between −0.06 and 0.12. A22 is at the defaults (seed 1). Some seeds sit
    slightly above 0.1, as A9's band scatters by about ±0.1 from seed to seed (1.9).
* A centred source stays centred at 100 % too (5.10 test: 0, 52.5, 75 and 100 %). The 7.2 test now includes Width
  and Modulation at both ends.

### S.3 Modulation (5.12, A23)

`modulation`, "Modulation", 0–100 %, **default 25 %**. With m the control:

* **Up to 25 %:** the in-loop allpasses' angle modulation (1.1, P.3) is multiplied by m / 25 %. At 0 % nothing in the
  reverb moves: it is exactly time-invariant (a second impulse adds exactly the first's response, shifted). At 25 %
  it is 1.5's movement exactly. 25 % leaves most of the travel to the chorus, while the gentle accepted movement
  still sits at a clearly marked point.
* **Above 25 %:** with c = (m − 25 %) / 75 %, each FDN line's delay also moves, by
  0.15 ms × c^1.5 × min(1, 10 s / T60) × sin(2π r t + φ). The rate r is 1–2 Hz per line and the phase φ is per line,
  both from the seed (their own random stream, so every earlier choice of every seed is unchanged). The offset is
  planned every 16 samples and moves linearly in between (block-size independent, like every other control-rate
  value, 1.7). The lines are then read with **fifth-order Lagrange interpolation** [PASP], [TXT].
  At depth 0 every offset is exactly 0 and the lines are read as in 1.5.
* **Why these values** (first pad corner, seeds 1–3, A18's drift):
  * **Depth.** It sets how far the paths' phases scatter; rate × depth sets the pitch movement. At 0.5–1 Hz, enough
    pitch movement at 440 Hz needed so much depth that the held notes broke into deep, random beating. Its phase
    jumps count 16 times as much in cents at 110 Hz as at 1760 Hz: 0.75–1.5 Hz with 0.15 ms gave 8.1 cents at
    110 Hz (limit 8).
  * **Rate.** 1–2 Hz with 0.114 ms in the pad corner (0.15 ms × 10 / 13.2) gives 4.1 and 5.7 cents at 440 and 880 Hz
    and 6.5 at the worst note (110 Hz). 1.5–3 Hz and 2–4 Hz with less depth gave the same or worse balance and
    approach vibrato.
  * **Decay law.** The pitch a path collects grows with its number of passes, so a long tail needs less per pass.
    Holding the depth constant below 10 s keeps short tails' fine structure close to 0 %'s: A9's correlations change
    by at most 0.05. With depth ∝ 1/T60 from 3 s (tried first, as for the allpasses) the default tail moved four
    times as much, and the 2 kHz correlation changed by 0.16.
  * **Curve.** c^1.5 makes the drift grow evenly: 440 Hz 0.27, 0.27, 0.57, 1.71, 4.06 cents at 0, 25, 50, 75 and
    100 %. c² was convex (0.42, 1.47 at 50 and 75 %).
* **Interpolation.** Third-order Lagrange, already used for gliding delays, loses 0.14 dB per pass at 8 kHz on
  average over the fraction. At the defaults that took 0.7 dB off the 8 kHz octave over 0.1–1 s, close to A23's 1 dB.
  Fifth order loses 0.03 dB per pass. It is passive (largest gain over all fractions and frequencies 1.000,
  computed), exact at whole samples, and costs 6 reads per line only while the chorus is on.
* **Measured** (A23):

  | Note | Drift at 0 / 25 / 50 / 75 / 100 % (cents) | Ideal drift | Swing at 0 % / ideal (dB) |
  |---|---|---|---|
  | 110 Hz | 0.941, 0.942, 1.40, 3.03, 6.51 | 0.488 | 0.93 / 0.94 |
  | 220 Hz | 0.478, 0.478, 0.58, 1.04, 1.90 | 0.306 | 1.06 / 0.72 |
  | 440 Hz | 0.2675, 0.2679, 0.57, 1.71, 4.06 | 0.150 | 1.01 / 0.79 |
  | 880 Hz | 0.114, 0.114, 0.53, 3.05, 5.72 | 0.160 | 0.91 / 1.62 |
  | 1760 Hz | 0.057, 0.057, 0.62, 2.71, 5.01 | 0.028 | 0.90 / 0.53 |

  * At 100 %: at least 3 cents at 440 and 880 Hz, at most 8 everywhere.
  * Across the control, at the defaults: RT60 within 3.2 %, octave levels over 0.1–1 s within 0.71 dB, A9's tail
    correlations within 0.05. In the pad corner: RT60 within 0.8 %, octave levels within 0.25 dB.
  * At 100 % a held sine's level beats by 8–28 dB peak to peak. A pure sine is the worst case for any chorus (a
    chorus pedal's two voices on a sine beat down to silence). Whether it sounds lush rather than seasick on a pad
    and the three-saw lead is for listening (A24, section 7).
* **CPU:** at Feedback, Diffusion, Size, Modulation and Width 100 % (Damp and Thin 50 %), 60 s render in 1.55 s
  (limit 1.8). The defaults are unchanged at 1.03 s.

### S.4 Where A21 and A23 at 0 % stand

* **A21.** At the defaults, the two pad corners, 1.5's widths 0, 50 and 100 % (mapped), seeds 1–3, impulses and
  10 s of pink noise (72 renders), the output is **bit-identical** to c9ba0bf's, rendered by c9ba0bf's own render
  tool. The new paths are exact no-ops there:
  * a multiplier of exactly 1;
  * the line reads unchanged while every chorus offset is 0;
  * w / 75 equal to v / 100 at 0, 50 and 100 %.

  The test (node/AcceptedSound.h) keeps 96 samples and the RMS of 30 of these renders, taken from c9ba0bf's output,
  and requires every sample to be within 100 dB of the RMS.
* **A23 at 0 %: drift fails at 110, 220 and 440 Hz for seeds 1–3, through 1.5's own rare notches** (question 5).
  * Swing is within 0.5 dB of the ideal at every note, and drift within 0.1 cents from 880 Hz up. At 110, 220 and
    440 Hz the drift is 0.45, 0.17 and 0.12 cents above the ideal's (limit 0.1). At 0 % these are exactly the
    default's values (±0.001): the modulation neither causes nor cures them. They are the network's modal
    statistics, as P.1 and P.4 found.
  * **Over 30 seeds and 32 ideal draws at 0 %:**
    * The node's median drift matches the ideal's within 0.05 cents at every note: 0.41/0.38, 0.36/0.30,
      0.19/0.18, 0.10/0.11 and 0.05/0.03.
    * Against the mean of all 32 draws, seeds 1–3 are above the limit only at 110 Hz (+0.40), through seed 1's
      notch (1.97 cents; seeds 2 and 3 0.14 and 0.72).
    * **The ideal reverb itself fails A23's 0 % limits in 70 % of resamplings** (three of its draws as "the node"
      against four others), the node in 80 % (random seed triples). The +0.1 cents and +0.5 dB margins are below
      the scatter of a mean over three seeds and four draws.
  * As question 5's answer asks, this is reported here and not redesigned for. The DAWg side decides.
  * The test checks what the requirement can mean for this design: at 0 %, swing within 0.5 dB at every note; drift
    within 0.1 cents from 880 Hz up and no higher than the default's below. It also checks that 0 % is exactly
    time-invariant.
* **InterfaceTests.cpp** expected exactly two free controls, as 1.5 had. 1.6's 4.1 adds the modulation control
  (and allows a second width control), so the count now accepts three or four, with every other rule unchanged.
  Question 6 asks the DAWg side to confirm its copy.

## P. Revision for SPEC 1.5 (2026-10-27)

SPEC 1.5 reports three shortfalls heard inside DAWg on a lead of three detuned saws:
* grainy highs (A14 now measured: at most 1.10 times a phase-randomised reference above 8 kHz);
* a Damp highs that bites too soon (A7: Damp 10 % may cost the highs at most 3 dB one second in);
* a long tail that is fluttery rather than a steady bed (A18: held notes' swing and pitch drift; A19: their
  release tails' evenness, both against an ideal, time-invariant reverb; A20 by listening).

It also asks for a long tail that is less dark (A15 gains lower limits). As CLAUDE.md asks, the design was checked
as a whole first.

### P.1 What fell short, measured

SPEC 1.5's measurements were implemented first, as written (node/Measure.h, section 4.5). On the accepted 1.4 build:

* **A14:** 1.18–1.34 times the reference at Diffusion 100 % (the DAWg side measured 1.27–1.49). Diffusion 70 %
  and 40 % scored about the same or more. The 1.1 proxies (section 4.3) passed: they look at the band above 4 kHz
  in the first 150 ms and at impulsiveness, not at this.
* **The cause is the darkening, not echo density.** With the darkening switched off (setDarkening), the same
  measurement gives 0.93–1.01. The ideal reverb, given the same frequency-dependent decay (noise split into bands
  above 8 kHz, each decaying at its own rate), scores 1.51 with 1.2's first-order shelf (60 dB/s above 8 kHz).
  With half that loss it scores 0.93–0.95. A loss that keeps rising across 8–24 kHz makes the decaying band crowd
  towards its lower edge: a narrowing band of noise, whose 2 ms frames fluctuate more than the average spectrum
  explains. The DAWg side's note that "the rest of the spectrum and the later tail are as smooth as noise" fits:
  only the band where the darkening's slope lies is affected.
* **A7:** Damp 10 % took 8.7 dB off the highs at 0.9–1.1 s (limit 3 dB): Damp's decay ratio was 10^(−1.5 d), so
  10 % already cut the 4 kHz-and-up RT60 by 30 %.
* **A15 at the pad corner** (Feedback 90 %, Diffusion and Size 100 %): RT60 at 8 kHz 0.24 and at 4 kHz 0.52 of
  the mids' (limits at least 0.3 and 0.6). A fixed loss per second can't meet both ends of A15: at Feedback 70 %
  and the default size (T60 4.4 s) the 8 kHz ratio must be at most 0.5. At the pad corner (13.5 s) it must be at
  least 0.3. A fixed loss L gives a ratio of 1 / (1 + L·T60/60), which falls too fast between the two.
* **A18, A19** (seeds 1–3, four ideal draws). Both pass, but:
  * In the first pad corner the bass drift is about twice the ideal's: 0.95 against 0.50 cents at 110 Hz, and
    0.48 against 0.31 at 220 Hz (limits +1 and +0.3).
  * In the second (Feedback 80 %, Diffusion and Size 80 %) the swing is about twice the ideal's: 0.19–0.32
    against 0.04–0.18 dB.
  * The modulation causes the second: switched off, the swing falls to 0.08–0.11 dB. It doesn't cause the first:
    switched off, the drift stays at 0.94 and 0.48 cents.
  * The first is structural. At an RT60 of 13 s the modes are about 0.17 Hz wide (2.2 / T60), and the network
    has about 1.4 modes per hertz (its total delay, [JC91]), so they overlap by only 0.24. A held note between
    two modes beats against their decaying build-up at the mode spacing, not at the decay rate as in the ideal
    reverb.
* **A correction to section 0b.2:** it expected the modulation to smear the long tail's modes by tens of hertz.
  Measured, it hardly moves them: switching it off changes the first pad corner's drift by 2 %. The modulation was
  only ever a gentle movement, not a cure for ringing.

### P.2 Does the design as a whole still fit?

| Option | For | Against | Verdict |
|---|---|---|---|
| **Keep the network; fix the elements found above** | every measured shortfall traces to one element: the darkening's shape and law, Damp's curve, the modulation's depth, the allpasses' share of the decay; A18/A19's remaining gap is modal statistics, near the ideal's (P.3) | the modal overlap of a 16-line network stays below 1 at the longest decays | **chosen** |
| Convolution with a synthesised response: decaying noise, the ideal reverb itself ([M79] describes the late response as exponentially decaying noise) | passes A18/A19 by construction (it is the reference); smooth by construction | an 18 s stereo response is about 1.7 million taps: partitioned FFT convolution costs several times 7.6's CPU budget (1.8 s per 60 s, now 1.1–1.4 s); every change of Feedback, Size or Damp needs a new response, which can't be computed in process() within bounded time (7.5), so changes would lag and need crossfades (5.11); the darkening that keeps fresh sound bright (5.2) would have to be built into the response as a time-varying spectrum | rejected |
| 32 lines | twice the modal density and echo density | CPU estimated at about 1.7 s per 60 s on this machine today (the network is most of the cost), at the limit; and more modal density alone doesn't remove A18's outliers (next row) | rejected |
| Longer lines (Size range 25–300 ms instead of 25–100 ms, T60 curve kept) | 2.4 times the modal overlap at large sizes: over 12 seeds the bass drift matches the ideal (110 Hz 0.54 against 0.59 cents, 220 Hz 0.31 against 0.32) | over 30 seeds no more robust: the first pad corner fails A18 for 20 % of random seed triples (25 % with the present lengths), the second fails A19 for 2.5 % (0.1 %); swing at 880 Hz has heavier outliers (one seed 9.1 dB); a range of 25–200 ms had a seed with a 220 Hz notch; it also changes the default's onset and the sound of every large size | rejected |
| An allpass-loop tank [D97] | dense quickly | fewer independent paths; modal density is still the total delay; Damp and Thin per band less exact (0c.2) | rejected |

The room-model option for the early stage (sections R.3, Q.1) isn't touched by 1.5: nothing in it concerns the early
reflections.

### P.3 The changes and how they were chosen

1. **The darkening's shape** (A14). It is now G + (1 − G)·LP in each line, with LP a fixed second-order Butterworth
   lowpass at 6.5 kHz (a trapezoidal state-variable filter [TXT]). Since |LP| ≤ 1 it is passive for any
   G ≤ 1. For small losses the share of G's loss at frequency f is 1 − Re LP(f): 0.25 near 3 kHz, 0.9 near 6 kHz,
   and 1.07–1.21 from 8 to 24 kHz (nearly flat above 8 kHz, where A14 looks). The 4 kHz octave gets about a third
   of the 8 kHz octave's loss, which A15's two octaves need at both ends of the Feedback range (a corner of
   6.5 kHz puts the ratio between the 0.25 that Feedback 70 % needs and the 0.37 that the pad corner allows).
   A14 at Diffusion 100 %: 0.93–1.04 (it was 1.18–1.34).
2. **The darkening's law** (A15, 5.2). The loss per second above the corner is min(20 dB/s, 55 dB/s × 2.6 s /
   (T60 + 2.6 s)):
   * Up to a T60 of about 4.5 s it is air absorption's fixed rate per second, as in 1.2 [M79].
   * Beyond that it weakens, so the highs' share of the decay still shrinks as the tail lengthens (5.2,
     "increasingly so"), but slowly enough that they ring for a fair share of it.
   * 2.6 s and 55 dB/s were fitted to put both ends of A15 in the middle of their bands: 8 kHz ratio 0.41 at 4.4 s and
     0.36 at 13.5 s (measured).
   * 20 dB/s brings the default tail back to its 1.4 tone. Without the cap the loss rose for short tails, and the
     default's 8 kHz octave decayed in 0.50 of the mid time instead of 0.65.
   * Measured, A15: 4 kHz / 8 kHz ratios 0.70 / 0.41 at Feedback 70 % (limits 0.75 / 0.5) down to 0.65 / 0.35 at
     100 %. At the pad corner, Feedback 90 %: 0.66 / 0.36 (limits at least 0.6 / 0.3). Fresh highs −0.1 dB; their
     share at 2 s −19.1 dB.
3. **Damp starts gently** (A7, 5.7). The in-loop decay ratio is 10^(−1.5 d²), and the two input shelves are
   −12 dB × d^1.5 each, the same as before at 100 %. At 10 % the highs lose 1.5 dB at 0.9–1.1 s (it was 8.7);
   at 8 kHz the tail goes −2.1, −6.2, −11.5, −16.4, −19.4 dB at 20–100 %. The input shelves were first left
   linear (2.2 dB at 10 %). Once the darkening's cap kept more highs at Damp 0 %, Damp 10 % cost 2.85 dB, too close
   to 3, so they got a gentle curve too.
4. **Diffusion sets the grain** (A14's ordering). The mixing angle is (π/4)·d², so below about 50 % the lines mix
   slowly and the tail keeps grain. Diffusion 40 % now measures 1.02–1.87 times the reference, against 0.93–1.04
   at 100 %; the 1.4 build's 40 % was at the noise floor at small sizes. Diffusion 70 % is as smooth as 100 %
   (0.97–1.02): by 0.2 s the network has mixed into noise at any Diffusion from about 50 % up, even with an angle
   ∝ d³ (tried). So "not lower than at 100 %" comes down to scatter for 70 %, about ±0.035 from seed to seed;
   question 4 asks how it is meant. The test allows 0.06.
5. **Uniform decay** (A19, 5.1). Jot and Chaigne's condition for every mode to decay at the same rate is that every
   delay element loses the same per sample [JC91]. The in-loop allpasses' internal delays didn't: their delay was
   counted in the line's gain. Modes where the allpass's group delay was long decayed more slowly, which section
   1.1 had measured as a 12 % longer decay near 125 Hz. Each allpass now carries the decay gain for its own delay
   (its state is read through 10^(−3 D / (T60 fs))), which keeps the section passive. Measured with the darkening
   off, 125 Hz–8 kHz: 1.70, 1.67, 1.61, 1.62, 1.61, 1.63, 1.62 s against 1.62 s designed. The shelves still act once
   per pass, on the mean time a pass takes.
6. **Less movement in long tails** (A18, 5.1: "the tail's slow movement stays below that"). The modulation depth is
   multiplied by min(1, 3 s / T60). Nothing changes up to a 3 s decay (the defaults keep their gentle movement); at
   the pad corners it is 0.4 and 0.22 of what it was. In the second corner the swing falls towards the ideal's (with
   the other changes: 0.13–0.25 against 0.07–0.20 dB for seeds 1–3).

### P.4 Where A18 and A19 stand, and how robust that is

For seeds 1–3, with four ideal draws, all checks pass:

| Pad corner | Note | Swing: node / ideal (dB) | Drift: node / ideal (cents) |
|---|---|---|---|
| Feedback 90 %, Diffusion and Size 100 % | 110 Hz | 0.94 / 0.94 | 0.94 / 0.49 (limit +1) |
| | 220 Hz | 1.09 / 0.72 | 0.48 / 0.31 (limit +0.3) |
| | 440 Hz | 1.00 / 0.79 | 0.27 / 0.15 |
| | 880 Hz | 0.90 / 1.62 | 0.11 / 0.16 |
| | 1760 Hz | 0.93 / 0.53 | 0.06 / 0.03 |
| Feedback, Diffusion and Size 80 % | 110–1760 Hz | 0.14–0.25 / 0.07–0.20 | 0.007–0.12 / 0.004–0.08 |

Release unevenness: 0.92 times the ideal's in the first corner, 1.09 in the second (limit 1.15).

Because these are statistics of a random-like response, they were also measured over 30 seeds and 32 ideal draws
(20,000 resamplings of three seeds against four draws):
* **The medians match the ideal's.** Swing 0.58–0.93 dB against 0.55–1.15. Drift 0.44, 0.37, 0.20, 0.10, 0.05
  cents against 0.40, 0.31, 0.19, 0.11, 0.03. Unevenness median ratio 0.93–0.94.
* **Failures come from rare notches,** where a note's frequency meets a small response and the still-ringing
  build-up dominates. They happen to the ideal reverb as much: one of its draws swings 16.7 dB at 1760 Hz.
  * First corner: an ideal reverb, three of its draws against four others, fails A18 in 41 % of resamplings. The
    node fails for 25 % of random seed triples, so it is not less steady than the reference.
  * Second corner: both pass A18 in practice; A19 fails for 0.1 % of random seed triples.
* **For seeds 1–3** against any four draws: A18 always passes. A19 in the second corner fails against about 6 % of
  the possible draws, because seeds 1–3 sit near the top of the node's spread (1.09 against a median of 0.94).
  Question 5 asks whether the mean over three seeds and four draws is intended.

What remains beyond the ideal is the bass drift in the first corner (about +0.05 cents at 110 and 220 Hz in the
medians, +0.1 in the means), the price of a modal overlap below 1. A20 decides by listening whether it is audible.

## Q. Revision for SPEC 1.4 (2026-10-26)

SPEC 1.4 answers the clean side's questions 2 and 3: the width-related early control may set the width of the whole
reverb, early part and tail (narrower at 0 %, wider at 100 %; 5.10 at every setting; A9 at the defaults and at
100 %), and A9's early criterion becomes the correlation of the two channels' 5 ms-smoothed energy envelopes (at
least 0.5), with time differences between the channels of up to 0.7 ms per reflection allowed.

### Q.1 Does the design as a whole still fit?

Section R recommended rebuilding the early stage on a room model once these questions were answered. Checked again
against 1.4:

* What 1.4 changes is rendering, not where reflections come from. The early stage already gives every reflection a
  direction (its pan angle), and a time difference between the ears follows from the same direction; the tail
  already has an output stage whose width is one number.
* A room model would not remove the guarantees: A9's lean holds "at every setting", which geometry alone doesn't
  give for every seed and room shape (a source near the middle of a narrow room has no lean), so the lean bisection
  and the balancing would stay.
* It would put A5 (the timing control's median shift, the controls' independence) and A6 (seed decorrelation) at
  risk again, for no gain on anything 1.4 asks.

So the architecture stays; the early stage gets time differences from its directions, and its width control is
extended to the tail. The room model remains the option for the early stage if later spatial requirements need
more than directions and levels (section R.3).

### Q.2 The changes and how they were chosen

* **Width replaces Early Width** (id `width`, name "Width", as question 2's answer allows renaming): it sets the
  width of the whole reverb. Early part: the crossfade from the shared, narrow pattern to each input's own lateral
  reflections, as before. Tail: above the 150 Hz crossover the left/right correlation goes linearly from 0.6 at 0 %
  (narrow, still positive and mono-safe) to 0.25 at 100 %; below it stays 0.92. The four mid/side gains are smoothed
  targets, so Width moves without clicks (A10: 1.18× at most).
* **Why 0.25 at 100 % and not wider:** the tail's per-octave correlation scatters by about ±0.1 from seed to seed
  near the crossover (R.1), so A9's band of 0.15–0.5 only holds for every seed near 0.25: 0.2 puts the 2 kHz octave at
  the 0.15 limit, 0.3 puts the 250 Hz octave at 0.49.
* **Default 100 %:** A9 applies at the defaults, and its tail band only holds near 0.25, so the defaults must be as
  wide as the tail goes. Width therefore works as most stereo-width controls do: full, natural width by default,
  narrower below. (It was 70 %, with the tail fixed.)
* **Time differences between the ears:** each side-pattern reflection reaches the far channel later by
  0.65 ms × cos 2φ, φ its pan angle: a reflection fully on its side (φ = 0, azimuth 90°) by 0.65 ms, about a head's
  width, one from the front (φ = 45°) not at all, one beyond the front with the other ear leading. 0.65 ms is within
  1.4's 0.7 ms. The early taps carry separate delays to each side for this; the panel's arrivals list each channel's
  own times.
* Measured at Width 100 % (A9): envelope correlation of the early part 0.99 (limit 0.5); fine-structure correlation
  within ±1 ms 0.12 (it was 0.46 without the time differences: the early field is now much less coherent in its fine
  structure, which is what widens it); hard-left lean at least 5.6 dB; tail 0.21–0.39 from 250 Hz, 0.69–0.88 below,
  within A9 for seeds 1–8. At 50 %: tail 0.39–0.53, early fine structure 0.50. At 0 %: tail 0.57–0.67, early 0.88.
* **Not changed:** the 55° spread and the 6 dB lean (the lean still binds, A9's 3 dB is unchanged); the identical
  early allpasses on both sides (with the time differences they would add little, and 1.3 measured them fragile).

## R. Width follow-up and architecture review (2026-10-26)

After 1.3 the DAWg side found the reverb no longer wide enough at Early Width 100 %, and asked whether, given the
kind of changes the revisions needed, the fundamental architecture is still well founded or another would suit this
reverb better.

### R.1 What limits the width, measured

* **The tail carries most of the impression of width, and Early Width doesn't touch it** (5.5: the early-reflection
  controls act within the early span). 1.3 gave the tail a correlation of 0.3 above 150 Hz at every setting.
* **The early field is close to what A9 allows.** For a centred source, level-panned reflections give a coherence of
  twice the mean of cos φ · sin φ, and A9's lean (at least 3 dB to a hard-panned source's side, at every setting)
  forces most of each source's early energy onto its side, which keeps that mean up. Measured at Early Width 100 %
  (A9 asks the early coherence within ±1 ms to be at least 0.25):
  * time differences between the ears per reflection (the natural cue for lateral reflections, and the one that
    makes an early field wide and external on headphones): 0.4 ms → 0.12, 0.6 ms → 0.13. Each reflection peaks at
    a different lag, so no lag within ±1 ms collects much correlation. Fails A9.
  * pans wider than 55° (70°, 85°): 0.46–0.47, no change: the 6 dB lean guarantee narrows them back; with a 4.5 dB
    guarantee the worst lean drops to 3.15 dB (A9: 3) and the coherence stays at 0.47.
  * slightly different early allpasses per side (lengths 5–10 % apart): 0.24–0.26 at the defaults but 0.11 at
    Diffusion 100 %. Too fragile.
  So under A9 as written, the early field can't become much wider by any rendering that keeps sources in place.
  Questions 2 and 3 in records/QUESTIONS.md ask whether Early Width may widen the tail too, and whether A9's early
  criterion is meant to exclude time differences between the ears.
* **A finding on the way: the tail's limits held only for the default seed.** Across seeds 1–8, per-octave
  correlation near the crossover scatters by about ±0.1: with 1.3's targets the 250 Hz octave reached 0.49 (limit 0.5)
  and the octaves below 250 Hz fell to 0.49 (limit ≥ 0.5).
* **Change:** tail targets 0.25 above 150 Hz (was 0.3: wider) and 0.92 below (was 0.85: a firmer low end, which
  also keeps the 125 Hz octave above 0.5 for every seed). Over seeds 1–8: 0.20–0.47 from 250 Hz to 4 kHz, at least
  0.55 below; at the defaults 0.21–0.28 above 1 kHz (was 0.26–0.32). A9's test now checks eight seeds. This is the
  widest the tail can go with margin: a target of 0.2 put the 2 kHz octave at 0.15, A9's limit.

### R.2 What the revisions changed, and where

| Revision | Asked for | Changed | Where | Kind of change |
|---|---|---|---|---|
| 1.1 | smooth highs at Diffusion 100 % | allpasses inside each line, modulation moved into them, inner taps, shorter lines, scaled input diffusion | tail | an extension within the delay-network family ([S62], [G71], [D97]): no structural change |
| 1.1 | an early control that moves the timing | Early Timing (a warp of the tap times) | early stage | a new parameterisation of the patterns |
| 1.1 | (found) four early allpasses blurred the default early pattern | a squared gain curve | early stage | a correction |
| 1.2 | a tail that darkens with its length | one more absorption shelf per line (air absorption) | tail | native to Jot's design: one more filter of the kind it already had |
| 1.3 | coherent, natural stereo | mid/side output with a per-band side gain | after the tail | a rendering stage added after the network |
| 1.3 | sources keep their place, centred stays centred | level pans per reflection, a bisection to guarantee the lean, balancing of the two sides, the first reflection forced to the source's direction, identical early allpasses | early stage | four constraints enforced after the fact |
| review | more width | tail targets | after the tail | a parameter |
| (all) | real-time, exactness | crossfades, control-rate ramps, exact settling, skipped identity filters | processing | independent of the architecture |

Two patterns stand out:

1. **Everything asked of the tail fitted the architecture.** Smoothness, a decay that darkens with length and a
   natural stereo image were each met by one principled element in the place the framework provides for it:
   diffusion inside the loop, an absorption filter per line, a matrix at the outputs. The separations the design
   was built on (the decay set per band by gains and filters, the density by the matrix and the allpasses, the
   stereo image by the outputs, the early part apart from the tail) held every time. Several measured properties
   also became better rather than worse (the 8 kHz decay exact once nothing was interpolated, A15 with 0.1 margin).
2. **The early stage changed in every revision, and its spatial behaviour is enforced, not modelled.** It is a
   statistical tapped delay line: three seeded patterns crossfaded by Early Width, with times warped by Early Timing.
   Nothing in it knows where a reflection comes from, so "the source keeps its place", "a centred source stays
   centred", "the first reflection is the strongest" and "wide but natural" had to be added as rules (a lean
   guarantee by bisection, a balancing step, a special case for the first tap). That is a sign of a weak foundation
   in this one part. The specification keeps moving towards a natural room (a source with a position, lateral
   reflections, air absorption, a diffuse field's coherence), and a statistical pattern has no handle on those
   things.

### R.3 Alternatives for the whole

| Option | For | Against | Verdict |
|---|---|---|---|
| Keep the architecture as it is | every acceptance test passes, measured margins, known behaviour | the early stage's rules grow with each spatial request | sound for now |
| **Early reflections from a room model** (a virtual room whose dimensions follow Size, randomised per seed; image sources up to second or third order; the two input channels as two sources left and right of centre; each reflection rendered by its direction), as Moorer proposed for early reflections [M79], with the tail unchanged | the spatial rules become consequences: the first reflection comes from near the source, a panned source keeps its side, a centred source is symmetric, density grows with time as in a room, Size scales everything; time differences between the ears come for free if A9 allows them (question 3); the controls get physical meanings (Early Timing as the source's distance or the room's proportions, Early Width as the room's width or the spread of lateral reflections) | a rebuild of the early stage and the panel's arrivals; rectangular rooms give regular, flutter-like patterns unless randomised; A5 (the median shift of at least 30 %) and A6 (seeds below 0.5 correlation) have to be designed in; and **it would not make the reverb wider under A9 as written**: the coherence-and-lean bound of R.1 holds for any level-panned early field | **recommended as the next step for the early stage**, once questions 2 and 3 are answered |
| A spatial network: each line of the tail rendered at its own direction | one spatial model for early and late | the mid/side stage already sets the tail's coherence exactly per band; directions per line would set it only indirectly, the same for all frequencies | rejected |
| A different tail: an allpass-loop tank [D97], parallel combs and allpasses [S62], [M79], or convolution with a synthesised response | — | each loses something the revisions relied on: exact decay per band (A1, A7, A15), many independent paths for density and stereo (A14, A9), or real-time control and modulation (5.11) | rejected |

**Verdict.** The tail architecture (a unitary feedback delay network with Jot's per-band decay, allpasses inside
the loop and a stereo output stage) is well founded: it met three quite different revisions with small, principled
additions and should stay. The early-reflection stage is the weak part. Rebuilt on a geometric model it would be
more natural and would need fewer special rules, and it would be the right base if the spatial requirements keep
growing. But it would not by itself make the reverb wider under A9's current early-part limits. How much wider
Early Width 100 % can be is a question of the specification (questions 2 and 3), not of the architecture.

## 0. Revision for SPEC 1.3 (2026-10-26)

SPEC 1.3 finds the widest settings too wide, to the point of being disorienting, and asks for wide but natural
stereo: coherent, no two separate sides, no hole in the middle, not phasey, the source keeping its place, and a less
wide low end (5.10, A9's new limits, A17). Following CLAUDE.md ("Revisions"), the design as a whole was checked first.

### 0.1 Where the design stood, measured

With a mono impulse, the tail's left/right correlation was about 0 in every octave from 63 Hz to 16 kHz (−0.05 to
0.04): the network's two halves came out fully decorrelated, the low end included. In the early part the largest
left/right correlation within ±1 ms was 0.11, and at Early Width 100 % the early reflections were two independent
patterns of the *mid* signal, so a hard-left source's early part could even lean right (−1.3 dB). A9's new limits
failed on every count. This is the "two separate sides" of SPEC 5.10: each output channel was its own reverb.

### 0.2 Does the architecture still fit?

The network itself does. Its first pass already keeps a source on its side (the left input feeds lines 0–7, which
feed the left output before any mixing), and later passes spread it, as in a room. What was wrong is the two places
where the stereo image is rendered: the tail's output (two decorrelated halves) and the wide early field (independent
patterns of the mid signal). Real rooms behave differently. The coherence of a diffuse field at two points is high at
low frequencies and falls with frequency (textbook room acoustics), and early reflections arrive from directions
around the source, so they reach both ears with level differences, not as two unrelated sets.

| Option | For | Against | Verdict |
|---|---|---|---|
| Tail: take each output from overlapping sets of lines | no new filters | the correlation is set by the overlap, the same at every frequency: no solid low end (A9 asks ≥ 0.5 below 250 Hz and ≤ 0.5 above) | rejected |
| Tail: a fixed mid/side matrix (one side gain) | simple | the same: one correlation for all frequencies | rejected |
| **Tail: mid/side with the side scaled per band** (Blumlein/Gerzon stereo shuffling [G71], textbook mid/side) | sets the correlation directly, per band, for any network; linear, cheap (a crossover and a shelf), mono-safe because the correlation stays positive | none found | **chosen** |
| Tail or early: binaural rendering (interaural delays and head-related filters) | most natural on headphones | comb filtering and phasiness on speakers and in mono (SPEC 5.10 asks for both), more CPU, delays lower the coherence within ±1 ms | rejected |
| **Early: each input's own reflections, panned by level from its side up to a limit** (sine-cosine pan law) | the source keeps its side, a centred source stays centred, coherence at lag 0, mono-safe; Early Width keeps its meaning (narrow to wide) | needs care so that no seed or setting makes the lean too small | **chosen**, with a guarantee (0.3) |
| Early: independent left and right patterns of the mid signal (the old wide end) | very wide | exactly what 1.3 rejects: incoherent, sources lose their place | replaced |

### 0.3 The changes and how their constants were chosen

* **Tail output.** Mid M = (L + R)/2 and side S = (L − R)/2 of the network's output; out L = M′ + S′, R = M′ − S′.
  For uncorrelated L and R of equal energy, side gain k relative to mid gives a correlation (1 − k²)/(1 + k²), and
  mid a and side b = ka with a² + b² = 2 keep the energy. Targets: 0.85 below the crossover (a solid low end, the
  coherence of a real diffuse field at low frequencies) and 0.3 above (wide, with margins of 0.15 and 0.2 to A9's
  limits 0.15 and 0.5). S goes through a fourth-order Linkwitz–Riley crossover: its two bands stay in phase and their
  magnitudes add up to 1, so the side gain moves smoothly between the two values, with no dip where the bands meet.
  M only needs its low end lifted to keep the energy (+1.5 dB): one first-order shelf. Crossover 150 Hz: at 180 Hz the
  250 Hz octave measured 0.44 (limit 0.5); at 150 Hz, 0.41, with the 125 Hz octave at 0.65 (limit ≥ 0.5).
* **Early Width.** It still crossfades (equal power) from the shared pattern M, each input on its own side (narrow,
  coherent), to the side patterns, but these are now each input's own reflections: the left input's pattern A goes to
  the left with cos φ and to the right with sin φ, φ from 0 (the source's side) up to 55°; the right input's pattern B
  mirrored. 55°: for directions spread evenly over [0, P] the energy across is ½ − sin 2P/(4P) of the total, which
  gives a lean of 4.7 dB at 55° (2.1 dB at 72°), and the lag-0 coherence for a centred source is about 0.36.
* **The first reflection of each side pattern comes from the source's own direction** (φ = 0; physically, the floor
  reflection). Without that, its random direction could make a later reflection the loudest: the response's peak then
  moved to a later reflection at Size 0 % (A3 read as "the peak"), and "first within 6 dB of the peak" picked
  different reflections at 44.1 and 48 kHz.
* **A guaranteed lean.** With Early Timing at 0 % most of the early energy sits in the first few taps, so their
  directions decide the lean; each side pattern's spread is narrowed (bisection, bounded work) until its own side is
  at least 6 dB louder. Measured worst case over 216 settings (Early Width, Early Timing, Diffusion, Size, Feedback
  and Seed at their corners): with a 4.5 dB guarantee 3.6 dB; 6 dB gives 4.7 dB (A9 asks 3, at every setting); 8 dB
  gives 6.8 dB but lowers the early coherence to 0.50.
* **Balanced sides.** The two side patterns are scaled to the same shares of energy on their own side and across, so
  a centred source's early taps are exactly balanced (what remains is the tail's first passes inside the short early
  window: up to 1.1 dB by chance).
* **The early allpasses are the same on both sides** (their lengths used to differ, for decorrelation): with the
  panning providing the width, different lengths only cost coherence. Early correlation within ±1 ms at Early Width
  100 %: 0.26 with different lengths, 0.47 with the same.
* The pans come from their own random stream, so every seed's tap times and gains stayed as they were.

Result (A9, 4.1): tail correlation 0.81 (63 Hz), 0.65 (125 Hz), 0.41, 0.32, 0.32, 0.26, 0.27 (250 Hz–4 kHz), 0.28 and
0.31 (8 and 16 kHz), the same at the defaults and at Early Width 100 %; mono sum +1.1 dB above the channels' energies;
early coherence 0.54 at the defaults, 0.47 at the widest; a hard-left source's early part leans left by at least
4.7 dB. Everything else still passes.

## 0b. Revision for SPEC 1.2 (2026-10-26)

SPEC 1.2 asks that at long Feedback the reverb works as a lush, soft bed: the tail darkens as it decays, more so
the longer it is, also at Damp highs 0 %, while the start of the reverb of fresh input keeps its high frequencies
so that new sounds cut through (5.2, A15, A16). Following CLAUDE.md ("Revisions"), the design as a whole was
checked first.

### 0b.1 Where the design stood, measured

At Damp and Thin 0 the decay was flat by design (Jot's gains make every band decay alike unless a shelf says
otherwise): at Feedback 70 %, RT60 4.67 s at 500 Hz–2 kHz, 4.63 s at 4 kHz, 4.62 s at 8 kHz; at 90 % the highs' share
of the energy was the same at 2 s as in the first 100 ms (+0.5 dB). A15 failed on every count. Damp highs could
darken the tail, but it is a user control whose T60 ratio is the same at every Feedback, so at Damp 0 % there was no
darkening, and Damp also darkens fresh sound through its input shelves.

### 0b.2 Does the architecture still fit?

What 1.2 describes is a frequency-dependent decay whose high-frequency share shrinks as the tail gets longer, with
young energy staying bright. In a feedback delay network, energy loses high frequencies on every pass through the
absorption filters, so the first passes and the parallel early reflections stay bright and only old energy darkens:
the architecture already separates "fresh" from "decaying" in exactly the way 1.2 asks. What was missing was the
law. Real rooms follow one: air absorbs high frequencies at a fixed rate per second, whatever the room, so the
longer the room's reverberation, the larger the share of it the highs lose [M79] (Moorer put lowpass filters in his
comb loops to simulate this). A fixed loss per second gives RT60(8 kHz) / RT60(mid) = T_air / (T_mid + T_air):
falling as T_mid grows, as SPEC 5.2 asks ("increasingly so the longer the tail").

| Option | For | Against | Verdict |
|---|---|---|---|
| A fixed loss per second in every line (air absorption [M79]) as a second in-loop high shelf | darkens per pass, so fresh sound stays bright; the ratio falls with T60 by itself; independent of Damp, which stays on top; fits Jot's design [JC91] as one more passive shelf; cheap (16 one-pole filters) | changes the default sound a little (shorter tails darken slightly too, as in a real room) | **chosen** |
| Let Damp's T60 ratio fall with Feedback, with a minimum at 0 % | no new filter | Damp at 0 % would no longer mean "no extra damping"; its input shelves would still darken fresh sound if raised | rejected |
| A fixed T60 ratio built in (the same at every Feedback) | simplest | the ratio wouldn't fall with T60 (5.2 asks for "increasingly so"); short ambiences would be dull for no reason | rejected |
| A lowpass on the tail's output, or one that closes with time or level (envelope-controlled) | strong darkening | an output filter darkens the start too (A15's first 100 ms); a time- or level-driven filter darkens fresh sound whenever a pad sustains, and is no longer linear and time-invariant | rejected |
| Two reverbs in parallel: a bright short one and a dark long one | fresh sound bright, bed dark | twice the CPU, a two-slope decay that measures badly against A1, more to tune; the same effect comes from per-pass absorption in one network | rejected |

The rest of the design still fits 1.2: the parallel early reflections keep fresh highs untouched, the inner taps
and first passes deliver the start of the tail before the absorption has acted much, and the modulated in-loop
allpasses keep long tails from sounding metallic (A16: at Feedback 90 % the modes are about 0.2 Hz wide and
1.2 Hz apart, so a static network could ring; the allpass modulation moves the loops' group delay by up to
±0.5 ms, shifting mode frequencies by up to about 1 % of their frequency, tens of hertz at 1 kHz: far more than the
mode spacing, so modes are smeared rather than heard). (Corrected in 1.5: measured, the modulation hardly moves
the modes, P.1.)

### 0b.3 The change and how its constants were chosen

* Each line gets a second first-order high shelf, after Damp's, with corner 8 kHz and a gain of
  10^(−A·d/20) per pass for a loop of d seconds, where A = 60 dB/s is the loss well above the corner. A first-order
  shelf's loss grows as f² below its corner, like air absorption: about 12 dB/s at 4 kHz and 30 dB/s at 8 kHz, of
  the order of the air absorption in a room at moderate humidity (textbook acoustics). It is passive (1.1), and
  doesn't depend on Feedback, Size, Damp or Thin.
* Chosen by measurement, at the defaults with Damp and Thin at 0 (A15's ratios at Feedback 70 %, 4 kHz / 8 kHz):
  30 dB/s at 8 kHz gave 0.78 / 0.53 (fails); 45 dB/s at 8 kHz 0.73 / 0.46; 60 dB/s at 8 kHz 0.69 / 0.40; 35 dB/s at
  6 kHz 0.67 / 0.43; 45 dB/s at 6 kHz 0.63 / 0.38. 60 dB/s at 8 kHz has margins of 0.06 and 0.10 against A15 and
  takes less from 2 kHz (part of the mid RT60) than a 6 kHz corner.
* Damp highs now defaults to 0 % (it was 20 %): the natural darkening is built in, and Damp's input shelves would
  dull fresh sound, which 1.2 wants to cut through. Damp still darkens further on top (A7 unchanged in kind).
* For A15's comparison "without the darkening", ReverbNode::setDarkening (false) turns the shelf into an identity
  (measurement only; DAWg never calls it). With it off, the decay is flat again (1.59–1.64 s from 250 Hz to 8 kHz),
  so it is the reverb exactly as before the change.
* Result (A15, 4.4): ratios 0.69 / 0.40 at Feedback 70 %, falling to 0.47 / 0.20 at 100 %; at 90 % the highs in the
  first 100 ms lose 0.3 dB against the version without, and their share falls 27.5 dB by 2 s. Everything else still
  passes; A14's smoothness is unchanged within its scatter (4.3).

## 0c. Revision for SPEC 1.1 (2026-10-26)

SPEC 1.1 asks for a tail whose high frequencies are smooth, with no grain or sizzle, above all at Diffusion 100 %
(5.1, 5.4, A14), and for one early-reflection control that moves the reflections' timing (5.5, A5). Following
CLAUDE.md ("Revisions"), the whole design was re-evaluated, not only its parameters.

### 0c.1 What the first design did wrong, measured

A measurement was needed before a redesign: A14 is judged by ear, the clean side can't listen, so it proposes a
proxy (section 4.3) and checked it against the first design first.

* **Flutter, sizzle.** The suspicion was the delay modulation: Lagrange interpolation loses a fraction-dependent
  amount of the highs, and the modulation sweeps the fraction through a full cycle about 48 times a second. But
  compared with a phase-randomised copy of itself (same spectrum, no time variation), the high band of the reverb
  of steady pink noise fluctuates only 1 % more: the modulation adds no measurable flutter above 4 kHz. (A first
  comparison against the input seemed to show 7–18 %, but that was the interpolation's slight darkening of the
  spectrum, which the surrogate accounts for.)
* **Grain.** The high band's echo density (fraction of samples beyond one standard deviation in 5 ms windows, re
  Gaussian noise) is 1 once the tail is established, but low at its start: at Diffusion 100 % it falls to 0.34 at
  Size 20 % (20–40 ms: the gap between the early taps and the network's first output) and stays at 0.56–0.85 for
  the first 140 ms at Size 80 % (discrete early taps, and the network's first passes arriving as separate bursts).
  The impulse response's high band is impulsive there (90th-percentile excess kurtosis 2.8 at Size 80 %, Feedback
  30 %; Gaussian noise gives 0.2). That is what separately audible clicks in the highs look like, and it is worst on
  transients: it matches "grainy" and "individual echoes" in SPEC 5.4. 1.0's character asked for "slightly grainy",
  and the first design left exactly that.
* **Timing.** Early Shape changed only levels; Early Width swaps patterns of the same distribution. Neither moves
  the median arrival.

### 0c.2 Alternatives considered

| Architecture | For | Against | Verdict |
|---|---|---|---|
| Keep the design, raise allpass gains | small change | can't fill the onset gap or the sparse first passes: diffusion before the network only smears the input, each line still delivers separate bursts | rejected |
| Parallel combs and series allpasses [S62], with lowpass combs [M79] | simple, the historical baseline | lowest echo density, known metallic coloration; decay per band less exact | rejected |
| An allpass-loop "tank": series allpasses inside one or two long loops, modulated [D97] | builds density fast because the diffusion is inside the loop; smooth tails | one damping filter and one gain per loop: Damp/Thin per band and Size-dependent T60 less exact (A1, A3, A7); far fewer independent paths than a 16-line network, so stereo decorrelation and late density depend on careful tapping; and the paper presents its specific topology and constants in the style of a commercial unit, which the clean-room rules keep me from reproducing | rejected as a whole; its key idea, diffusion inside the loop, adopted |
| A 32-line FDN | doubles density per pass | twice the CPU (the modulated case was already at 2.6 %); the first pass is still separate bursts | rejected |
| **A unitary network with allpasses inside each line** (Gerzon's unitary feedback networks [G71] with Jot's decay design [JC91]; "allpass in the loop" [S62], [D97]) | keeps everything the FDN gives (exact T60 per band, provable stability, decorrelated stereo, Diffusion as the mixing angle); each pass is smeared by the line's allpass, so density grows by a dense burst per pass instead of one echo; allpasses are lossless, so the decay design is unchanged; the allpass coefficients can carry the modulation losslessly, without interpolating anything | 16 more short delay lines (cheap); allpasses colour transients if their gain is too high | **chosen** |

### 0c.3 The changes

1. **Allpasses inside the loop.** Each line gets a Schroeder allpass of 10–20 % of the size length (seeded), in
   normalised form (an orthogonal 2×2 rotation, so it stays lossless even while its coefficient moves). Its gain
   follows Diffusion. Jot's gains use the line's length plus the allpass's (the allpass's energy-weighted mean
   delay is exactly its length D: the response −g, (1−g²)g^(k−1) at kD has mean delay D).
2. **Modulation moves into those allpasses.** The delay lines are no longer modulated, so at rest nothing is
   interpolated. The allpass angles move slowly (seeded rates and phases), which moves the lines' dispersion and so
   the modes, with a flat magnitude response by construction.
3. **No gap at the onset.** The input allpasses scale with Size, the shortest lines get shorter relative to the
   size length (0.3 instead of 0.6), each line gets a second output tap a quarter of the way along, and the early
   part gets four allpasses instead of two, so the early field and the tail overlap and are each dense at Diffusion
   100 %. (The inner taps came from measuring what was left after the first three changes: 4.3, section 6.)
4. **Early Timing replaces Early Shape.** It warps the taps' times (bunched early ↔ gathered late) and lets the
   levels follow, so it moves the timing as SPEC 5.5 asks. Early Width stays: spatial, different from timing.
5. **A14's proxy** (4.3): the early echo-density profile, tail kurtosis and flutter, with thresholds chosen from the
   measurements, checked on SPEC 1.1's grid.

Sections 1 to 8 describe the design as it now stands (after 1.3); section 6 continues the decision log.

## 1. Architecture

```
in L,R ─► mute ramp ─┬────────────────────────────────────────────────────────────► × dry(mix) ─► (+) ─► out L,R
                     │                                                                            ▲
                     └─► tone: 2 low shelves (Thin) + 2 high shelves (Damp)                       │
                            │                                                                     │
                            ├─► input buffers (L, R, mid) ─► early taps ─► 4 allpasses ─┐          │
                            │       (3 seeded patterns, crossfaded when they change)     │          │
                            │                                                           (+) ─► pre-delay ─► × wet(mix)
                            └─► 4 allpasses ─► 16-line network, an allpass in each line ─┘
                                               (taps at each line's end and a quarter of the way along)
```

The reverb is the classic split into **early reflections** from a tapped delay line and a **late reverberator**
fed in parallel [M79]. The late part is a **unitary feedback delay network** [G71], [JC91] with a Schroeder allpass
inside each line [S62], [D97] and Schroeder allpasses for input diffusion [S62]. First decided 2026-10-26; revised
the same day for SPEC 1.1 (section 0c), 1.2 (section 0b) and 1.3 (section 0).

* **Why a feedback delay network for the tail.** [JC91] separates the two things a tail needs: the *decay time* is
  set exactly, per frequency, by a gain and absorption filters in each delay line, and the *density* comes from an
  orthogonal (lossless) feedback matrix. That makes Feedback, Size, Damp highs and Thin lows independent,
  well-defined controls (decay in seconds, bands in hertz), and stability provable: every loop gain is below 1, the
  absorption filters are passive, and the matrix and the in-loop allpasses are orthogonal, so the network cannot run
  away at any setting (SPEC 5.2). Measured: with Damp, Thin and the tail's darkening off, the decay is 1.59–1.64 s in
  every octave from 250 Hz to 8 kHz against the designed 1.62 s (125 Hz: 1.81 s, section 1.1).
* **Why an allpass inside each line (SPEC 1.1).** Each pass through a line is smeared into a dense burst instead of
  a single echo, so the echo density grows by a burst per pass and the high band is noise-like from the start of
  the tail (section 0c, 4.3). The allpasses are lossless, so the decay design is unchanged.
* **Why the tail darkens per pass (SPEC 1.2).** A fixed high-frequency loss per second in every line (like air
  absorption) darkens energy the longer it has circulated: fresh sound (the early reflections and the first passes)
  keeps its highs, the decaying bed loses them, and the longer the tail, the larger the share they lose (section 0b).
* **Why parallel early reflections.** Feeding the network from the input, not from the early taps, keeps the two
  early-reflection controls (5.5) from changing the tail, so they cannot move the RT60 (A5). Measured: they change it
  by less than 0.1 %.
* **Why the pre-delay is at the output.** The network is slowly modulated (5.1), so it is time-varying: delaying its
  *input* would let the modulation sit at a different phase when the delayed impulse arrives, and the response would
  no longer be an exact shift of the 0 ms response (A13). Delaying the reverb's *output* shifts the whole response
  exactly, early part and tail together, and the dry path stays undelayed (latency 0). Measured: correlation
  1.000000 with the shifted 0 ms response at 20, 100 and 250 ms.
* **Stereo** (revised for SPEC 1.3 and 1.4, sections 0 and Q). The left input feeds lines 0–7 and the right input
  lines 8–15, and the left output sums lines 0–7, the right lines 8–15 [G71]: a source's first pass stays on its
  side, later passes spread it. The two halves come out decorrelated, so the tail's output is then matrixed into mid
  and side with the side scaled per band (1.9): a natural correlation, about 0.92 in the low end and, above 150 Hz,
  from 0.6 to 0.25 as Width goes from 0 to 100 %. The early reflections come from directions around their source's
  side, by level and by time difference between the ears (1.3). Measured at Width 100 %: tail correlation 0.21–0.39
  from 250 Hz up, 0.69–0.88 below; mono sum +1.0 dB relative to the channels' energies (A9).

### 1.1 Feedback delay network

* **N = 16 lines.** Echo density grows with N and modal overlap with the total delay [JC91], [PASP]. 16 lines with
  loops of 0.4–1.55 × the size length (25–100 ms) give enough modal density for a smooth long tail at 32 butterflies
  per sample for the matrix (CPU, 7.6).
* **Matrix: a rotation network whose angle is set by Diffusion** (θ = (π/4)·d², revision 1.5: P.3). Four butterfly
  stages (strides 1, 2, 4, 8), each a plane rotation by θ of pairs of lines: (a, b) → (a cos θ + b sin θ, b cos θ − a
  sin θ). A product of rotations is orthogonal for every θ, so the network stays lossless while θ moves (and while cos θ
  and sin θ ramp separately, each butterfly's gain is √(cos² + sin²) ≤ 1: still passive). At θ = 0 it is the identity:
  16 independent comb filters [S62], i.e. separate, periodic, distinct echoes. At θ = π/4 every stage is a ±45°
  butterfly and the matrix has all entries ±1/4, the maximally mixing (Hadamard-like) case [JC91], [PASP]. Diffusion
  moves θ between them. (My construction from the orthogonality condition in [JC91].)
* **An allpass in each line** (revision 1.1). After the delay, each line passes a Schroeder allpass
  H(z) = (−g + z^−D)/(1 − g z^−D) [S62] of length D = 10–20 % of the size length (seeded), built in normalised form:
  the input x and the allpass's delayed state q give the output c·q − s·x and the new state c·x + s·q, with
  s = sin φ = g and c = cos φ. That 2×2 matrix is orthogonal, so the section is lossless even while φ moves
  ([G71]'s unitary networks; textbook normalised lattice [TXT]). Gain g = 0.5 × Diffusion: 0 at 0 % (a plain delay,
  so the echoes stay distinct), 0.5 at 100 %, below Schroeder's 0.7 because inside a loop every pass adds its
  colour. Jot's gains count the line plus the allpass: the allpass's impulse response −g, (1−g²)g^(k−1) at kD has
  an energy-weighted mean delay of exactly D. Its group delay does vary with frequency, from D(1−g)/(1+g) to
  D(1+g)/(1−g) with peaks at multiples of 1/D. Since revision 1.5 the allpass's delay carries its own decay gain
  (next bullets), so the modes' decay doesn't depend on that group delay (it had lengthened the decay near 125 Hz by
  12 %).
* **Line lengths.** Seeded: line i draws a length in its own stratum of a log-spaced range [0.3, 1.35] × the size
  length, assigned in a shuffled order so both halves (left, right) get short and long lines. Lengths never
  coincide, which [S62] and [PASP] ask for. The range starts at 0.3 so the tail begins while the early reflections
  are still arriving (revision 1.1: the first design's range [0.6, 1.6] left a sparse gap). Targets are whole
  samples.
* **Two output taps per line** (revision 1.1). Each side sums its lines' outputs (after their allpasses) and a
  second tap a quarter of the way along each line, at 0.7 times the level, with its own sign pattern. Several
  output taps per line multiply the echo density at no cost to the decay [M79], [JC91], and the inner taps let the
  tail start at about 0.08 × the size length.
* **Decay: Jot's gains.** Every delay element loses the same per sample: a line of d seconds gets the gain
  g = 10^(−3 d / T60) per pass, and the allpass inside it the same for its own delay, applied to its delayed state
  (revision 1.5, P.3). So every mode loses 60 dB in T60 seconds, whatever the lengths and the allpass's group delay
  at its frequency [JC91].
* **Absorption (Damp highs, Thin lows).** In each line, after the gain, a first-order low shelf (corner 200 Hz) and
  a first-order high shelf (corner 4 kHz) [TXT] whose shelf gains are Jot's extra loss per pass for a shorter T60
  below and above the corners [JC91]: G = 10^(−3 d (1/(ρT60) − 1/T60)) with ρ the band's T60 ratio. Built as
  1 + (G − 1)·LP and G + (1 − G)·LP, with LP a bilinear one-pole lowpass, they have a magnitude between G and 1 at
  every frequency: passive. At ρ = 1 they are exact identities.
* **The tail's own darkening** (revision 1.2; shape and law revised for 1.5, P.3). A third filter per line,
  G + (1 − G)·LP with LP a fixed second-order Butterworth lowpass at 6.5 kHz: a high shelf whose loss is nearly flat
  above 8 kHz, with about a third of the 8 kHz octave's loss in the 4 kHz octave. G = 10^(−L d/20) for a pass of d
  seconds, with a loss per second L = min(20 dB/s, 55 dB/s × 2.6 s / (T60 + 2.6 s)): fixed, like air absorption
  [M79], up to a T60 of about 4.5 s, weaker for longer tails. Because the loss is per second rather than per T60,
  the longer the tail, the smaller the ratio of the high bands' RT60 to the mids'. Passive like the others. Measured
  at the defaults with Damp and Thin 0 %, RT60 per octave 125 Hz–8 kHz: 1.70, 1.67, 1.61, 1.60, 1.52, 1.34, 1.00 s;
  at Feedback 90 %: 4 kHz 0.66 and 8 kHz 0.36 of the mid RT60 (A15).
* **Modulation (the "gently moving" tail, 5.1; revision 1.1).** The delay lengths are no longer modulated. Instead each
  in-loop allpass's angle φ moves around its centre by a slow sine (seeded rates 0.1–0.4 Hz and phases): depth 0.01 rad
  × Diffusion at the default size, scaled inversely with the size length so the group-delay excursion is the same at
  every size, and by min(1, 3 s / T60) so a long tail is a steady bed (revision 1.5, P.3). That moves the lines'
  dispersion, and with it the modes, with a flat magnitude response by construction: nothing is interpolated at rest, so
  no fraction-dependent loss of the highs (the first design lost 3 % of its 8 kHz decay to interpolation; now 1.63 s
  against 1.62 s). Calculation for the amount at Diffusion 100 %, default size: D ≈ 385 samples, dτ/dg at a group-delay
  peak = 2D/(1−g)² ≈ 3100 samples, Δg = cos φ × 0.01 ≈ 0.009, so the peak delay moves ±27 samples (0.56 ms); at 0.4 Hz
  that bends the pitch by 2π × 0.4 × 0.56 ms ≈ 0.14 % ≈ 2.4 cents at the most sensitive frequencies, much less
  elsewhere: movement, not chorus. At Diffusion 0 there is no modulation (the network is a set of combs by design). The
  angles are updated every 16 samples (a change of about 10⁻⁵ rad per update); sin and cos of the small offset use
  second- and third-order series, for which s² + c² = 1 − a⁴/12 ≤ 1, so the section stays passive.

### 1.2 Input diffusion

Four Schroeder allpasses per channel in series before the network [S62] smear each input impulse into a short
burst. Their gain is 0.7 × Diffusion (0 at 0 %, a plain delay). Lengths at the default size: left 4.31, 3.19, 2.27,
1.53 ms; right 4.73, 3.47, 2.53, 1.71 ms, decreasing by about 1.35× per stage, ratios far from simple fractions so
the stages' echoes don't coincide. Revision 1.1: they scale with the size length (0.47× at Size 0 %, 1.87× at
100 %), so a larger room gets a longer burst to match its longer lines.

### 1.3 Early reflections

* **Tapped delay line** [M79], [S62]: each output channel sums 16 taps from each of its two patterns, with seeded
  times and gains. Times are fractions of the **early span** that Size sets (5 ms to 100 ms, 2.3).
* **Pattern.** Seeded positions u ∈ (0, 1]: the first tap of every pattern is +1, the strongest before shaping, at
  4–8 % of the span; the last at 97–100 %; the 14 between stratified (one per slice, so ordered and never
  coinciding) and denser towards the end (slice^0.7): in a room the number of reflections grows with time. Gains
  0.4–1 with random signs.
* **Early Timing** (2.1) warps the positions to u^w and sets the levels' slope; the taps are at u^w × span.
* **Three patterns.** M (shared: the same times and gains for both channels, each input to its own side), A (the
  left input's own reflections, each from a direction between the left and 55° across, reaching the right up to
  0.65 ms later) and B (the right input's, mirrored); see Width (2.1) and 1.9.
* **Normalisation.** Each pattern is scaled to a fixed energy after shaping, so Early Timing and Width change
  where the energy goes, not how much there is.
* **Diffusion on the early part.** Four short allpasses per channel after the taps, lengths proportional to the span
  (1.1, 1.7, 2.9, 4.3 %, the same for both channels since 1.3, for coherence), gain 0.55 × Diffusion². Squared, so the pattern stays
  distinct at moderate settings (0.27 at the default 70 %: the response's peak stays on the first reflection) and is
  fully smeared only near 100 % (revision 1.1: four stages instead of two, for a dense early field). Because their
  lengths scale with the span, the whole early response scales with Size, so the "first strong reflection" stays
  the same reflection at every size (A3).
* **Whole samples and crossfades.** Tap times are always whole samples. When the taps change (Size, Early Timing,
  Width, Seed), the new tap set crossfades in linearly from the old over 10 ms; a change during a fade waits
  until it ends. A crossfade is click-free, keeps reflections crisp during changes, and lets a block's taps be summed
  with contiguous loops.

### 1.4 Tone (on the reverb only, 5.7)

Before the early taps and the network, the wet signal passes two identical first-order low shelves at 350 Hz (Thin
lows, each 0 to −20 dB) and two identical first-order high shelves at 3 kHz (Damp highs, each 0 to −12 dB). At 0 %
the shelf gain is exactly 1, which makes them exact identities. Together with the in-loop shelves of 1.1 they make
both the early part and the tail darker or thinner, the tail progressively more so as it decays. The dry path never
passes them. Measured at 100 %: −17.2 dB at 8 kHz, −20.8 dB at 100 Hz (A7 asks for 10 and 12). Damp highs defaults
to 0 % since 1.2 (2.5).

### 1.5 Mix, mute, pre-delay

* **Mix:** equal-power law, dry = cos(πm/2), wet = sin(πm/2) [TXT]: for an uncorrelated wet signal about as loud as
  the dry one, the sum's power is constant across the range (5.9). Measured with pink noise: within 0.3 dB of the
  input from 0 to 100 %. At 0 % the dry gain is exactly 1 and the wet exactly 0: the input bit for bit.
* **Mute:** a linear 5 ms gain ramp on the input before both the dry path and the reverb. Linear, so it reaches
  exactly 0 (input after the ramp never gets through, A8); 5 ms, so the largest step it adds to a full-scale signal
  is 1/240 at 48 kHz. The reverb's state is untouched, so its tail rings out (measured −4.9 dB 100 ms after Mute at
  Feedback 60 %). records/QUESTIONS.md #1, answered: a fade of up to 10 ms is acceptable, and A8's blocking applies
  from the end of the fade, which is what the clean side's A8 test checks.
* **Pre-delay:** a stereo delay line on the reverb output, read with linear interpolation at a delay that glides to
  the target: a one-pole with a 50 ms time constant, rate-limited to 0.5 ms per ms, landing exactly on the target
  (1.7). While gliding the reverb's pitch bends by at most a factor 0.5–1.5; it never clicks or drops out (5.4a).
  The target is a whole sample so that, at rest, the response is an exact shift.

### 1.6 Level normalisation

An exponentially decaying tail's energy grows in proportion to T60, which from 0.25 s to 10 s would be +16 dB,
against SPEC 5.2 and A2. The tail's gain is therefore √(1 − 10^(−6 d̄/T60)), the exact inverse of the energy of a
geometric series of passes with mean loop length d̄, times (T60/T60,default)^(1/8), so long tails still sound a
little bigger (energy ∝ T60^(1/4)). The early reflections have a fixed energy. The calibration constants (early
energy 0.53 per channel, tail level 1.29, inner taps included) put pink noise at Mix 100 % within −0.4 to +1.1 dB of
the input for Feedback 0–90 % (A2, measured after 1.3: the tail's now partly correlated channels add a little level
in the low end).

### 1.7 Processing, parameter changes and block-size independence

* Every derived quantity (gains, filter coefficients, delay targets, tap sets) is a pure function of the parameters
  and the sample rate, computed once at the start of a block when a parameter changed.
* **Ramps at a control rate** (revision 1.1, for CPU under modulation). Every 16 samples, counted from prepare() so
  the grid doesn't depend on the block sizes, each delay and gain plans its next point: a one-pole step towards its
  target (time constants 80 ms for delays, 20 ms for gains), delays rate-limited to 0.25 samples per sample (a pitch
  bend of at most 25 % while Size moves fast). In between it moves by a constant step: one addition per value per
  sample. The tone gains, the mute ramp and the pre-delay glide still move every sample.
* **Values arrive exactly** (fixed during the revision). A one-pole step in floating point never reaches its
  target: once the step is below half the value's resolution, adding it changes nothing, and the first design's
  values stopped short (a delay of a few thousand samples about 0.06 samples off, the pre-delay up to a sample off,
  which would have broken A13 after a Pre-delay change). Now, below a minimum change per step (10⁻⁶ for gains,
  10⁻⁴ samples for delays, per sample) a value moves linearly at that rate and then lands on its target; if rounding
  would still leave it in place it moves by one unit in the last place. Once every delay and gain is exactly on its
  target the node is "at rest": every delay is a whole number of samples and is read directly, which gives the same
  result as interpolation, and nothing ramps. Tested: after a change, the response is bit-identical to that of a
  node prepared with the new settings.
* A block runs in three stages: (1) per sample, the input stage (mute, tone smoothing, tone filters, input
  buffers); (2) the early taps over the whole block (constant within a block, crossfading if they changed); (3) per
  sample, the output stage (ramps, allpass modulation, early allpasses, network, pre-delay, mix). Every running value
  belongs to one stage and moves once per sample or per control period there, so the output is bit-identical for
  any sequence of block sizes (7.3, tested with controls changing too).
* No allocation, locks, I/O or exceptions in process(); every function on the audio thread is noexcept. All buffers
  are sized in prepare() for the longest delays the controls allow at that sample rate. The seeded pattern is a
  fixed-size struct drawn in bounded time (about two hundred random numbers) when Seed changes. Denormals are
  flushed by the SDK's EffectNode (7.2).
* Meters (SPEC 6): the arrivals are recomputed on the message thread from the parameters (a pure function: no
  shared state); the peak levels are atomics the audio thread raises with compare-exchange and meters() resets.

### 1.8 Randomness

A SplitMix64 generator [TXT] seeded with the Seed value draws, in a fixed order and in two independent streams, the
three early patterns (times, gains, signs) and the tail's texture (line and allpass lengths, their order,
modulation rates and phases). The same seed always gives the same output (5.6); a different seed changes the early
pattern and the tail (measured: correlation of seeds 1 and 2 −0.005, A6).

### 1.9 Stereo (SPEC 5.10; revision 1.3)

* **Tail output:** mid M = (L + R)/2 and side S = (L − R)/2 of the network's two outputs. S is split by a
  fourth-order Linkwitz–Riley crossover at 150 Hz (two Butterworth sections per band [TXT]; the bands stay in phase)
  and scaled by 0.28 below and, above, by 0.87 at Width 100 % (0.63 at 0 %); M is scaled by 1.12 (1.26 at 0 %) with a
  first-order low shelf lifting its low end to the low band's gain (1.39). Out L = M′ + S′, R = M′ − S′. These are
  the mid and side gains a and b = ka (a² + b² = 2) that give uncorrelated, equal-energy channels a correlation
  (1 − k²)/(1 + k²) of 0.92 below and, above, 0.6 + (0.25 − 0.6) × Width, at unchanged energy (sections 0.3, R.1,
  Q.2). The gains are smoothed targets.
* **Early field:** Width crossfades from M (each input on its own side, coherent and narrow) to A and B, each
  input's own lateral reflections (2.1). Each reflection's direction φ sets its levels (sine-cosine law: constant
  energy) and its time difference between the ears (the far side later by 0.65 ms × cos 2φ; SPEC 1.4 allows 0.7 ms). Each side pattern's directions are
  narrowed if needed so it leans at least 6 dB to its source's side, the first reflection comes from the source's
  direction, and the two side patterns are balanced so a centred source stays centred (section 0.3).
* **Centred and panned sources:** a mono source's early part and tail are balanced within 1.1 dB at every setting
  tested (the early taps exactly; the rest is the tail's first passes falling in the short window by chance); a
  hard-left source's early part leans left by at least 5.6 dB.

## 2. Controls, curves and defaults

| Id | Name | Default | Curve |
|---|---|---|---|
| `feedback` | Feedback | 45 % | T60 = 0.25 s × 64^f × size factor |
| `diffusion` | Diffusion | 70 % | angle θ = (π/4)·d²; allpass gains 0.7·d (input), 0.5·d (in-loop), 0.55·d² (early); modulation depth ∝ d |
| `size` | Size | 55 % | early span = 5 ms × 20^s; size length = 25 ms × 4^s |
| `predelay` | Pre-delay | 10 ms | delay of the reverb output, a whole sample, glides |
| `earlyTiming` | Early Timing | 40 % | tap times u^w with w = 2.5^(1−2t); level slope (2t − 1) × 24 dB across the span |
| `width` | Width | 75 % | 0–75 % (1.5's 0–100 %, new = 0.75 × old): early: equal-power crossfade from shared to each input's own lateral reflections; tail: correlation above 150 Hz from 0.6 to 0.25. 75–100 %: the tail on to 0 (S.2) |
| `damp` | Damp highs | 0 % | HF T60 ratio 10^(−1.5 d²); 2 input high shelves, −12 dB × d^1.5 each (on top of the tail's own darkening) |
| `thin` | Thin lows | 20 % | LF T60 ratio 10^(−0.6 t); 2 input low shelves, 0 to −20 dB each |
| `modulation` | Modulation | 25 % | 0–25 %: the in-loop allpasses' movement × m / 25 %; 25–100 %: the lines' delays move too, 0.15 ms × c^1.5 × min(1, 10 s / T60), c = (m − 25 %) / 75 % (S.3) |
| `mute` | Mute | off | 5 ms linear ramp on the input |
| `mix` | Mix | 25 % | equal power |
| `seed` | Seed | 1 | SplitMix64 seed |

### 2.1 The two early-reflection controls (SPEC 5.5)

1. **`earlyTiming`, "Early Timing": when the reflections arrive within the span** (revision 1.1, replacing Early
   Shape, which only changed levels). With t the control from 0 to 1, each tap's position u becomes u^w with
   w = 2.5^(1−2t): at 0 % (w = 2.5) the reflections are bunched right after the onset, at 50 % (w = 1) they keep the
   pattern's natural spacing, at 100 % (w = 0.4) they gather towards the end of the span. The levels follow, with a
   slope of (2t − 1) × 24 dB across the span: early and loud first (a close, tight room) at 0 %, late and building up
   (a swelling, reverse-like early field leading into the tail) at 100 %. The energy is normalised, so it moves the
   early energy in time without changing its amount. Measured: the panel's median arrival moves by 70–73 % of the
   span from 0 to 100 % (A5 asks at least 30 %), with every arrival at a new time; early correlation 0 % vs 100 %:
   0.31 (0.26 with the window counted from the input).
   Default 40 % (w = 1.2, slope −4.8 dB): slightly front-weighted and gently decaying, like a natural room.
2. **`width`, "Width": how wide the whole reverb is** (revised for SPEC 1.3; renamed and extended to the tail for
   SPEC 1.4, section Q). At 0 % both channels get the same early pattern (M), each from its own input, and the tail's
   channels correlate 0.6 above 150 Hz: narrow, following the source's position, mono-compatible. At 100 % each input
   has its own pattern of reflections arriving from around its side, with level and time differences between the
   ears by direction (up to 55° across, up to 0.65 ms), and the tail correlates 0.25: wide and enveloping, but
   coherent, with the source keeping its place (it leans at least 6 dB to its side, the first reflection comes from its
   direction) and a centred source staying centred. In between, an equal-power crossfade (cos and sin of width × π/2)
   keeps the early energy constant, and the tail's correlation moves linearly. Measured early correlation 0 % vs
   100 %: 0.27 (0.14 from the input); fine-structure correlation of the early part 0.88 / 0.50 / 0.12 at 0 / 50 /
   100 %. Default 100 % (section Q.2). Since SPEC 1.6 that is 75 %: the scale was compressed to 0–75 %, and
   75–100 % widens the tail further (S.2).

The change Early Timing makes and the change Width makes correlate 0.02 (A5 asks |c| < 0.7). Neither changes the
decay (RT60 1.542 vs 1.542 s and 1.539 vs 1.542 s; A5 allows 25 %); Width changes the tail's width only. Both differ from Size (which scales the whole span) and from
Diffusion (which smears the reflections). Ids are lower-case camelCase and don't clash with the fixed ones; names
are 12 and 5 characters.

### 2.2 Diffusion

θ = (π/4)·d² (since 1.5: below about 50 % the lines mix slowly and the tail keeps some grain, so Diffusion sets how much
grain the tail has, A14; P.3); input allpass gain 0.7·d; in-loop allpass gain 0.5·d; early allpass gain 0.55·d²;
modulation depth 0.01 rad·d. [S62] uses 0.7 for series allpasses, above which a single allpass rings audibly: 0.7 is the
ceiling outside the loop; inside the loop, where every pass adds an allpass's colour, 0.5. Measured echo density (peaks
above −40 dB per 10 ms, 50–150 ms): 10.5, 127.5, 154.6, 157.6, 159.1, 157.9 at 0–100 % in 20 % steps (measured with the
linear angle; A4 at 1.5: 11.5 vs 159.7). The count saturates from about 60 % (at 48 kHz a dense signal has a local peak
every three samples or so). SPEC 1.5's roughness (A14) above 8 kHz: 0.93–1.04 times the reference at 100 %, 0.97–1.02 at
70 %, 1.02–1.87 at 40 % (4.1).

### 2.3 Size

* Early span = 5 ms × 20^s: 5 ms at 0 %, 100 ms at 100 % (5.3); exponential so equal steps are equal ratios.
* Size length L = 25 ms × 4^s. Lines 0.3–1.35 L, in-loop allpasses 0.1–0.2 L, inner taps at a quarter of each line,
  input allpasses scaled by L/L(default).
* T60 size factor = (L / L(default))^(1/2), 0.66 at 0 % to 1.32 at 100 %: larger sizes lengthen the decay (5.3)
  monotonically but less than in proportion (Sabine's T60 ∝ volume/area would be proportional to L), so Feedback
  stays the main decay control. Measured RT60 at Size 0–100 %: 1.07 to 2.14 s, rising at every 10 % step (A3).
* Default 55 %: a 26 ms early span and a 53 ms size length, a medium room (4.2).

### 2.4 Feedback

T60 = 0.25 s × 64^f: 0.25 s at 0 % (A1 asks ≤ 0.4 s, measured 0.24 s) and 16 s at 100 % (A1 asks ≥ 8 s, measured
12.9 s: the tail's darkening (1.1) takes a little from the 2 kHz band, which is part of A1's RT60). Exponential, so
every step multiplies the decay by the same ratio: steady, no jumps (measured: every 5 % step 1.05–1.5×, monotonic).
Default 45 %: 1.6 s designed, 1.54 s measured (500 Hz–2 kHz), a medium plate-like length for a vocal or a snare
(4.2).

### 2.5 Damp highs, Thin lows

* Damp: HF T60 ratio 10^(−1.5 d²) (1 at 0 %, 0.03 at 100 %), in-loop shelf corner 4 kHz; two input high shelves at
  3 kHz, −12 dB × d^1.5 each (one first-order shelf takes at most about 9 dB off at 8 kHz). Both curves start gently
  (revision 1.5, A7: at 10 % the highs lose 1.5 dB one second in). Measured tail at 8 kHz re 0 %: −2.1, −6.2,
  −11.5, −16.4, −19.4 dB at 20–100 %: gentle at first, progressive, clearly dull at 100 %.
* Thin: LF T60 ratio 10^(−0.6 t) (1 to 0.25), in-loop shelf corner 200 Hz; two input low shelves at 350 Hz, 0 to
  −20 dB each. Measured at 100 Hz: −7.7, −14.1, −18.3, −20.6, −21.8 dB at 20–100 %.
* Defaults: Damp highs 0 % (since 1.2: the tail darkens by itself, and fresh sound should keep its highs to cut
  through; it was 20 %), Thin lows 20 % (a slightly leaner tail keeps a snare's low end from building up). Measured
  at Damp and Thin 0 %, RT60 per octave from 125 Hz to 8 kHz: 1.70, 1.67, 1.61, 1.60, 1.52, 1.34, 1.00 s (1.5).

### 2.6 Mix and the other defaults

* Mix default 25 %: dry at −0.7 dB, wet at −8.3 dB re the input: "the dry sound still leads" (4.2).
* Pre-delay default 10 ms: separates the dry onset from the reverb, keeping a vocal's consonants and a snare's attack
  clear without sounding like an echo.
* Diffusion default 70 %: smooth and dense (5.1) while the early pattern keeps some definition (its allpasses are at
  0.27).

## 3. Every other constant

| Constant | Value | Reason (2026-10-26 unless dated) |
|---|---|---|
| Lines, taps per pattern | 16, 16 | 1.1, 1.3; 32 arrivals per channel at most = SPEC 6's limit |
| Line lengths | 0.3–1.35 × L, log-stratified | 1.1: spread without coinciding; tail starts early |
| In-loop allpass lengths | 0.1–0.2 × L, uniform per line | 1.1: about a tenth of a loop, long enough to smear a pass, short enough not to ring as an echo |
| In-loop allpass modulation | 0.01 rad × d × L(default)/L × min(1, 3 s / T60), 0.1–0.4 Hz | 1.1: about 2.4 cents at most at Diffusion 100 %; 2026-10-27 (P.3): 3 s keeps the defaults' movement and takes the pad corners' to 0.4 and 0.22 of it |
| In-loop allpass decay | 10^(−3 D / (T60 fs)) on each allpass's delayed state | 2026-10-27 (P.3): every delay element loses the same per sample [JC91] |
| Inner taps | 0.25 of each line, level 0.7 | 1.1: chosen by measurement (lowest early density 0.89–0.90 at Size 0–80 % with 25 %, 0.78–0.87 with 40 %) |
| Input/output/inner signs | fixed ± patterns, half of each sign per side | arbitrary but balanced |
| Injection | 1/√8 per line | each input feeds 8 lines with unit total energy |
| Early tap positions | first 4–8 %, last 97–100 %, power 0.7 | 1.3 |
| Early tap gains | first +1, others 0.4–1 with random signs | 1.3 |
| Early Timing | warp 2.5^(1−2t), slope ±24 dB | 2.1 |
| Early energy, tail level | 0.53, 1.29 | 1.6, calibrated by A2 |
| Tail level exponent | 1/8 | 1.6: ±2 dB over the Feedback range |
| Early allpass lengths | 1.1, 1.7, 2.9, 4.3 % (L), 1.3, 1.9, 3.1, 4.7 % (R) of the span | 1.3 |
| Input allpass lengths | 4.31, 3.19, 2.27, 1.53 ms (L); 4.73, 3.47, 2.53, 1.71 ms (R) at the default size | 1.2 |
| Control period | 16 samples | 1.7: 0.33 ms at 48 kHz, far below anything audible in a ramp |
| Gain ramps | 20 ms one-pole per period | short enough to follow modulation, long enough that a full-range jump has no audible step (A10) |
| Delay ramps | 80 ms one-pole per period, ≤ 0.25 samples/sample | 1.7: Size changes bend pitch by at most 25 % |
| Pre-delay glide | 50 ms one-pole, ≤ 0.5 samples/sample | 1.5 |
| Minimum changes | 10⁻⁶ (gains), 10⁻⁴ samples (delays) per sample | 1.7: below audibility, and values land exactly |
| Early crossfade | 10 ms linear | 1.3 |
| Mute ramp | 5 ms linear | 1.5; QUESTIONS #1 allows up to 10 ms |
| Arrivals threshold | taps within 20 dB of the channel's strongest | SPEC 6 "main" reflections; at most 32 |
| Tail correlation | 0.92 below; above a 150 Hz Linkwitz–Riley crossover (mid/side), 0.6 at Width 0 % to 0.25 at 100 % | 0.3 (revision 1.3), R.1 (widened): the widest that stays inside A9 for seeds 1–8; 150 Hz chosen by measurement (180 Hz: 0.44 in the 250 Hz octave) |
| Early directions | level pans from 0 to 55°, first reflection at 0°, at least 6 dB to the source's side; time difference 0.65 ms × cos 2φ | 0.65 ms: about a head's width, within SPEC 1.4's 0.7 ms (Q.2); 0.3: 55° from the pan-law integral (4.7 dB); 6 dB chosen by measurement from 4.5, 6 and 8 dB |
| Chorus (revision 1.6) | line delay excursion 0.15 ms × c^1.5 × min(1, 10 s / T60); 1–2 Hz per line, seeded; fifth-order Lagrange reads | S.3 |
| Very wide (revision 1.6) | tail correlation above 150 Hz from 0.25 to 0 between Width 75 and 100 %; 0.92 below | S.2 |
| The tail's darkening | G + (1 − G)·LP, LP second-order Butterworth at 6.5 kHz; loss above the corner min(20 dB/s, 55 dB/s × 2.6 s / (T60 + 2.6 s)) | 2026-10-27, P.3: flat above 8 kHz for A14; 6.5 kHz for A15's 4 kHz/8 kHz balance; 55 dB/s and 2.6 s fitted to A15's two ends, 20 dB/s keeps the default's tone (it was 60 dB/s above an 8 kHz first-order corner, 0b.3) |

## 4. Measurements (harness, 2026-10-26; updated for 1.5 on 2026-10-27)

All numbers from `build/Release/tests` (node/AcceptanceTests.cpp, node/BehaviourTests.cpp), 48 kHz, Mix 100 % unless
stated, other settings at their defaults. Methods are in node/Measure.h: RT60 is the mean over the 500 Hz, 1 kHz and
2 kHz octave bands (two cascaded RBJ bandpasses) of a T20 fit to Schroeder's backward integral of both channels'
energy; correlations are normalised and taken over both channels.

### 4.1 Acceptance tests (SPEC 8)

| # | Measured | Required |
|---|---|---|
| A1 | RT60 at Feedback 0, 10, …, 100 %: 0.25, 0.37, 0.56, 0.82, 1.26, 1.90, 2.88, 4.28, 6.41, 9.64, 14.51 s | ≤ 0.4, ≥ 8, monotonic |
| A2 | Pink noise output re input, Feedback 0–90 %: −1.27 … +1.46 dB (100 %: +1.76) | ±6 dB |
| A3 | First reflection within 6 dB of the peak, Size 0–100 %: 10.65 … 23.10 ms (with the 10 ms pre-delay), rising at every step; within 20 dB: 10.38 … 18.81 ms; the response's peak is the first reflection at every size; RT60 1.06 … 2.12 s, rising; arrivals span at 100 %: 95.4 and 96.2 ms (L, R) | monotonic; ≥ 80 ms |
| A4 | Echo density 50–150 ms: 11.5 (0 %) vs 159.7 (100 %) peaks per 10 ms | ≥ 4× |
| A5 | Early correlation 0 vs 100 %: Early Timing 0.36 (0.31 with the window from the input), Width 0.27 (0.14); RT60 change < 0.1 %; correlation of the two changes 0.02; Early Timing moves the median arrival 63–67 % of the span, every arrival to a new time | < 0.7; ≤ 25 %; < 0.7; ≥ 30 % |
| A6 | Seeds 1 and 2: correlation 0.006; same seed: bit-identical | < 0.5; identical |
| A7 | Damp 100 %: −19.4 dB at 8 kHz; Thin 100 %: −21.7 dB at 100 Hz; Mix 0 %: bit-identical; Damp 10 %: energy above 4 kHz at 0.9–1.1 s −1.5 dB re 0 % (1.4: −8.7) | ≥ 10, ≥ 12 dB; unchanged; ≥ −3 dB |
| A8 | Input after the 5 ms ramp: output bit-identical to silent input; tail 100 ms after Mute: −4.4 dB; largest step with Mute toggled: 0.0065 (Mix 25 %), 0.0075 (Mix 100 %) | blocked; audible; ≤ 0.05 |
| A9 | Mono impulse, at the defaults = Width 100 %: tail L/R correlation 0.83 (63 Hz), 0.63 (125 Hz), 0.41, 0.31, 0.22, 0.19, 0.23 (250 Hz–4 kHz), 0.23, 0.24 (8, 16 kHz); over seeds 1–8: 0.19–0.49 at 250 Hz–4 kHz, at least 0.56 below; mono sum +0.9 dB re the channels' energies; early part, envelope correlation (5 ms) 0.99. Hard-left source: early part at least 5.6 dB louder on the left over 216 settings. (Width 0 % and 50 %: tail 0.56–0.68 and 0.37–0.54 above 250 Hz, reported only) | 0.15–0.5 at 250 Hz–4 kHz, ≥ 0.5 below, > −0.1 everywhere; ≥ −3 dB; ≥ 0.5; ≥ 3 dB |
| A10 | Largest step with each control swept over 1 s / unswept, 1 kHz sine, 64-sample blocks: ≤ 1.46× at Mix 25 % and ≤ 1.55× at Mix 100 % (Size, which glides the lines); Mix swept 0–100 %: 1.15× | ≤ 4× |
| A11 | 60 s at 48 kHz: 1.06–1.39 s on 2026-10-27 (defaults, and at Feedback, Size, Diffusion 100 %), with the accepted 1.4 build measuring 1.11–1.32 s alongside, so 1.5 costs nothing measurable; the machine runs slower than in the first sessions (0.84–0.91 s then) | ≤ 1.8 s |
| A12 | node/InterfaceTests.cpp: all pass | pass |
| A13 | Before the pre-delay: exactly 0; correlation with the shifted 0 ms response: 1.000000 at 20, 100, 250 ms; Mix 50 %: dry impulse 0.7071 at sample 0 | < −80 dB; > 0.9; at 0 |
| A14 | SPEC 1.5's roughness above 8 kHz (4.5), Feedback 30/60/90 %, Size 20/55/80/100 %: Diffusion 100 % 0.93–1.04 times the reference (1.4: 1.18–1.34); 70 % 0.97–1.02, within 0.06 of 100 % (question 4); 40 % 1.02–1.87, never lower than 100 %. The 1.1 proxies (4.3) still pass. Listening: section 7 | ≤ 1.10; not lower at 70, 40 % |
| A15 | Damp and Thin 0: RT60 at 4 kHz / 8 kHz re 500 Hz–2 kHz: 0.70 / 0.41 at Feedback 70 %, 0.68 / 0.38 at 80 %, 0.66 / 0.36 at 90 %, 0.65 / 0.35 at 100 % (falling); at Diffusion and Size 100 %: 0.70 / 0.39, 0.68 / 0.37, 0.66 / 0.36, 0.64 / 0.34 (falling); at 90 %: highs in the first 100 ms −0.1 dB re no darkening, their share −19.1 dB at 2 s re the first 100 ms (4.4) | ≤ 0.75 / ≤ 0.5, not rising; at the pad corner ≥ 0.6 / ≥ 0.3; ≥ −3 dB; ≤ −12 dB |
| A16 | Listening (DAWg side): section 7, item 3 | listening |
| A17 | Listening (DAWg side): section 7, item 2 | listening |
| A18 | Seeds 1–3, four ideal draws: first pad corner swing 0.90–1.09 dB against 0.53–1.62, drift 0.94 / 0.48 / 0.27 / 0.11 / 0.06 cents against 0.49 / 0.31 / 0.15 / 0.16 / 0.03 (110–1760 Hz); second corner swing 0.14–0.25 against 0.07–0.20 dB, drift ≤ 0.12 against ≤ 0.08 cents (P.4; robustness over 30 seeds there) | ≤ ideal + 1 dB; ≤ ideal + 1 cent (110 Hz), + 0.3 (from 220 Hz) |
| A19 | Release unevenness re the ideal: 0.92 (first corner), 1.09 (second) | ≤ 1.15 |
| A20 | Listening (DAWg side): section 7, item 1 | listening |

### 4.2 Further behaviour

* Sample rates (7.4), 44.1/48/96/192 kHz: RT60 1.54/1.54/1.53/1.53 s, first reflection 13.37–13.41 ms, band levels
  within 1.3 dB at 8 kHz and 0.0 dB at 100 Hz.
* Block sizes (7.3): bit-identical for 64, 128 and 512-sample blocks while Size, Feedback, Pre-delay and Early Timing
  change every 512 samples; the interface test with irregular blocks passes.
* Settling (5.11): after Size, Feedback, Early Timing, Damp and Pre-delay change, the response is bit-identical to a
  freshly prepared node's.
* Peaks (7.2): largest sample 1.87 over 64 corners of the controls with −12 dBFS noise (limit 8.0).
* Seed change at Mix 100 %: largest step 1.53× the unchanged one (5.11).
* Mix loudness with pink noise: 0.0, 0.2, 0.6, 0.9, 1.0 dB at 0–100 % (5.9).
* Centred source (5.10): a mono impulse's left and right energies within 1.1 dB, early part and tail, over Early
  Width, Early Timing and Seed corners.
* Decay per octave at Damp 0, Thin 0, 125 Hz–8 kHz: 1.70, 1.67, 1.61, 1.60, 1.52, 1.34, 1.00 s; with the tail's
  darkening off: 1.70, 1.67, 1.61, 1.62, 1.61, 1.63, 1.62 s against 1.62 s designed (125 Hz was 1.81 before the
  allpasses carried their own decay, P.3).

### 4.3 A14: a measurement for smooth high frequencies

SPEC 1.1 asks the clean side to propose a measurement that tracks what the DAWg side hears as grain or sizzle. Grain
in a dense tail is the high band behaving unlike noise: separate clicks standing out (impulsive, sparse), or an
envelope that flutters. Three measures on the band above 4 kHz (two cascaded RBJ highpasses), all textbook
statistics:

1. **Echo density profile** (grain): in 5 ms windows, the fraction of samples beyond one standard deviation of the
   window, divided by its value for Gaussian noise (0.3173). 1 means as dense and smooth as noise; separate echoes
   give much less (Diffusion 0: about 0.2). Reported on the impulse response from 20 to 150 ms after the reverb's
   onset: the mean and the lowest window.
2. **Excess kurtosis** (crackle in the decaying tail): kurtosis − 3 of the high band in 10 ms windows from 20 ms after
   the input has stopped until the band has decayed 40 dB; the 90th percentile over windows. Gaussian noise gives
   0 with a scatter of about 0.25 at this window length; clicks push it up. On the impulse response, the snare-like
   hit and a 100 ms pink-noise burst.
3. **Flutter** (sizzle in a steady sound): with steady pink noise in, the coefficient of variation of 2 ms frame
   energies of the high band, divided by the same for a phase-randomised copy of the output (same spectrum, no time
   variation). 1 means the high band's envelope fluctuates exactly as much as its spectrum explains.

Thresholds, checked at SPEC 1.1's grid (Diffusion 100 %; Feedback 30, 60, 90 %; Size 20, 80 %): density mean ≥ 0.93
and lowest ≥ 0.8; impulse kurtosis p90 ≤ 0.8, snare and pink burst ≤ 0.5; flutter ≤ 1.05. The first design fails
them (lowest density 0.34, impulse kurtosis 2.8); the revised one passes:

| Size, Feedback | Density mean / lowest | Kurtosis p90: impulse, snare, pink burst | First design: lowest, impulse kurtosis |
|---|---|---|---|
| 20 %, 30 % | 0.98 / 0.85 | 0.38, 0.29, 0.44 | 0.34, 0.50 |
| 20 %, 60 % | 0.99 / 0.89 | 0.33, 0.29, 0.31 | 0.34, 0.34 |
| 20 %, 90 % | 0.99 / 0.89 | 0.35, 0.33, 0.34 | 0.34, 0.31 |
| 80 %, 30 % | 0.96 / 0.84 | 0.26, 0.34, 0.28 | 0.56, 2.76 |
| 80 %, 60 % | 0.98 / 0.86 | 0.34, 0.33, 0.35 | 0.56, 0.60 |
| 80 %, 90 % | 0.98 / 0.86 | 0.36, 0.34, 0.34 | 0.56, 0.38 |

(Measured after the 1.3 change, at its defaults: Damp 0 %. After 1.1, with Damp 20 %, the lowest densities were
0.85–0.91 and the impulse kurtosis 0.25–0.57; the differences are within the measures' scatter.)

Flutter with steady pink noise: 0.97 (Feedback 60 %) and 0.98 (90 %); the first design gave 1.01. At Size 100 %
the first 10–25 ms after the onset stay sparse (0.24–0.70): there a large hall's first reflections arrive one by
one, as they should; from 20 ms after the onset it stays above 0.79.

Limits of the proxy: kurtosis on noise-like inputs (the snare, the pink burst) barely tells designs apart, because
the input is already noise; the impulse response is the sensitive probe. None of these measures can say whether
the tail *sounds* smooth: the DAWg side's listening (A14) decides.

### 4.4 A15: how the darkening is measured

Following SPEC 8's introduction, with Damp highs and Thin lows at 0 % and the other controls at their defaults, an
impulse in, Mix 100 %:

* RT60 at 4 kHz and 8 kHz: the same T20 method as the mid RT60, on one octave band (node/Measure.h, rt60At), divided
  by the mean of the 500 Hz, 1 kHz and 2 kHz octaves. At Feedback 70, 80, 90 and 100 %.
* "A version without the darkening": the same node with ReverbNode::setDarkening (false), which makes the darkening
  shelf an identity and changes nothing else; its decay is flat (4.2), the reverb as it was before 1.2. The energy
  above 4 kHz (two cascaded RBJ highpasses at 4 kHz) in the first 100 ms of the reverb (from the 10 ms pre-delay to
  110 ms) is compared between the two.
* The highs' share: energy above 4 kHz minus energy at 500 Hz–2 kHz (highpasses at 500 Hz and lowpasses at 2 kHz,
  each two cascaded RBJ sections), in dB, over 1.95–2.05 s after the impulse, minus the same over the first 100 ms.

All in node/AcceptanceTests.cpp, test A15. Since 1.5 also at Diffusion and Size 100 %, where A15's lower limits
apply.

### 4.5 SPEC 1.5's measurements (A14, A18, A19)

Implemented as SPEC 1.5 defines them, in node/Measure.h (2026-10-27); where the definition leaves a detail open, the
choice is stated:

* **Filters:** fourth-order Butterworth (two biquads, Q 0.5412 and 1.3066, bilinear transform with prewarping), run
  forwards and backwards. A18's band-pass is a fourth-order highpass at 0.8 f followed by a fourth-order lowpass at
  1.25 f (SPEC: "band-passed from 0.8 f to 1.25 f (fourth order)"; the reading is the clean side's).
* **A14 roughness:** 2 ms frames (96 samples at 48 kHz); the 25-frame centred moving average is taken where it fits
  (12 frames cut at each end); the reference's random phases are drawn on the segment zero-padded to a power of two,
  its first 0.8 s kept and scaled by the ratio of 50 ms centred moving RMS values. The ratio is the response's
  roughness (mean of both channels) over the reference's (mean of both channels and four draws).
* **The ideal reverb:** per channel, Gaussian noise (std::mt19937, normal distribution; seeds 1–4) times
  10^(−3t/T), T the node's RT60 in the octave band nearest f (125 Hz for 110 Hz, …, 2 kHz for 1760 Hz), measured
  with the same T20 method as A1. Two responses are convolved with the note in one complex FFT.
* **A18:** the analytic signal by FFT (negative frequencies removed); instantaneous frequency from the phase step
  between samples; the 20 ms moving averages centred; the held note's 5 ms fade occupies 7.995–8.0 s.
* **A19:** frames start 50 ms after the note's end (8.05 s); the cubic is a least-squares fit over frame index.
* Different random-number libraries (the DAWg side's, MSVC's) draw different noise; the ideal reverb's figures then
  differ by its sampling scatter (P.4, question 5).

## 5. Build notes

* Revision 1.7: CMake and MSVC (Visual Studio 2022) were available in this session, so the node, the tests and
  the measurements were built with both MSVC (Release, a fresh build/msvc) and g++ −O3. Before that:
* This session has no CMake on its PATH (the guard confines it to the workspace). Everything was built with MSYS2
  g++ 14.2 using the same sources, include paths and warning flags as CMakeLists.txt (`-std=c++20 -O2 -Wall -Wextra
  -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`), with a script kept outside version control.
  CMakeLists.txt lists every file (node/Pattern.cpp in `reverb_node`; node/AcceptanceTests.cpp,
  node/BehaviourTests.cpp, node/Measure.cpp in `tests`). The DAWg side's MSVC build found two C4244 warnings that g++
  doesn't give; both were fixed. **The DAWg side should confirm the CMake/MSVC build again**, which this session can't
  run.
* CPU (revision 1.3): while Thin lows or Damp highs is at 0 %, the in-loop shelves for it are exact identities and
  are skipped (bit-identical output), which pays for most of the stereo stage at the defaults.
* No clang-format was available either; the code follows .clang-format by hand (checked: no line over 120 columns,
  no tabs).
* Bit-identical output across block sizes relies on the compiler not contracting a*b+c into fused multiply-adds
  differently in different loops. Default x86-64 builds have no FMA instructions, so this holds; with FMA enabled,
  build with `-ffp-contract=off` (MSVC's default `/fp:precise` doesn't contract).

## 6. How the design changed (decision log, 2026-10-26)

1. **First design** (SPEC 1.0): an FDN with modulated delay lengths (Lagrange interpolation), parallel early taps,
   Early Shape (levels across the span) and Early Width, one input damp shelf, a 10 ms mute ramp, per-tap glides.
2. **Level calibration** (A2): the wet level up 2.5 dB.
3. **Mute ramp 10 → 5 ms** (QUESTIONS #1; since answered: up to 10 ms is fine).
4. **CPU:** whole-sample reads at rest, early taps crossfading between whole-sample sets, three-stage blocks, array
   loops: 1.34 → 0.9 s static, 2.0 → 1.5 s modulated, output unchanged.
5. **Margins:** Early Shape ±24 → ±36 dB; a second input damp shelf (Damp −18 dB at 8 kHz).
6. **MSVC warnings:** two narrowing conversions in the tests (C4244), fixed.
7. **SPEC 1.1 revision** (section 0c): a measurement for A14 first (4.3), which showed the grain as sparse high-band
   echo density at the tail's onset and no measurable flutter from the modulation; then, after comparing the
   architectures in 0c.2: an allpass inside each line, carrying the modulation instead of the delay lengths; lines
   from 0.3 L; input allpasses scaled with Size; four early allpasses with a squared gain curve (with four stages
   at a linear curve the early pattern blurred at the default Diffusion and the response's peak moved into the tail);
   inner output taps (chosen by measuring 40 % and 25 %: 25 % lifted the lowest early density from 0.41–0.85 to
   0.89–0.90 at Size 0–55 %); Early Timing replacing Early Shape.
8. **Ramps at a control rate** (revision): Size modulated every block took 2.0–2.2 s for 60 s after the revision;
   ramps every 16 samples, the allpass angles updated every 16 samples and whole-sample allpass reads at rest bring
   it to 1.5–1.7 s (static 0.86–0.94 s).
9. **Exact arrival** (found while writing the settling test): one-pole steps stalled short of their targets in
   floating point, so after a change the node never came to rest (and the pre-delay could stay up to a sample off).
   The per-sample glide had also skipped the new delays. Fixed with approach() (1.7) and ramps over every group;
   tested.
10. **SPEC 1.2 revision** (section 0b): measured flat decay at Damp 0 %; compared five ways to darken the tail; chose
    a fixed loss per second per line (like air absorption), 60 dB/s above 8 kHz, from five measured candidates;
    Damp highs' default 20 → 0 %; A15 tested; a measurement switch, setDarkening.
11. **SPEC 1.3 revision** (section 0): measured a fully decorrelated tail and an incoherent wide early field; kept
    the network; the tail's output matrixed into mid and side with a per-band side gain (crossover 150 Hz, chosen
    over 180 Hz by measurement); Early Width's wide end replaced by each input's own level-panned reflections (55°),
    with the first reflection on the source's side, a guaranteed 6 dB lean (chosen from 4.5, 6 and 8 dB by
    measurement), balanced sides and the same early allpasses on both sides; A9's test follows 1.3; a centred-source
    test added.
12. **CPU after 1.3:** identity Thin/Damp shelves skipped (5).
13. **Width follow-up and review** (section R): tail targets 0.25 / 0.92, A9 checked over eight seeds; interaural
    delays, wider pans and per-side early allpasses rejected by measurement; questions 2 and 3; architecture review
    with a recommendation for the early stage.
14. **SPEC 1.4** (section Q): Width replaces Early Width and sets the tail too; interaural time differences by
    direction; default 100 %; A9's test on the envelope measure.
15. **SPEC 1.5** (section P, 2026-10-27): SPEC 1.5's measurements implemented first. A14's grain traced to the
    darkening's slope above 8 kHz; the darkening became a second-order shape with a loss that weakens for long tails
    (capped at 1.2's fixed rate below 4.5 s after the uncapped law darkened the default); Damp's curves start
    gently; Diffusion's angle squared; every delay element loses the same per sample; modulation scaled down for
    long tails. Longer lines (Size range ×3, ×2), 32 lines and convolution compared and rejected; A18/A19 measured
    over 30 seeds against 32 ideal draws; questions 4 and 5.

## 7. To check by listening

The clean side cannot listen. These were chosen by measurement and literature; the DAWg side should audition them.

0. **SPEC 1.6 (A24).** On headphones and on speakers:
   * **Width 100 %** on a snare and a vocal: very wide and enveloping, yet a centred sound stays centred, no
     hole in the middle, not phasey, the bass solid (its correlation stays 0.92 below 150 Hz; S.2). Also the
     sweep from 75 to 100 %: a steady widening, no jump.
   * **Modulation 100 %** in the pad corner on the pad chord and the three-saw lead: lush and chorused, not
     seasick, out of tune or vibrato-like (1–2 Hz per line; 4–6 cents of drift; S.3). If it sounds too fast,
     the rate and depth trade off as S.3 measured (slower needs more depth, and the bass then wobbles).
   * **Modulation 0 %:** static but still smooth, no ringing tones. It is the accepted sound without its slow
     movement, which never smeared the modes much (P.1), so ringing is unlikely; sparse material (a sine, a
     snare at Feedback 95 %) shows it first.
   * Modulation 25 % and Width 75 % are 1.5's sound exactly (A21).

1. **A steady bed (A20, 5.1; new in 1.5).** In the pad corner (Feedback 90 %, Diffusion and Size 100 %; and
   Feedback, Diffusion and Size 80 %) at Mix 30–50 %: a sustained pad chord with a bass note, and the lead of three
   slightly detuned saws on held notes. Listen for:
   * swelling, dipping or wobble in level or pitch (the lead's own beating aside). The modulation is now much
     smaller in long tails (0.22 and 0.4 of before in the two corners). What remains is the network's modal
     structure: over 30 seeds the held notes' swing and drift match an ideal, time-invariant reverb's medians, with
     the bass drift about 0.05–0.1 cents above (P.4). If the bass still wobbles audibly, the next step would be more
     modal density (P.2 has the measured options and why they were not taken);
   * whether a long tail with almost no movement now rings or sounds metallic or "frozen" on sparse material (a
     single held sine, a snare at Feedback 95–100 %): the modulation no longer moves anything much there (it never
     smeared the modes as much as 0b.2 expected, P.1);
   * the release: do held notes' tails fade evenly?
   * the highs: smooth, no grain (A14, item 4), and not dying away early: at the pad corner the 8 kHz octave now
     rings for 0.36 of the mid RT60 (it was 0.24), the 4 kHz octave for 0.66 (0.52);
   * whether the steady bed sounds too static at the default and medium settings: below a 3 s decay the movement is
     unchanged.
2. **Wide but natural (A17, 5.10; new in 1.3, revised for 1.4).** A snare and a vocal at Feedback 80 %, Size 100 %,
   Diffusion 100 %, Width 100 % (the default now; the only width-related control), on headphones and on speakers.
   Listen for:
   * wide enough now? After the feedback that the widest setting wasn't wide enough, Width 100 % adds time
     differences between the ears to the early reflections (up to 0.65 ms, by direction) and takes the tail to 0.25,
     the widest A9 allows for every seed (section Q). If it is still not wide enough, the tail can't go further within
     A9; the early field could (more spread, if the 6 dB lean guarantee may come down towards A9's 3 dB);
   * wide and enveloping, but coherent: one room around the source, not two reverbs, one per side; no hole in the
     middle on speakers; nothing phasey, behind or inside the head on headphones. The time differences should make
     the early field sound more external on headphones; on speakers, listen for comb-like colour or phasiness in
     the early part, and in mono;
   * a hard-panned source keeping its place (its early part leans at least 5.6 dB to its side); a centred source's
     reverb staying centred;
   * the low end: solid and less wide than the highs (0.69–0.88 below 150 Hz); a step in width around 150 Hz on
     bass-heavy material;
   * Width from 0 to 100 %: from narrow (tail 0.6, early field on the source's side) to wide and natural, never
     exaggerated; and whether 100 % as the default suits a vocal or a snare, or whether the default should be
     narrower (A9 would then need the tail's band at the defaults to allow it).
3. **The bed (A16, 5.2; new in 1.2).** A sustained pad at Feedback 85–95 % and Mix 30–50 %, then drums or a vocal
   entering on top. Listen for:
   * a lush, soft bed: the tail should warm up as it decays (highs dying away faster than the mids), never hissy,
     sizzly or metallic; the darkening is built in at Damp 0 %, Damp adds more;
   * the new sound's attack staying clear above the bed: its early reflections and the first ~100 ms of its tail keep
     their highs (measured: −0.3 dB against no darkening), so a snare or a consonant should cut through;
   * metallic ringing or a "frozen" tail at Feedback 95–100 % (item 1);
   * too dark or too bright a bed, or too dark short reverbs. The loss above 6.5 kHz is a fixed 20 dB/s up to a
     4.5 s decay (the default's 8 kHz RT60 is 0.63 of the mids', 0.65 in 1.4) and weaker beyond, 0.36 at the pad
     corner (1.5). The darkening's three constants (section 3) are what to change;
   * the low end at long Feedback: at Thin 0 % the 125 Hz band now decays about 5 % longer than the mids (it was
     15 %, P.3).
4. **Smooth highs (A14, 5.1, 5.4).** Diffusion 100 %, Feedback 30/60/90 %, Size 20/80 %, on a snare, a vocal and
   pink-noise bursts: no grain, sizzle or crackle in the highs, no individual echoes. 1.5 found the grain the DAWg
   side heard in the darkening's slope above 8 kHz and flattened it (P.1, P.3): listen above all to the highs of the
   first second of the tail on bright sources (the detuned saws, cymbals). Also listen to the first 150 ms after a
   snare hit at Size 80 % and Feedback 30 %, and to the onset of very large sizes (Size 90–100 %), where the first
   reflections still arrive one by one.
5. **Movement (5.1).** At the default and medium settings, on sustained notes and pads: a gentle movement, no
   chorusing or vibrato (about 2.4 cents at most at Diffusion 100 %, less at lower Diffusion, none at 0; less in
   tails longer than 3 s, item 1).
6. **Colour.** In-loop allpasses can colour transients: listen for ringing, a "boing" or a hollow quality on snares at
   Diffusion 100 %.
7. **Defaults (4.2).** On a dry vocal and a snare: a useful medium reverb with the dry still leading? Damp 0 % (new),
   Thin 20 %, Mix 25 %, Pre-delay 10 ms, Diffusion 70 %. With Damp at 0 the early reflections are full-bandwidth:
   is a vocal's sibilance in the reverb acceptable?
8. **Early Timing (A5's confirmation).** On a snare or a clap at Size 70 %, Feedback 30 %: at 0 % the reflections
   come bunched and loud right after the hit and die away (a tight, close room); at 100 % they gather late in the
   early span and swell into the tail; 50 % is the pattern's natural spacing.
9. **Width (A5's confirmation).** On headphones with a mono source: at 0 % the early reflections sit in the centre
   with the source and the tail is narrow; at 100 % the reflections come from around the source and the tail is wide,
   wide but coherent, without two separate sides. With a hard-panned source, its reflections stay on its side at
   every width.
10. **Diffusion (5.4):** from 0 % (separate echoes, comb-like by design) to 100 % (a smooth wash), steady by ear.
    Since 1.5 the mixing angle goes with Diffusion², so the tail keeps audible grain below about 50 % and is smooth
    from about 70 %: is that the progression wanted ("Diffusion is how much grain the tail has")?
11. **Size and Pre-delay changes (5.11, 5.4a):** sweeping Size bends the tail's pitch while its lines glide (at most
   25 %) and crossfades the early reflections; sweeping Pre-delay glides the reverb's pitch (factor 0.5–1.5 at most).
12. **Seed (5.6):** several seeds at the defaults: audibly different, none clearly bad.
13. **Damp highs, Thin lows (5.7)** at 10, 50 and 100 %: a little Damp (8–10 %) takes the edge off without making
    the highs die away much sooner (1.5); progressive, clearly dull and clearly thin at 100 %, Damp on top of the
    built-in darkening.
14. **Mute (5.8):** no click on drums or bass when toggled.
15. **Sample rates (7.4):** the same patch at 44.1 and 96 kHz should sound the same.

## 8. Summary

**Built:** a stereo algorithmic reverb node, `dawg.loopReverb`, in node/ (Reverb.h/.cpp: the node; Pattern.h/.cpp:
the seeded patterns; Dsp.h: delay lines, shelves, allpasses, ramps). Seeded early reflections from a tapped delay
line (three patterns, timed and widened by the two early-reflection controls, crossfaded when they change, four
allpasses for diffusion) in parallel with a 16-line unitary feedback delay network: a rotation-network matrix whose
angle is the Diffusion, a slowly modulated lossless allpass inside each line, two output taps per line, Jot's
per-line gains and passive absorption shelves, among them a fixed high-frequency loss per second that darkens the
tail as it decays. Schroeder allpass input diffusion, tone shelves on the reverb only, an output pre-delay that
shifts the whole response exactly, equal-power mix and a 5 ms mute ramp. Real-time safe, block-size independent,
sample-rate independent, 0 latency; every running value lands exactly on its target.

**SPEC 1.7 draft (section T):** the network stays and computes exactly what it computed (bit-identical with
each compiler), at about 60 % of the cost under MSVC: two delay banks, whole delays converted once, and the
per-line arithmetic four lines at a time (node/Simd.h: SSE2, NEON or scalar, no fused multiply-add). In silence the
node sleeps 1 s after its tail falls below −120 dBFS and restarts as prepared on the next input (A25: bit-identical
to a fresh node, block-size independent). The first pad corner's tail is still audible 20 s into A25's silence, so
it can't sleep in A25's window; question 7.

**SPEC 1.6 (section S):** the network stays, because A21 asks for the accepted output to 100 dB at the new
controls' defaults; both additions are exact no-ops there (bit-identical to c9ba0bf). Width now runs
0–75 % for 1.5's 0–100 % (new = 0.75 × old) and widens the tail on to uncorrelated mid and high frequencies up to
100 % (A22 passes). Modulation (default 25 %) fades the accepted allpass movement in from a time-invariant 0 %.
Above 25 % it moves each line's delay at 1–2 Hz, read with fifth-order interpolation, into a 4–6 cent chorus at
100 % (A23's 100 % and "nothing else changes" pass). A23 at 0 % fails at 110–440 Hz through 1.5's rare notches,
which the ideal reverb fails as often (S.4). Question 6 concerns the interface test's control count.

**SPEC 1.5 (section P):** SPEC 1.5's measurements were implemented first, as written. The grain the DAWg side
heard above 8 kHz came from the tail's darkening, whose loss kept rising across that band. The fix:
* **Darkening:** a second-order shape that is flat above 8 kHz, with a loss per second that is fixed up to a 4.5 s
  decay and weaker beyond, so a long tail's highs keep ringing (A15's new lower limits).
* **Damp highs** starts gently (A7).
* **Diffusion's** mixing angle goes with Diffusion², so low settings keep grain (A14's ordering).
* **Steady bed:** every delay element, the in-loop allpasses' included, loses the same per sample (uniform mode
  decay), and the modulation shrinks for tails longer than 3 s.

The network stays. Convolution, 32 lines and longer lines were compared and rejected (P.2). Held notes now match an
ideal, time-invariant reverb's medians over 30 seeds; seeds 1–3 pass A18 and A19. Two questions record where the
specification's measurements are decided by scatter (4: A14 at 70 %; 5: A18/A19's rare notches). A20 and the
steady bed are for listening (section 7, item 1).

**SPEC 1.4 (section Q):** the design was checked against 1.4 first; its changes are about rendering, so the
architecture stays (the room model for the early stage remains an option). Early Width became Width, which also sets
the tail's width (0.6 to 0.25); early reflections got time differences between the ears by direction (0.65 ms at
most); Width defaults to 100 %, because A9's tail band only holds near the widest setting; A9's test uses the new
envelope measure.

**Width follow-up and architecture review (section R):** the tail widened to the limit A9 allows for every seed;
interaural delays, wider pans and decorrelated early allpasses measured and rejected under A9's early limits; two
questions recorded. The review found the tail architecture well founded (three revisions met with small, principled
additions) and the early-reflection stage the weak part (spatial behaviour enforced by rules); a rebuild of that
stage on a room model is recommended as the next step once the width questions are answered.

**SPEC 1.3:** checked as a whole first (section 0): the network still fits, its first pass keeps a source on its
side; what changed is how stereo is rendered. The tail's output is matrixed into mid and side with the side scaled
per band (a natural correlation: solid low end, wide but coherent highs), and Early Width's wide end is now each
input's own reflections, panned by level around its side, with a guaranteed lean and balanced sides. A9's new limits
pass with margins; A17 is for listening (section 7, item 1).

**SPEC 1.2:** the design was checked as a whole first (section 0b): a network that absorbs per pass already keeps
fresh sound bright and darkens only old energy, so the architecture fits; what it lacked was a darkening law that
grows with the tail's length. A fixed loss per second, like air absorption, gives exactly that, independent of
Damp. A15 passes with margins; A16 is for listening (section 7, item 1).

**SPEC 1.1:** re-evaluated from the architecture down (section 0c): diffusion inside the loop, modulation moved into
lossless allpasses; A14's proposed measurement (4.3) passes on the whole grid, where the first design failed it.

**The two early-reflection controls:** `earlyTiming` "Early Timing" (when the reflections arrive: bunched early to
gathered late, levels following; moves the median arrival 63–67 % of the span) and `width` "Width" (the width of the
whole reverb: shared, narrow reflections and a narrow tail at 0 %, each source's own reflections around its side with
time differences between the ears and a wide tail at 100 %).

**Covered by the clean side's own tests** (node/AcceptanceTests.cpp, node/BehaviourTests.cpp, 38 tests, together with
node/InterfaceTests.cpp; with g++ −O3 and with MSVC, all passing but A11's absolute times on this session's loaded
machine (T.2), and under MSVC A21's samples recorded from g++ as before): A1, A2, A3, A4, A5 (including the timing
control's median shift), A6, A7 (with 1.5's Damp 10 %), A8, A9 (SPEC 1.4's limits, the tail over eight seeds), A10,
A11 (1.7's limits, best of three), A13, A14 (SPEC 1.5's roughness, 4.5, and the 1.1 proxies, 4.3), A15 (4.4, with
1.5's lower limits), A18 and A19 (4.5), A21 against the accepted output, A22, A23 (S.4), A25 (T.3), and A12
through the interface tests; plus 5.2, 5.4, 5.6/5.11 (seed changes; settling after a change), 5.7, 5.9, 5.10 (a centred
source stays centred), 6, 7.2 (128 corners), 7.3 (with moving controls) and 7.4 (four sample rates).

**Left to check by listening:** section 7: above all the steady bed in the pad corner (A20), the stereo image at
the widest settings (A17), the bed with fresh sound on top (A16), the smooth highs (A14),
the movement and colour of the in-loop allpasses, the defaults (Damp 0 %, Width 100 %), and what Early Timing and
Width do (A5).

**Open:** SPEC 1.7 is a draft; A11 on the DAWg side's idle machine (T.2); question 7 (records/QUESTIONS.md): A25 in
the first pad corner, and the restart after a silence (T.3, T.4). No incidents: nothing in this
workspace described another reverb's internals (records/INCIDENTS.md unchanged).
