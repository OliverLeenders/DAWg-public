# DAWg — Project Plan

DAWg is an open-source hobby DAW with three core features:

1. **Visual MPE piano roll** — notes carry per-note expression curves (pitch, pressure, timbre, …) that are drawn and edited directly.
2. **Node-based effects routing** — tracks and instruments are processing graphs edited on a canvas.
3. **Scriptable effects** — DSP written in a script language, compiled and hot-swapped live.

## Decisions

| Topic | Decision | Notes |
|---|---|---|
| Licence | AGPLv3-or-later | Required by JUCE's open-source licence. |
| Engine language | C++20 | |
| Framework | JUCE 9 | Audio I/O, plugin hosting (VST3, AU, LV2), WebView integration. |
| Build | CMake + CPM | All dependencies are fetched and pinned automatically. |
| UI | React + TypeScript in an embedded WebView | Single process: `juce::WebBrowserComponent` (WebView2 / WKWebView / WebKitGTK). |
| Plugin editors | Native JUCE windows | Web content cannot embed native plugin views. |
| VST3 | Always on | |
| VST2 | Optional (`DAWG_ENABLE_VST2`) | The VST2 SDK is no longer distributed; users supply their own copy. |
| Engine | Custom, on plain JUCE | Tracktion Engine was considered; its model fights per-note curves and voice-scope graphs. |
| Scripting | Faust via libfaust (LLVM JIT) first; Lua (sol2) for control-rate scripts; WASM sandbox later | |

## Architecture

```
Web UI (React)  ──commands──▶  Session (C++ model, undo/redo)  ──compile──▶  EngineState snapshot
      ▲                              │                                             │ atomic swap
      └──── patches / telemetry ─────┘                                             ▼
                                                                             Audio thread
```

* **Model** (`engine/include/dawg/Model.h`): an immutable value. Commands build a new `Project`; unchanged
  children are shared (`shared_ptr<const T>`). Undo/redo swaps snapshots.
* **Commands** (`Commands.h`): pure functions `(Project, args) -> Project`. All edits go through them.
* **Session** (`Session.h`): current snapshot, revision counter, undo/redo, gesture merging (one drag = one undo step).
  Emits JSON-Patch style diffs to the UI.
* **Realtime handoff** (`Realtime.h`): `SnapshotExchange` passes immutable snapshots to the audio thread without
  locks; retired snapshots are freed on the message thread.
* **Bridge** (`app/src/Bridge.h`): one native function `dawg_call(method, params)` and events
  (`dawg_patch`, `dawg_telemetry`, `dawg_audio`, `dawg_plugins`). Transport-agnostic so a WebSocket could replace it.
* **Plugin scanning**: each plugin file is scanned in a child process (`DAWg --scan-plugin …`), so crashing plugins
  cannot take the DAW down.

### MPE note model (phase 3)

```
Note { start, length, basePitch, velocity, releaseVelocity,
       expr: { pitch: Curve, pressure: Curve, timbre: Curve, ...custom } }
Curve = breakpoints (time relative to note start, value, segment shape)
```

Pitch is stored in float semitones. Curves move and stretch with their note. Optional note links model glides.
Output: built-in instruments read curves directly; plugins receive MPE over MIDI 1.0 (per-note channels,
±48 st bend, pressure, CC74); CLAP note expressions come later.

### Node graph (phases 8 and 12)

