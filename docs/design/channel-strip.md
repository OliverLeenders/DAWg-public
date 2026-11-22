# Design: channel EQ, default inserts and the channel strip

Status: **implemented** (notes in section 5). The decisions are numbered C1–C6; the owner's answers are in section 4.

## 1. Goals

* A **channel EQ**: the EQ gains a high-pass and a low-pass, and every band can be switched off.
* **Default inserts**: new tracks start with an EQ and a compressor, neutral until touched.
* A **channel strip** in the device dock: the selected track's EQ and compressor side by side.

Non-goals: a mixer view (phase 13), presets, a dynamic EQ, matching the filter node's state-variable filter.

## 2. Decisions

**(C1) The EQ has eight bands: HP, low shelf, four bells, high shelf, LP.** The bands use the matched second-order
designs (docs/PLAN.md, phase 8). Since 2026-11-15 the EQ's *Cuts* setting (`cuts`) can make a cut the exact reciprocal
of the boost by as much instead, so a mirrored de-emphasis cancels (parameter-linking.md, section 10), and every
section runs as direct form I in double precision. The HP and LP are Butterworth from 6 to 48 dB/octave in 6 dB steps (orders 1 to
8: a first-order section for odd orders and a second-order one per pole pair; 12 or 24 dB/octave before format
version 7). The shelves have a Q (`lowQ`, `highQ`; 1/sqrt 2 is the plain shelf, higher overshoots). Every
band has an on/off switch (bypass) and, since format version 6, an "in use" switch (`hpUsed`, `lowUsed`,
`mid1Used`–`mid4Used`, `highUsed`, `lpUsed`): a band runs only while it is in use and on. Bands start on but not in
use, so a new EQ is neutral and its panel shows no bands until some are added. The shelves' and filters'
frequencies range from 20 Hz to 20 kHz, like the bells' (before version 6: HP 20 Hz–2 kHz, low shelf 20 Hz–1 kHz,
high shelf and LP 1–20 kHz).
New parameters: `hpOn`, `hpFreq`, `hpSlope`, `lpOn`, `lpFreq`, `lpSlope`, and `lowOn`, `mid1On`–`mid4On`,
`highOn`. Parameters are stored by id, so existing EQs keep their settings and get the new bands' defaults (HP and
LP off, the other bands on). Bands not in use, off, or flat are skipped.

**(C2) New tracks get an EQ and a compressor.** `addTrack` puts `EQ → Compressor` before the Gain node on
instrument, audio, group and return tracks (not the master). The compressor starts **bypassed**: new tracks
sound untouched and cost nothing until it is switched on. Existing projects and duplicated tracks are left as they
are.

**(C3) The strip finds its devices in the chain.** The channel strip shows the first EQ and the first compressor
in the track's chain. There is no special "strip" flag: deleting one leaves an "Add EQ" or "Add compressor" button
in its place, and a second EQ is just another insert.

