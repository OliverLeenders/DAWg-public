# Design: mapping imported samples, and keeping a note's length

The Sampler (sampler-midi-delay-limiter.md, section 4) maps a file to a zone: one file covers the keyboard, several take a
key each from a kick's. Sample libraries come in other shapes, which a folder of them shows (the Open Samples library, used
from the user's own copy through the sound explorer, not shipped: its licence forbids selling it, which the AGPL can't
promise):

* several notes in one file, in equal slots: three or more C octaves of a synthesizer preset, 7.2 or 9.6 seconds apart,
  without any metadata saying so;
* a note per file, named in the file name in one of many ways (`_C3`, `Bb3`, ` G4`, tracker style `C-2` and `C#2`,
  lowercase `d#0` beside its MIDI number `027`, `noteF#5`), with the octave of middle C not agreed (C3 or C4 at key 60);
* a note per file with no name to go by (`key01`…`key23`);
* loudness layers named by dynamics (`pp`…`ff`), velocity (`Velocity 20`) or a range (`_0_47`);
* takes of the same note (`-001`, `-002`).

This design maps them. Round robin and layered zones come after it, and Kontakt's NCW and NKI files aren't read.

## Mapping (SM)

* **SM1 What gets mapped.** Files dropped on the Sampler without a pad or key, and folders dragged from the sound explorer
  (their audio files, not their subfolders, sorted by name, at most 512). A drop on a pad keeps putting the file on that
  pad, and one unpitched file still covers the keyboard.
* **SM2 Analysis.** Each file is read when it's copied into the project (the first ten minutes), on the import's
  background thread: its slots and each slot's pitch (SM3, SM4). The engine does the planning (`dawg/SampleMapping.h`), so
  it's tested without the app.
* **SM3 Slots.** A sound starts where the level rises above 45 dB below the file's peak after at least 150 ms below it.
  A file is cut into slots by the shortest period, counted from its first sound, whose grid its sounds fall on (within
  100 ms): at least three slots and at most 32, at least 0.3 s each, a quarter of them allowed no start of their own (a
  sound that rings into the next slot), the last one as long as the file leaves. Sounds off the grid (a second strike, a
  phrase, a hit and its echo) don't make slots, and a file whose sounds fit no grid stays whole. A slot's zone starts 2 ms
  before its sound and ends where the next begins; a slot with nothing above the threshold is dropped.
* **SM4 Pitch.** YIN (`detectPitch`, audio-features.md) over a second of each slot, from 50 ms after its start: the
  median of the pitched frames, if at least 40 % of the louder frames have a pitch. A slot's root key is that pitch,
  rounded. The slots of one file go up by the same interval, the commonest between neighbours (13 and -11 counting as 1,
  so octave errors don't hide a run of semitones): a slot whose pitch wasn't found or is an octave off takes its place in
  the run, and when most neighbours agree every slot does (a pitch above the detector's 1.5 kHz is found as something
  else). Numbered files with no note in their names (`key01`, `key02`…) are a run too, in number order.
* **SM5 Names.** A note in the file name gives the root key (the last one, if there are several; `#`, `♯`, `b` and `♭`;
  tracker style `C-2` is C2). Where the detected pitches disagree with the named keys by an octave for most files, the
  names count middle C as C3 and all named keys move by it. A MIDI number beside the note (`027--d#0`) says the same.
  Dynamics (`ppp` to `fff`), `Velocity NN`, `velNN` and a trailing range `lo_hi` give a loudness layer.
* **SM5a Stored in the file.** What a WAV file stores goes before its name. The root key in its `smpl` chunk is exact:
  no octave convention or pitch moves it. Only a stored middle C (60) against a different note in the name loses, since
  many recorders write 60 by default. A file whose name has no note may name it in its title (`INAM`). The first loop in
  the `smpl` chunk (its end inclusive there) makes a whole-file zone play in Loop mode between its frames.
* **SM5b After the import.** A hint says how many zones came from how many files. A zone's **Detect pitch** (its
  context menu) measures its sound as SM4 does and sets its root key to the nearest key and its tune to the rest, so the
  sample plays in tune at that key; a sound with no clear pitch leaves the zone as it is and says so.
