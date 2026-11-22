# Design: macros, octave modulation and the modulation column

Status: **accepted, being built**. The owner took the decisions in section 2 before building started (2026-11-09).
It changes modulation.md (M5, depth scaling) and device-strip.md (D3, what the strip shows), which point here.

## 1. What this is

Three things that belong together:

* **Octave modulation.** A depth on a frequency means octaves, not hertz, so an LFO on a cutoff sounds the same at
  200 Hz and at 8 kHz. Other skewed parameters move along their knob.
* **Macro controls**, from the owner's 1.0 list: knobs that each drive several parameters at once, assignable to a
  MIDI CC or by MIDI learn like any parameter, and automatable.
* **A new dock layout**, as the device strip got crowded: a fixed modulation column on the dock's left holds the macros
  and the track's modulation sources, and instruments move to the inspector and a window of their own.

```
 ┌ Modulation ──┬─ strip (MIDI effects, audio effects) ─────────────────┬ fader
 │ ◯ ◯ ◯ ◯      │ Arp │ EQ ~curve~ │ Saturator │ Comp │ + Add effect     │ ▐
 │ ◯ ◯ ◯ ◯      │     │            │           │      │                  │ ▐
 │ [LFO 1][ADSR]│                                                         │ ▐
 │ [Macros] [+] │                                                         │ ▐
 │ ┌ LFO 1 ───┐ │                                                         │ ▐
 │ │ editor   │ │                                                         │ ▐
 └──────────────┴─────────────────────────────────────────────────────────┴──
```

## 2. Decisions

### Octave modulation

**(MC1) A frequency is modulated in octaves.** A parameter whose unit is "Hz" and whose range starts above 0 is
modulated around its base by a factor: `value = base × 2^(amount × octaves)`, where `amount` is the summed source ×
depth (M5) and `octaves` is the parameter's declared `modulationSpan` or, without one, its range's octave span
(log2 max/min: 10 octaves for 20 Hz to 20 kHz). So 5 % depth moves a cutoff by half an octave whether it sits at
200 Hz or at 8 kHz. The result is clamped to the range as before. Filter cutoffs, EQ bands, the sidechain filters,
the modulation effects' rates and the Bit Crusher's rate all follow, as the rule reads the unit, not a list.

**(MC2) Other skewed parameters move along their knob.** A parameter with a `midValue` (its control is skewed) and no
declared span is modulated in its knob's position: `value = fromKnob(toKnob(base) + amount)`, so a depth of 50 %
moves it half the knob's travel, and a release time reacts as the knob looks. A skewed parameter that declares a
span, such as the Feedback node's Time (small moves in milliseconds for chorus and flanging), keeps the linear rule
with that span, as the span was declared for that. Linear parameters are unchanged.

**(MC3) One function applies it everywhere.** `ParamInfo::modulationScale()` says which rule a parameter uses and
`modulatedParamValue()` (Node.h) applies it. The block-rate write and the per-sample values
(`ProcessContext::modulatedParams`) use the same function around the same base, so they agree, and a voice graph runs
the same steps. Plugin parameters are normalised 0..1 and stay linear.

**(MC4) The graph view and the dock speak the same units.** The registry sends each parameter's scale with its span.
A depth on a frequency shows as octaves ("+1.5 oct"), on a skewed parameter as knob travel ("+25 %"), and on a linear
one in its unit as before.

### Macros

**(MC5) A Macros device.** `dawg.macros` is a modulation source with eight knobs, `macro1`..`macro8` (0..100 %), each
with a neutral position, `neutral1`..`neutral8`: Bottom (the default), Centre or Top. Each knob has a name
(`GraphNode::macroNames`; empty shows "Macro N"), set with `renameMacro`. It has eight modulation outputs, one per
knob, and no inputs. A track has at most one, in its own graph (not in a component or voice graph): the macros
belong to the track.

**(MC6) What a knob sends.** Knob value `v` (0..1) gives the signal `v` with Bottom neutral, `2v − 1` with Centre and
`v − 1` with Top, so at its neutral position a knob leaves its targets at their own values, and turned all the way
from it sends ±1. The signal ramps from the last block's value across each block, so a knob turned by a MIDI
controller in steps of 1/127 doesn't zipper a cutoff that follows per-sample modulation.

