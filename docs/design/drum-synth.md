# Design: the Drum Synth

Status: **accepted, not yet built**. The decisions are numbered DR1–DR16 and listed in section 8; the questions for
the owner are in section 9, and the owner took every recommendation (2026-11-18, section 9.1); the steps and tests
are in section 10.

## 1. Goals and non-goals

Goals:

* **A built-in drum machine that makes its sounds**, with no samples: kick, snare, closed and open hats, crash and
  ride, and toms, each one a voice with a handful of knobs that mean something ("Sweep", "Snappy", "Bell").
* **One device on one track, played from one clip**, laid out as pads, like the Sampler's drum view: a pad per key,
  General MIDI keys by default, so a drum clip, a MIDI file or the Step Sequencer plays it without remapping.
* **Each pad to its own mixer strip** in one step, as the Sampler does today (multi-output-instruments.md).
* **Drum-machine behaviour:** choke groups (a closed hat stops an open one), velocity, and per-pad level, pan, tune and
  decay.
* **Kits and pads as presets**, plus randomise, so a new kit is a click away.
* **Cheap:** a full kit playing a busy pattern should cost about as much as one synth voice does today.
* Every sound parameter automatable, modulatable and linkable like any node parameter.

Non-goals (for the first version):

* Physical models (waveguide meshes, finite-difference membranes and plates). They cost far more (section 3.6) and are
  harder to tune by ear.
* Sample layering inside a pad (a kick synth plus a sampled transient). The Sampler does samples; both can sit in one
  graph.
* Per-pad effect chains inside the device. Per-pad output tracks (DR9) give each pad a full chain.
* Pitch bend and MPE expression on drums.

## 2. What exists

### 2.1 Instruments

* **MpeSynth** (`dawg.mpeSynth`, `engine/src/BuiltinNodes.cpp:320`, `:332`): sine voices with per-note expression,
  four parameters (`:322`). Not a basis for drums.
* **Sampler** (`dawg.sampler`, `engine/src/Sampler.cpp:714`): key zones in `GraphNode::sampleZones`
  (`engine/include/dawg/Model.h:965`), each a `SampleZone` (`Model.h:659`) with its own tune, gain, pan, envelope,
  **choke group** (`Model.h:744`, 1..8, `maxChokeGroup`, `engine/include/dawg/Sampler.h:19`), layer and **output**
  (`Model.h:755`). A choked strand fades over 5 ms (`Sampler.cpp:29`); a note counts once per choke group so its own
  layers don't choke each other (`Sampler.cpp:629`).
* **Voice Instrument** (`dawg.voiceInstrument`, `engine/src/VoiceNodes.cpp:80`): runs a voice graph per note
  (phase12-voice-scope.md). Its voices sum into **one** output (multi-output-instruments.md, non-goals, line 20).
  There is **no key-split node**: every note runs the same voice graph.
* **Components** (`dawg.component`, component-nodes.md): a box around part of a graph, flattened away by the compiler
  (N4), with exposed parameters (N6) and saved components in the browser (N9).
* **Faust** (`dawg.faust`, `engine/include/dawg/FaustNode.h:242`): a node whose ports and parameters come from its
  script. It is the precedent for a **parameter list per node** rather than per type: commands look up
  `descriptorOf (node)` (`engine/src/Commands.cpp:533`), and `portsOf` already serves ports per node
  (`Commands.cpp:512`).

### 2.2 Several outputs

* The Sampler has 16 output ports, Main and Out 2..16 (`Sampler.cpp:58-75`, `maxSampleOutputs`, `Sampler.h:29`). A
  strand writes into its zone's output (`Sampler.cpp:136-139`), or into Main while that output isn't wired: the
  compiler hands the node a bit per wired port, `ProcessContext::wiredOutputs` (`engine/include/dawg/Node.h:713`,
  used at `Sampler.cpp:749-753`). This is MO2.
* `splitSamplerOutputs` (`Commands.cpp:6109`, registered at `:7847`) gives groups of zones output tracks of their own,
  each with an Output to track node (`dawg.busOutput`, `engine/include/dawg/NodeRegistry.h:42`), in one undo step.
  The UI's **Each pad to its own track** calls it.
* A track with output tracks becomes a header; its Main plays on a Main output track (MO12).

### 2.3 Building blocks in the engine

| Block | Where | Use for drums |
|---|---|---|
| Band-limited oscillator: sine, triangle, saw, pulse, noise; unison | `dawg.oscillator`, `engine/src/Oscillator.cpp:28-51` | PolyBLEP (`blep`, `Oscillator.cpp:78`, used for saw and pulse at `:350`, `:353`) is what the hats' squares need |
| White noise | xorshift32 in the oscillator, `Oscillator.cpp:355-368` | the noise of snares, hats, cymbals, claps |
| State-variable filter, LP/HP/BP/notch, 12–48 dB, drive | `dawg.filter`, `engine/src/EffectNodes.cpp:790-825`; the SVF itself is private to `FilterNode` (`:953`) | the snare's noise filter, the hats' bandpasses and high-pass |
| ADSR with tension handles | `dawg.adsr`, `engine/src/ModulationNodes.cpp:60-70` | a modulation source; a pitch envelope in a voice graph needs it wired to an oscillator's pitch |
| Curves (linear, power, Bezier, smooth) | `engine/include/dawg/Curve.h:17-25` | drawing envelopes in the panel |
| Saturator, Clipper, Waveshaper with ADAA | `dawg.saturator` (`EffectNodes.cpp:2060`), vicanek-adaa.md, `engine/include/dawg/TransparentAdaa.h` | the kick's drive |
| Note events with `choke` and `chokeAll` | `engine/include/dawg/Node.h:340`, `BuiltinNodes.cpp:93` | transport stop and choke |

There is **no noise node of its own** (noise is an oscillator waveform), **no resonator or modal bank** outside the
Resonator component (a feedback patch), and **no envelope that runs inside a node** other than the Sampler's per-zone
one.

### 2.4 Notes, drum maps, the Step Sequencer

* The Sampler's pads are keys **36–51** on a 4 × 4 grid, lowest at the bottom left (`ui/src/dock/samplerLayout.ts:5-16`).
  Those sixteen keys are exactly General MIDI's core kit: 36 kick … 51 ride.
* The UI's drum map (`ui/src/store/drumMap.ts:37-60`) names GM keys and gives them roles (kick, snare, hat closed,
  hat pedal, hat open, toms, crash, ride, ride bell) and limbs, for drum humanise and the roll's drum display. A track
  is drums when its `drumMode` says so, or else when its name looks like it (`drumMap.ts:134-138`).
* A new Step Sequencer takes one row per key of the track's Sampler zones, named after their samples, or else a GM kit
  (Crash, Open hat, Closed hat, Clap, Snare, Low tom, Kick) (`Commands.cpp:4333-4373`).

### 2.5 Dock, panels, presets, design system

