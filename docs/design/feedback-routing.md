# Design: feedback routing (phase 16)

Status: **accepted, steps 1 to 5 done** (section 7): loops can be patched in the app, and factory patches are next. The decisions are numbered R1–R12 and listed in section 9, and
section 10 holds the owner's answers. Output protection, which this phase needs, is done (output-protection.md).

## 1. What this is, in plain terms

Today a cable can only go forward: the graph refuses anything that would send a node's output back to an earlier node.
Feedback lifts that, but only through one new node, the **Feedback** node. It is a short delay line with a safety on it, and it
is the only thing a loop may close through.

Why a loop needs a delay at all: in a loop, each node needs its input before it can compute its output, so with no delay
nothing can go first. The delay gives the loop a starting point: the Feedback node plays what went in a moment ago, which is
already known.

```
              ┌──────────────── Feedback ◄───────────────┐
              │            (delay, safety)               │
              ▼                                          │
 Input ───► Mix ───► Filter ───► Saturator ──────────────┴───► Output
```

Each trip round the loop, the sound passes through the filter and the saturator again. Everything feedback is good for comes
from that: the loop's **length** decides timing or pitch, and what's **in the loop** decides how the sound changes each time
round.

| Loop length | What you hear | Example |
|---|---|---|
| Over about 50 ms | separate echoes | dub delay |
| About 5 to 50 ms | a blur or a metallic room | short reverb, flange at the low end of this |
| Under about 20 ms, steady | a **pitch**: 1000 ms / length = the note's frequency | plucked string, resonator |

## 2. Use cases

Each one is what a user would patch, and is meant to become a test or a factory patch.

### 2.1 Dub delay (track graph)

Echoes that get darker and dirtier each time, which a plain delay can't do.

```
 Input ─► Mix ─► Feedback (3/8 note, 70 %) ─► Filter (low-pass) ─► Saturator ─┬─► Mix ─► Output
           ▲                                                                  │
           └──────────────────────────────────────────────────────────────────┘
```

Automate the amount above 100 % on the last beat of a phrase, and the echoes swell by themselves; the saturator and the node's
limit keep them from running away.

### 2.2 Plucked string, Karplus-Strong (voice graph)

The classic: a burst of noise goes round a very short loop with a low-pass in it. It sounds like a plucked string, because the
loop length sets the pitch, and the low-pass makes the highs die first, as on a real string. Every note gets its own loop.

```
 Noise ─► Gain (short ADSR) ─► Mix ─► Feedback (follows the note, 98 %) ─► Filter ─┬─► Voice Output
                                ▲                                                  │
                                └──────────────────────────────────────────────────┘
```

The amount is the decay; the filter is how dull the string is. Swap the noise for a sample, and it sounds like hitting a
tuned object. This is the use case that sets the loop's shortest length (R3).

### 2.3 Resonator on a drum bus (track graph)

Tune a track's drums to the key of the song: a short loop rings at a note whenever something hits it. Several in parallel
make a chord of resonances.

### 2.4 Flanger with bite (track graph)

A 1 to 10 ms loop, its length moved by an LFO through the Feedback node's Time modulation input. The jet sound of a flanger is
the amount; a negative amount (R5) gives the hollow version.

### 2.5 Self-oscillation: a sound from nothing

Amount above 100 %, a filter and a saturator in the loop: the loop keeps going without any input, like a no-input mixer or a guitar
held near an amp. The filter's cutoff picks the pitch it howls at. Silence stays silent, so it needs a small push, such as one
hit on the input. This is why going above 100 % has to be safe (R6).

### 2.6 A rising shimmer

A pitch shifter (a Faust script or a plugin) and the Loop Reverb in the loop: each trip goes up an octave and gets washed out,
so tails climb and fade. Here the plugin's and reverb's own latency become part of the loop's length (R4).

### 2.7 A compressor listening to itself

Old hardware compressors react to their own output, which is much of their sound. Route the compressor's output through a Feedback
node at its shortest length into its own key input. No Mix node, and no loud loop: only the control signal goes back.

### 2.8 A reverb of your own

Three or four Feedback nodes of different lengths, crossing into each other through Mix nodes, make a feedback delay network, which is
what many reverbs are inside. It needs several loops sharing nodes, which R2 handles as one region.

### 2.9 What it won't do

* **Feedback between tracks** (a send back to an earlier track). See R10.
* **Single-sample loops.** A very high string or a zero-delay filter needs a Faust script, whose `~` operator loops per sample.
* **Feedback of notes or modulation.** Only audio; see R9.

## 3. The node

