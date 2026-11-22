# Functional specification: DAWg's Loop Reverb

| | |
|---|---|
| Version | 1.7 draft, not yet approved (1.6 approved for handover; changes from 1.0 in section 11) |
| Written by | the DAWg side (Oliver Leenders), 2026-10-26 (1.5, 1.6: 2026-10-27; 1.7: 2026-11-16) |
| Approved by the monitor | Oliver Leenders, 2026-10-26 (1.5, 1.6: 2026-10-27; records/MONITOR.md) |
| Specification hash | in records/MONITOR.md |

## 1. Purpose

Build a stereo algorithmic reverb as an effect node for DAWg, an open-source digital audio workstation
(AGPL-3.0-or-later). This document says **what** the reverb must do. **How** it does it (its architecture, its
structure, every constant and curve) is for the implementer to design, and to explain in DESIGN.md.

## 2. Ground rules

* Work only from this workspace: this specification, the SDK in sdk/, the harness in harness/, the skeleton in node/,
  the answers in records/QUESTIONS.md, and the published literature in section 9.
* Do not look at any other reverb's implementation, structure, patches, presets or internal parameters, and do not
  search for them. That includes DAWg's own repository and any existing reverb in DAWg.
* Open questions go in records/QUESTIONS.md. Answers come back through the monitor and only ever describe behaviour.
* Record every design decision, its reason and its date in DESIGN.md.

## 3. The host

* DAWg runs a graph of nodes per track. An effect node has one stereo input and one stereo output.
* The engine calls `prepare()` when the sample rate or the largest block size changes, then `process()` for each
  block. Block lengths vary from 1 sample to `maxBlockSize`.
* Parameters are set between blocks: by the user, by automation, or by modulation (an LFO or envelope routed to a
  parameter), which can change a continuous parameter every block. The node reads them with `params().get()`.
* sdk/include/dawg_sdk/Node.h is DAWg's node API, without JUCE. The DAWg side integrates the finished node into
  DAWg; nothing in this workspace needs to change for that.

## 4. Interface

* Type `dawg.loopReverb`, display name "Loop Reverb", category effect, one stereo input and one stereo output
  (`node/Reverb.h` has the descriptor).
* Latency: 0 samples.
* `prepare()` clears all state: after it, silence in gives silence out.

### 4.1 Controls

Ids, names, ranges, units and kinds of the nine named controls are fixed (saved projects refer to them). The two
early-reflection controls are the implementer's to define (5.5): their ids, names and what they do; only their range,
unit and kind are fixed. The same holds for the modulation control (5.12) and, if the implementer adds one, a
second width control (5.10). The order of all controls is free.

| Id | Name | Range | Unit | Kind |
|---|---|---|---|---|
| `feedback` | Feedback | 0–100 | % | continuous |
| `diffusion` | Diffusion | 0–100 | % | continuous |
| `size` | Size | 0–100 | % | continuous |
| `predelay` | Pre-delay | 0–250 | ms | continuous |
| *implementer's* | *implementer's* | 0–100 | % | continuous |
| *implementer's* | *implementer's* | 0–100 | % | continuous |
| `damp` | Damp highs | 0–100 | % | continuous |
| `thin` | Thin lows | 0–100 | % | continuous |
| *implementer's* | *implementer's* (the modulation control, 5.12) | 0–100 | % | continuous |
| *implementer's, optional* | *implementer's* (a second width control, 5.10, if the implementer adds one) | 0–100 | % | continuous |
| `mute` | Mute | 0–1 | | toggle |
| `mix` | Mix | 0–100 | % | continuous |
| `seed` | Seed | 1–999,999 | | integer |

### 4.2 Defaults

The implementer chooses every default and records why. The defaults together must give a useful medium-sized reverb
on a vocal or a snare drum, at a mix where the dry sound still leads. Mute defaults to off.

## 5. Behaviour

### 5.1 Character