* A node type's graphical panel is registered in `ui/src/dock/panels.ts:34-62`; the Sampler's panel
  (`ui/src/dock/SamplerPanel.tsx`, 549 lines) has the pad grid (`SamplerPanel.tsx:42`) and per-zone editing.
* Each panel states a minimum width (`ui/src/dock/panelMinWidth.ts`); cards follow DS14: a header at `--control-s`,
  sections of 58 px knob slots, graphs that shrink first, no scroll bar inside a card at the default dock height.
* Node presets: `NodePreset` (`ui/src/dock/presets.ts:11-20`) holds `params` and, for the LFO and Waveshaper, a
  `shape`; factory presets per type (`presets.ts:24`), user presets saved apart (`presets.ts:814`). There is no
  randomise anywhere yet.
* Tokens: colours by role (DS1), the type scale (DS3), control heights (DS5), canvases from the theme (DS13),
  pressed states (DS11).

### 2.6 What it costs today

The CPU investigation of the demo project (2026-11-15, 48 kHz, 10 ms blocks) measured a Voice Instrument synth voice at
about 0.1–0.25 ms per block, so 1–2.5 % of a core each, largely the nodes' per-step overhead and the filters' 2x
oversampling. A sampler drum track with four output tracks cost under 0.05 ms. A drum kit built as a voice graph
would sit in the first range per hit, and a busy pattern keeps 6–12 hits ringing.

## 3. How drums are synthesised

### 3.1 Kick

* **The analogue recipe (808, 909 and nearly every synth kick since):** a sine whose pitch falls fast from a high start
  to a low end, under a decaying amplitude envelope. The 808 pings a bridged-T resonator (a decaying sine, slight pitch
  sweep from the circuit); the 909 sweeps an oscillator and adds a click.
* **Pitch:** f(t) = f_end + (f_start − f_end) · e^(−t / τ). Typical: f_end 40–60 Hz, start 2–4 octaves higher,
  τ 10–60 ms. An exponential in pitch (semitones) sounds smoother than in hertz; both are one multiply per sample.
* **Click:** a 1–5 ms burst of filtered noise or a short pulse at the start, mixed in; this is what cuts through a mix
  on small speakers.
* **Drive:** a tanh on the sine adds the odd harmonics that make it audible on a laptop; at a 50 Hz fundamental the
  harmonics stay far below Nyquist, so first-order ADAA (as in the Saturator) is enough, with no oversampling.
* Optional **hold** before the decay (a long 808 "boom" vs a short punch).

### 3.2 Snare

* **Body:** two (808) or two to three (909, acoustic) tuned modes, about 150–250 Hz, with a partial at 1.5–1.8 × the
  fundamental. On an ideal membrane the first modes sit at 1, 1.59, 2.14, 2.30 × (zeros of the Bessel functions); the
  808's two resonators are near 1 : 1.8. Each mode is a decaying sine with a small pitch drop.
* **Snares:** white noise through a high- or band-pass (2–8 kHz), with its own, usually longer, envelope: the "snappy"
  control is that noise's level.
* **Tone:** the balance of body and noise is most of what tells snares apart.

### 3.3 Toms

