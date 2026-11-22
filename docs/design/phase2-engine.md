# Phase 2 design: processing graph and note/curve model

Status: **accepted** (2026-10-17). All decisions (D1)…(D8) were accepted as recommended.

## 1. Goals and non-goals

Phase 2 builds the foundation the piano roll (phase 3), plugin hosting (phase 4), the node editor (phase 5)
and scripting (phase 6) plug into.

**Goals**

* A project model with clips, notes and per-note expression curves, edited through commands with undo.
* A processing graph per track, compiled on the message thread into an immutable, allocation-free
  structure that the audio thread runs.
* A sequencer that turns clips into sample-accurate note events.
* A built-in MPE synth that reads expression curves per sample, proving the path end to end.
* Everything testable offline with `renderOffline` and the allocation guard.

**Non-goals (later phases)**

* Tempo changes (tempo map), loops and time signatures beyond `beatsPerBar` — the time base must not
  prevent them.
* Plugin nodes, audio clips, recording, multi-threaded graph execution, the voice-scope graph — the
  interfaces must leave room for them.

---

## 2. Time base

**(D1) Model positions are integer ticks, not floating-point beats.** Recommended.

```cpp
using Ticks = juce::int64;
constexpr Ticks ticksPerBeat = 960 * 16; // 15360: divisible by 2, 3, 4, 5, 6, 8, 16 …
```

* Exact arithmetic for quantise, snap, move and compare: no drift, no `-Wfloat-equal` workarounds, and
  ordering is stable. Cubase and Logic use the same approach.
* 15360 PPQ is about 33 µs per tick at 120 BPM, finer than the ear can tell and fine enough for curves.
* `int64` covers more than a million years at any sane tempo.
* The engine converts ticks to samples through a `TimeMap` (constant tempo in phase 2; a tempo map later
  only changes `TimeMap`).

Alternative: `double` beats, as the transport uses today. It's simpler, but every edit operation then needs
epsilon comparisons.

---

## 3. Project model

```
Project
 ├─ tempo, beatsPerBar
 └─ tracks[]
     Track
      ├─ id, name, colour, gainDb
      ├─ clips[]                      (sorted by start)
      │   NoteClip
      │    ├─ id, start: Ticks, length: Ticks
      │    └─ notes: shared_ptr<const NoteList>   (sorted by start, then id)
      └─ graph: TrackGraph            (section 5)
```

### 3.1 Notes

```cpp
struct Note
{
    ObjectId id;
    Ticks start;              // relative to the clip start
    Ticks length;             // > 0
    double pitch;             // base pitch in MIDI semitones, may be fractional (microtonal)
    double velocity;          // 0..1 (note-on strike)
    double releaseVelocity;   // 0..1 (note-off lift)
    Expression expression;    // per-note curves, section 3.2
};
```

* Pitch is a `double`, so microtonal base pitches need no special case. MIDI output rounds it and puts the
  remainder into pitch bend.
* Velocities are normalised `0..1`; conversion to 7-bit or 16-bit happens at output.

### 3.2 Expression curves

Each note has a fixed set of lanes plus room for custom ones:

| Lane | Unit | Range | Meaning |
|---|---|---|---|
| `pitch` | semitones, offset from `Note::pitch` | ±48 | glides, bends, vibrato |
| `pressure` | normalised | 0..1 | MPE "Z" / channel pressure |
| `timbre` | normalised | 0..1 | MPE "Y" / CC74 |

```cpp
struct CurvePoint
{
    Ticks time;         // relative to the note start; may exceed the note length (release phase)
    double value;
    CurveShape shape;   // shape of the segment *leaving* this point
    double tension;     // -4..4, used by CurveShape::power (0 = linear; -1..1 is the usual musical range)
};

enum class CurveShape { linear, hold, power };

struct Curve
{
    std::vector<CurvePoint> points; // sorted by time, unique times
};

struct Expression
{
    Curve pitch, pressure, timbre;
};
```

Rules:

* An empty curve means "use the lane default" (pitch 0, pressure 0, timbre 0.5).
* Before the first point the curve holds the first value; after the last point it holds the last value.
* Point times are note-relative, so moving or transposing a note keeps its expression intact. Stretching
  a note scales point times by default (UI option).
* Points after the note end shape the release tail (a voice keeps reading the curve while it rings out).
* **(D7)** `power` segments use one tension parameter, which covers ease-in and ease-out and is cheap to evaluate.
  The editor allows the full ±4 range so a free handle can pass through any point in its segment's rectangle; the
  usual musical range is ±1. Bezier handles would need two control points per segment and a solver; they're not
  worth it yet.

**(D2) Lane set:** fixed `pitch / pressure / timbre` in phase 2, with custom named lanes added later as a
`std::map<juce::String, Curve>`. Recommended. The alternative, a generic map from day one, makes the common
case slower and the UI vaguer.

**Not in phase 2, but planned for:** note *links* for legato glides between notes
(`Note::glideTo: std::optional<ObjectId>`). The voice-allocation design below already supports handing a
voice from one note to the next.

### 3.3 Sharing and edit cost

Notes live in `shared_ptr<const NoteList>`, so a clip copy or an undo snapshot shares note data. Editing one
note copies the clip's note vector (pointer-sized `shared_ptr<const Note>` entries), which is a few
microseconds for thousands of notes. If profiling ever shows this matters, a persistent vector can replace
it without changing the commands.

### 3.4 Commands

New commands, all pure and undoable like the existing ones:

| Command | Arguments |
|---|---|
| `addClip` / `removeClip` | `{ trackId, start, length }` / `{ id }` |
| `moveClip` / `resizeClip` | `{ id, start }` / `{ id, start, length }` |
| `addNotes` | `{ clipId, notes: [...] }` → returns the new ids |
| `updateNotes` | `{ clipId, notes: [{ id, ...changed fields }] }` |
| `removeNotes` | `{ clipId, ids: [...] }` |
| `setCurve` | `{ clipId, noteId, lane, points: [...] }` |
| `addNode` / `removeNode` | `{ trackId, type, position }` / `{ trackId, nodeId }` |
| `connect` / `disconnect` | `{ trackId, from: { node, port }, to: { node, port } }` |
| `setNodeParam` | `{ trackId, nodeId, param, value }` |
| `batch` | `{ label, commands: [...] }`: several commands as **one** undo step |

Multi-note operations (move a selection, quantise) go through `updateNotes` or `batch`, never one command
per note, so undo stays usable.

### 3.5 Patches for large collections

**(D3) Serialise id-keyed collections as objects, not arrays.** Recommended.

Today arrays whose length changes are replaced whole, so adding one note to a 5000-note clip would send
all 5000 notes to the UI. With `notes: { "17": {...}, "18": {...} }` the existing diff emits a single
`add` op per new note. Order is restored in the UI by sorting on `start`, which it needs to do anyway.
Tracks and clips switch to the same form. The alternative is an id-aware array diff: more code for the same
result.

---

## 4. Engine overview

```
message thread                                         audio thread
──────────────                                         ────────────
Project ──► GraphCompiler ──► CompiledEngine ──publish──► AudioEngine::process()
              │  reuses node instances                     │ for each track (in order):
              │  prepares new ones                         │   sequencer → events
              ▼                                            │   run compiled graph
         NodeRegistry (type → factory)                     │ master sum → device
```

* `CompiledEngine` replaces today's `EngineState`. It is immutable once published and handed over
  through the existing `SnapshotExchange`.
* The compiler runs on every structural change (clips, notes, graph topology) and is cheap. Parameter
  changes do **not** recompile (section 5.5).

---

## 5. Processing graph

### 5.1 Graph in the model

Each track owns a `TrackGraph`: nodes, connections and parameter values. A new track gets the default graph
`Sequencer → MPE Synth → Gain → Track Output`. The phase 5 node editor edits exactly this data.

