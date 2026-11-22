# Design: graph modulation

Status: **implemented, recorded after the fact, for review**. Modulation was built without a proposal being approved
first. This document records the decisions as the code implements them, numbered M1–M7, so the code's references
resolve and the owner can review them. The open questions are in section 4.

## 1. Goals

* Low-frequency control of parameters inside a track's graph: an LFO wobbling a filter cutoff, an ADSR ducking a
  plugin's volume from the track's notes or from an audio trigger.
* The same mechanism for built-in nodes and for hosted plugins.
* Patches survive chain edits in the inspector and round-trip through project files.

Non-goals (for now): per-note (polyphonic) modulation, which belongs to the voice-scope phase (phase12-voice-scope.md);
audio-rate parameter modulation; modulating choice and toggle parameters; modulation sources outside the track.

## 2. Decisions

**(M1) A modulation port type.** `PortType::modulation` carries a mono control signal, nominally -1..1, in an
ordinary audio buffer (both channels hold the same values). It connects only modulation outputs to modulation inputs.
The graph view draws it in its own colour with a dotted edge.

**(M2) Every effect and instrument has a pool of modulation inputs.** `withModulationInputs()` appends
`maxModulationInputs` (8) ports after a node's ordinary inputs, for every built-in effect, the synth, Faust nodes and
both plugin types. The pool is fixed so a connection's port index stays stable while slots are used and freed. Each
port is a **slot**, `GraphNode::modulations[port]`: a target parameter id (built-in or plugin), a depth -1..1 and an
active flag. The source is whatever is connected to the port; several sources on one port sum. A slot exists only
while something is connected (`pruneModulationSlots`); `setModulation` with an empty parameter removes it. New slots
start at depth 0, so assigning a target does not change the sound until the depth is raised. The graph view shows the
used slots plus the first free one.

A built-in parameter can be a target unless it is a choice, a toggle or marked not modulatable
(`ParamInfo::modulatable`, `acceptsModulation()`). The gain's dB is such a trim: its Amplitude, 0..200 % around a
default of 100 %, is the one target, so tremolo, ducking and an amp envelope all use it. An envelope that opens a gain
from silence starts at Amplitude 0 % with a depth of +100 %. Format version 10 (`migrate9To10`) moves old slots on
the gain's dB to its Amplitude.

The graph view shows a depth as the amount a full-scale source moves the parameter, in the parameter's units:
"+100%" on the Amplitude, "+2.40 st" on an oscillator's pitch. A plugin parameter's depth shows as a share of its
range.

**(M3) The ADSR.** A modulation source with a note input and an audio trigger input (the trigger is also its key
input, so key sends from other tracks can drive it). Attack, decay and release in ms, sustain in percent. Attack,
decay and release are each a free curve segment like the LFO shape's (CurveShape::free): a cubic Bezier whose
handles point at one tension point, anywhere in the rectangle between the segment's ends (each handle as long as the
way to it, scaled by how far it lies along the line from the handle's node to the rectangle's opposite side:
`freeSegment` in `Curve.h`, since 2026-11-04), stored as two percentages across
it (50 % and 50 % is straight); a retriggered attack rises from the current level along its curve. Its dock panel
draws the envelope with draggable corners and handles, over its knobs; the inspector leaves the handles to the dock,
as it leaves the LFO's shape and zero position. It is
gated while any note is held (or the sustain pedal is down) or while the trigger is above -40 dB; overlapping notes
release only when the last one ends. One envelope per track (see the non-goals).

