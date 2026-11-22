# Design: the Loop Reverb

Status: **built-in node and panel done.**

## 1. Goal

* An algorithmic reverb as a built-in node (`dawg.loopReverb`): smooth, dense and gently moving, wide but never
  disorienting, from a short room to a long hall, with a seedable pattern instead of a fixed set of room presets.
* Every control does one audible thing, and changing any control while audio plays never clicks.

## 2. Provenance

The reverb is a clean-room implementation. An independent implementer wrote it from a functional specification
alone, without access to DAWg's code or any other reverb's code, and it was accepted against that specification by
measurement (its acceptance tests) and by listening.

* `engine/src/loopreverb/SPEC.md`: the specification (version 1.7, a draft: CPU and silence), describing behaviour only.
* `engine/src/loopreverb/DESIGN.md`: the implementer's design, with every constant's reason, the measurements and
  the published literature it builds on.
* `engine/src/loopreverb/README.md`: what was copied from the clean-room workspace, at which commit.

Both documents are copied unchanged from the workspace. The code in `engine/src/loopreverb/` and the tests in
`tests/loopreverb/` are copied too, changed by DAWg's clang-format and the few DAWg-side changes its README lists. Changes to the reverb's behaviour go through
the specification and the implementer, not through edits here.

## 3. How it works

A summary of DESIGN.md, section 1.

```
in ─► mute ─┬──────────────────────────────────────────────────────────────► dry ─► (+) ─► out
            └─► tone (Thin lows, Damp highs) ─┬─► early taps ─► 4 allpasses ─┐       ▲
                                              │                             (+) ─► pre-delay ─► wet
                                              └─► 4 allpasses ─► 16-line feedback delay network ─┘
```

* **Early reflections** from a tapped delay line [Moorer 1979]: 16 taps per pattern, at times within an early span
  that Size sets (5 to 100 ms). Three seeded patterns: one shared by both channels and one per input whose
  reflections arrive from around that input's side, with level and time differences between the ears. Short
  allpasses smear the pattern as Diffusion rises.
* **The tail** is a unitary feedback delay network [Gerzon 1971; Jot and Chaigne 1991]: 16 delay lines with a
  Schroeder allpass inside each, mixed by a rotation matrix whose angle is the Diffusion. Each line's gain sets the
  decay time exactly; shelves in the lines shorten it for the lows (Thin) and highs (Damp), and a fixed loss per
  second darkens the tail the longer it rings. Slowly moving allpass angles keep the tail alive without chorus; the
  longer the tail, the less they move, and every mode decays at the same rate, so a long, dense, large setting
  is a steady, almost pad-like bed (SPEC 1.5). Modulation fades that movement in from a still tail at 0 % to the
  accepted sound at 25 %; above it the lines' delays move too, at 1–2 Hz each, into a lush chorus at 100 % (SPEC 1.6).
* **Stereo:** the left input feeds half the lines and the right input the other half. The tail's output is matrixed
  into mid and side so the channels correlate naturally (closely in the low end, less above 150 Hz as Width rises). Up to 75 % Width
  goes from narrow to naturally wide; from 75 to 100 % the tail above 150 Hz goes on to fully uncorrelated, while the
  bass and the early reflections stay as at 75 % (SPEC 1.6).
* **Pre-delay** delays the whole reverb output, so the response shifts exactly and the dry path has no latency.
* **Real time:** no allocation, locks or I/O while processing. Every value ramps from block to block, and the output
  is bit-identical for any sequence of block sizes.

## 4. Controls

| Id | Name | Default | What it does |
|---|---|---|---|
| `size` | Size | 55 % | The room: the early span (5–100 ms) and the delay lines' lengths; a larger room also decays a little longer |
| `width` | Width | 75 % | How wide the whole reverb is: narrow and mono-compatible at 0 %, naturally enveloping at 75 %, very wide at 100 % |
| `predelay` | Pre-delay | 10 ms | Delays the reverb against the dry sound, 0–250 ms |
| `earlyTiming` | Early Timing | 40 % | When the early reflections arrive within the span: bunched at the start at 0 %, gathering towards the end at 100 % |
| `feedback` | Feedback | 45 % | The decay time, 0.25 s to about 13 s |
| `diffusion` | Diffusion | 70 % | Separate echoes at 0 %, a smooth dense wash at 100 % |
| `damp` | Damp highs | 0 % | Darker reverb, the tail more so as it decays |
| `thin` | Thin lows | 20 % | Leaner reverb in the low end |
| `modulation` | Modulation | 25 % | How much the tail moves: still at 0 %, lively at 25 %, a lush chorus at 100 % |
| `mute` | Mute | off | Stops new input reaching the reverb and the output; the tail rings out |
| `mix` | Mix | 25 % | Dry and wet at equal power |
| `seed` | Seed | 1 | The pattern of early reflections and the tail's texture; the same seed always sounds the same |

The curves and the reasons for each default are in DESIGN.md, section 2.

## 5. In DAWg

* **Adapter** (`engine/src/LoopReverb.cpp`). The reverb is written against its own copy of DAWg's node API
  (`engine/src/loopreverb/dawg_sdk/`, in namespace `dawg::reverb`). The adapter owns one `reverb::ReverbNode`,
  converts its descriptor (adding DAWg's modulation inputs), and each block hands it the node's parameters (after
  automation and modulation), the audio and the transport. Bypass goes to the reverb's own bypass.
* **Meters for the panel.** `arrivalsLeft` and `arrivalsRight`: the main early reflections' times in ms after the
  input, pre-delay included, ascending, at most 32 each. The adapter computes them from its own parameters, so a
  node that isn't processed (one that doesn't reach the output) still shows its settings. `level`: the output's peak
  per channel, linear.
* **Panel** (`ui/src/dock/LoopReverbPanel.tsx`): each channel's arrivals on a time axis with the pre-delay shaded,
  a live level strip of the last two seconds behind them (from the dock's analysis), the seed with a dice button,
  and the controls to its right in signal-flow order, in two rows so none is out of view: Early (Pre-delay, Early
  Timing), Space (Size, Width), Loop (Feedback, Diffusion); Tone (Damp highs, Thin lows), Motion (Modulation) and
  Output (Mix, Mute). The graph gives way in width.
* **Tests.** `tests/LoopReverbTests.cpp` tests the adapter: its descriptor matches the reverb's, its output is
  bit-identical to the reverb's own over varying block sizes and parameter changes, bypass, the meters, no
  allocation while processing. `tests/loopreverb/` holds the implementer's own tests (the specification's
  acceptance tests and behaviour tests), a separate JUCE-free executable run by ctest in release
  configurations.
* **Old projects.** Project files from before version 5 aren't read (`projectfile::oldestVersion`), so every
  Loop Reverb a project holds is this reverb.

## 6. References

As cited in DESIGN.md:

* M. R. Schroeder, "Natural sounding artificial reverberation", JAES, 1962.
* J. A. Moorer, "About this reverberation business", Computer Music Journal, 1979.
* M. A. Gerzon, "Synthetic stereo reverberation", Studio Sound, 1971–72.
* J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", JAES, 1997.
* J.-M. Jot and A. Chaigne, "Digital delay networks for designing artificial reverberators", AES Convention, 1991.
* J. O. Smith III, *Physical Audio Signal Processing*, chapters on artificial reverberation.