A dense, smooth algorithmic reverb with a gently moving tail (a little slow pitch movement in the tail is welcome;
audible chorusing or vibrato is not, at the modulation control's default and below: 5.12). The tail's high frequencies are smooth: no grainy, sizzling or crackling
texture, at any setting and above all at high Diffusion. It should sound good on drums, vocals, synthesisers and pads,
from short ambience to long, washy tails.

At long, dense, large settings (Feedback, Diffusion and Size all high) the tail is a steady, almost pad-like bed: a
held note or chord doesn't swell, dip or wobble audibly in level or pitch, in the bass as much as higher up, and its
tail after release fades evenly. The tail's slow movement stays below that. Smaller and shorter settings may keep the
irregularity of a real room (A18, A19).

### 5.2 Feedback

How long the tail lasts. At 0 % only a short ambience remains; at 100 % the tail is very long. The decay time grows
steadily (no jumps) across the range. At any setting and with any input the reverb stays stable: it never runs
away, and its level doesn't keep climbing as Feedback rises.

At long settings the reverb must work as a lush, soft bed of reverberation under a mix. For that, the tail darkens
as it decays: high frequencies die away faster than the mids, increasingly so the longer the tail, so a long tail
becomes warm and soft rather than staying bright and hissy. Fresh input must cut through this bed: the start of the
reverb of a new sound (its early reflections and the first part of its tail) keeps its high frequencies, and only
the decaying part darkens. This darkening is part of the reverb's character and happens with Damp highs at 0 %;
Damp highs (5.7) darkens further on top of it. The darkening is no stronger than this needs: a long tail stays warm
but not dull, and its high frequencies still ring for a fair share of the tail's length (A15).

### 5.3 Size

The scale of the space: how far apart in time the early reflections are, from a few milliseconds (small) to about a
tenth of a second (large). Larger sizes also lengthen the decay.

### 5.4 Diffusion

From distinct, separately audible echoes at 0 % to a smooth, dense wash at 100 %. At 100 % the tail is much smoother
than "dense" alone: no grain or sizzle in the high frequencies and no audible individual echoes, at every Feedback
and Size, on transients (a snare) as well as on sustained sounds.

### 5.4a Pre-delay

The time between the dry sound and the start of the reverb, in milliseconds: the whole reverb (early reflections and
tail) starts that much later, the dry sound is not delayed, and the node's latency stays 0. At 0 ms the reverb starts
as it would without a pre-delay. Pre-delay doesn't change the reverb's sound otherwise: its decay, tone and pattern
stay the same. It can be modulated like any continuous control; while it changes, the reverb may glide or crossfade
to the new time, but must not click or drop out.

### 5.5 Early-reflection controls

Two controls give the user a say over the early reflections, within the span that Size sets. What each one does,
its id and its name are the implementer's design: choose two that are musically useful, clearly audible, and
different from each other and from Size and Diffusion, and record the choice in DESIGN.md. Ids are lower-case
camelCase and must not clash with the fixed ones; names are at most 12 characters. One of the two must move the timing of the reflections: it changes when they
arrive within the span, not only how loud they are. The other may instead, or as well, set the width of the whole
reverb, early part and tail (narrower at its minimum, wider at its maximum); 5.10 holds at every setting. (DAWg's earlier version had two
controls here as well; the DAWg side maps or resets their saved values at integration. Nothing about them is given
here on purpose.)

### 5.6 Seed

Chooses a random variation of the early-reflection pattern and the tail's texture, so the same settings can give a
different character. Different seeds sound audibly different. The same seed and settings always give exactly the
same output (DAWg's panel offers a dice button that picks a new seed; it must be possible to go back to a previous
pattern by typing its number).

### 5.7 Damp highs and Thin lows

They shape the tone of the reverb, never the dry sound. Damp highs makes the reverb progressively darker (at 100 %,
clearly dull), starting gently: a little Damp highs takes the edge off the reverb without making its high
frequencies die away much sooner (A7); Thin lows takes progressively more low end out of it (at 100 %, clearly thin). At 0 % neither changes
the sound.

### 5.8 Mute

While on, the input no longer reaches the output: neither dry nor into the reverb. What is already in the reverb
rings out naturally. Turning it on or off never clicks.

### 5.9 Mix

The balance between the dry input and the reverb. 0 % is the input unchanged (bit-for-bit within 1e-6); 100 % is the
reverb only. In between, the perceived loudness stays roughly constant. The curve is the implementer's choice.

### 5.10 Stereo

The tail is wide, also for a mono input, and the reverb works for mono-summed playback (no strong cancellation when
left and right are added).

Wide, but never disorienting. The reverb surrounds the sound like a room around a stage: it doesn't sound like two
separate reverbs, one per side, it leaves no hole in the middle, and it never sounds phasey or as if it came from
behind or inside the listener's head. The source keeps its place: a sound panned to one side keeps its reverb's early
part leaning to that side, and a centred sound's reverb stays centred. Low frequencies are less wide than high ones,
as in a real room, so the low end stays solid. This holds at every setting up to the widest that 1.5 offered: a control that
widens the reverb (for example one of the early-reflection controls) goes from narrow to wide and natural, not to
an exaggerated, unnaturally wide effect. On headphones and on speakers alike.

Wider by choice. The user can make the reverb wider than that natural width, up to a tail whose mid and high
frequencies are fully decorrelated between the channels. The implementer either extends the width-related control's
range or adds a second width control, and records the choice and why in DESIGN.md. Either way:

* the default sound is 1.5's default sound, and every width 1.5 offered is still reachable with the same sound; if
  the existing control's scale changes, DESIGN.md gives the exact mapping from 1.5's values to the new ones, so
  DAWg can convert saved projects;
* beyond 1.5's widest setting, "natural" gives way to "very wide", but the rest of this section still holds: a
  centred source stays centred, no hole in the middle, never phasey, the low end less wide than the highs and still
  solid, mono-summed playback still safe, and a panned source's early part still leans to its side (A22).

### 5.11 Changing controls

Every continuous control can change at any time, including every block from modulation, without clicks, zipper
noise or pitch artefacts beyond the reverb's own character. Changing Seed may restart the pattern; it must not click
loudly. Pre-delay changes as in 5.4a.

### 5.12 Modulation

One control sets how much the tail moves. At 0 % the tail doesn't move at all; at its default it moves exactly as in
the delivery accepted against 1.5 (commit c9ba0bf); at 100 % a held note in a long tail is audibly and lushly
chorused, like a gentle chorus pedal: never seasick, out of tune or vibrato-like. In between it grows steadily. It
changes nothing else: decay time, tone, density, width and the early part stay as they are. 5.1's "audible chorusing
is not welcome" and A18 apply at the default and below; above the default, chorusing is the point. 5.11 applies
(A23).

## 6. Data for the device panel

`meters()` (message thread) returns, when the design has them:

* `arrivalsLeft`, `arrivalsRight`: the times, in ms after the input (including the pre-delay), of the main early reflections of each channel
  at the current settings, ascending, at most 32 each. The panel draws them on a time axis.
* `level`: the output's peak level per channel since the previous call, linear.

Keys the design can't provide are left out. The DAWg side adapts the panel to what is provided.

## 7. Engineering requirements

1. `process()` overwrites every sample of the output.
2. The output is always finite. No denormal slow-downs (the SDK's `EffectNode` flushes denormals; don't rely on
   anything else). With -12 dBFS RMS noise in, no sample exceeds ±8.0 at any setting.
3. The output doesn't depend on how the audio is cut into blocks: the same input gives bit-identical output with
   any sequence of block sizes.
4. Sample rates from 44,100 to 192,000 Hz sound the same: everything audible is defined in seconds and hertz, not
   samples.
5. Real-time safety in `process()`: no allocation or deallocation, no locks or waits, no I/O, no exceptions, no
   unbounded work. Memory is allocated in `prepare()`. Functions on the audio thread are `noexcept`.
6. CPU, at 48 kHz in a Release build on a current desktop, with every compiler the DAWg side builds with (MSVC
   among them): at most 1.5 % of one core at the defaults, and at most 2.5 % at any setting, the modulation control
   included. That is, the harness renders 60 s of noise in at most 0.9 s and 1.5 s. Measured as the best of three
   runs on an otherwise idle machine. The node runs once per track, and a song has many tracks.
7. sdk/CODING_GUIDELINES.md applies, with .clang-format.
8. Licence: AGPL-3.0-or-later. Third-party code only under a compatible licence, attributed in DESIGN.md. Published
   algorithms and textbook techniques are fine; cite them.
9. Silence costs almost nothing. Once the input has been exactly zero, and the output below −120 dBFS, for at least
   1 s, `process()` costs at most 0.1 % of one core until input that isn't zero arrives. Meanwhile the output may be
   exact zeros. When input returns, the output is what it would have been had the node gone on processing, within
   −120 dBFS. 7.3 still holds: when the node stops and starts again doesn't depend on the block sizes.

## 8. Acceptance tests (run by the DAWg side, black box)

The DAWg side renders the finished node through the harness and checks the following. It reports only pass or fail
and a functional description of any failure. "RT60" is measured from the impulse response, in octave bands from
500 Hz to 2 kHz; settings not mentioned are at their defaults.

Definitions used by A14, A18 and A19 (all at Mix 100 %, Pre-delay 0, 48 kHz; filters are Butterworth, applied
forwards and backwards):

* **The ideal reverb** for a note at frequency f: for each channel, independent Gaussian white noise multiplied by
  10^(−3t/T), where T is the node's RT60 in the octave band (centred at 125 Hz, 250 Hz, … 8 kHz) nearest to f, of
  length 3T but at most 18 s, convolved with the same input as the node. It is time-invariant and infinitely dense:
  what a perfectly steady tail of the same length does. Its results are averaged over 4 independent noise draws.
* **The pad corner:** Feedback 90 %, Diffusion 100 %, Size 100 %; and Feedback 80 %, Diffusion 80 %, Size 80 %.
* **A held note:** a sine of amplitude 0.25 at f = 110, 220, 440, 880 and 1760 Hz, held for 8 s, ending with a 5 ms
  linear fade, then silence for 10 s.

| # | Check |
|---|---|
| A1 | Feedback 0 %: RT60 ≤ 0.4 s. Feedback 100 %: RT60 ≥ 8 s. RT60 rises monotonically with Feedback in 10 % steps. |
| A2 | Mix 100 %, pink noise in: the output's RMS stays within ±6 dB of the input's for Feedback 0–90 %. |
| A3 | The arrival of the first strong reflection and the RT60 both rise monotonically with Size; at Size 100 % the early reflections span at least 80 ms. |
| A4 | Echo density 50–150 ms after an impulse (peaks above −40 dB per 10 ms) is at least 4 times higher at Diffusion 100 % than at 0 %. |
| A5 | For each early-reflection control, moving it from 0 to 100 % changes the impulse response's early part (the span Size sets) clearly: correlation between the two below 0.7. It changes the RT60 by no more than 25 %. The two controls' effects differ: the change caused by one is correlated below 0.7 with the change caused by the other. DESIGN.md says what each does, and the DAWg side confirms by listening that it does that. For the timing control: between 0 and 100 %, the median of the arrival times reported for the panel (6) moves by at least 30 % of the span, and the arrivals' times (not only their levels) change. |
| A6 | Seeds 1 and 2: the impulse responses' correlation is below 0.5. The same seed twice: bit-identical. |
| A7 | Damp highs 100 %: the tail is at least 10 dB lower at 8 kHz than at Damp highs 0 %. Thin lows 100 %: at least 12 dB lower at 100 Hz than at 0 %. At Mix 0 %, neither changes the output. Damp highs 10 %, other controls at their defaults, an impulse in, Mix 100 %: the energy above 4 kHz (fourth-order highpass) from 0.9 to 1.1 s after the impulse is no more than 3 dB below that at Damp highs 0 %. |
| A8 | Mute on: input arriving afterwards doesn't reach the output; with Feedback ≥ 50 %, the tail present when Mute turned on is still audible 100 ms later. No click (no step larger than 0.05 in the output) when Mute changes. |
| A9 | A mono impulse in, at the defaults and at the widest setting that 1.5 offered: the left/right correlation of the tail (from 100 ms) is between 0.15 and 0.5 from 250 Hz to 4 kHz, at least 0.5 below 250 Hz, and above −0.1 in every octave band; the mono sum of the tail is no more than 3 dB below the sum of its channels' energies. In the early part (the span Size sets), the energy envelopes of the two channels, each smoothed over 5 ms, have a correlation of at least 0.5; time differences between the channels of up to 0.7 ms per reflection are allowed. A source panned hard left: the early part is at least 3 dB louder on the left than on the right, at every setting. |
| A10 | A steady 1 kHz sine in, each continuous control swept from its minimum to its maximum over 1 s in 64-sample blocks: no step between consecutive output samples larger than 4 times the largest step without the sweep. |
| A11 | CPU as in 7.6, measured by the DAWg side with the harness, with MSVC as well as any other compiler it builds with. |
| A12 | The interface tests in node/InterfaceTests.cpp pass. |
| A13 | Mix 100 %, an impulse in: with Pre-delay at 0, 20, 100 and 250 ms, the output stays more than 80 dB below its peak until the pre-delay time has passed, and the response from then on matches the 0 ms response shifted by that time (correlation above 0.9 over the first 200 ms). At Mix 50 %, the dry impulse is at sample 0 at every Pre-delay. |
| A14 | Diffusion 100 %, Feedback 30, 60 and 90 %, Size 20 and 80 %: on a snare, a vocal and pink-noise bursts, the DAWg side hears a smooth tail with no grain or sizzle in the high frequencies (listening). The implementer proposes in DESIGN.md a measurement that tracks this, and reports it. Measured: an impulse in, at Feedback 30, 60 and 90 % and Size 20, 55, 80 and 100 %; the band above 8 kHz (fourth-order highpass); on each channel, the segment from 0.2 to 1.0 s after the impulse; its roughness is the standard deviation of its 2 ms frame energies in dB minus their 50 ms (25-frame) centred moving average. The reference is a phase-randomised copy of the segment (the same magnitude spectrum, uniformly random phases), scaled sample by sample so that its 50 ms moving RMS equals the segment's. At Diffusion 100 %, the roughness is at most 1.10 times the reference's (mean of both channels and 4 random-phase draws) at every setting. At Diffusion 70 % it may be up to 0.05 below that at 100 % at any one setting (the measurement's scatter), and over the twelve settings its mean is not lower than at 100 %; at Diffusion 40 % it is higher than at 100 % at every setting: Diffusion is how much grain the tail has. |
| A15 | Damp highs 0 %, Thin lows 0 %, an impulse in. At Feedback 70 % and above: the RT60 in the 8 kHz octave is at most half, and in the 4 kHz octave at most three quarters, of the RT60 measured as in the introduction to this section (500 Hz–2 kHz). At Feedback 90 %, Diffusion 100 % and Size 100 %, the RT60 in the 8 kHz octave is at least 0.3 times, and in the 4 kHz octave at least 0.6 times, that RT60; the ratio of the 8 kHz RT60 to that RT60 does not rise as Feedback rises from 70 to 100 %. At Feedback 90 %: the energy above 4 kHz in the first 100 ms of the reverb is no more than 3 dB below that of a version without the darkening (the implementer shows how in DESIGN.md), while at 2 s after the impulse it is at least 12 dB below the energy at 500 Hz–2 kHz, relative to the same ratio in the first 100 ms. |
| A16 | Listening: a sustained pad at Feedback 85–95 % and Mix 30–50 %, with drums or a vocal entering on top. The bed sounds lush and soft, not hissy or metallic, and the new sound's attack stays clear above it. |
| A17 | Listening: a snare and a vocal at Feedback 80 %, Size 100 %, Diffusion 100 %, at the widest setting that 1.5 offered, on headphones and on speakers. The reverb sounds wide and enveloping but natural and coherent: not disorienting, no two separate sides, no hole in the middle, not phasey. |
| A18 | Held notes in the pad corner, seeds 1, 2 and 3. From 5.0 to 7.9 s after the note starts, on each channel: the output band-passed from 0.8 f to 1.25 f (fourth order), its analytic signal, and from that its amplitude in dB and its instantaneous frequency in cents re f, each smoothed by a 20 ms moving average. The swing is the amplitude's peak-to-peak range; the drift is the standard deviation of the frequency. The same for the ideal reverb. Averaged over channels and seeds, at every note: the swing is at most 1 dB more than the ideal reverb's; the drift is at most 1 cent more than the ideal reverb's at 110 Hz and at most 0.3 cents more from 220 Hz up. |
| A19 | Held notes in the pad corner, seeds 1, 2 and 3. After each note ends, on each channel from 50 ms after its end: energies of 100 ms frames in dB, up to the first frame 30 dB below the first; the unevenness is the standard deviation of these around a cubic fit over time. Averaged over the five notes, both channels and the seeds, it is at most 1.15 times the ideal reverb's. |
| A20 | Listening, in the pad corner at Mix 30–50 %: a sustained pad chord with a bass note, and a lead of three slightly detuned sawtooth oscillators playing held notes. The reverb is a steady, almost pad-like bed: no grain in the high frequencies, no audible swelling, dipping or wobble in level or pitch (the lead's own beating aside), and the high frequencies don't die away early. |
| A21 | The accepted sound is kept. With the modulation control at its default and the width control(s) at their defaults or at the values that map from 1.5's (5.10), the output is the same as the delivery accepted against 1.5 (commit c9ba0bf) for the same settings: the difference is at least 100 dB below the output. Checked at the defaults, both pad corners, 1.5's widths 0, 50 and 100 % and Seeds 1–3, with impulses and 10 s of pink noise. |
| A22 | Wider by choice. A mono impulse at the widest new setting, other controls at their defaults: the tail's left/right correlation (from 100 ms) is between −0.1 and 0.1 in every octave band from 500 Hz to 4 kHz, at most 0.3 at 250 Hz, and at least 0.3 at 63 and 125 Hz; the mono sum of the tail is no more than 3 dB below the sum of its channels' energies; the two channels' tail energies differ by at most 1.5 dB. A source panned hard left: the early part is at least 3 dB louder on the left. From 1.5's widest setting to the widest new one, the correlation from 1 to 4 kHz falls steadily. |
| A23 | Modulation. Held notes as in A18 (its definitions and seeds), in the first pad corner and at the defaults: at 0 %, the drift is at most 0.1 cents and the swing at most 0.5 dB more than the ideal reverb's, at every note. At 100 %, in the first pad corner, the drift is at least 3 cents at 440 and 880 Hz, and at most 8 cents at every note. At 0, 25, 50, 75 and 100 %, the drift at 440 Hz grows steadily. Across the control, the RT60 changes by at most 10 %, each octave band's level in the tail (0.1–1 s, an impulse in) by at most 1 dB, and A9's tail correlations by at most 0.1. |
| A25 | Silence as in 7.9, at the defaults, at both pad corners and at every control's maximum. 10 s of pink noise, then 60 s of silence: rendering the silent part from 20 s on costs at most 0.1 % of one core. Then 10 s of pink noise again: from its start, the output is within −120 dBFS of a fresh node's output for that noise alone. The same input in blocks of 1, 64, 480 and 4096 samples, and in random sizes from 1 to 4096: bit-identical output. Silence at the start, then noise, behaves the same. |
| A24 | Listening, on headphones and on speakers: in the pad corner, the pad chord and the three-saw lead of A20; a snare and a vocal at the widest new setting. At the widest, very wide and enveloping, but centred sounds stay centred, no hole in the middle, not phasey, the bass solid. Modulation at 100 % lush and chorused, not seasick; at 0 % static but still smooth, no ringing tones. |

## 9. References the implementer may use

* M. R. Schroeder, "Natural sounding artificial reverberation", JAES, 1962.
* J. A. Moorer, "About this reverberation business", Computer Music Journal, 1979.
* M. A. Gerzon, "Synthetic stereo reverberation", Studio Sound, 1971–72.
* J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", JAES, 1997.
* J.-M. Jot and A. Chaigne, "Digital delay networks for designing artificial reverberators", AES Convention, 1991.
* J. O. Smith III, *Physical Audio Signal Processing*, online book, chapters on artificial reverberation.
* Textbooks on digital filters (first-order shelves, one-pole smoothers, allpass and comb filters, interpolated
  delays) and on pseudo-random number generation.

Not allowed: any other product's reverb internals, patches or presets; DAWg's repository; this specification's
authors' notes outside this workspace; web searches.

## 10. Deliverables

* The node in node/ (Reverb.h, Reverb.cpp, and further files there as needed), building with the workspace's CMake
  and passing node/InterfaceTests.cpp.
* The implementer's own tests in node/, added to the `tests` target.
* DESIGN.md: the architecture, every constant and curve, why each was chosen (listening, measurement with the
  harness, literature), with dates.
* A git history in this workspace with meaningful commits.

## 11. Changes

### 1.1 (2026-10-26, after the first implementation was delivered)

* 5.1, 5.4, A14: the tail must be smooth in the high frequencies, with no grain or sizzle, above all at Diffusion
  100 % (it was "slightly grainy" in 1.0). The first implementation's tail was judged too grainy and sizzly in the
  highs at Diffusion 100 %.
* 5.5, A5: one of the two early-reflection controls must move the timing of the reflections.
* Because these change what the reverb must do, re-evaluate the design from the ground up (CLAUDE.md, "Revisions"),
  not only its parameters.

### 1.2 (2026-10-26, after listening to the revised implementation)

* 5.2, A15, A16: at long Feedback settings the reverb must work as a lush, soft bed. The tail darkens as it decays,
  more so the longer it is, even with Damp highs at 0 %, while the start of the reverb of fresh input keeps its high
  frequencies so that new sounds cut through the bed. The numbers in A15 are the DAWg side's own targets for this
  use.
* As in 1.1, follow CLAUDE.md, "Revisions": check whether the design as a whole still fits, not only its
  parameters.

### 1.3 (2026-10-26, after listening to the revision for 1.2)

* 5.10, A9, A17: at the widest settings the reverb was judged too wide, to the point of being disorienting. It
  should still be able to sound wide, but wide and natural: coherent, no two separate sides, no hole in the middle,
  not phasey, the source keeping its place, and a less wide low end. A9's limits are new targets for this.
* As before, follow CLAUDE.md, "Revisions".

### 1.4 (2026-10-26, answering the clean side's questions 2 and 3 in records/QUESTIONS.md)

* 5.5: the width-related early-reflection control may set the width of the whole reverb, early part and tail, not
  only within the early span. 5.10 holds at every setting; A9 applies at the defaults and at the widest setting.
* A9: the early-part coherence criterion is now the correlation of the two channels' smoothed energy envelopes (at
  least 0.5), and time differences between the channels of up to 0.7 ms per reflection are allowed. The hard-pan
  requirement is unchanged.

### 1.5 (2026-10-27, after listening inside DAWg, on a lead of three slightly detuned saws)

The implementation accepted against 1.4 was integrated into DAWg. Used there, three things fell short:

* 5.1, 5.4, A14: the high frequencies still sound grainy. The DAWg side's measurement places it in the band above
  8 kHz during the first second of the tail; the rest of the spectrum and the later tail are as smooth as noise.
  A14 now includes that measurement, which the delivered implementation doesn't pass (1.27–1.49 times the
  reference). The existing measures (DESIGN.md 4.3) didn't detect it.
* 5.7, A7: a little Damp highs (8 %) made the high frequencies die away too fast; Damp highs must start gently.
* 5.1, 5.2, A15, A18–A20: at long, dense, large settings the tail sounds fluttery and not steady enough for a
  pad-like bed: held notes swell and drift in level and pitch, most in the bass and more at larger Size, and their
  release tails fade less evenly than a steady tail would. The impulse response's own statistics are as smooth as
  noise; the shortfall is in how the tail moves over time. A18 and A19 measure it against an ideal, time-invariant
  reverb. The long tail is also darker than needed: A15 gains lower limits.
* As before, follow CLAUDE.md, "Revisions": check whether the design as a whole still fits, not only its
  parameters. Every earlier requirement still holds, including 5.10, A9 and A17.

### 1.6 (2026-10-27, after accepting the revision for 1.5)

The DAWg side accepted the revision for 1.5 and asks for two controls, each keeping the accepted sound at its
default:

* 4.1, 5.10, A22: width beyond the natural width, up to a fully decorrelated tail, by extending the width-related
  control or adding a second one (the implementer's choice). 5.10's other qualities still hold.
* 4.1, 5.12, A23: a control for how much the tail moves, from none to an audible, lush chorus.
* A21 checks that the accepted sound is unchanged; A24 by listening.
* A14: the Diffusion ordering allows the measurement's scatter (question 4). A9 and A17 now name the widest setting
  that 1.5 offered; A22 and A24 cover the new range.
* As before, follow CLAUDE.md, "Revisions". Every earlier requirement still holds at the new controls' defaults.

### 1.7 (2026-11-16, after using the accepted implementation in a song)

In a song of 16 tracks inside DAWg, five instances of the reverb (one per track that needs one) took about a quarter
of the processing of the whole song: each 3.7–4.9 % of one core at 48 kHz, in blocks of 480 samples, at the song's
settings. That is more than 7.6 allowed. Fed silence through the harness, the node costs as much as with noise in
(2.3–2.6 s against 2.4–3.2 s for 60 s, on the same loaded machine), so tracks that are silent for long stretches
cost as much as playing ones. The 1.6 acceptance already noted A11 failing under MSVC at every control's maximum.

* 7.6, A11: the CPU limit is halved at the defaults (1.5 %) and set at 2.5 % at any setting, the modulation control
  included, and applies with MSVC as well. The numbers are the DAWg side's own targets.
* 7.9, A25 (new): once the input has been silent for a while and the tail has died away below −120 dBFS, the node
  costs almost nothing until input returns; nothing audible changes.
* The sound is unchanged: every earlier requirement still holds, A21 included, at the same settings.
* As before, follow CLAUDE.md, "Revisions": check whether the design as a whole still fits, not only its
  parameters.