```cpp
struct GraphNode
{
    ObjectId id;
    juce::String type;                        // registry key, e.g. "dawg.mpeSynth"
    std::map<juce::String, double> params;    // by stable parameter id
    juce::Point<float> position;              // editor layout only
};

struct Connection
{
    ObjectId fromNode; int fromPort;
    ObjectId toNode;   int toPort;
};
```

**(D4) One graph per track plus a fixed master bus** in phase 2. Recommended. Sends, groups and
sidechains come later as a project-level routing graph whose nodes are track graphs. Starting with one
global graph makes the simple case (an instrument with an effect chain) harder to edit and to parallelise.

### 5.2 Node interface (C++)

```cpp
struct ProcessSpec
{
    double sampleRate;
    int maxBlockSize;
};

struct PortInfo
{
    PortType type;   // audio, events (later: modulation)
    int channels;    // for audio ports
    juce::String name;
};

class Node
{
public:
    virtual ~Node() = default;

    [[nodiscard]] virtual std::span<const PortInfo> inputs() const noexcept = 0;
    [[nodiscard]] virtual std::span<const PortInfo> outputs() const noexcept = 0;
    [[nodiscard]] virtual std::span<const ParamInfo> params() const noexcept = 0;

    /** Message thread. May allocate. Called before first use and whenever the spec changes. */
    virtual void prepare (const ProcessSpec& spec) = 0;

    /** Audio thread. Never allocates, locks or throws. */
    virtual void process (ProcessContext& context) noexcept = 0;

    /** Audio thread. Silences tails and drops voices (e.g. after a seek). */
    virtual void reset() noexcept = 0;

    /** Latency in samples, used for delay compensation. */
    [[nodiscard]] virtual int latencySamples() const noexcept { return 0; }

    /** Parameter storage, written by the message thread, read by the audio thread. */
    [[nodiscard]] ParamBlock& paramBlock() noexcept;
};
```

`ProcessContext` gives a node its input and output buffers (`juce::AudioBuffer<float>` views over the
compiled buffer pool), the block's events, and the transport (`TimeMap`, block start in ticks and
samples, playing flag).

### 5.3 Events

```cpp
struct NoteEvent
{
    int sampleOffset;                  // within the block
    NoteEventType type;                // noteOn, noteOff, choke
    juce::uint32 voiceKey;             // unique per sounding note instance
    double pitch;                      // base pitch
    float velocity;
    const CompiledExpression* expression; // owned by the CompiledEngine, never null
    Ticks noteStart;                   // to evaluate note-relative curve time
};
```

Event buffers are fixed-capacity arrays preallocated by the compiler (overflow is dropped and counted,
never allocated).

**(D5) Expression reaches built-in voices as curve data, not as a stream of value events.** Recommended.

Each `noteOn` carries a pointer to the note's compiled curves, and the voice evaluates them itself at
sample (or 32-sample sub-block) resolution from `currentTick - noteStart`. That gives sample-accurate
expression with no event flood and no smoothing artefacts. It's also exactly what the phase 7 voice
graphs need. For plugins (phase 4), a converter node samples the curves at a fixed rate and emits MPE MIDI
(per-note channel, pitch bend, channel pressure, CC74), sending only values that changed.

### 5.4 Compilation

Given the `Project` and the current `ProcessSpec`, the compiler:

1. **Instantiates nodes.** It reuses the existing instance for every `(trackId, nodeId, type)` that
   survives, so synth voices, reverb tails and later plugin state carry over. New nodes are created
   through the `NodeRegistry` and `prepare()`d on the message thread before publishing.
2. **Validates and sorts.** It checks port types and indices, rejects cycles (feedback only through an
   explicit delay node, later), and topologically sorts each track graph. Invalid graphs are refused at
   command time, so a compiled graph is always valid.
3. **Assigns buffers.** Liveness analysis over the sorted order reuses audio buffers whose last reader
   has run. Each track gets its own arena of slots (starting with its own silence and scratch slots), so
   independent tracks can run at the same time without sharing scratch memory.
4. **Compensates latency.** Where paths of different latency merge, it inserts delay lines on the shorter
   ones, and reports the track's total latency for master alignment.