* Like a snare's body without the snares: two to four membrane modes (1, 1.59, 2.14 …), the higher ones dying faster,
  with a clearer **pitch drop** (2–12 semitones over 50–300 ms, the head's tension relaxing), and a little noise for the
  stick.
* A kit's toms are the same voice at different tunings, so the model only needs Tune; the GM keys 41, 43, 45, 47, 48,
  50 become six tunings of one model.

### 3.4 Hats

* **The 808 way:** six square-wave oscillators at fixed, unrelated frequencies (usually cited as 205.3, 304.4, 369.6,
  522.7, 540 and 800 Hz), summed, through two band-passes (around 3.4 kHz and 7.1 kHz), then a high-pass, under a short
  (closed, 20–80 ms) or long (open, 200–800 ms) envelope. The squares' sum is dense and inharmonic, which reads as
  metal. Each square needs PolyBLEP: their upper harmonics fold back otherwise, and the folding moves when Tune does.
* **Metallic FM:** pairs of sine operators at inharmonic ratios (1 : 1.4, 1 : 3.7 …) with a high index, high-passed.
  Bright and glassy, a little less "machine".
* **Modal:** a bank of 20–60 resonators at measured or random inharmonic frequencies, excited by a noise burst. The most
  natural, and the costliest of the three.
* **Noise:** high-passed noise alone makes a thin, sizzling hat; mixed under the metal it smooths it.
* **Recommendation:** the six squares with noise mixed under them, a Colour control moving the band-passes, and a
  Metal choice of Square or FM later if wanted (DR6).

### 3.5 Cymbals (crash, ride)

* The same metal source as the hats, with longer envelopes, and split into bands with their own decays (the 808's
  cymbal does three bands: the low one rings longest).
* **Ride:** less noise, a clear **bell** (a few strong partials in the 0.5–3 kHz range, the bell's own decaying modes),
  a long decay, a sharper attack. **Crash:** bright, noisy, a short swell into a long, bright decay.
* **Width:** a slightly detuned second metal source on the other side gives a real stereo spread, which cymbals need
  and the other voices don't.

**Revised (2026-11-21): a modal cymbal.** The owner found the six squares and the bell too bell-like, and no setting
made them natural; a noise cymbal was too noisy (section 9.2). The cymbal is now modal, after prototype WAVs:
* **Modes:** 128 two-pole resonators for a crash (96 for a ride), at inharmonic frequencies from 300 Hz to 16 kHz,
  denser towards the top as on a thin plate (density ∝ f^0.5). Each has its own decay (2.5 s at 1 kHz for a crash,
  4 s for a ride, shorter higher up, ±30 % at random), gain and place in the stereo field. Close modes beat against
  each other, which is the shimmer. The modes are drawn from one fixed seed, so every hit rings the same cymbal.
* **The stick:** a click and a short burst of noise, through a low-pass at Tone. A burst of noise alone made some hits
  far more tonal than others.
* **Wash and sizzle:** noise through a high-pass at 400 Hz and a low-pass that closes from 10 to 4 kHz as the cymbal
  rings, falling fast then slowly; and high noise above 7 kHz that swells in over Swell.
* **One cymbal rings on:** a new hit on a sounding cymbal's pad strikes that voice again instead of starting a second
  one (unless the pad is mono). The modes are linear, so this sounds like a real cymbal hit twice, at the cost of one.
* **Ride** moves every one of these from a crash's to a ride's: higher lowest mode, fewer and sparser modes, longer and
  more tilted decays, a shorter stick.
* **A crash's coupling (2026-11-21):** a hard hit bends a real cymbal non-linearly, which passes energy from the modes
  it rang into others: the crash's burst. The modes' sum, soft-clipped and cubed, comes back in as excitation, fading
  out over 0.3 s after each hit, scaled by 1 − Ride (a ride stays linear). Soft hits barely couple, hard ones burst.
  At about 0.6 the loop rings on by itself, louder and tonal; the crash uses 0.375, and a test checks that no setting
  of Tone, Colour, Tune and Decay, nor a roll of restrikes, grows after the hit.

### 3.6 Analogue-style vs modal vs physical, and their cost

| Approach | Per sample, one hit | Sound | Tuning by ear |
|---|---|---|---|
| Analogue-style (oscillators, noise, a few filters, envelopes) | kick ≈ 20 operations, snare ≈ 60, hat ≈ 150 (six PolyBLEP squares, three SVFs), cymbal ≈ 250 | the electronic drum sound everyone knows | easy: each knob is one thing |
| Modal (a bank of two-pole resonators) | ≈ 5 per mode: toms 4–8 modes ≈ 40, a cymbal 32–64 modes ≈ 160–320 | more natural, still electronic | good with a few macro knobs (size, material, damping) |
| Physical (2D waveguide mesh, finite differences) | thousands to millions | the most realistic | hard; stability and tuning need care |

At 48 kHz, 250 operations is about 12 M a second, under 1 % of a core for the heaviest hit. Twelve hits ringing at once
stays around 2–4 %, the cost of one or two synth voices today (2.6). A voice-graph build of the same kit would be
roughly 5–10 times that, as each node is a step with its own buffers and each Filter oversamples 2x by default.

**Recommendation:** analogue-style voices for kick, snare, hats and cymbals, and modal modes (a handful of sines, which
are the same as resonators here) for the bodies of toms and snares. Physical models stay out (section 1).

References for the build: Gordon Reid's *Synth Secrets* articles on synthesising kicks, snares, hats and cymbals (Sound
On Sound, 2001–2002), and Kurt Werner's circuit-informed models of the TR-808's bass drum and cymbal (DAFx 2014, ICMC
2014).

## 4. Architecture options

### Option A: one fixed C++ instrument node with pads (recommended)

`dawg.drumSynth`, an instrument like the Sampler: note events in, Main plus Out 2..16 out, sixteen pads on keys 36–51,
each pad holding one **voice model** (Kick, Snare, Tom, Hat, Cymbal …) with its parameters. The DSP is plain C++
classes, one per model, sharing small building blocks (DR4).

* \+ Cheapest by far (3.6); choke, velocity, outputs and the pad grid fit naturally, all copied from the Sampler.
* \+ One device in the dock, one panel, one preset: a kit.
* \+ Testable as plain DSP: render a pad, measure its pitch and decay.
* − A new voice model is C++ work, not something a user builds. (Option B's later "Drum Voice" node can answer that.)

### Option B: one node per voice type, combined by a kit component

`dawg.kick`, `dawg.snare`, `dawg.hat` … as nodes, combined into a "Drum Kit" component: a key split routes notes to
each, their outputs mixed or sent to output tracks.

* \+ Each voice can be used alone, or in a voice graph with filters after it.
* − Needs a **key-split** node (none exists, 2.1), and choke **across** nodes, which means note events passing between
  nodes (new).
* − Sixteen nodes in a graph and sixteen cards in the dock for one kit, or a component panel built for just this.
* − Presets become saved components (N9), heavier than node presets.

### Option C: built from existing graph nodes, as voice graphs

A Voice Instrument per drum, its graph made of Oscillators, the noise waveform, Filters, ADSRs wired to pitch, and a
Saturator.

* \+ No new DSP; a good teaching patch.
* − Each hit costs a synth voice (2.6), and a hat needs six Oscillators and three Filters per voice.
* − No key split, no choke between drums, and a Voice Instrument has one output (2.1), so no per-pad strips.
* − The knobs are spread over many nodes, not "Sweep" and "Snappy".

### Option D: Faust scripts

A Faust node per drum, or one script with all of them.

* \+ Quick to try voices; Faust's libraries have envelopes, filters and some modal models.
* − Pads, choke, per-pad outputs and a pad panel are all outside what a Faust node is; the panel would be Faust's
  generic one; scripts compile when loaded.

### Recommendation

**Option A.** B and C solve "drums" by adding general machinery (key split, cross-node choke, multi-output voice
instruments) that is worth having on its own but is far more work than the drum machine, and still costs more to run.
A's voice models can later be offered one at a time as a **Drum Voice** node for voice graphs (as `dawg.sampleVoice`
offers the Sampler's sound, `NodeRegistry.h:208`), which gives B's flexibility without its costs (DR16).

## 5. Model

**(DR1) One instrument node, `dawg.drumSynth`, "Drum Synth", with 16 pads on keys 36–51.** Pads are fixed to keys, as
the Sampler's pad grid is (2.4), so the grid, the GM drum map, the Step Sequencer and the roll's drum display all agree
with no mapping step. A note outside 36–51 plays nothing. Track and component graphs only; not in voice graphs (like
the Sampler, `NodeRegistry.h:334-339`).

**(DR2) A pad's structure lives in the node, its sound in parameters.**

```cpp
enum class DrumModel { off, kick, snare, tom, hat, cymbal, clap, rim };

struct DrumPad
{
    juce::String name;      // "Kick", "Closed hat"; shown on the pad, in the roll and the Step Sequencer
    DrumModel model = DrumModel::off;
    int output = 1;         // 1 (Main) .. maxDrumOutputs, as SampleZone::output (MO1, MO2)
    int chokeGroup = 0;     // 0 none, 1..8, as SampleZone::chokeGroup
    bool mono = false;      // a new hit on this pad cuts its last one (DR8)
};

// GraphNode
std::vector<DrumPad> drumPads; // 16, only for dawg.drumSynth
```

What changes the node's shape (model, output, choke group, name) is model data edited by commands, as zones are.
Everything that shapes the sound is a node **parameter**, so automation, modulation, links, macros and MIDI learn work
unchanged.

**(DR3) The parameter list is per node, built from the pads' models, as a Faust node's is from its script.** Each pad
has the common parameters, then its model's own:

| Common (every model) | id | range | default |
|---|---|---|---|
| Level | `p{n}.level` | −60..+6 dB | 0 |
| Pan | `p{n}.pan` | −100..100 % | 0 |
| Tune | `p{n}.tune` | −24..+24 st | 0 (the model's own base pitch) |
| Decay | `p{n}.decay` | 10 %..400 % of the model's | 100 % |
| Velocity | `p{n}.velocity` | 0..100 %: how much velocity changes level (DR7) | 70 % |

| Model | Its own parameters |
|---|---|
| Kick | Pitch (30–120 Hz), Sweep (0–48 st), Sweep time (2–200 ms), Hold (0–200 ms), Click (0–100 %), Click tone, Drive (0–24 dB) |
| Snare | Pitch (100–400 Hz), Body decay, Tone (body/noise), Snappy (noise level), Noise decay, Noise tone (cutoff), Sweep |
| Tom | Pitch (50–400 Hz), Sweep (0–12 st), Sweep time, Overtones (upper modes' level), Stick (attack noise) |
| Hat | Tune is enough for pitch; Tone (high-pass), Colour (band-pass centres), Noise (metal/noise mix), Attack (0–20 ms) |
| Cymbal | Tone (the stick, 2–12 kHz), Colour (low modes against high), Wash, Sizzle, Ride (0 % a crash, 100 % a ride), Swell (0–200 ms), Width (0–100 %); revised 2026-11-21, section 3.5 |
| Clap | Tone (band-pass), Spread (time between the bursts, 5–30 ms), Bursts (2–5), Tail |
| Rim | Pitch, Tone, Click |

Ids are `p{n}.{id}` with n = 1..16 (`p3.snappy`), names "{pad name} · {parameter}" ("Snare · Snappy"), so an
automation lane, a modulation target and a link read properly. The node's descriptor is built from the sixteen models
and cached by that list, as `descriptorOf` caches a Faust script's.

The rejected alternative is a fixed list of sixteen pads × (five common + eight generic "A–H") parameters, with the UI
renaming A–H per model. It needs no per-node descriptor, but lanes and modulation would read "Pad 3 · C", ranges and
units would be generic 0–100 %, and A on a kick would mean something else on a hat.

**(DR4) Changing a pad's model is a command, `setDrumPad`, and one undo step.** The pad's common parameters keep their
values; the model's own start at the new model's defaults; automation lanes, modulation, links and macro slots on
parameters the new model lacks are removed (the UI says how many, as removing an instrument with output tracks does).
Copying a pad onto another (`copyDrumPad`) copies its structure and all its parameters.

## 6. Engine

**(DR5) Voices are plain classes, one per model, in `engine/include/dawg/DrumVoices.h` and
`engine/src/DrumVoices.cpp`.** Each has `prepare (sampleRate)`, `trigger (params, velocity, seed)`, `choke()` and
`render (left, right, count)`, and reads its parameters once per block (smoothed where they would click). They share:

* a sine and a PolyBLEP square from `Oscillator.cpp` (`blep` moves to a small shared header);
* the xorshift32 noise (`Oscillator.cpp:365-368`), seeded per hit (DR11);
* a TPT state-variable filter: the Filter's SVF (`EffectNodes.cpp:953`) moves into a shared header so both use it. The
  drum voices run it **without oversampling**: their filters sit at fixed or slowly moving cutoffs with low resonance,
  where the prewarped SVF is accurate. The Filter node keeps its oversampling exactly as it is.
* exponential envelopes (attack, hold, decay), one multiply per sample, and a pitch envelope in semitones;
* the Saturator's first-order ADAA tanh for the kick's drive.

**(DR6) The models.**

* **Kick:** sine with the pitch envelope of 3.1, Hold then exponential decay, a noise-and-pulse click through a
  band-pass, drive by ADAA tanh.
* **Snare:** two modes at 1 and 1.75 × Pitch, each a sine with a small pitch drop and its own decay, plus noise through
  an SVF high-pass at Noise tone, with its own envelope; Tone crossfades body and noise.
* **Tom:** three modes at 1, 1.59 and 2.14 × Pitch, the upper ones decaying 1.6 and 2.3 times faster, a Sweep down to
  Pitch, and a short noise burst for Stick.
* **Hat:** six PolyBLEP squares at the 808 ratios scaled by Tune, two band-passes moved by Colour, a high-pass at Tone,
  noise mixed by Noise. Closed and open are the same model with different Decay (and choke group 1, DR8).
* **Cymbal:** 96–128 resonators rung by a stick, under a wash and a sizzle of noise (section 3.5, revised
  2026-11-21); four modes at a time with SSE2 or NEON.
* **Clap:** two to five noise bursts a few ms apart, then a longer tail, through a band-pass.
* **Rim:** two short modes at a high pitch with a click.

Clap and Rim are optional for the first version (section 9, question 3).

**(DR7) Velocity.** A hit's level is `level · (1 − v + v · (velocity / 127)²)` for the pad's Velocity v (so 0 % plays
every hit alike, 100 % is a square law from silence). Velocity also brightens a little, fixed per model (more click on
the kick, a higher noise tone on the snare, a higher Tone on the hats: an accent), at a quarter of v.

**(DR8) Voices, choke and retrigger.**

* A pool of **32 voices**, allocated in `prepare`, never on the audio thread. A pad plays at most **4** at once; the
  fifth hit takes the pad's oldest, and a full pool takes the oldest anywhere. A stolen or choked voice fades out over
  5 ms, as the Sampler's (`Sampler.cpp:29`).
* **Choke groups 1..8**, as the Sampler's: a hit in a group fades the group's other voices, and a pad never chokes
  itself unless it is **Mono**. The factory kits put closed, pedal and open hats in group 1.
* **Mono** pads (on by default for kicks and toms): a new hit fades the pad's last voice over 5 ms, so a fast kick roll
  doesn't pile up low end.
* Note-offs are ignored: every model is a one-shot. `choke` and `chokeAll` (transport stop) fade every voice.

**(DR9) Outputs work as the Sampler's.** Main plus Out 2..16; a pad plays into its `output`, or into Main while that
port isn't wired (`wiredOutputs`, MO2). **Each pad to its own track** generalises `splitSamplerOutputs` into
`splitInstrumentOutputs { trackId, nodeId, outputs: [{ name, pads | zoneIds }] }`, so both instruments share it (or the
Drum Synth gets a twin of it; section 9, question 5). Removing an output track puts its pads back on Main, as for zones.

**(DR10) Pads and the rest of DAWg.**

* The Step Sequencer's default rows (`Commands.cpp:4333`) take a Drum Synth's pads that aren't off, by their names,
  as they take a Sampler's zones.
* A track whose instrument is a Drum Synth counts as drums (`drumMode` auto, `drumMap.ts:134`), and the roll names its
  rows after the pads. Each model gives a drum-map role (kick, snare, hat closed or open by choke group and decay, tom,
  crash or ride by preset) for drum humanise.
* Node status reports a hit counter per pad, so the panel can flash pads as they play.

**(DR11) Deterministic.** A hit's noise seed comes from the node id, the pad and a hit counter that the transport resets
at a start. The same project renders the same, live and exported, at any block size. Tests rely on it.

## 7. UI, presets and file

**(DR12) A dock panel with the pad grid on the left and the selected pad on the right.**

```
┌ Drum Synth ───────────────────────────── [Kit: Boom ▾] [⋯] ┐
│ ┌────┬────┬────┬────┐  SNARE · Snare ▾        Out 3 ▾  Choke — ▾  │
│ │Crsh│HiTm│Ride│    │  ○Level ○Pan ○Tune ○Decay ○Velocity          │
│ ├────┼────┼────┼────┤  ○Pitch ○Body ○Tone ○Snappy ○N.dec ○N.tone   │
│ │LMTm│HMTm│ ...     │  ┌──────────────────────────────────────┐   │
│ ├────┼────┼────┼────┤  │ pitch and level envelopes, drawn      │   │
│ │ Kck│ Rim│ Snr│ Clp│  └──────────────────────────────────────┘   │
│ └────┴────┴────┴────┘                                                │
└──────────────────────────────────────────────────────────────────────┘
```

* **Pads:** the Sampler's 4 × 4 geometry (`samplerLayout.ts:9-16`, shared), each pad with its name, a small model
  glyph, its output and choke group as chips, and a flash on each hit (DR10). Click selects; click with Alt, or the
  pad's play mark, auditions at velocity 100 (as the Sampler's pads do). Drag a pad onto another to copy it; the pad's
  menu has Rename, Copy, Paste, Model, Own output track, Randomise, Reset, Save pad preset.
* **The selected pad:** its model as a `Select`, Output and Choke as `Select`s, then a row of the common knobs and a row
  of the model's own (DS14's 58 px slots), and a canvas showing the pitch envelope and the amplitude envelope (DS13:
  accent for level, `--tone-1` for pitch, theme colours only) that redraws as the knobs move. Knobs can show
  modulation and automation as any parameter does (DS12).
* **Narrow docks:** the grid shrinks to 2 × 8 strips of names, then the envelope canvas goes first (DS14); the panel
  states its minimum width in `panelMinWidth.ts`.
* Registered in `panels.ts` for `NODE_TYPES.drumSynth`; a new track menu entry "Drum Synth" next to the Sampler.

**(DR13) Kit presets and pad presets.**

* A **kit preset** is the whole node: the sixteen pads (structure) and all parameters. `NodePreset` gains an optional
  `drumPads`, as it gained `shape` for the LFO; `applyPreset` sets both in one command so one undo takes it back.
  Loading a kit leaves the pads' outputs as they are, so a kit split to output tracks stays split.
* A **pad preset** is one model's parameters ("909-ish snare"), offered per model in the pad's menu and the model
  selector.
* **Factory kits**, named by character, not after machines: an 808-style kit (long boom, squares), a 909-style kit
  (punchy click, brighter snare), a tight electro kit, an "acoustic-ish" kit (modal toms, a softer snare, a bell-heavy
  ride), a lo-fi kit (dark, driven). About twenty factory pad presets.

**(DR14) Randomise.** In the ⋯ menu and each pad's menu:

* **Randomise** picks every parameter of the chosen pads within a musically safe range per parameter (narrower than its
  full range: a kick's Pitch within 40–80 Hz, not 30–120), never the model, output, choke group or Level.
* **Vary** moves each by up to ±10 % of its range, for "another take" of the same kit.
* A pad can be **locked** (a lock on the pad) so a kit-wide randomise skips it.
* One command, `randomiseDrumPads { nodeId, pads, amount, seed }`, so it is one undo step and repeatable in tests; the
  safe ranges live in the engine, next to the models.

**(DR15) File and commands.** `GraphNode.drumPads` (16 entries: name, model, output, chokeGroup, mono, locked) in the
project file, one format step, no migration (no project has one; no-backwards-compat-yet). Commands: `setDrumPad`,
`copyDrumPad`, `randomiseDrumPads`, `splitInstrumentOutputs` (DR9), and `auditionPad` as a transient, unrecorded
command, unless the pads can audition through the `Auditioner` the Sampler panel uses (`ui/src/dock/SamplerPanel.tsx:99`).

**(DR16) Later: a Drum Voice node for voice graphs.** One model per node (`dawg.drumVoice`, a choice of model), so a
voice graph can put a filter or a waveshaper after a kick per hit, and a component can make a custom kit. Not in the
first version.

## 8. Decisions

| # | Decision | Recommendation |
|---|---|---|
| DR1 | One instrument node, `dawg.drumSynth`, 16 pads fixed to keys 36–51 | Yes |
| DR2 | Pad structure (name, model, output, choke, mono) in `GraphNode::drumPads`; sound in parameters | Yes |
| DR3 | Parameter list per node, built from the pads' models (`p{n}.{id}`, "Snare · Snappy") | Yes |
| DR4 | Changing a model is a command; lanes and links on parameters that go are removed | Yes |
| DR5 | Voices are plain C++ classes sharing a PolyBLEP square, noise, a shared SVF (no oversampling) and envelopes | Yes |
| DR6 | Analogue-style models; modal bodies for toms and snares; six squares for hats and cymbals | Yes |
| DR7 | Velocity to level by a per-pad amount, plus a small fixed accent per model | Yes |
| DR8 | 32 voices, 4 per pad, 5 ms fades; choke groups 1..8; Mono per pad; note-offs ignored | Yes |
| DR9 | 16 outputs as the Sampler's, unwired in Main; one shared split command | Yes |
| DR10 | Step Sequencer rows, drum mode, roll names and humanise roles from the pads; hits in status | Yes |
| DR11 | Seeded noise per hit: renders are deterministic at any block size | Yes |
| DR12 | Dock panel: pad grid left, selected pad right with knobs and an envelope view | Yes |
| DR13 | Kit presets (pads and parameters) and pad presets; factory kits named by character | Yes |
| DR14 | Randomise within safe ranges, Vary by ±10 %, locked pads skipped; one command | Yes |
| DR15 | `drumPads` in the file, one format step; new commands as listed | Yes |
| DR16 | A Drum Voice node for voice graphs, later | Later |

## 9. Questions for the owner

1. **One kit node (option A), or a voice per node combined by a component (B)?** Recommended: **A**. It is several
   times cheaper to run and to build, and B's flexibility can come later as the Drum Voice node (DR16).
2. **Pads fixed to keys 36–51, or a key per pad?** Fixed keys match the Sampler's grid and General MIDI and need no
   mapping UI. A key per pad allows a kit on, say, C1–C2 for a keyboard player, or two pads on one key for layering.
   Recommended: **fixed** for now; a "first key" setting could shift all sixteen later.
3. **Clap and Rim in the first version?** GM puts a side stick on 37 and a clap on 39, inside the grid, and the Step
   Sequencer's default rows already include a Clap. Each is a small model. Recommended: **yes, both**; otherwise those
   two pads start off.
4. **A parameter list per node (DR3), or a fixed list with generic A–H parameters?** Per node gives "Snare · Snappy" in
   Hz or ms in lanes and modulation; fixed is simpler in the engine. Recommended: **per node**, as the Faust node
   already needs it.
5. **One split command for both instruments (`splitInstrumentOutputs`), or a Drum Synth twin of
   `splitSamplerOutputs`?** Recommended: **one shared command**, renaming the Sampler's; there's no backwards
   compatibility to keep.
6. **Hats' metal: the six squares only, or also an FM choice?** Recommended: **squares only** in the first version,
   with Colour and Noise; FM if the squares turn out too narrow.
7. **Polyphony: 4 voices per pad and 32 in all, Mono on for kicks and toms?** Recommended: **yes**; a crash hit four
   times quickly still rings, a kick roll stays clean.
8. **Factory kit names:** by character (Boom, Punch, Electro, Acoustic, Lo-fi) rather than machine names.
   Recommended: **by character**, with "808-style" and "909-style" only in their descriptions.

### 9.1 The owner's answers (2026-11-18)

The owner accepted every recommendation:

1. **Option A**, one kit node; the Drum Voice node for voice graphs later (DR16).
2. **Pads fixed to keys 36–51.**
3. **Clap and Rim are in the first version**, on 39 and 37.
4. **A parameter list per node** (DR3).
5. **One shared split command**, `splitInstrumentOutputs`, replacing `splitSamplerOutputs`.
6. **The six squares only** for the hats' and cymbals' metal; FM only if they turn out too narrow.
7. **4 voices per pad, 32 in all**, Mono on by default for kicks and toms.
8. **Factory kits named by character**, with "808-style" and "909-style" only in their descriptions.

**Revised (2026-11-18), answer 4: one fixed parameter list.** Building the node showed that a per-node list would need
the Faust node's special path in the compiler, the commands, project loading and the UI, and a new instance whenever a
pad changed model. The owner chose the alternative: every pad has the five common parameters and every model's own,
40 in all, 640 for the node, in one static descriptor (`p3.level`, `p3.snare.snappy`, named "Pad 3 Snare Snappy").
The UI shows a pad's current model's. So DR3 and DR4 change:

* A pad keeps each model's settings while it plays another: Snare to Tom and back brings the snare back as it was.
* Changing a model removes nothing. Lanes, modulation and links on another model's parameters stay, and do nothing
  while that model isn't the pad's.
* Defaults follow the default kit per pad (the six toms' pitches, the hats' decays, the ride's bell).

### 9.2 Open after building (2026-11-19)

**Revised (2026-11-20), the kit's balance.** The owner found the hats, cymbals and toms too loud against the kick and
snare. The default kit now sets each pad's Level: kick and snare 0 dB, Snare 2 −1, clap −2, rim −4, toms −5, closed
and open hat −9, pedal hat −11, crash −10, ride −12 dB. These follow common mix balances, with the hats and cymbals
more than 6 dB under, as asked. Factory kits leave Level alone, so they keep this balance.


1. **A cheaper cymbal?** A cymbal voice costs about 200 ns per sample, three to seven times any other model (step 7).
   Options:
   * one metal bank for both channels, made wide by giving the right channel's band-passes other centres (about a
     third cheaper; the width sounds a little different);
   * the bands and the bell worked out every second sample and interpolated (about half; a slight loss above 12 kHz);
   * leave it.

   Recommended: **the first**, if a busy kit's 9 % of a block is too much; else leave it.

   **Answered (2026-11-19): the first, built.** The cymbal plays one metal bank on both sides; the right channel
   hears it through bands up to a fifth apart from the left's (Width sets how far), plus its own noise. A cymbal
   voice went from about 205 to 133 ns per sample, and the busy pattern from 0.89 to 0.60 ms per block. The width
   test (left/right correlation under 0.9 at Width 100) still passes.
2. **Revised (2026-11-21): the cymbal, after prototypes.** The owner found crash and ride "way too bell-y", with no
   setting that sounded natural. Measured 0.3 s into a hit, from 500 Hz to 12 kHz, as the loudest bin over the median
   bin: the old crash 59 dB, the old ride 46 dB. A noise-and-shimmer prototype measured 11–17 dB and was too noisy; a
   modal one 49–53 dB, still tonal. The owner chose the hybrid, modes under a wash (about 32 dB), now the Cymbal
   model (section 3.5): the default crash measures about 35 dB, the ride about 30 dB. The new parameters replace
   Noise and Bell; a crash and a ride are one model at Ride 0 and 100 %. Its trim keeps the kit's balance: as loud as
   the old cymbal, a crash within 1.5 dB RMS over 2 s, a ride 2 dB quieter.

   Cost, on a desktop that measured every model about 3.9 times slower that day: a cymbal voice 473 ns per sample
   against a kick's 107, so about 120 ns on the earlier figures (the old cymbal 133). The busy pattern runs 1.15 ms
   per block that day, about 0.3 ms on the earlier figures (from 0.60), as the ride and the crash now play one voice
   each. The modes are 65 % of the cymbal, the wash most of the rest. The prototype stays as the hidden
   `tests/CymbalPrototypeTests.cpp` ([.cymbal-proto], [.cymbal-modal], [.cymbal-engine]).
   **Revised (2026-11-21): a crashier crash.** The owner liked the ride, but found the crash not crash-y enough.
   Prototyped: energy spreading between modes (a cubic coupling, section 3.5), a louder and brighter splash of wash,
   and both. The owner chose the coupling, as a soft hit of the stronger variant sounded (`crash2-1c-coupled-soft`);
   the engine's full-velocity crash matches it. The crash's trim rose 1.4 dB to keep its loudness.
3. **The pad flash** needs the per-window watch from the session that fixed `nodes.watch` (its branch, commit
   `ecb1f08`), and has not been seen with an audio device on.

## 10. Steps and tests

Each step ends with its tests passing and doxygen clean (local-doc-check).

1. **Shared DSP blocks.** Move `blep` and the Filter's SVF into shared headers; the Filter node uses the moved SVF,
   oversampling untouched. Add the exponential envelope and the pitch envelope.
   *Tests:* the Filter's existing tests (`NodeGraphTests`, `VoiceGraphTests`) pass unchanged (the Filter nulls against its old output); an envelope
   reaches −60 dB at its decay time ±2 %; the pitch envelope in semitones matches its formula.
   **Done (2026-11-18):** `dawg/PolyBlep.h` (blep, blamp), `dawg/Svf.h` (`SvfCoefficients::fromBiquad` and
   `::bilinear`, `SvfState`), `dawg/DrumEnvelopes.h` (`DrumEnvelope`: attack, hold, decay to −60 dB, a linear choke
   fade; `PitchSweep`). The Filter's output at every mode, slope and oversampling hashed the same before and after the
   move, bit for bit. Tests in `tests/DrumDspTests.cpp`.
2. **Voice models (DR5, DR6, DR7).** Kick, Snare, Tom, Hat, Cymbal (and Clap, Rim if question 3 says yes) as classes,
   with no node yet. *Tests* (`tests/DrumVoiceTests.cpp`), each rendering one hit at 48 kHz:
   * **Kick pitch:** zero-crossing frequency in the first 2 ms within 3 % of Pitch × 2^(Sweep/12), and within 1 % of
     Pitch after 5 Sweep times; **decay:** the RMS envelope crosses −60 dB within 10 % of the expected time, scaled by
     the pad's Decay; **click:** energy above 2 kHz in the first 5 ms rises with Click and is absent after 20 ms.
   * **Snare:** spectral peaks within 2 % of Pitch and 1.75 × Pitch; Snappy 0 leaves no energy above 4 kHz; Noise tone
     moves the noise's centroid.
   * **Tom:** the fundamental settles on Pitch (±1 %), the 1.59 mode is present, Sweep 0 has no pitch drop.
   * **Hat:** energy below 1 kHz at least 40 dB under the peak; closed decay shorter than open at equal settings;
     **aliasing:** a hit at 48 kHz and at 96 kHz, compared below 20 kHz, differ by less than −50 dB.
   * **Cymbal:** Width 0 renders identical channels, Width 100 a left/right correlation below 0.9; Bell raises energy
     at its partials. Revised 2026-11-21 for the modal cymbal: Bell's test goes; the loudest bin stays under 42 dB
     over the median for a crash and 38 dB for a ride, over four seeds, and the wash lowers it by over 6 dB; Ride
     rings longer; Colour and Tone brighten; a restrike adds to the ringing modes, and only a sounding cymbal
     restrikes.
   * **Velocity:** level follows DR7 for Velocity 0, 50 and 100 %.
   * **Determinism:** two renders of a seeded hit are bit-identical; different hit counters differ.

   **Done (2026-11-18):** `dawg/DrumVoices.h` and `engine/src/DrumVoices.cpp`: `DrumModel`, the common and per-model
   parameter tables (section 5's, with Clap and Rim), `DrumHit` (a hit's values, velocity and seed), `DrumMetal` (the
   six polyBLEP squares) and `DrumVoice`, which plays any model and allocates nothing after `prepare`. Tests in
   `tests/DrumVoiceTests.cpp`; a hidden `[.drum-wavs]` test writes one hit of each model to `DAWG_DRUM_WAVS` for
   listening. As built, differing from the plan above:

   * The hats' high-passes are 24 dB/oct (two SVF stages): at 12 dB/oct the squares' fundamentals (205–800 Hz) sat
     only 36 dB under the peak.
   * **Aliasing** is measured on the metal bank itself, steady, as the energy outside the six squares' odd harmonics:
     about −39 dB at Tune 0 and −35 dB an octave up, against −23 and −19 dB for naive squares. Comparing whole hits at
     48 and 96 kHz measured the band-passes' bilinear warping more than any aliasing, so that test was dropped. The
     −50 dB of the plan would need more than two-point polyBLEP (a longer BLEP or 2x for the metal alone); the
     high-passes after the bank remove most of what folds below them.
   * The snare's noise tone is checked by the share of energy between 2 and 5 kHz, not the centroid: the high-pass
     leaves the noise flat up to Nyquist, so the centroid hardly moves.
   * Each model has a fixed output trim, so all peak near −3 dBFS at their defaults (tested between
     −12 and 0 dBFS).
3. **The node (DR1, DR2, DR3, DR8, DR9, DR11).** `dawg.drumSynth`, its per-node descriptor, `drumPads`, the voice
   pool, choke, mono, outputs, status hits; the file format step. *Tests* (`tests/DrumSynthTests.cpp`):
   * key 38 plays the snare pad only; keys outside 36–51 play nothing;
   * **choke:** an open hat then a closed hat in group 1: the open hat's level falls 60 dB within 6 ms of the closed
     hit; pads in other groups ring on; a pad doesn't choke itself unless Mono; a Mono kick's second hit fades the
     first over 5 ms;
   * **stealing:** five quick hits on one pad keep four voices; 40 hits across pads never allocate
     (`AllocationGuard`) and never click (no sample-to-sample step above a bound);
   * **outputs:** a pad on Out 3 with Out 3 wired is absent from Main and present on Out 3; unwired, it plays in Main;
   * **block size:** a pattern rendered in 64- and 480-sample blocks nulls;
   * transport stop (`chokeAll`) fades everything within 5 ms;
   * project file round trip of `drumPads` and the per-node parameters; descriptor cache returns the same descriptor
     for the same models.

   **Done (2026-11-18)**, with the fixed list of 9.1: `dawg/DrumSynth.h` and `engine/src/DrumSynth.cpp`. `DrumModel`
   and `DrumPad` live in `dawg/Model.h`, `GraphNode::drumPads` holds sixteen (empty plays the default kit), and the
   file writes them as `pads` (`projectfile::drumPadsFromVar` reads them, clamping outputs and choke groups and
   refusing a wrong count or an unknown model). `makeNode` gives a new Drum Synth the default kit. The pads reach the
   audio thread through `configure()`, one packed atomic per pad, so editing a pad never recompiles. A hit reads its
   pad's parameters, modulated ones at its sample, when it starts. The node status (`meters()`) has `hits` per pad
   and `voices`. A seek or start (`chokeAll`) fades every voice and starts the seeds over. Tests in
   `tests/DrumSynthTests.cpp`; the descriptor-cache test went with the per-node list.
4. **Commands (DR4, DR9, DR10, DR14, DR15).** `setDrumPad`, `copyDrumPad`, `randomiseDrumPads`,
   `splitInstrumentOutputs`; Step Sequencer default rows; drum-mode detection. *Tests* (`CommandTests`,
   `MultiOutputTests`, `MidiEffectTests` for the Step Sequencer):
   * changing a pad's model keeps Level, Pan, Tune, Decay and Velocity, resets the rest, removes an automation lane on
     a parameter that went, and one undo restores all of it;
   * each pad to its own track: one undo; removing an output track puts its pads on Main; the header and Main output
     track follow MO12;
   * randomise with a seed is repeatable, stays inside the safe ranges, leaves locked pads, Level, model, output and
     choke alone;
   * a new Step Sequencer on a Drum Synth track takes the pads that aren't off, by name.

   **Done (2026-11-18):** `setDrumPad`, `copyDrumPad` and `randomiseDrumPads` in `Commands.cpp`;
   `splitSamplerOutputs` became `splitInstrumentOutputs`, taking `zoneIds` for a Sampler or `pads` for a Drum Synth (the
   Sampler panel and its tests now call it so). Removing an output track puts the pads that fed it back on Main, and a
   Drum Synth's Main counts as playing while a pad that isn't off plays in it, so MO12's Main output track comes and
   goes as for a Sampler. A new Step Sequencer takes the pads that play, by name. With the fixed parameter list
   (section 9.1) these differ from the plan above:

   * Changing a pad's model removes nothing (there is nothing to restore), so that test checks that a model's values
     survive switching away and back.
   * `copyDrumPad` copies the name, model, choke group, Mono and all 40 parameters (defaults included, as pads'
     defaults differ); the target keeps its output and lock, so a split kit stays split.
   * Randomise and Vary touch only the pad's current model's parameters, and Tune (±2 semitones; Vary: ±0.5, and Decay
     by up to a tenth of itself). A tom's pitch and a cymbal's Ride (its Bell before 2026-11-21) are kept, as they decide which tom it is and
     whether it is a crash or a ride. The safe ranges are `drumModelRandomRanges`, next to the models. Without `pads`,
     every pad that plays and isn't locked; a pad named in `pads` is randomised even when locked.
   * Drum-mode detection for the roll and humanise belongs to the UI, so it moves to step 5.
5. **Panel (DR12).** `DrumSynthPanel.tsx` with `drumSynthLayout.ts` for its geometry. *Tests* (Vitest,
   `DrumSynthPanel.test.ts`, `drumSynthLayout.test.ts`):
   * the pad grid's keys and reading order match the Sampler's (`padKeys`);
   * the model's knob row lists that model's parameters in order, with names and units from the node's info;
   * the envelope view's points follow Sweep, Sweep time, Hold and Decay;
   * the panel's minimum width holds every control at the narrow layout;
   * hit flashes follow the status counters;
   * a CDP check in the running app (cdp-ui-driving): add a Drum Synth, play the default pattern, select pads, change
     a model, split to tracks.

   **Done (2026-11-19):** `ui/src/dock/DrumSynthPanel.tsx` and `ui/src/dock/drumSynth.ts` (its pads, parameter ids,
   labels, envelopes and outputs, tested in `drumSynth.test.ts`). As DAWg shows instruments, the panel is in the
   instrument's own window (the inspector's Edit), not in the dock's strip. The pads are the Sampler's grid; each shows
   its name, a model letter, its output and choke group as chips, and a lock. Pressing a pad plays it, dragging one onto
   another copies it, and its menu has the models, Copy and Paste, Randomise, Vary, Lock and Own output track. The ⋯ menu
   has Randomise kit, Vary kit and Each pad to its own track (as many as there are free outputs; the rest stay on Main,
   and a hint names them). The header has the pad's name to type, its model, output, choke group, Mono and lock; under
   it the common knobs, the model's own, and the level (accent) and pitch (a tone) over the hit.

   Elsewhere: `paramApplies` hides parameters of other models and of pads that are off, so the inspector, the
   automation picker and the graph's modulation targets list only what plays; lanes, the picker and the inspector's
   knobs name a parameter after its pad ("Crack Snappy"). A track playing a Drum Synth counts as drums, and the roll and
   humanise take its pads as the drum map (names, and each key's General MIDI role where the pad plays that kind of
   drum, else its model's). The graph shows the outputs the pads use; the Drum Synth is kept out of voice graphs.

   Checked in the running app (2026-11-19, a test instance over CDP): the default kit's pads, switching a pad's model
   (the knobs follow), and splitting the kit to 15 output tracks with Ride left on Main. **Not checked there:** the hit
   flash. The test instance had no audio device open, so nothing played. Also, the app keeps one list of watched nodes
   for all its windows, so the main window's dock replaces the instrument window's watch when it redraws. The flash
   needs the instrument window's watch to hold; that is a fix in `PanelControl.cpp` of its own. The panel's minimum
   width is set in `deviceWidths.ts`, and was checked by eye at 1400 px.
6. **Presets and randomise in the UI (DR13, DR14).** Kit and pad presets in `presets.ts`, factory kits, the lock, the
   menus. *Tests* (`presets.test.ts`): a kit preset applies pads and parameters in one command and keeps outputs; a
   pad preset applies only to its model; factory presets name only parameters their model has.

   **Done (2026-11-19):** `ui/src/dock/drumKits.ts`, tested in `drumKits.test.ts`.
   * **Kits** are the Drum Synth's node presets in the usual selector (in the inspector, and now in the panel's Pads
     bar). A kit is the whole node: loading one sets each parameter to the kit's value, or else to its default, and
     sends only the ones that change. It also sets each pad's name, model, choke group and Mono, all as one undo step.
     Outputs and locks stay. A saved user kit keeps the pads (`NodePreset.drumPads`).
   * **Factory kits:** Boom, Punch, Electro, Acoustic and Lo-fi, each with a description in its tooltip ("808-style",
     "909-style" only there). Each is written as settings per model for every pad of that model, plus a few per pad
     (the hats' and the ride's), over the default kit's pads.
   * **Pad sounds:** 21 of them, 2 to 4 per model. A pad's menu lists its model's sounds. Loading one sets that
     model's parameters and Decay and Tune (to the sound's values, else the defaults) in one step, and switches the pad
     to the model when it plays another. Level, Pan and Velocity stay. Saving a pad as a sound of its own isn't in
     this step.
   * The models' parameter ids are written out again in `MODEL_PARAMS` to check the presets against; they mirror
     `DrumVoices.cpp` and have to follow it.
   * Not checked in the running app: this step reuses the preset selector and the batch command as they are, and its
     commands are tested.
7. **CPU check.** A hidden-tag bench: the busiest factory kit playing sixteenth hats, crashes and a tom fill, 12 voices
   ringing, under 0.2 ms per 10 ms block in a release build, and compared with the same kit as voice graphs if one is
   ever built.

   **Done (2026-11-19), and the target missed:** two hidden benches, run in a Release build on the 16-thread desktop
   with no other build running.
   * `[.drum-bench]` (`tests/DrumSynthBenchTests.cpp`) plays the default kit, with cymbals and toms at Decay 400 %:
     sixteenth hats with an open one each beat, eighth rides, kick, snare, a crash each bar and a tom fill. It runs
     20 s in 10 ms blocks, with 15 voices sounding on average and 17 at most. **It costs about 0.89 ms per block (9 %
     of the block), against the 0.2 ms planned.** The first build cost 1.44 ms.
   * `[.drum-voice-bench]` (`tests/DrumVoiceBenchTests.cpp`) times one voice of each model, in ns per sample, first
     build → now: kick 92 → 29, snare 69 → 37, tom 93 → 39, hat 110 → 72, cymbal 350 → 205, clap 21 → 15, rim
     109 → 35. Flushing denormals changes nothing.

   What made it faster:
   * every sine through a polynomial (`sinOfPhase`, within 4e-6);
   * a pitch sweep that stops calling `exp2` once it has landed;
   * phases wrapped by a subtraction, not `floor`;
   * the squares' steps worked out once per hit;
   * noise, filters, modes and cymbal bands skipped once their envelope has ended or their gain is 0;
   * a width-0 cymbal worked out on one channel.

   The kick's drive became a rational soft clip, without ADAA: its input is a low sine, whose harmonics stay far below
   Nyquist, and ADAA's `log`/`exp` were most of the kick's cost.

   The estimate of section 3.6 was several times too low. Measured, a drum voice costs 0.007–0.1 ms per 10 ms block,
   cymbals the most, against 0.1–0.25 ms for a voice of a Voice Instrument synth. The busy pattern's 0.89 ms is about
   what four to eight synth voices cost. Six long cymbals are most of it, at about 200 ns per sample each (two banks of
   six squares and eight filters). The bench's check is now a regression bound, under 1.5 ms. Section 9.2 asks whether
   to make the cymbal cheaper.