**(C4) The dock has Channel and Device tabs.** Selecting a track (its header or a clip) shows its Channel view;
selecting a node (in the graph or the inspector's chain) shows that Device, as today. Tabs in the dock header
switch between the two.

**(C5) The Channel view.** The EQ panel on the left (curve, handles for the bands in use, spectrum)
and the compressor panel on the right (transfer curve, gain reduction, knobs; the history when there is room).
Each has a title with its power button and a button to open it as a device.

**(C7) The fader section.** A column at the dock's right end, on both tabs, for the track the dock shows: mute and
solo (not on the master), a pan slider (drag, Shift for fine, double-click to centre, wheel), a volume fader with
the track's stereo meter and clip indicator beside it, and the gain readout (drag or type). The fader follows the
gain lanes' curve up to +6 dB, the top of its marked scale, and its top tenth runs on to the gain's +12 dB maximum.
It uses `setTrackGain`, `setTrackPan` and `setTrackSwitches` with gestures and automation touch, and shows the
automated values while automation plays, like the track headers. A stripe in the track's colour marks its left edge.

**(C8) EQ bands filter both channels, the mid or the side.** Each band has a mode (`hpMode`, `lowMode`,
`mid1Mode`–`mid4Mode`, `highMode`, `lpMode`: Stereo, Mid or Side; Stereo by default, so existing EQs are
unchanged). While no running band is on the mid or the side only, the EQ filters left and right as before. Once one
is, the whole EQ runs on mid and side (M = (L + R) / 2, S = (L - R) / 2, and back): a Stereo band filters both, as
it would left and right, since the filters are linear. Switching domain carries the sections' state across with
the same transform, so it doesn't click; a section whose mode changes starts from a cleared state. The panel shows
the mid's and the side's curves when they differ (the side's thinner, behind the mid's, in colours of their own:
orange for the mid and violet for the side),
the mid's and the side's spectra behind them, quieter than the one spectrum, and a ring with an M or S around a mid
or side band's handle.

**(C9) Every band can be any type.** Each band has a type (`hpType`, `lowType`, `mid1Type`–`mid4Type`, `highType`,
`lpType`): bell, low or high shelf, high- or low-pass, notch or tilt. A band's default type is the one its
parameters are named after (the first band a high-pass, the second a low shelf, four bells, a high shelf, a
low-pass), so existing EQs are unchanged. Every band has every parameter a type needs (frequency, gain, Q, slope;
`hpGain`, `hpQ`, `lpGain`, `lpQ`, `lowSlope`, `mid1Slope`–`mid4Slope` and `highSlope` are new), and four sections,
enough for a pass filter at 48 dB/octave. Parameter names show "Band 1" to "Band 8". A notch is the analogue
(s^2 + 1) / (s^2 + s / Q + 1) and a tilt the high shelf divided by A, so half its gain is cut below its frequency
and half boosted above, both matched like the other designs. On the panel a handle's menu sets the type; a band
added on the curve or from the menu takes a free band whose default type it is, else any free band.

**(C6) Several nodes can be watched.** `nodes.watch` takes a list, so the EQ's spectrum and the compressor's meters
both run while the Channel view shows. Each node's analysis arrives keyed by `trackId:nodeId`.

## 3. Steps

1. Engine: the EQ's bands (C1), the default inserts (C2), tests.
2. App: watching several nodes (C6).
3. UI: the EQ panel's new bands, the Channel/Device tabs and the Channel view (C3–C5).

## 4. Owner's answers

* **EQ bands:** HP, low shelf, four bells, high shelf, LP.
* **Compressor:** inserted but off.
* **Track kinds:** instrument and audio tracks, and also groups and returns (not the master).
* **Dock:** Channel / Device tabs.
* **Fader section (C7):** on both tabs, with the meter beside the fader, +6 dB marked and +12 dB reachable.

## 5. Implementation notes

* Engine (EffectNodes.cpp): `EqNode` runs 32 matched sections (four per band, `eqBands` naming each band's
  parameters), skipping bands that are not in use, off or flat, and a pass filter's sections beyond its slope; `bandSections` maps them to
  the eight bands the panel's curves show. A section switched back on starts from a cleared state. The first-order
  section is matched too: its pole by impulse invariance, its zero fitted at DC and Nyquist.
* `addTrack` builds the chain with `makeChainGraph()` (Commands.cpp); `groupTracks` makes its group the same way.
  New effects inserted without an index go after the compressor; index 0 is the first effect slot.
* `nodes.watch` takes `{ nodes: [...] }`; the app emits one `dawg_analysis` per watched node per frame, and the UI
  keeps the latest of each by key (`useAnalysis().byKey`, `useWatchNodes`). Each call names its window
  (`{ window, nodes }`, "main" when left out): the app keeps one list per window and runs the union, so the instrument
  window's panel keeps its analysis while the main window's dock watches other nodes. A node's tap stops when no window
  watches it, and a window's list is dropped when it closes.
* UI: ChannelStrip.tsx (the Channel view, `stripNodes()`), the dock's tabs (DeviceDock.tsx, `ui.dockTab`:
  selecting a track switches to Channel, selecting a node to Device), and the EQ panel (EqPanel.tsx). Only bands in
  use get a handle, without a label; a bypassed band's is faded. A double-click on the curve adds a band: under
  100 Hz an HP below the 0 dB line and a low shelf above it, over 5 kHz an LP or a high shelf the same way, a bell in
  between (a bell too when the band asked for is taken). A press near the 0 dB line and a drag adds one too: a bell
  in the middle and a shelf at those edges, whichever way it is dragged. The context menu adds any free band. A
  double-click on a handle bypasses the band or switches it back on; Delete or the handle's menu takes it out of use.
  The hovered or dragged band shows its own curve and a tooltip with its name and values. HP and LP handles sit on
  the 0 dB line and move only in frequency; the wheel (one step per notch, also while the handle is held) or their
  menu sets their slope. The wheel or Alt with a vertical drag sets a bell's or shelf's Q. A handle's menu sets the
  band to Stereo, Mid or Side (C8); `nodes.response` then adds the side's curve (`side`, with `y` the mid's).
* Format version 6 (ProjectFile.cpp, `migrate5To6`): older files left a switch out when it was at its default (on
  for shelves and bells, off for the HP and LP). The migration marks in use the shelves and bells with a gain or
  whose gain a lane, a modulation or a MIDI mapping moves, and the HPs and LPs that were switched either way or
  whose switch something moves; the rest could not be heard, so projects sound the same. Lanes on the widened
  frequencies are moved to the same frequencies on the new ranges, and modulation depths on them scaled to sweep
  about the same span. MIDI mappings to them now cover the whole range.
* Format version 7 (`migrate6To7`): the slopes' 0 and 1 (12 and 24 dB/octave) become 1 and 3, with the points of
  lanes on them; modulation depths on them are scaled to the wider range. An unset slope stays 12 dB/octave.
* Tests: the HP/LP slopes at every order against Butterworth, the shelves' Q against the analogue shelf, bands
  switching off, the default chain for each track kind, and the existing graph and
  plugin tests updated for the longer default chain.
* The fader section is TrackFader.tsx (`dbToFader` and `faderToDb` for its curve, with a unit test); the meter has
  a vertical mode (`Meter vertical`).