5. **Flattens clips.** For each track it builds a sorted array of note instances with absolute tick
   positions, and converts curves into a flat `CompiledExpression` pool (contiguous points, no per-note
   allocation, ready for binary search).
6. **Emits a `CompiledEngine`.** That's a list of track programs (`[node*, buffer indices]` steps) plus the
   sequencer data. It holds `shared_ptr`s to node instances, so an instance lives as long as any compiled
   engine uses it, and the last release happens on the message thread via `SnapshotExchange`.

A node instance can be referenced by both the old and the new compiled engine for a moment. That is safe
because only the audio thread calls `process()`, and it only runs one engine at a time.

### 5.5 Parameters

**(D8)** Parameter edits from the UI must not recompile (a fader drag sends dozens per second). Each node owns a
`ParamBlock`: one `std::atomic<float>` per parameter, written by the message thread when `setNodeParam`
is applied and read by the node at the start of each block, with the node smoothing as needed. Sample-accurate
automation (phase 8) will add parameter events to `ProcessContext` without changing this path.

### 5.6 Execution

Every track but the master runs on a small fixed worker pool, one block at a time. A track's step program writes
only its own arena, so tracks need no locks between them. Groups, returns, output tracks and keyed tracks read
buses that other tracks fill: the compiler lists, per track, the tracks that write a bus it reads
(`CompiledTrack::fedBy`, always earlier in mixing order) and the tracks it feeds. A track is ready once all of its
`fedBy` tracks have rendered; the thread that takes it first sums their contributions into its buses, in mixing
order, then renders it. Tracks are taken in the order they became ready, so a group starts as soon as its own
members are done, whatever else is still rendering. Once every track has rendered, the audio thread sums what goes
to the master mix, in mixing order too. Each bus therefore adds up its inputs in the same order as running one
track at a time, and the result is identical to the bit. Each worker has its own live-event scratch, and every
worker runs under its own denormal and no-allocation guards.

The pool is created on the message thread when the first engine is published, never on the audio thread. It
wakes with a waitable event per worker and the audio thread helps; the audio thread waits until the last worker
is done, so only one block is ever in flight.

A thread with no track to take helps the tracks still rendering. A node can hand independent pieces of its work to
`ProcessContext::parallel` (`Parallel::forEach`): the calling thread runs pieces itself, and every idle worker and
the waiting audio thread take pieces too, until all are done. The pieces must touch nothing the others touch. A
Voice Instrument renders its playing voices this way, each into its own buffer, and adds the buffers in slot order,
so the result is identical to rendering the voices one after another (docs/design/phase12-voice-scope.md, V9).
`parallel` is set only while tracks render on the pool; the master track and a project with a single track run
everything on the audio thread. A worker waits for a track or for pieces on an atomic wait, the audio thread by
spinning. Parallel rendering is on by default; set the `DAWG_PARALLEL_TRACKS=0` environment variable before
start-up to run the serial path (an escape hatch for plugins that misbehave when their processing thread changes).

---

## 6. Sequencer

* Per block: convert `[blockStartTick, blockEndTick)` through `TimeMap`, advance a cursor through the
  track's sorted note instances, and emit `noteOn` / `noteOff` with exact sample offsets (rounded once,
  from ticks to samples, never accumulated).
* Notes that overlap the play position when playback starts are started mid-note: curve time begins at the
  offset, not at zero. This needs a decision:

**(D6) Start notes mid-way when playback starts inside them.** Recommended. Most DAWs offer this
("chase"); it matters for long pads and slow swells. It can be a per-track option later.

* On stop, seek or recompile with changed note data: send `noteOff` (or `choke` on seek) for all sounding
  voices of the affected track, then continue from the new cursor.
* Every structural recompile keeps the cursor position; the sequencer re-finds its place by binary search.

---

## 7. Built-in MPE synth (`dawg.mpeSynth`)

A deliberately simple instrument whose job is to make expression audible and testable:

