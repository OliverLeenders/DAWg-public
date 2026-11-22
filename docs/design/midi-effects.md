# Design: MIDI effects and swing

Status: **implemented** (2026-11-09). The owner answered section 9 on 2026-11-09 (section 10); section 11 has the notes on
what was built and where it differs from the plan. The decisions are numbered ME1–ME19.

From the gap list's "MIDI and composing tools", the owner chose the MIDI effects next, and asked for a selectable swing
percentage for quantise. Scale highlighting in the piano roll and the step sequencer are not in scope, but scale lock's
scale model (ME12) is written so that the piano roll can use it later.

## 1. Goals and non-goals

Goals:

1. Four MIDI effects: an **arpeggiator**, a **chord** tool (one note in, a chord out), **scale lock** (notes moved
   to, or filtered by, a scale and root) and **note repeat**.
2. **Swing** in quantise: 50 % straight, about 66 % a triplet feel, up to 75 %, on the quantise grid. The arpeggiator
   and note repeat share the same math.
3. They play with everything a DAWg note has: per-note expression curves, vibrato, links and glides, live MIDI input
   with MPE, CLAP note ids, the tempo map.
4. **Render MIDI effects to clip**, so their output can be edited as notes.

Non-goals for v1:

* **MIDI effect plugins** (VST3 or CLAP note effects). The node kind leaves room for them.
* **Scale highlighting** in the piano roll, a project key, and the **step sequencer**.
* **Groove templates** taken from audio or MIDI. Swing is one number.
* MIDI effects in **voice graphs**: a voice plays one note, and an arpeggio or a chord is made of many (ME5).
* A **MIDI output** to hardware.

## 2. How notes flow today

* The **Sequencer** node (`BuiltinNodes.cpp`) turns the track's `TrackSequence` into `NoteEvent`s in the block's
  `EventBuffer`, sample accurate through `Transport::timeMap`. It also forwards the track's live notes
  (`ProcessContext::liveEvents`: audition and MIDI input).
* A `noteOn` carries a `const CompiledNote*`. A note's curves are **relative to its start** and evaluated at an
  absolute tick (`CompiledNote::evaluate`, `pitchOffset`). Instruments never read a note's `end`: note-offs end
  notes.
* Instruments (`VoiceAllocator` for the MPE Synth, Sampler, Note Expression and the Voice Instrument; `MpeEncoder` and
  `ClapNoteEncoder` for plugins) keep that pointer for as long as a voice sounds, and **re-resolve it by id** when the
  sequence changes (`adoptSequence`). Notes with **negative ids** (live notes) are skipped there: their storage is
  stable and owned elsewhere.
* Linked notes arrive as `legato` events (`fromNoteId`). Live MPE expression arrives as `expression` events by note id.
* The graph already lets a node take an event input and give an event output, and an event output can feed several
  inputs (`GraphCompiler::planOutputs`, `planInput`). Nothing uses that yet except Sequencer → instrument.
* Quantise is the piano roll's Q (`useRollEditing.ts`): note starts snap to the nearest grid line through
  `TempoMap.snap` (bar-relative), and Shift+Q snaps lengths too. It has no strength or swing, and no dialog.
* Live notes are recorded from the MIDI input, not from the graph (`MidiInput.cpp`), so what is recorded is what was
  played.

## 3. Decisions: where MIDI effects live

**(ME1) Nodes in the track graph, a new category.** `NodeCategory::midiEffect`: one event input (Notes) and one event
output (Notes), plus modulation inputs for their continuous parameters. A separate per-track MIDI chain was rejected:
the graph already carries events, it would be a second router with its own save format, commands and UI, and a node
gives fan-out (one arpeggio to two instruments), components and modulation for free. On a simple chain they sit
between the Sequencer and the instrument, in order, so most users never see a cable.

Types: `dawg.arpeggiator` (Arpeggiator), `dawg.chord` (Chord), `dawg.scaleLock` (Scale Lock), `dawg.noteRepeat`
(Note Repeat).

**(ME2) The chain knows them.** `Chain` (Commands.cpp) gains `midiEffects`, the nodes between the Sequencer and the
instrument; `walkBackToSource` keeps walking past the instrument while the input is a MIDI effect. `insertNode` on a
simple chain puts a MIDI effect after the last MIDI effect (or at a given index among them), and `moveNode` reorders
them among themselves. A track without an instrument can hold MIDI effects; whatever instrument is chosen later goes
after them. The instrument and audio effects never move into the MIDI section, nor MIDI effects into the audio one.