**(R1) Loops close only through a Feedback node.** One new node type, `dawg.feedback`, one audio input, one audio output, allowed
in track and voice graphs. A cycle with no Feedback node in it stays refused. A cable can't make a loop by accident: dragging one
that would close a loop inserts a Feedback node on it and selects it, and undo takes it back in one step.

The graph draws the cable out of a loop's Feedback node dashed, in its own colour, so loops can be read at a glance.

| Parameter | Range | Default | Meaning |
|---|---|---|---|
| Length mode | Time, Sync, Note | Time | how the loop's length is set |
| Time | shortest (R3) to 2000 ms | 100 ms | the length of the whole trip round the loop |
| Sync | note values, as the Delay's | 1/8 | the same, from the tempo |
| Pitch | ±120 semitones from C4 | 0 | the same, as a pitch, as the oscillator's (V3) |
| Tune | ±100 cents | 0 | fine correction in Note mode |
| Trim | 0 to 16 samples | 0 | takes a fixed delay off in Note mode: a filter on the loop delays by the same small time at every note, so it flattens high notes most, which Tune can't correct |
| Amount | -100 %..100 %, or ±150 % with Above 100 % | 50 % | how much goes round again |
| Above 100 % | off, on | off | lets the loop grow (R6) |

Time, Amount and Pitch have modulation inputs from the modulation pool. A depth of 1 on Time moves it by 50 ms rather than its whole range, as a flanger moves a loop by a few
milliseconds, which a depth over 2 seconds couldn't set (`ParamInfo::modulationSpan`). In a voice graph, Note mode takes its pitch from Note
Expression's Pitch through that input, as the oscillator does (V3), which is how a string follows the keyboard.

The node has no filter or mix of its own on purpose: what's in the loop is the point, and it's patched with the nodes that exist.

A Feedback node whose output doesn't lead back to its input is simply a delay, and costs what a delay costs.

## 4. How it runs

The problem: DAWg runs a graph one block at a time, and blocks are the device's buffer size, say 128 to 1024 samples. Letting a loop
take a block per trip is the easy way, but it fails on three counts: a string's pitch would change with the buffer size, a loop
couldn't be shorter than about 3 to 20 ms, and an export at another block size would sound different.

**(R2) A loop runs as a region, in small fixed steps.** The compiler finds the nodes on any loop (strongly connected
components, cut nowhere but at Feedback nodes) and treats each such group as a **region**: one entry in the track's step list.
Within a block, a region runs its nodes over and over, in order, in chunks of at most N samples, with each Feedback node reading
before and writing after. Everything outside a region still runs a block at a time.

This has been done before in the engine: a voice already renders its block in pieces, through `runSteps()` with a sample offset,
with events at their offsets in each piece (V9). A region reuses that. Several loops that share a node are one region, as in 2.8.

Since a Feedback node's read never needs what's written in the same chunk, the result doesn't depend on how a block divides up. **A
loop sounds the same at any buffer size and in an export**, within the differences parameter smoothing already has. That is a test (section
8).

**(R3) N is 16 samples, confirmed by measurement (below).** N limits the loop's shortest length, and so a string's highest note. Reading between
samples needs up to 2 samples more (section 4), so the shortest loop is N + 2 samples:

| N | Shortest loop at 48 kHz | Highest pitch | Highest note | Calls per second, per node |
|---|---|---|---|---|
| 64 | 66 samples, 1.38 ms | 727 Hz | F#5 | 750 |
| 32 | 34 samples, 0.71 ms | 1412 Hz | F6 | 1500 |
| **16** | **18 samples, 0.38 ms** | **2667 Hz** | **E7** | **3000** |
| 8 | 10 samples, 0.21 ms | 4800 Hz | D8 | 6000 |

At N = 16 the highest note is D7 at 44.1 kHz, E7 at 48 kHz and E8 at 96 kHz, so at 48 kHz the piano's top 8 keys are out of reach. The owner chose to stay with 16 knowing that
(2026-11-02); a Faust script loops per sample for those. Above the limit a loop stays at its shortest length, and a filter in the loop can lower the top note a little more, since its
delay can't be taken out of a loop that has no length left. There is no limit at the low end: MIDI note 0 is about 122 ms. N doesn't change with the sample rate, so at 96 kHz the same notes cost twice the calls.

**Measured on 2026-11-02** (`tests/FeedbackBenchTests.cpp`, hidden tag `[feedback-bench]`): real nodes through `runSteps()` in every voice slot, as a
voice graph compiles them, with the copy through a delay line per chunk, in a Release build on one core of the owner's machine. The patch is Oscillator,
Mix, Filter, Saturator and Gain, 5 calls per voice per chunk, at 48 kHz:

| 16 voices, block of 128 | Whole block | N = 64 | N = 32 | **N = 16** | N = 8 |
|---|---|---|---|---|---|
| Share of one core | 34.1 % | 35.8 % | 38.1 % | **39.4 %** | 50.6 % |

A block of 512 gives about the same: 34.3 %, 35.2 %, 37.1 %, 40.5 % and 45.2 %. So N = 16 costs about 5 to 6 percentage points of a core for 16 voices, and
one voice 2.6 % against 2.9 %. That is about 250 ns for each extra call, and it doesn't depend on the node, so a loop of lighter nodes pays the same points on a smaller total. N = 8 costs
two to three times as much again for an octave nobody plays much. The nodes' own work dominates. The figures are from one run, and the signal came from the
oscillator rather than around a loop, which costs the same but isn't the same test as the real region of step 2.

**(R4) Latency inside a loop is part of the loop.** A plugin's latency can't be compensated inside a loop, because the signal is coming
round to meet itself. So the Length is the length of the *whole trip*, and the compiler subtracts the latency of the nodes in the
loop from the Feedback node's own delay:

```
 own delay = Length - (latency of the other nodes on the loop), and at least N
```

So a plugin added to a string's loop doesn't change its pitch unless the loop can't get that short, and the node then shows the
shortest length it can reach. With two paths of different latency through a loop, the longer one counts, and paths inside a region
get no compensation delays. A filter's own delay is not latency, so a string with one runs flat, more the higher it plays: a 7 kHz low-pass measured 6 cents flat at A2 and 50 at A5. Trim takes out that fixed delay (1.6 samples there).

The region's latency to the rest of the graph is the most any output path has, ignoring the way back through the Feedback node, and is
compensated outside the region as usual.

**Reads between samples.** A length rarely lands on a whole sample, and strings need it to, so the read interpolates between 4
points. That takes the read a sample or two further back, which the shortest length allows for.

**(R5) The amount can be negative.** That inverts each trip, which puts the loop's resonances between where they'd be, and gives a
string or flanger a hollow sound at an octave up. It costs nothing to allow.

## 5. Keeping it safe

A loop is the one thing in the graph built to grow, so a mistake in one has to be harmless: to ears first, and to the state of the
project second.

**(R6) Every Feedback node limits what it passes on, always.** This isn't a parameter.

* **A soft limit**: a saturation curve that does nothing to speak of below 0 dBFS, and approaches +6 dBFS and goes no further. So a loop
  can't grow without bound whatever is in it, and at Amount 100 % or above it settles at a level rather than running away.
* **A DC blocker** at about 5 Hz. A loop with any offset would otherwise add it up every trip.
* **A NaN check**: a non-finite sample reaching the node empties its delay line, and the node passes one invalid sample on, so that S1 (output protection)
  silences and resets the track and tells the user, even if the bad node doesn't otherwise reach the output. A loop can't hold a NaN and bring it back.
* **Amount stops at ±100 % unless Above 100 % is on**, and modulation can't pass the limit either. It's a switch of its own so that a slip
  of the mouse can't reach it.

The node reports when it's limiting, and the graph shows it on the node. That's normal for self-oscillation, and tells the user why
a patch got loud or dull.

The existing protection stays underneath: the track's invalid-audio check (S1), the device ceiling (S2) and the mute on a runaway output
(S3). A loop limited to +6 dBFS, followed by a +24 dB gain, is still caught by S3.

**(R7) Stopping a loop.** A loop's tail rings on after the transport stops, as a delay's does, and at Amount above 100 % it would ring for
ever. So:

* A **Clear** button on the node empties its delay line at once.
* A **voice** with a self-oscillating loop already can't ring for ever: after note-off it ends at the 10-second safety tail, or sooner
  once quiet (V9), and a new note in its slot resets it.
* **Export:** the automatic tail already has a limit of 30 seconds (X2), and a self-oscillating loop would run into it, so the export
  dialog says so if a Feedback node has Above 100 % on.

**(R8) A bypassed Feedback node outputs silence.** Every other effect passes its input through when bypassed, but here that would close the loop
with no delay, so bypass breaks the loop instead. The same goes for any node that a bypass would turn into a wire: a loop that still has
its Feedback node keeps its delay.

## 6. Limits of the first version

**(R9) Audio only.** Modulation and events can't go through a Feedback node. A modulation loop (an oscillator modulating its own
pitch, an envelope follower pushing a filter) is worthwhile, and it's the same mechanism with another port type, so it can come later.
Note that an LFO's or ADSR's output feeding a node in a loop works normally: only the way *back* must be audio.