**(M4) The LFO.** A modulation source with the same two inputs. It runs free at a rate in ms, or tempo-synced with a
cycle picked as a note length: straight, dotted and triplet values from a 64th note to four whole notes. With sync on
and retrigger off it follows the timeline; otherwise note-ons (when retrigger is on) and the audio trigger restart it
at its phase. Its one-cycle shape is a curve over `modulationShapeTicks`, values -1..1 (`setModulationShape`),
edited in its device dock panel with sine, triangle, saw, ramp and square presets. There is one mental model, the
curve: the Sine preset is four free segments each bent as close to a quarter sine as one tension point allows
(`lfoSineShape`, within 0.4 %), and an LFO with no shape plays that same curve rather than a true sine. A zero
position maps the shape onto -1..0, -1..1 or 0..1. The inspector shows the plain parameters; both show only the
rate or the cycle, whichever the sync mode uses. Early builds stored the cycle as `syncBeats`, a number of beats;
format version 8 (`migrate7To8`) turns it into the nearest note length.

**(M5) Applying modulation.** Once per block, just before the target node runs, the engine samples each slot's
source at the middle of the block, applies the target's polarity, multiplies by the depth and adds the result,
scaled by the target's range or, for a very wide one, a smaller span it declares (plugin parameters are normalised 0..1), to a **base** value. Since 2026-11-09 a frequency is modulated in octaves and another skewed parameter along its knob (macros-and-modulation-column.md, MC1 to MC3); linear parameters keep this rule. Slots on one parameter sum
before it is written once. The base is the parameter's unmodulated value: when the parameter differs from what
modulation last wrote (a manual edit or automation), that value becomes the new base, so automation and modulation
combine. The base belongs to the slot and is kept across recompilations, and so is its depth: changing a depth only
stores it in the slot's shared state, without a recompile, so dragging a depth stays cheap (2026-11-15). A slot at depth 0, or inactive, writes the
base; a removed slot hands a plugin parameter back at its base once (built-in parameters are set from the model on
every compilation anyway). A built-in target also gets its modulated value at every sample of the block
(`ProcessContext::modulatedParams`), around the same base. A node can follow that instead of the block-rate value:
the filter does for its cutoff, with 1 ms of smoothing instead of the 20 ms it gives manual and automated changes,
so a ramp LFO's or an envelope's jump stays a sharp attack. The gain (gain and amplitude) and the utility (gain, pan
and width) follow it the same way (`modulationSmoothingSeconds`), so an ADSR on a gain is a sample-accurate amp
envelope.

**(M6) Modulation polarity.** A built-in parameter declares which half of a source it accepts
(`ParamInfo::modulationPolarity`): bipolar, positive or negative. A unipolar target ignores the other half and uses
the depth's magnitude. The Loop Reverb passes its parameters' own declarations through; the gain's Amplitude, which
once accepted only the negative half, is bipolar since format 10, so it can rise to 200 %. Plugin parameters are
always bipolar.

Note Expression (phase12-voice-scope.md, V2) is a third source: a note's pitch, pressure, timbre, velocity and
gate as five outputs.

**(M7) Sources are extras in the chain view.** The inspector's chain ignores modulation sources and modulation edges:
a track with an LFO still shows a simple chain, and rewiring the chain (insert, reorder, remove) keeps modulation
connections and slots. Sources are added in the graph view, not as chain inserts; a custom graph lists them in their
own inspector section.

## 3. Limits

* Block rate: apart from the parameters that read the per-sample values (the filter's cutoff, the gain, and the
  utility's gain, pan and width), the applied value steps once per block, so a fast LFO on a sensitive parameter can
  zipper at large buffer sizes, and a jump in the source lands up to a block early or late and is then smoothed.
  Built-in nodes smooth most parameters; plugins do their own smoothing or not.
* One LFO or ADSR state per node, not per note, until the voice scope (phase12-voice-scope.md) runs them per voice.
* A tempo-synced LFO without retrigger switches to its free-running phase while an audio trigger is held, then back to
  the timeline, which can jump.

## 4. Questions for the owner

1. Are the decisions above the intended behaviour, in particular depth scaled by the whole parameter range (M5) and
   the per-parameter polarity (M6)?
2. Should modulation be smoothed or run at a finer rate than per block for built-in nodes?
3. ADSR sustain changed from 0..1 to 0..100 % to match the other percent parameters; ADSR sustain values saved by
   earlier development builds now read as a hundredth of what they were.