**(ME3) The device strip.** MIDI effect cards come before the instrument, after a small divider labelled MIDI, like the
Modulation section's. Their gap has its own + that opens the effect picker filtered to MIDI effects; the strip's
Add effect slot stays for audio effects. Dragging reorders within the MIDI section. The inspector and the mixer's
insert list show them as a group above the instrument.

**(ME4) Derived notes: owned by the effect, with negative ids.** An effect that makes notes (arpeggiator, chord, note
repeat, and scale lock when it moves a note) sends `noteOn`s whose `CompiledNote` lives in a fixed pool inside the
effect node, so the pointer stays valid across sequence changes exactly as a live note's does. Ids are negative, so
every instrument's `adoptSequence` leaves them alone, and they never clash with live note ids or another effect's:
`derivedNoteId (graphNodeId, counter)` = −(2^52 + graphNodeId · 2^20 + counter mod 2^20). Live ids count down from −1
and never get near 2^52. Chained effects work unchanged: the second sees the first's derived notes as its input.

A derived note copies its source's `start`, curves (`lanes`, the same spans), vibrato and velocities, so it evaluates
the **source's** curves at any absolute tick. Only `id`, `pitch` and, for the arpeggiator and note repeat, `velocity`
differ. The effect re-resolves its sources when `ProcessContext::sequence` changes (before it emits anything in that
block) and rewrites its derived notes' spans in place; a source that is gone ends its derived notes with a `choke`.
Since the effect is earlier in the step order than its instrument, the instrument only reads a derived note after the
effect updated it in that block.

**(ME5) Not in voice graphs.** `allowedInVoiceGraph` refuses the four types. Allowed in components, which run at track
level.

**(ME6) Hung notes, removal and rewiring.** Every effect follows these rules, each covered by a test:

* `releaseAll` (transport stopped) and `chokeAll` (seek) end every derived note and clear the held set, latch included
  (ME9), then pass on.
* `noteOff` and `choke` of a source end its derived notes (a `choke` chokes them).
* **Bypass** is read in `process()` (from `configure()`, an atomic). Turning it on sends `noteOff` for every derived
  note and a `noteOn` for each source still held, so a held chord keeps sounding, then passes events through. Turning
  it off ends those pass-through notes and starts the effect on what is held.
* **Removal and rewiring.** When an instrument's event source is a different port than in the previous compile (an
  effect inserted, removed or moved before it, or a cable changed), the compiler marks that step, and the engine sends
  it a new `sourceChanged` event at offset 0 of its first block: voices choke and **drop their note pointers** (keeping
  their last expression, as for a note deleted from the sequence), and plugin encoders end their notes. This also fixes
  notes that hang today when the Sequencer's cable is moved. The old effect node is freed only after the audio thread
  runs the new engine, so no pointer is read after it is gone.
* Every pool is fixed size and full pools drop new notes instead of allocating: the arpeggiator and note repeat hold up
  to 32 sources, the chord tool 128 sources with up to 6 notes each, and each effect at most 256 sounding derived notes.

## 4. Decisions: timing

**(ME7) Tempo sync through the tempo map.** The arpeggiator's and note repeat's steps are tick positions on the
timeline: multiples of the step length counted from the bar start of the bar they are in (`TimeSignatureMap`), so a
1/16 arpeggio stays on the grid across tempo ramps and signature changes. Each step's sample offset comes from
`Transport::timeMap` (`sampleOffsetOf`), so timing is sample accurate and independent of the block size. A step that
falls in a loop wrap's second piece plays there.

**(ME8) While the transport is stopped** (playing live), ticks don't move, so an effect runs its own clock: it starts at
the first held note, at the tempo at the playhead, and its steps are counted from that note. When the transport
starts, it switches to the timeline's grid at the next step.

**(ME9) Swing: one function, shared.** `swungTick (gridIndex, gridTicks, swing)`: in each pair of grid cells counted from
the bar start, the second cell's line moves from 50 % of the pair to `swing` of it, so 50 % is straight, 66.7 % is a
triplet feel and 75 % puts it on the pair's last sixteenth (for an 8th grid). The C++ version (`dawg/Swing.h`) serves the
arpeggiator and note repeat; the TypeScript version (`ui/src/time/swing.ts`) serves quantise. Both are tested against
the same table. Swing is off (shown disabled) for triplet grids, where a pair of cells doesn't make a beat.

## 5. Decisions: the four effects

**(ME10) Arpeggiator.**