* **SM6 Melodic or drums.** The files map to keys by their pitch when at least half of them name a note or at least
  three quarters have a pitch found, all slots of a sliced file counting; otherwise they take a key each from a kick's, as
  today, in name order.
* **SM7 Key ranges.** Zones of one loudness layer, by root key: each plays from the key after the last one's range to
  halfway to the next root, the lower one taking the middle key, the lowest from key 0 and the highest to 127. Takes of
  one note share its range (until round robin plays them in turn, the first one plays).
* **SM8 Velocity ranges.** The layers, by loudness (ppp 16, pp 32, p 48, mp 64, mf 80, f 96, ff 112, fff 127, or the
  named velocity): each reaches halfway to the next, the softest from 0, the loudest to 127, the same on every key, since
  each layer spans the keyboard by SM7. Named ranges are kept.
* **SM9 Command.** `addSampleZones` takes `zones`: for each, the index of its file in `files`, its frames and its keys and
  velocities. Without it, the command does what it did.

## Pitch mode (PM)

* **PM1** The Sampler (and Sample Voice) gets a **Pitch mode** choice: **Tape** (as now: a note an octave up plays at
  twice the speed and lasts half as long) or **Keep length**.
* **PM2 Keep length, rendered.** Each zone is rendered offline at the pitches its keys need, by the time-stretching
  renderer audio clips use (time-stretching.md, TS7 to TS9: Rubber Band, cached on disk by what the render depends on,
  rendered in the background nearest first). A zone of up to 25 keys gets a render for every key it covers; a wider one
  (a single sample across the keyboard) one every 3 semitones. A note plays the render nearest its pitch at its start,
  at the same length as the sample, and the little that remains (the zone's tune, a bend, at most 1.5 semitones between
  renders) is made up tape style, so the length is kept within a few per cent and bends still follow. A zone whose root
  key is the note needs no render.
* **PM3 While rendering.** Until all of a zone's renders are ready, its notes keep their length the cheap way: two read
  heads, each reading 50 ms of the sample at the note's rate from where the sample has got to, under Hann windows half a
  grain apart. That sounds grainy and its partials beat against each other, which is why it is only the stand-in. The
  renders play from the next compilation after they're ready, as a stretched clip's do.
  An export renders them all first, as it does a stretched clip's (time-stretching.md, TS10), so it never plays the
  grains; the panel says "Rendering pitches…" while they're on their way.
* **PM4 Loops** keep their frames: a render is as long as the sample, so the loop points stay where they were, and a
  one-shot ends at the sample's end. Turning Keep length on asks for the renders; turning it off plays tape again at
  once.

## Takes and layers (TL)