**(MC7) A mapping is a modulation slot that names its knob.** `ModulationSlot::macro` (0..7, or −1 for a slot fed by
cables) lets a slot take its source from the track's Macros device without a cable. Everything else about a slot
stays: its target, its depth around the parameter's own value (M5, so automation and manual edits combine, and the
scaling of MC1 and MC2 applies), its polarity and the pool of eight per node. A macro slot stays while nothing is
connected to its port (`pruneModulationSlots` keeps it). This is why a mapping isn't a cable: it reaches parameters
inside components and the Voice Instrument's voice graph, which a cable from the track's graph can't.

**(MC8) How the compiler feeds a macro slot.** In a track, after components are flattened (component-nodes.md, N4),
each macro slot becomes a connection from the Macros device's output to the slot's port, so the planner orders and
buffers it like any source. In a voice graph, the compiler adds a follower of the Macros device to each voice slot's
program, wired the same way: an instance per voice that reads the track instance's knob values (and so their
automation and MIDI), with its own ramp. Each voice follows the macro. A macro slot on a track without a Macros
device does nothing.

**(MC9) Mapping.** `mapMacro { trackId, nodeId, param, macro, depth }` maps a parameter of any node on the track, at
any depth of components and voice graphs, to a knob: it adds the Macros device if the track has none, reuses the
node's slot that already maps that parameter to that knob, or takes a free modulation port (no cable, no slot), and
fails when all eight are used. Removing a mapping is `setModulation` with an empty parameter, as for any slot.
Two ways in from the UI:

* Right-click any parameter or panel knob: *Map to Macro ▸ 1–8*, at a depth of 50 %.
* *Map* on a macro knob: while it's on, a parameter that is moved isn't changed but mapped, at the depth it was moved
  (the move in the parameter's modulation units, MC1 and MC2, signed so turning the macro away from its neutral moves
  the parameter the way it was moved). Map ends with Escape, a click on Map again or another knob's Map.

**(MC10) MIDI and automation.** Macro knobs are ordinary node parameters, so MIDI learn, `midiMappings` and automation
lanes work on them unchanged. The macro editor also has a CC number field per knob, which writes a MIDI mapping
(any device, channel 1) with `setMidiMapping`, or forgets it when cleared.

**(MC11) Where the knobs show.** In the dock's modulation column (MC12), in the inspector when the track has a Macros
device, and on the mixer strip under the inserts when it has one. The column's Macros editor lists each knob's name,
neutral position and CC, and its targets with their depths, each removable.

### The dock

**(MC12) A modulation column on the dock's left.** A fixed column, 248 px, before the strip: the eight macro knobs at
the top (or a button to add the Macros device), then a row of chips, one per modulation source in the track's graph
("LFO 1", "ADSR", "Expr", "Macros"), and "+" to add an LFO, ADSR or Note Expression. Clicking a chip opens that
source's editor under the chips, one at a time: the LFO and ADSR panels, laid out for the column's width, Note
Expression's outputs, and the macros' names, neutral positions, CC fields and targets. Sources are no longer cards in
the strip. The column shows the track's own sources even while the strip is inside a component or voice graph (D4),
as those levels' sources are wired in the graph view.

**(MC13) Linking.** A modulated knob draws a thin ring outside its arc over the range its sources swing it (for a
macro, also where it is now). Hovering a chip marks every parameter that source drives, wherever they are shown.
Dragging a chip onto a knob maps it there: a macro chip maps that knob at 50 %; an LFO, ADSR or Expression chip
connects the source to a free modulation port of the knob's node and targets the parameter at 50 %, when the node is in
the same graph as the source. Note Expression's editor has a chip per output, and each macro knob a grip that drags
like a chip.