| Parameter | Range | Default | |
|---|---|---|---|
| Rate | 1/1 to 1/32, with dotted and triplet | 1/16 | As the LFO's note values |
| Mode | Up, Down, Up-Down, Down-Up, Up & Down (ends twice), As played, Random, Chord | Up | Chord plays all held notes each step |
| Octaves | 1 to 4 | 1 | Each pass repeats an octave higher |
| Gate | 5 to 200 % | 50 % | Of the step; over 100 % overlaps the next note |
| Swing | 50 to 75 % | 50 % | ME9 |
| Velocity | From note, Fixed | From note | |
| Fixed velocity | 1 to 127 | 100 | With Velocity Fixed |
| Latch | toggle | off | |
| Retrigger | Off, On new note | On new note | Whether a new note restarts the pattern |

* **Expression:** an arpeggiated note is a derived note of the held note it plays (ME4), so it follows that held
  note's curves **as they are when it plays**: a bend drawn on the held note bends every step that falls in it, and
  pressure swells across the steps. Live MPE expression of a held note goes to the step that sounds from it.
* **Links:** a `legato` into the arpeggiator is a release of the old note and a hold of the new one; arpeggiated notes
  are never linked to each other (no gliding arpeggios in v1).
* **Latch:** releasing every key keeps the last held set playing; the next key pressed after all were up starts a new
  set. Transport stop, seek and bypass clear it (ME6).
* **Order** of "As played" is note-on order; ties at one offset by pitch. Random is a seeded generator, reseeded at
  transport start, so a played-back part sounds the same every time.

**(ME11) Chord.**

| Parameter | Range | Default | |
|---|---|---|---|
| Shape | Major, Minor, 5, sus2, sus4, 6, m6, 7, maj7, m7, m7♭5, dim, dim7, aug, add9, Octave, Custom | Major | |
| Note 2 to Note 6 | −24 to +24 semitones, or Off | per shape | Editable with Custom; showing the shape otherwise |
| Inversion | −3 to +3 | 0 | Moves the lowest notes up an octave (or the highest down) |
| Strum | 0 to 200 ms | 0 | Time between chord notes |
| Strum order | Up, Down | Up | |
| Velocity | 0 to 100 % | 100 % | The added notes' velocity relative to the played note |

* **Expression:** every chord note shares the source note's curves and vibrato (ME4): a bend bends the whole chord,
  pressure moves all of it. Live MPE expression goes to every chord note. The played note is replaced by a derived
  note too (the first chord note, interval 0), so all six are treated alike.
* **Links:** a `legato` from note A to B becomes a `legato` from chord note *i* of A to chord note *i* of B, so a
  linked line of chords glides voice by voice. With a strum, the legato chord notes don't strum.

**(ME12) Scale lock, and the scale model.**

| Parameter | Range | Default | |
|---|---|---|---|
| Root | C to B | C | |
| Scale | Major, Natural minor, Harmonic minor, Melodic minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Major pentatonic, Minor pentatonic, Blues, Whole tone, Diminished (half-whole), Chromatic | Major | |
| Mode | Nearest, Up, Down, Filter | Nearest | Filter drops notes out of the scale |

* **The scale model:** `dawg/Scales.h` and `ui/src/music/scales.ts` hold one table of scales as 12-bit pitch-class
  masks with names, plus `inScale`, `nearestInScale (pitch, root, scale, direction)` and degree helpers. The piano
  roll's highlighting can later use the same table; a project key, if it comes then, can feed a "Project" root
  choice here.
* **Expression:** scale lock moves the **base pitch** only. The pitch curve is relative to the base, so a drawn bend
  keeps its shape, starting from the locked pitch. A fractional base (a microtonal note) keeps its fraction: the
  integer part is locked and the detune is added back.
* A note already in the scale passes through unchanged, with its own id (no derived note). A moved note becomes a
  derived note. Ties in Nearest go down.
* **Links** pass through with the ids mapped. A filtered note that is linked drops its link: the next note starts
  its own voice.

**(ME13) Note repeat.**

| Parameter | Range | Default | |
|---|---|---|---|
| Rate | 1/4 to 1/64, with dotted and triplet | 1/16 | |
| Gate | 5 to 100 % | 50 % | |
| Swing | 50 to 75 % | 50 % | ME9 |
| Velocity | From note, Fixed | From note | |
| Fixed velocity | 1 to 127 | 100 | |

Each held note repeats on its own (unlike the arpeggiator, which plays one at a time): the first hit at the note's
start, the next ones on the rate's grid (ME7). Expression is inherited as for the arpeggiator. Note repeat is the
pad-drumming tool, so it takes held live notes like any other; it has no latch.