Typed ports (audio, events, modulation). A graph compiler topologically sorts nodes, assigns buffers, computes
delay compensation and groups parallel stages. No feedback yet: loops are refused (feedback routing is phase
16, designed for safety first). Two scopes:
a per-note **voice graph** (receives that note's curves) feeding a monophonic **track graph** (plugins live here).

## Roadmap

1. **Skeleton** ✅ — CMake + JUCE app, WebView with React UI and dev hot reload, command/patch bridge with
   undo/redo, audio device settings, metronome transport, out-of-process VST3 scan, tests, CI.
2. **Engine core** ✅ — compiled processing graph replacing `EngineState`, built-in MPE sine synth, sample-accurate events.
3. **Piano roll v1** ✅ — Canvas 2D roll with expression overlay and tabbed lanes, smart-pointer editing,
   point-by-point curve editing, audition, playhead follow.
4. **Plugin hosting** ✅ — instrument + insert chain per track, async loading, native editor windows,
   MPE MIDI output, plugin state in the model, master latency alignment, temporary JSON save/open.
5. **Project workflow** ✅ — native project format (see below); new / open / save / save as, recent projects,
   unsaved-changes marker and prompt, autosave with crash recovery, welcome screen (new empty or from a
   template, open, recent). Replaces the temporary JSON save/open from phase 4.
6. **Arrangement view v1** ✅ — timeline with one lane per track; create, move, resize, split, duplicate and loop
   clips; double-click opens a clip in the piano roll (which then shows that clip); markers and loop range.
7. **Audio tracks** ✅ — audio track type, audio clips with waveforms, disk streaming, recording, clip fades and gain,
   sound explorer with preview.
   - **Mixing basics** ✅ — equal-power pan, track and input meters with clip indicators in the track headers, and a
     master track with fader, pan, meter and insert effects (docs/design/mixing-basics.md).
8. **Node graph v1** ✅ — React Flow canvas for track and master graphs, free routing, built-in EQ, filter,
   compressor, utility and mix nodes, node parameters in the inspector.
   - **De-cramped EQ and filter** ✅ — the EQ's bands are Vicanek's matched second-order filters ("Matched Second
     Order Digital Filters", 2016) instead of RBJ cookbook biquads: analogue poles by impulse invariance, zeros fitted
     to the analogue magnitude at DC, Nyquist and the band frequency. A 15 kHz peak at 48 kHz now stays within 1 dB of
     the analogue curve (the bilinear one was 6 dB off at 20 kHz). The filter node keeps its TPT state-variable filter,
     which stays stable under fast modulation, with its g and k set from the matched poles and its output a mix of
     its high-, band- and low-pass outputs that makes the matched numerator.
   - **Graph modulation** ✅ (docs/design/modulation.md, recorded after the fact and awaiting review) — LFO and ADSR
     source nodes, a pool of modulation inputs on every effect and instrument, and per-slot target and depth for
     built-in and plugin parameters, applied once per block around the parameter's manual or automated value.
9. **Export and MIDI input** ✅ — render the song, the loop range or selected tracks to WAV or FLAC, with realtime
   and offline modes. Hardware keyboards and MPE controllers (Seaboard, Linnstrument, Osmose): device selection,
   play-through to the armed or selected track, recording notes with their expression curves into clips (including
   loop recording into takes), and MIDI learn.
10. **Automation** ✅ — automation lanes in the arrangement for track gain and pan, node parameters and plugin
    parameters; sample-accurate parameter events in `ProcessContext`, plus write, touch and latch recording from
    UI controls and plugin editors.
11. **Faust scripting and node panels** ✅ — Faust nodes compiled by libfaust (interpreter backend) in the
    background and swapped with a crossfade, a Code view with Monaco, example scripts and a script library, and a
    device dock with generated Faust controls. Graphical panels for the built-in nodes: an EQ curve with draggable
    bands and a spectrum analyser, a compressor transfer curve with a scrolling level and gain-reduction history, a filter response, and a
    goniometer with a correlation meter (docs/design/phase11-faust-panels.md).
    - **Loop Reverb** ✅ — an algorithmic reverb of seeded early reflections and a feedback delay network, with
      Pre-delay, Early Timing and Width controls and a panel showing the early arrivals: a clean-room
      implementation from a functional specification, accepted by its acceptance tests and by listening
      (docs/design/loop-reverb.md).
    - **Saturator** ✅ (docs/design/saturator.md) — tanh saturation with transparent first-order
      antiderivative anti-aliasing (docs/design/vicanek-adaa.md) and optional 2x or 4x oversampling, a bias for even harmonics tuned as in the owner's ADAA
      plugin, DC removal, mix and a transfer-curve panel; usable per voice in voice graphs.
    - **Clipper** ✅ (docs/design/clipper.md) — a hard clip with a quadratic knee from hard to soft, anti-aliased
      by transparent first-order ADAA with the closed-form antiderivative and optional 2x or 4x oversampling, a ceiling that no
      output sample exceeds (a 1 ms peak guard after the shaper at every oversampling choice, sharing the Limiter's lookahead), Drive, Output
      and Mix, and a panel with the transfer curve, the ceiling and a clipping meter; usable per voice in voice graphs.
    - **Waveshaper** ✅ (docs/design/waveshaper.md) — a choice of curves (Soft, Tube, Fold, Sine Fold, Half and Full
      Rectify) and a curve drawn on the panel (the default; symmetric by default, the positive half mirrored), each
      anti-aliased by transparent first-order ADAA with a closed-form antiderivative (piecewise for the drawn curve) and optional
      2x, 4x or 8x oversampling; Drive, a bipolar Bias, a tone tilt after the shaper, DC removal, Output and Mix;
      shape changes crossfade; usable per voice in voice graphs.
    - **Convolution Reverb** ✅ (docs/design/convolution-reverb.md) — the signal convolved with an impulse response
      from the audio pool (dropped from the sound explorer or loaded in a dialog, and copied into the project), or a
      generated default room: our own zero-latency convolver (a direct head and FFT levels of 64, 512 and 4096
      samples), mono, stereo and true-stereo IRs up to 20 s, built on a background thread (Start, Length, Decay,
      Reverse, Stretch, windowed-sinc resampling, normalising) and swapped with a crossfade that keeps the tail;
      Pre-delay, Low and High cut, Width, Gain and Mix in real time; a panel with the IR's envelope, the kept part and
      the reverb time. Track graphs only.
12. **Voice-scope graph** ✅ (docs/design/phase12-voice-scope.md) — per-note curves drive per-voice nodes: a Voice
    Instrument with an editable voice graph, an oscillator, and per-note envelopes, LFOs and MPE expression as
    modulation sources. The voice graph plays and is edited in the graph view, polyphonic Faust included.
13. **Routing and time** — sends, buses and groups, sidechain inputs, a mixer view; a tempo map with
    time-signature changes instead of one tempo per project (the transport, time map, audio clip stretching,
    metronome and rulers all follow it).
    - **Group tracks and effect sends** ✅ (docs/design/groups-and-sends.md) — group tracks with Ctrl+G: a
      folder whose tracks' main outputs are summed on the group's bus, with its own inserts, fader and pan, drawn
      as a "C" around its members (name on the top arm, gain, pan and meter on the bottom arm) and foldable.
      Return tracks in a fixed section above the master, fed by per-track sends (post-fader by default,
      automatable; later also into sidechain inputs). Multi-track selection and track reordering came with groups.
    - **Tempo map and time signatures** ✅ (docs/design/tempo-map.md) — tempo points with steps and ramps, time
      signatures on bars (x/2 to x/16), file format version 2. The engine, audio clips (which keep their real-time
      length), vibrato, the metronome, recording and a play head for hosted plugins follow the map; the arrangement
      has a tempo lane and a signature strip, rulers and grids follow the bars, and the position display toggles to
      min:sec.
    - **Sidechain inputs** ✅ (docs/design/sidechain.md) — the compressor and effect plugins with a sidechain bus
      take a key: from another track through a key send (pre-fader by default, loop-checked, delay-compensated) or
      wired in the graph. The compressor's detector gets a key high-pass and Listen; its panel picks the key source
      and its history shows the key's level.
    - **Mixer view** ✅ (docs/design/mixer.md) — the mixer in its own window (a second web view on the same
      bridge, selection shared with the main window): a strip per track in signal-flow order, a group after its
      members with brackets in its colour, the returns, and the master pinned on the right. Strips show I/O, inserts
      (bypass, reorder, replace), sends as a list with level knobs, pan, switches and the dock's fader and meter;
      sections fold and strips go narrow.
    - **Per-track timing offsets** ✅ — a per-track delay in milliseconds or samples, positive and negative, for
      aligning layered parts and hardware or plugin latency. A positive offset delays the track; a negative one
      pulls it earlier, which the compiler realises by shifting that track's events and audio reads and delaying
      the rest of the mix by the largest advance, so the master stays aligned and the added latency can be
      reported. Sends, key sends and groups follow the same offset. Scope: model and `setTrackOffset` command with
      undo, project format v3 (the field defaults to 0 in the migration), compiler delays, inspector and mixer
      controls with a ms/samples readout, and tests for delay compensation with sends and sidechains.
      **Complexity:** medium (about 3 days); the negative-offset alignment across sends and sidechains is the
      fiddly part.
14. **Linked notes** ✅ (docs/design/linked-notes.md) — legato and glides between separate notes: a link from one note to the next makes the second
    continue the first's voice instead of starting its own. The envelope doesn't restart, and the pitch glides to the
    new note over a set time or curve, while each note keeps its own start, pitch and expression curves. Built-in
    instruments and voice graphs move the voice on to the linked note; plugins over MPE keep the note on its member
    channel and bend to the new pitch rather than sending a note-off and a new note-on. Today a glide can only be
    drawn as one note's pitch curve; the voice-scope design left links out of v1 (phase12-voice-scope.md, section 1).
    Also since phase 13, outside the phases: the piano roll moved to its own window (the main window keeps the
    arrangement and the graph), humanise and node presets (docs/design/humanise-and-presets.md), and on Windows the UI
    draws the title bar (docs/design/window-chrome.md).
    **Sampler, MIDI files, delay and limiter** (docs/design/sampler-midi-delay-limiter.md), the owner's next step on
    2026-11-01, before phase 14: done.
15. **Release readiness** (all before 1.0; the owner, 2026-11-02: after phases 16 and 17) — CLAP hosting, plugins hosted out of
    process (scanning already is; docs/design/out-of-process-plugins.md: built on Windows and Linux, macOS to come), so a crashing plugin doesn't take the DAW down; installers for Windows, macOS and
    Linux, update checks, MP3 export and macro controls.
    - **The owner's 1.0 list** (2026-11-09):
      * **Installers for macOS and Linux**, beside the Windows one. macOS builds are **not signed or notarised**: an
        Apple developer account costs money, and DAWg is free. The download page and the installer's readme explain
        opening an unsigned app (Open from the context menu, or removing the quarantine attribute). **Linux done**
        (2026-11-14): `tools/build_installer.py` on Debian or Ubuntu builds a .deb (the app in /opt/dawg, a `dawg-daw`
        command, a menu entry and icons, its dependencies read from the executable through dpkg) and a .tar.gz to unpack
        anywhere. Installed from the .deb on Ubuntu 24.04 in WSL, DAWg starts and shows its welcome screen. Getting
        there fixed a crash on start on Linux: the web view's child process died in Mesa's GL driver, which called into
        the LLVM DAWg had exported, so the app now keeps static libraries' symbols to itself. macOS is still to do.
      * **Update checks**: the app asks whether a newer release exists and says so; it never downloads or installs
        on its own. **Done** (docs/design/update-checks.md, 2026-11-09): GitHub releases checked at most once a day
        and from Help, a quiet notice with the release notes, skip and beta options, nothing sent about the user.
      * **MP3 export** beside WAV and FLAC (an LGPL encoder such as LAME, which GPL allows). **Done**
        (docs/design/phase9-export-midi-input.md, X6, 2026-11-09): LAME 3.100 built with the app, constant bitrates
        from 128 to 320 kbps or variable V0/V2, renders above 48 kHz filtered down, normalised to −1 dBFS by default,
        the written file's peak reported, and ID3v2 tags from the dialog.
      * **Macro controls**: knobs that each drive several parameters at once, assignable to a MIDI CC or by MIDI learn
        like any parameter, and automatable. **Done** (docs/design/macros-and-modulation-column.md, 2026-11-09): a
        Macros device of eight knobs per track, mapped by right-click or Map mode into any graph of the track, with
        a CC field; built with octave modulation for frequencies and the dock's modulation column, with instruments
        moved to the inspector and a window of their own.
      * **Parameter links**: one parameter follows another, on any node, plugin or track, through a mapping (the
        same, inverted, offset, scaled or a range), such as a de-emphasis EQ mirroring its emphasis with the gains
        inverted. **Done** (docs/design/parameter-linking.md, 2026-11-15): links in the project, followed by the
        engine before the target's modulation (automation, modulation and plugin editors included), edits of a
        target going to its source, Mirror on another device from the device menu, and a Links editor in the dock's
        modulation column.
      * **Not in 1.0**: groove templates (quantising to a recorded or preset feel; swing and strength exist), track
        templates (a whole track, instrument, devices, sequencer patterns, sends and colour, saved to add new tracks
        from; node presets and project templates exist) and DAWproject (below).
    - **CLAP hosting** — beside VST3, as a 1.0 feature (owner, 2026-10-28). CLAP is the plugin format made for what
      sets DAWg apart: per-voice parameter modulation (the voice graph's per-note curves and modulation reaching a
      plugin's parameters per voice), modulation that adds to the automated value instead of overwriting it (the
      modulation pool without its last-written detection; modulation.md, M5), and per-note expression by note id
      without MPE's channel per note. Scanning, state, editors and latency as for VST3. Whether JUCE 9 hosts CLAP
      itself, or a CLAP host layer is needed beside it, is to be checked first.
    - **DAWproject — out of 1.0** (evaluated 2026-11-09, owner: "update the roadmap with that result"). The format is
      read and written by Bitwig, Studio One and Cubase/Nuendo (Reaper only through a third-party tool; no sign of
      Ableton Live, Logic or Pro Tools), and holds the common ground: tracks, mixer, tempo, markers, note and audio
      clips (with per-note expression and warp markers), automation and plugins with their state. What makes a DAWg
      project sound like itself would not survive: node and voice graphs, components and feedback loops, every
      built-in device (the format's few generic devices sound different in every DAW), the step sequencer and MIDI
      effects, Faust scripts, linked notes and parametric vibrato. What it would give is mostly there already: audio
      export renders the mix and stems, File › Export MIDI file writes the notes, and freeze and bounce turn
      DAWg-only sound into audio, all readable by every DAW. Later, only if projects need to move to or from those
      DAWs: export only (tracks, tempo, markers, notes with expression, audio clips with timing markers, plugin
      tracks, volume and pan automation; anything DAWg-only rendered to audio on request), import only when asked for.
16. **Feedback routing** (docs/design/feedback-routing.md, accepted) — cables that feed a node's output back to an earlier input, for feedback
    delays, Karplus-Strong strings, resonators and self-oscillating patches, in track and voice graphs. Today the
    graph refuses every loop. Feedback can grow without bound, so the design must make runaway loops harmless to
    ears and speakers before anything is built, e.g.:
    * a loop closes only through an explicit feedback point with at least one block (or sample) of delay, shown in
      the graph, never by accident;
    * the feedback point limits what it passes on: a gain that can't exceed unity unless asked for, soft saturation
      and a DC blocker;
    * output protection that also guards everything else (resonant filters, Faust scripts, plugins): NaN and infinity
      are caught and silenced at each track and the master, and the master has a safety limiter (or hard ceiling) that
      mutes and warns when the level stays far above full scale; the user can't switch it off by accident;
    * tests for runaway loops, NaN injection and sustained overload.
    Output protection is done (docs/design/output-protection.md, 2026-10-28): invalid audio silenced and reset per
    track, a mute until confirmed for a runaway device output, and a hard ceiling at 0 dBFS.
17. **Component nodes** (docs/design/component-nodes.md, done) — a node in the graph view that holds part of a graph, with the inputs, outputs and knobs
    given to it, and that opens like a Voice Instrument. For tidying graphs and reusing patches, in track and voice graphs. It runs once, where it is, and the
    compiler flattens it away, so grouping nodes can't change the sound. After feedback routing, whose patches it can then hold.
18. **Time stretching** (docs/design/time-stretching.md) — audio that follows the tempo, only when the user asks.
    Each pool file is one-shot (the default: it plays at its own speed, as before) or musical, in the stretch style
    (offline renders of Rubber Band's R3 engine, played from a cache, with a repitch preview while they render) or
    the repitch style (speed and pitch together, like tape). Timing markers pin a file's frames to musical positions
    and are edited in an audio editor window with snapping to detected transients; Alt + drag on a musical clip's
    edge stretches it; clips transpose independently of speed; export renders what is missing first. Steps 1–4 are
    built (file format version 16); later: quantise audio to the grid and tempo detection from onsets. Freeze and
    bounce came in phase 19, through the export path rather than the stretch job queue.
19. **Audio clip tools, crossfades, punch, freeze and bounce** ✅ (docs/design/audio-features.md, file format version
    17) — the missing audio features, before MIDI tools and DAWproject (the owner, 2026-11-08):
    * clip operations: Reverse (through a reversed copy of the file, linked to the original, timing markers mirrored)
      and Normalise (clip gain to a sample peak from a dialog); Slice to Sampler at transients, the grid or timing
      markers, with a MIDI clip that plays the slices; audio to MIDI for drums (optionally split into kick, snare and
      hats) and monophonic melodies (YIN, with vibrato and bends kept as pitch curves);
    * crossfades between stacked audio clips: a top clip's fade over a covered clip, which plays on under it with the
      mirrored fade; automatic 10 ms crossfades where clips are placed over audio (a project setting), an S-curve shape;
    * punch recording with a punch range of its own (one take per loop pass inside a loop), and a pre-roll of 0–4 bars;
    * Freeze (an instrument or audio track rendered before its fader and played instead of its chain, which stays
      loaded but locked; stale freezes are marked), Bounce in place (a whole track), Consolidate (selected audio
      clips without the chain) and Render selection (tracks after their faders onto a new track), all rendered
      offline through the engine as an export is.
    Not built yet: slice lines previewed in the audio editor, the crossfade's own menu, splitting repeated notes of
    one pitch in audio to MIDI, and a sensitivity dialog for it.
20. **MIDI effects and swing** ✅ (docs/design/midi-effects.md) — the first of the "MIDI and composing tools" (the
    owner, 2026-11-09):
    * MIDI effects as nodes of a new kind (notes in, notes out) in the track graph, shown on a simple chain as a MIDI
      section before the instrument: an **Arpeggiator** (eight modes, octaves, gate, swing, fixed or played velocity,
      latch, retrigger), a **Chord** tool (16 shapes or custom notes, inversion, strum, velocity), **Scale Lock** (15
      scales, nearest, up, down or filter) and **Note Repeat** (rate, gate, swing). The notes they make play their
      source's curves, vibrato and live expression; links pass through Chord and Scale Lock. Steps follow the tempo
      map's grid, or a clock of their own while the transport is stopped. No note hangs on stop, seek, bypass,
      removal or a deleted source; a node whose event source changes is told so and forgets its notes.
    * The piano roll shows the clip's own notes, with a note naming the track's MIDI effects; **Render MIDI effects to
      clip** replaces a clip's notes with their output and bypasses them. Recording records what was played.
    * Quantise has **Swing** (50–75 %) and **Strength** (0–100 %) beside the grid, sharing the swing math with the
      arpeggiator and note repeat.
    Not in v1: MIDI effect plugins, step patterns, diatonic chords, a project key and scale highlighting in the piano
    roll (Scale Lock's table in `dawg/Scales.h` and `ui/src/music/scales.ts` is ready for it), groove templates.
21. **Scale highlighting** ✅ (docs/design/scale-highlighting.md; the owner, 2026-11-09) — the project's keys as key
    changes on bars (`setKey`, `moveKey`, `removeKey`), edited in the arrangement's Key strip and from the piano roll's
    toolbar: the roll draws each stretch in its key, lightening the scale's rows and tinting the root's, marks the
    scale on the keyboard and marks notes outside their key. The roll folds to the scales (notes outside them keep a
    row) or to the pitches in use, with notes drawn, dragged and nudged with Up and Down onto the shown rows and pitch
    curves staying continuous between them; unfolded, Snap to scale moves drawn, dragged and nudged notes into the
    scale where they start. Scale Lock follows the key where each note plays, by default.
22. **Step sequencer** ✅ (docs/design/step-sequencer.md; the owner, 2026-11-09) — a device of the MIDI effect kind,
    first in the MIDI section, that plays patterns of steps inside sequencer clips: a clip says where it plays, which
    of up to 16 patterns, and from which step (step 1 at the clip's start by default). Rows (from the Sampler's zones
    or a General MIDI kit, or any pitch) have their own lengths per pattern; steps hold velocity, length, chance
    (repeatable per clip and pass) and ratchets; patterns have a step size and swing. Edited in the device panel's
    grid, previewed while stopped, and rendered to a MIDI clip from the clip menu.

### Project files (phase 5)

A project is a zip (`.dawg`) holding `project.json` (the model, versioned, with a format migration step per
version), one file per plugin state (`plugins/<nodeId>.bin`) and, from phase 7, the project's audio files.
JSON diffs well, is easy to inspect and migrate, and has the same layout as
[DAWproject](https://github.com/bitwig/dawproject) (XML + files in a zip), the open exchange format of
Bitwig, Studio One and others. DAWproject is not the native format, and since the 2026-11-09 evaluation not a 1.0
target either (phase 15): it cannot hold node graphs, scripts, curve shapes or parametric vibrato.

## Dependency policy

Use the latest stable release of each dependency, **pinned to an exact version** so builds are reproducible.
Upgrade deliberately (one dependency at a time, build + test) rather than with floating version ranges.

| Dependency | Version | Where |
|---|---|---|
| JUCE | 9.0.4 | `cmake/Dependencies.cmake` |
| Catch2 | 3.16.1 | `cmake/Dependencies.cmake` |
| WebView2 SDK | 1.0.4258.31 | `cmake/Dependencies.cmake` |
| CPM.cmake | 0.43.2 | `cmake/CPM.cmake` |
| React / React DOM | 19.3.1 | `ui/package.json` |
| Vite | 8.3.4 | `ui/package.json` |
| @vitejs/plugin-react | 6.1.2 | `ui/package.json` |
| TypeScript | 7.0.2 | `ui/package.json` |
| zustand | 5.0.15 | `ui/package.json` |
| @juce-framework/webview | 1.0.0 | `ui/package.json` |
| lucide-react (all UI icons) | 1.59.0 | `ui/package.json` |
| Vitest (UI unit tests) | 5.0.5 | `ui/package.json` |
| @xyflow/react (node canvas) | 12.12.0 | `ui/package.json` |
| elkjs (graph layout, EPL-2.0 or GPL-3.0-or-later) | 0.12.1 | `ui/package.json` |
| libfaust (interpreter backend) | 2.88.0 | `cmake/Faust.cmake` |
| Faust libraries | commit f1d729e (pinned by Faust 2.88.0) | `cmake/Faust.cmake` |
| monaco-editor | 0.52.2 | `ui/package.json` |
| CLAP SDK (headers, MIT) | 1.2.9 | `cmake/Dependencies.cmake` |
| Free MIDI Chords (shipped MIDI files, MIT) | 20260914 | `cmake/Dependencies.cmake` |
| Rubber Band (time stretching, GPL-2.0-or-later, used under GPLv3) | 4.0.0 | `cmake/Dependencies.cmake` |
| @monaco-editor/react | 4.7.0 | `ui/package.json` |

Planned for later phases: sol2.