* 16 voices, oldest-note stealing.
* Oscillator: sine plus a timbre-controlled waveshaper (timbre 0 = sine, 1 = bright).
* Pitch from `note.pitch + pitchCurve(t)`, evaluated per 32-sample sub-block and smoothed linearly within.
* Amplitude from velocity, with a pressure-controlled gain envelope (pressure 0 is quiet but audible, so
  notes with no pressure curve still sound).
* Release: a short exponential tail that keeps reading the curves (section 3.2).
* Params: `gain`, `attack`, `release`, `pressureAmount`.

The phase 1 metronome becomes a separate optional click node on the master bus.

---

## 8. Threading and safety summary

| What | Thread | Mechanism |
|---|---|---|
| Commands, compile, node creation, `prepare()` | message | plain code, may allocate |
| Publishing a compiled engine | message → audio | `SnapshotExchange<CompiledEngine>` |
| Parameter values | message → audio | `std::atomic<float>` per parameter |
| Telemetry (meters, position, voice count) | audio → message | atomics, polled at 30 Hz |
| Freeing old engines and node instances | message | `SnapshotExchange::collectGarbage()` |

`process()` paths run under `ScopedNoAllocation`, and tests assert zero violations for every node type.

---

## 9. Testing plan

* **Model:** each new command (valid, invalid, clamping, undo/redo, `batch` as one step); curve rules
  (empty lane, hold before and after, release points).
* **Curve evaluation:** linear, hold and power segments at boundaries, against hand-computed values.
* **Compiler:** topological order, cycle rejection, buffer reuse (a chain of N gain nodes needs two
  buffers), latency compensation with a fake latency node, node-instance reuse across recompiles.
* **Sequencer:** a note at beat 1 at 120 BPM / 48 kHz starts at sample 24000 exactly; notes spanning block
  boundaries; chasing; seek mid-note.
* **Synth:** a pitch curve from 0 to +12 st produces the expected frequency at the end (zero-crossing
  count); silence without notes.
* **Real-time:** every test that renders runs under the allocation guard and asserts zero violations.

---

## 10. Implementation order

1. Ticks, `TimeMap`, clips and notes in the model, commands, id-keyed serialisation (D1, D3).
2. Curves and their evaluation (D2).
3. `Node` interface, registry, `ParamBlock`, gain node, track output.
4. Graph compiler and `CompiledEngine`, replacing `EngineState`.
5. Sequencer (D6).
6. MPE synth (D5).
7. Telemetry additions (voice count, per-track meters) and a minimal UI hook to add a test note, so the
   path is audible before the phase 3 piano roll exists.

---

## 11. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| D1 | Positions as integer ticks (15360 PPQ) instead of double beats | Yes |
| D2 | Fixed `pitch / pressure / timbre` lanes now, custom lanes later | Yes |
| D3 | Id-keyed object serialisation for tracks, clips and notes | Yes |
| D4 | One graph per track plus a fixed master bus | Yes |
| D5 | Voices read compiled curves directly; MIDI conversion only for plugins | Yes |
| D6 | Chase notes when playback starts inside them | Yes |
| D7 | Curve segment shapes: `linear`, `hold`, `power` (one tension value), no Bezier | Yes |
| D8 | Parameters as per-node atomics, no recompilation on parameter change | Yes |

---

## 12. Implementation notes (phase 2 as built)

* Node type ids live in `dawg::nodetype` (`engine/include/dawg/NodeRegistry.h`).
* `addClip` also accepts `notes`, so a clip with content is one command and one undo step.
* Latency compensation is implemented inside track graphs (tested with a fake latency node). Aligning
  *tracks* with different total latency at the master bus is not done yet: every built-in node has zero
  latency, so it becomes relevant with plugin hosting (phase 4). Each `CompiledTrack` already reports
  `latencySamples` for it.
* Per-track meters and voice-count telemetry were left out; the UI has nothing to show them in until the
  mixer exists.
* The UI has a "demo clip" button per track (`ui/src/demo.ts`) that adds an MPE phrase using all three
  lanes, so the engine is audible before the phase 3 piano roll. It was removed before release (2026-11-03).