**(ME14) Modulation, automation and MIDI learn.** The parameters are ordinary node parameters, so automation lanes,
MIDI learn and the modulation pool reach them with nothing new. Continuous and integer ones (Gate, Swing, velocities,
Strum, Octaves, Note 2–6) are modulatable; choice and toggle ones (Rate, Mode, Shape, Scale, Root, Latch) can be
automated but not modulated, as everywhere (`ParamInfo::acceptsModulation`). A change of Shape, Scale, Root or Mode
takes effect at the next note (held chords don't re-voice).

## 6. Decisions: editing, recording and rendering

**(ME15) The piano roll shows the input notes.** The clip holds what was written or played; the effects run on
playback. The roll's header shows a small "MIDI effects: Arpeggiator, Chord" note for a track that has any, so a
listener isn't surprised that the clip doesn't look like what plays.

**(ME16) Recording records the input notes.** Recording already takes notes from the MIDI input, not the graph
(section 2), and keeping it so means the recorded take plays back the same through the same effects. Monitoring goes
through the effects (live notes enter at the Sequencer).

**(ME17) Render MIDI effects to clip.** A clip context-menu and piano-roll command (`renderMidiEffects`): the engine runs
the track's Sequencer and MIDI effects offline over the clip's range (from a little before, to catch notes that
started earlier; up to the clip end, cutting notes there), captures their output, and replaces the clip's notes with
it. Each rendered note gets its source's curves shifted to its own start and cut to its length, vibrato included;
links from the chord tool stay links. The track's MIDI effects are then bypassed. One undo step. Latch and random
patterns render as they would play from the clip start.

**(ME18) Quantise with swing.** The piano roll's toolbar gets a Swing control beside the grid selector (a drag number,
50–75 %, double-click resets to 50 %), disabled for triplet grids and Off. Q uses it: each start goes to the nearest
**swung** grid line (`swungTick`), and with Shift each end too. It is the user's setting, remembered like the grid,
not part of the project. A **Strength** control (0–100 %, default 100 %) sits beside it: each note moves that share of
the way to its swung grid line, so 100 % is today's Q and 50 % halves every note's distance from the grid.

## 7. Panels (ui/src/dock/)

**(ME19) One panel per effect,** like the other device panels, with presets (`dock/presets.ts`).

* **Arpeggiator:** Rate, Mode, Octaves, Gate, Swing, Velocity, Latch, Retrigger; a strip showing the held notes in
  pattern order with the current step lit (from `meters()`).
* **Chord:** Shape and a 25-key keyboard showing the chord's notes from the root, clickable to toggle intervals
  (switches to Custom); Inversion, Strum, order, Velocity.
* **Scale Lock:** Root and Scale, and a 12-key keyboard of the scale (read-only); Mode.
* **Note Repeat:** Rate, Gate, Swing, Velocity.

The keyboards and the arpeggio strip count as graphs: controls never overlap them at any dock height or card width
(`panelMinWidth.ts`). The MIDI effect cards are narrower than audio ones; their minimum widths come from the check in
step 7.

## 8. Implementation order (one commit each)

1. **Swing math and quantise.** `dawg/Swing.h` and `ui/src/time/swing.ts` with matching tests; the roll's Swing control
   and Q with swing.
2. **The node kind and the plumbing.** `NodeCategory::midiEffect`, derived notes (ME4), `sourceChanged` (ME6) in the
   compiler, engine, `VoiceAllocator` and both encoders; `Chain::midiEffects`, `insertNode` and `moveNode` (ME2); the
   voice-graph refusal (ME5). Tests with a pass-through test effect: removal and rewiring mid-note, sequence edits.
3. **Scale lock** and the scale model (C++ and TS tables).
4. **Chord**, with links and strum.
5. **Arpeggiator**, with latch and the stopped-transport clock.
6. **Note repeat.**
7. **Panels and the device strip's MIDI section,** checked in a separate dev instance at dock heights 140–520 and card
   widths from the minimum up.
8. **Render MIDI effects to clip**, the roll's header note, the file format version and `docs/PLAN.md`.

