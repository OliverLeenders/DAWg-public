# Design: humanise and node presets

Status: **implemented**, recorded after the fact (2026-11-01). Both arrived from separate branches without a design.

## Humanise (pianoroll/humanise.ts, HumaniseDialog.tsx)

The piano roll's Humanise dialog (H, or the context menu) changes the selected notes, or all notes if none is selected,
as one undo step. A seed makes the result repeatable while the sliders move.

* **Melodic:** Gaussian timing errors, smaller on beats and bar starts; velocity errors linked to timing (earlier is a
  little harder); a slow tempo drift across bars (a first-order autoregressive process); chords, meaning notes within
  1/40 of a beat, spread from the lowest note up; a small change in length.
* **Drums:** Gaussian timing errors per role, from a **drum map** (pitch to name, role and limb). The kick is tightest. The
  snare has a **pocket**: negative plays behind the beat, positive ahead. Hi-hats lead a snare they strike with. Hi-hats
  and rides are softer between the beats, ghost notes (velocity up to 0.35) may be looser and stay quiet.
* All times are in milliseconds, turned into ticks through the tempo map; note positions in ticks use `TICKS_PER_BEAT`.

**Drum mode** is per track and saved in the project (`Track::drumMode`, format 12, `setTrackDrumMode`): `auto` goes by the
track's name, `drums` and `melodic` are the user's choice. In drum mode the roll labels keys and notes with the drum
names.

**Drum maps:** General MIDI, Roland V-Drums and Drum Rack are built in, and each can be edited in the dialog. The map in
use and the edited maps belong to the user, not to a project, so they live in the app's settings (`settings.get`,
`settings.set`).

## Node presets (dock/presets.ts, PresetSelector.tsx)

Factory presets for the Loop Reverb, EQ, Filter, Compressor, Saturator, Utility, LFO and ADSR, plus the user's own. A
preset is a list of parameter values, and for an LFO also its shape; loading one is one undo step. The selector shows
the preset the node matches, with a star after the name if it was changed since.

* The selector sits in the middle of the device dock's and the channel strip's slot headers (a three-column grid, so
  the sides can't push it off centre).
* User presets live in the app's settings too. Those saved in the web view's storage before that are taken over the
  first time.