**(R10) No feedback between tracks.** Tracks run in an order set by their sends, in parallel on a pool. A loop across tracks would need
regions spanning tracks, which is a much larger change. Inside one track covers the use cases above. The refusal for sends stays.

**(R11) Plugins are allowed in a loop in a track graph,** and stay out of voice graphs (V6). A plugin called every 16 samples is costly,
more so in a helper process, and many plugins compute in larger blocks internally, which they report as latency, which R4 then adds to the
loop. It works and is the user's choice; the plugin's own limits show as a longer shortest loop. Each call to a helper is a hand-off,
so a plugin in a loop costs about 8 times what it costs at 128 samples a block, and a late block there loses the region's whole
block. The node's tooltip says so.

**(R12) Model and file.** `dawg.feedback` is one more node type with ordinary parameters. Connections stay as they are, and `connect` allows a
cable when every cycle it would close goes through a Feedback node's input: `createsCycle` stops at Feedback nodes. No backward compatibility
is needed (only test projects exist), and old projects have no such node anyway.

A graph with a loop isn't a simple chain, so it shows as custom routing in the inspector (G6).

## 7. Steps

1. **Measure.** Done: N = 16 (R3).
2. **Compiler and engine,** done: the node without Length modes, regions, `connect`, `topologicalOrder` and `nodesReaching` aware of Feedback nodes,
   and the null test across buffer sizes.
3. **Safety,** done: R6 to R8, with the runaway tests, except the UI's part of R7. A voice resets the node at each new note, so `reset()` forgets the line without
   clearing it, which would take a megabyte's writes per note.
4. **Length,** done: Sync and Note modes, interpolation, and R4's latency.
5. **UI,** done: the node, dashed cables, inserting a Feedback node on a closing cable, the limiting light, Clear.
6. **Factory patches** from section 2: dub delay, plucked string, resonator and flanger, the three effects as components (component-nodes.md) are in the graph's add menu (`ui/src/graph/patches.ts`), measured, and waiting for the owner's ears.

## 8. Tests

* A loop gives the same audio at buffer sizes 32, 128, 480 and 1024, and offline.
* A loop's length is what it says: an impulse comes back at the Length, with and without a latent node in the loop.
* A string's pitch at several notes is within a few cents, measured.
* Runaway: Amount 150 % with +24 dB in the loop, for minutes of audio: the Feedback node's output never passes its limit.
* NaN injected in a loop: the track goes silent, resets and recovers.
* Bypassing the Feedback node stops the loop, and un-bypassing starts from silence.
* A cycle with no Feedback node is refused, and removing a loop's only Feedback node removes its cables.
* Two loops sharing a node compile as one region, and a Feedback node with no loop as a plain delay.

## 9. Decisions

| # | Decision |
|---|---|
| R1 | Loops close only through a Feedback node; a closing cable inserts one |
| R2 | A loop runs as a region in fixed small chunks, so it sounds the same at any buffer size |
| R3 | Chunks of 16 samples, as measured |
| R4 | Length is the whole trip; latency in the loop is subtracted from the node's delay |
| R5 | The amount can be negative |
| R6 | A limit, DC blocker and NaN check are always on; above 100 % needs its own switch |
| R7 | Clear on the node; voices and exports have limits already |
| R8 | A bypassed Feedback node outputs silence |
| R9 | Audio only for now |
| R10 | No feedback between tracks |
| R11 | Plugins allowed in track graph loops, at their cost |
| R12 | No new model structure; `createsCycle` stops at Feedback nodes |

## 10. The owner's answers

The owner accepted every recommendation below on 2026-11-02.

1. **The name.** "Feedback" says what it's for, and "Feedback Delay" says what it is, but sounds like the effect. Recommended: Feedback.
2. **Length is the whole trip (R4), or just the node's own delay?** The whole trip keeps a string in tune when the loop changes, but the number
   isn't what the node alone delays. Recommended: the whole trip.
3. **Should the limit sit at +6 dBFS (R6)?** Lower means safer and more colour from the limit. Recommended: +6, with a look at it by ear in step 6.
4. **A closing cable (R1):** offer to insert a Feedback node, or just insert one? Recommended: insert it and select it, since undo is one
   step away and the amount starts at a safe 50 %.
5. **Deleting a loop's only Feedback node:** remove the cables that closed the loop, or refuse? Recommended: remove the node's cables, as
   deleting any node does, which always leaves a valid graph.
6. **Is a modulation loop (R9) needed in the first version?** Recommended: no; it follows easily.