Tests for every effect (tests/MidiEffectTests.cpp): sample-accurate step times at a constant tempo and across a ramp
and a signature change; the same output at block sizes 1, 17, 64 and 512; no allocation in `process()` under the Debug
allocation guard; no sounding note left after transport stop, seek, bypass, removal and a sequence edit that deletes
the source; expression inheritance (a pitch curve on the source shows in a derived note's `pitchOffset`); links
through chord and scale lock.

## 9. Open questions for the owner

1. **Placement (ME1, ME2):** MIDI effects as graph nodes, shown as a MIDI section before the instrument on a simple
   chain? Recommended. The alternative is a separate MIDI chain per track.
2. **Expression (ME10–ME12):** arpeggiated and repeated notes follow the held note's curves at the time they play;
   chord notes share the source's curves; scale lock moves only the base pitch and keeps a bend's shape? Recommended.
3. **Quantise strength:** add a Strength (0–100 %) beside Swing now? Recommended: yes, it is a few lines in the same
   place and the usual partner of swing. Without it, only swing.
4. **Where swing lives (ME18):** the user's setting beside the grid, not saved in the project? Recommended. The
   alternative is a per-clip groove saved with the project.
5. **Arpeggiator scope (ME10):** the modes and parameters above, without step patterns (per-step velocity, rests, ties)
   in v1? Recommended; a pattern lane could come with the step sequencer.
6. **Chord (ME11):** chromatic shapes and custom intervals only, or also diatonic chords that follow Scale Lock's
   root and scale (a "Diatonic" shape building triads or sevenths from scale degrees)? Recommended: chromatic and
   custom in v1, diatonic later with the project key.
7. **Render (ME17):** replace the clip's notes and bypass the track's MIDI effects, one undo step? Recommended. The
   alternative is a new clip on a new track, leaving the original alone.
8. **Stopped transport (ME8):** effects run on their own clock while you play live with the transport stopped?
   Recommended. The alternative is that the arpeggiator and note repeat are silent unless the transport runs.

## 10. Owner's answers (2026-11-09)

1. Placement: graph nodes, a MIDI section before the instrument on a simple chain.
2. Expression and scope: as recommended (ME8, ME10–ME12): derived notes follow their source's curves, scale lock moves
   the base pitch only, no step patterns, chromatic and custom chord shapes only, an own clock while stopped.
3. Quantise: Swing and Strength, both beside the grid.
4. Swing lives with the user, beside the grid, not in the project.
5. Render: replace the clip's notes and bypass the track's MIDI effects, one undo step.

## 11. Implementation notes

* **Where it is.** `engine/src/MidiEffects.cpp` holds a shared base (`MidiEffectNode`: the notes coming in, the notes
  made, bypass, sequence edits, the event types) and the four effects; the arpeggiator and note repeat share a step
  clock (`SteppedEffect`). `dawg/MidiEffects.h` exposes the pieces the tests check (`arpPick`, `invert`, `nextGridLine`,
  `derivedNoteId`, `renderMidiEffects`); `dawg/Scales.h` and `dawg/Swing.h` the scale table and the swing line. The UI
  has copies in `ui/src/music/scales.ts`, `ui/src/music/chords.ts` and `ui/src/time/swing.ts`, each tested against the
  engine's table.
* **Derived notes (ME4)** are copies with a `CompiledNote::source` field naming their root sequence note, so a chain of
  effects finds every copy again after a sequence edit, not only the first effect's. Slots are handed out round-robin
  from 1024, so a slot that ended keeps its values while an instrument plays its release.
* **Source changes (ME6)** work by node serials rather than by comparing compiles: every `Node` has a process-unique
  `serial()`, each compiled step knows the serial of the node writing its event input, and `Node::takeEventSource`
  reports a change, on which the engine sends `NoteEventType::sourceChanged` at offset 0. Voices choke and drop their
  note pointers; the MPE and CLAP encoders end every note; MIDI effects forget everything and pass it on. A serial
  can't be reused by a new node at the same address, which a pointer could.
* **Transport stop and seeks** end the notes made from sequence notes, latched ones included; notes made from live
  notes play on, as live notes do. The stepped effects end what they are sounding at any transport change and start
  their grid again.
* **Strum** is in milliseconds only; the synced variant was left out.
* **Render (ME17)** plays only the clip's own notes, from the clip start, through a fresh copy of each effect at 48 kHz,
  and replaces the notes, so a looped clip comes back unlooped with its offset at 0. Links the Chord makes stay links,
  with their glide times. The command is `renderMidiEffects` (one undo step); the clip menu and the piano roll's
  "MIDI effects" note in its toolbar offer it.
* **The panels** are in `ui/src/dock/MidiEffectPanels.tsx`: the Arpeggiator shows the held keys in pattern order with
  the playing one lit (its `meters()`), the Chord a three-octave keyboard to click notes in or out (switching to Custom
  in one undo step, the inversion folded in), Scale Lock a one-octave keyboard of the scale. The device strip has a MIDI
  section before the instrument with its own + gaps and drag order; the picker, the device menu's Replace and browser
  drops know the MIDI effect role.
* **Commits:** the engine plumbing and the four effects went in as one commit, since the effects share the base
  class the plumbing was written for; the swing, the UI and render are their own.
* No project file format change: MIDI effects are ordinary nodes, and render writes ordinary notes.