Libraries record a note more than once: several takes to alternate (`-001`, `-002`), and several sounds to play together
(a left and a right microphone in files of their own, `_L` and `_R`; a drum kit's close and overhead microphones). Until
now a note played the first zone that held it.

* **TL1 Layers.** A zone is in a layer, 1 to 8 (1 by default). A note plays one zone from each layer that holds it,
  each with its own sample, envelope, level, pan and choke group, in the one voice the note has; so a layered note costs
  no more voices. Within a layer, the first zone that holds the note plays it, as before.
* **TL2 Takes.** The zones of that layer with the same keys and velocities as that first zone are its takes. The
  Sampler's **Takes** choice says which plays: **In turn** (the default), or **Random**, which never plays the same take
  twice in a row. A take that has its keys or velocities changed stops being one. Each set of takes counts on its own; the
  count lives with the compiled zones on the audio thread, as the choke counts do, and starts over when the zones change.
* **TL3 Choke groups.** A note starting counts once in each choke group its layers are in, so its own layers don't choke
  each other.
* **TL4 Mapping.** In a file's name, a token `L` or `Left` puts it in layer 1 panned left, `R` or `Right` in layer 2
  panned right; a microphone (a token ending in `Mic`, or `OH`, `OHL`, `OHR`, `Room`, `Close`) puts it in a layer of its
  own, one per microphone in name order, up to 8. A number with a leading zero (`01`, `-001`) marks a take. Takes need
  nothing else: they have the same note and loudness, so they get the same keys and velocities.
* **TL5 Drums.** Files mapped as drums (SM6) are grouped into pieces: a file's name without its take number, microphone,
  side, dynamic and velocity. Each piece takes a key, from a kick's (36) in name order, its files layered and taking turns
  by TL1, TL2 and TL4. A single file without a note or a pitch still covers the keyboard.
* **TL6 Seeing and changing them all.** Where any zone is in a layer above 1, the zone list shows each zone's layer
  (`L2`). The **⋯** button beside **+** (in Pads and in Zones) sets every zone to One-shot, Gate or Loop, or removes all
  of them; each is one step to undo.

## SFZ patches (SF)

SFZ is the open text format samplers share their mappings in, and ConvertWithMoss writes it from Kontakt's NKI and NCW (the
Yamaha grand, the Wurlitzer, the marching percussion and the ukulele in Open Samples). A patch brings what names and
pitches can only guess: keys, velocities, loops, envelopes, round robins and layers.

* **SF1 Where.** An `.sfz` file shows in the sound explorer beside audio files and is dragged onto a Sampler, as a folder
  is (SM1); the Sampler's Add samples dialog takes one too. Its samples are copied into the project like any others, and
  its regions become zones after those there are. Selecting it in the explorer doesn't preview it.
* **SF2 Reading.** The engine reads the text (`dawg/Sfz.h`): comments, `#define` and `#include`, the headers `<control>`,
  `<global>`, `<master>`, `<group>` and `<region>`, each passing its opcodes down to the regions under it. `sample=` takes
  the rest of the value up to the next opcode, spaces and all, relative to the patch's folder and `default_path`. Keys are
  numbers or names (C4 is 60), moved by `note_offset` and `octave_offset`.
* **SF3 Opcodes to zone fields.** `lokey`, `hikey`, `key` (all three keys), `pitch_keycenter`, `lovel`, `hivel`; `offset`,
  `end`; `tune` (cents) and `transpose`; `volume` (dB) and `pan` (-100 to 100); `loop_mode` (`one_shot` is one-shot,
  `loop_continuous` and `loop_sustain` loop, `no_loop` is gate), `loop_start`, `loop_end`; `ampeg_attack`, `ampeg_decay`,
  `ampeg_sustain` (%) and `ampeg_release`, in seconds. A region without an envelope keeps a new zone's.
* **SF4 Choke groups.** `group` and `off_by`: groups another region is cut off by become choke groups 1 to 8, in the order
  they appear.
* **SF5 Takes and layers.** SFZ plays every region that holds a note, so regions overlapping in keys and velocities are
  layers: each goes in the lowest layer it overlaps nothing in. Regions with the same keys and velocities that alternate
  (`seq_position`, or `lorand` and `hirand`) are takes in one layer, in `seq_position` order; random ones set the
  Sampler's Takes to Random.
* **SF6 Left out.** Release triggers (`trigger=release`) and regions whose sample can't be read are skipped; other opcodes
  are ignored. A hint after the import says how many zones came in and names the opcodes it ignored.
  A numbered family (`amp_velcurve_1`, `amp_velcurve_127`…) is named once (`amp_velcurve_N`), and labels aren't named.
* **SF7 Velocity crossfades.** `xfin_lovel` and `xfin_hivel` fade a region in across those velocities, `xfout_lovel`
  and `xfout_hivel` fade it out, equal power as SFZ's default curve. The zone keeps them (`fadeInLowVelocity` and the
  others), so overlapping velocity layers blend instead of adding up; ConvertWithMoss writes them for Kontakt patches
  with velocity crossfades (the Yamaha grand). `amp_veltrack` sets the Sampler's Velocity knob.