**(MC14) Instruments leave the strip.** The strip shows MIDI effects and audio effects; on a custom graph it leaves
out instruments too. The inspector shows the instrument: its name, preset selector, main controls (its knobs, a
plugin's Open button, a Voice Instrument's voice count) and an *Edit* button. The full editor (Sampler zones, Synth
panel, a Voice Instrument's or component's graph, a plugin's own window for plugins) opens in a floating window, a
second web view on the same bridge with `?window=instrument&track=<id>&node=<id>`, like the MIDI and audio editors'
windows: one window, which opening another instrument retargets, its place remembered. A Sampler or Synth shows its
whole panel there; a Voice Instrument or component its graph view, with the strip of what it holds underneath. A small keys icon in the track header opens it too. An instrument
dragged from the browser onto the strip still goes to the instrument slot and adds no card.

**(MC15) Collapse all except selected.** The strip's right-click menu has it: every card but the selected one is
collapsed (D5).

**(MC16) Alt-drag sets a modulation's depth (2026-11-12).** Dragging a modulated knob with Alt held changes the depth of
its modulation instead of its value, as Serum does: 160 px for a knob's whole travel, Shift ten times finer. With
several modulations on the knob it is the one whose source is open in the modulation column, else the newest. The
knob's tooltip says so; the ring follows the depth as it changes.

## 3. Limits

* A macro moves its targets by modulation, so an automation lane on a target still shows the target's own value, not
  the macro's effect, as for an LFO.
* The Macros device's knobs can't themselves be modulated (it has no modulation inputs): a macro of macros is out.
* Map mode maps controls that go through the common parameter hook (knobs, sliders, panel controls); a drag on an EQ
  curve or a filter graph moves the parameter as usual.
* Chip drags (MC13) wire a cable source only within one graph; deeper targets take a source of their own level.

## 4. Notes from building it

* **Tests that leaned on linear hertz.** The ramp-down LFO test on a filter (ModulationTests) swept 200 Hz to 5195 Hz at a
  depth of 0.25, a quarter of 20 to 20 000 Hz; in octaves the same sweep is a depth of log2(5195/200)/log2(1000), 0.47.
  Projects saved with depths on frequencies now sound different: there are only test projects, so there is no migration.
* **The per-sample and block-rate values share one function** (`modulatedParamValue` in Node.cpp), so the test drives
  `runSteps` with a recording node and a rising source and checks both against it, rather than listening to a filter.
* **A macro slot is a cable the compiler adds.** `wireMacros` runs after `flattenComponents`, so a slot inside a
  component needs nothing more. In a voice graph the compiler copies the Macros device into the flat graph and makes
  each voice slot's instance follow the track's (`followMacros`): the follower keeps the leader alive and, should the
  leader ever change, keeps the old one too, as the audio thread may still read it.
* **Removing the Macros device** drops every macro slot of the track (`pruneMacroSlots` after each graph edit), so no
  slot is left holding a port with nothing to feed it.
* **Map mode lives in the two edit hooks**: `useParam` (knobs, sliders, choice slots) and `useNodeDrag` (panel graphs
  such as the filter's handle, which moves cutoff and resonance together; both are mapped when both moved more than
  half a percent). A parameter at its default has no value in the model, so a move's start falls back to the default.
* **Right-click on a knob's dial** as well as its name: the dial is wrapped in `MidiLearnable` with `display: contents`,
  so the menu (MIDI learn, automation, Map to Macro) is on both without changing the layout. The filter's readouts
  (cutoff, resonance) are wrapped the same way, as its graph has no knob for them.
* **The column scrolls, its parts don't shrink.** At the dock's default 220 px the macros take about half the height and
  the LFO's editor scrolls under them; as flex items the editor's parts first squeezed over each other.
* **The instrument window keeps its track.** It doesn't take part in the selection relay, so selecting another track in
  the main window doesn't swap its graph. An instrument with no panel (the MPE Synth) shows its knobs, as its card did.
* **The mixer** gives every strip a macros section once any track has macros (empty on the others), so the faders stay
  level.
* **Checked in a dev instance** (2026-11-09): the column with an LFO and macros; mapping the filter's cutoff by right-click
  and by Map mode; the CC field writing a mapping; the ring on a mapped knob and the chip hover marking it; the
  instrument window from the header (MPE Synth) and from the inspector (a Voice Instrument's graph and strip); the strip
  without the instrument and sources; Collapse all except selected; the mixer's macros. Not checked by hand: sound
  (the engine tests cover it), a MIDI controller moving a macro, a source chip dragged onto a knob (HTML drag and drop
  isn't driven by the CDP mouse), and the plugin instrument path of the header button.
