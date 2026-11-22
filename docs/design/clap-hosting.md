# Design: CLAP hosting (phase 15)

Status: **being built**. The decisions are numbered C1–C12 and summarised in section 9.

## 0. As built so far (2026-11-01)

The owner's answers to section 10: C2's shape; a plugin installed as both formats is listed twice, tagged by format;
per-note modulation in v1 is Note Expression only. Linked notes have landed (linked-notes.md), so C12 can follow.

* **Built:** `ClapPluginFormat` and its instance (engine/src/ClapPlugin.cpp) on the CLAP C API alone: clap-helpers turned out
  not to be needed. Scanning in the child process (C3), audio, notes as MIDI (or converted to plain CLAP notes for a plugin that
  only takes those), parameters with the CLAP id as their stable id, state, latency, tails, gestures from the plugin (C4 at
  block start), thread-check (C5), requests queued to the message thread (C6), timers, and the embedded editor on Windows
  (app/src/ClapEditor.cpp). The plugin attaches once its window is on screen, and that window must be opaque: Windows doesn't
  show a child inside a layered window.
* **Native notes** (C7, C8, C11, C12): a plugin that takes CLAP's own notes gets them from `ClapNoteEncoder`, by note ids it
  hands out, with tuning, pressure and brightness on the 32-sample grid, sent on change. A voice's values keep going until
  `NOTE_END`, or 10 seconds for a plugin that never says. The sustain pedal is held back in the host. A linked note keeps the
  first note's id and glides by tuning, with `LinkGlide`, so it never retriggers and has no bend range. A plugin with only MIDI
  dialects keeps `MpeEncoder`.
* **Modulation** (C9): slots on `CLAP_PARAM_IS_MODULATABLE` parameters send `PARAM_MOD` at block rate, with no base tracked.
* **Per-note modulation** (C10): a slot fed by one source with notes going into it, on a parameter flagged
  `CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID`, sends one `PARAM_MOD` per note id on the 32-sample grid, with the parameter's other
  slots added in, since a voice then heeds only its own amount. Note Expression's values are each note's already. An LFO or
  ADSR gets an instance per note, run by the plugin node with that note's events, up to 32 notes at once, whose edits and
  automation reach all of them. What modulates such a source's own parameters doesn't reach those instances. Anything else on
  that input stays one value for the whole plugin.
* **Automation and whole-plugin modulation** go in as events on the 32-sample grid, sent on change, for every CLAP plugin (C4, C9).
* **Offline rendering and state:** export switches a plugin to `clap.render`'s offline mode, and project state uses
  `clap.state-context` where a plugin has it.
* **Identity:** a saved identifier names the file, so loading falls back to the plugin's CLAP id (`findMovedClapPlugin`): moving a
  .clap file doesn't make it missing once it has been rescanned.
* **Parameter lists that change** (2026-11-14): `params.rescan` with `CLAP_PARAM_RESCAN_ALL`, through `ParameterListSource`.
  Until an instance is first prepared, a state set reads the list again in place (whether or not the plugin says so), since
  nothing uses it yet. Once it runs, JUCE's list is fixed and the engine holds indices into it, so the app makes a new
  instance from the node's state (as "Run inside DAWg" does), which lists the new parameters; lanes on parameters that went
  stay unused. The same holds across the helper boundary.
* **Copies** (2026-11-14): `DuplicateStateSource` saves with `CLAP_STATE_CONTEXT_FOR_DUPLICATE`, in a helper too. A duplicated
  track's plugins start from that (`duplicateTrack`'s `pluginStates`, filled in by the app, which stamps the originals as
  well), and a pasted plugin node now starts from its source's state for a copy (`addNodes`' `pluginState`); before, it
  started from the plugin's defaults.
* **Editors on Linux** (2026-11-14): X11, through JUCE's `XEmbedComponent` as JUCE's VST3 hosting does, with
  `posix-fd-support` on JUCE's Linux event loop. Timers and file descriptors a plugin registers in `init()`, before its
  instance exists, are kept and taken over (Surge XT registers its editor's idle timer there; without it the editor stayed
  black). Surge XT 1.3.4's editor draws embedded, tried on Xvfb and in WSLg.
* **Not yet:** editors on macOS, which get the generic editor.
* **Testing:** tests/clap holds two small plugins for automated tests; `[.real-clap]` runs an installed plugin by hand. Surge XT
  1.3.4 loads, plays, saves and shows its editor.

The owner decided on 2026-10-28 that CLAP hosting is a 1.0 feature beside VST3 (PLAN.md, phase 15). This document
answers the question the plan asks first, whether JUCE 9 hosts CLAP itself, and then describes what a host layer
would have to cover in DAWg's terms, how CLAP's features map onto the voice graph, the modulation pool and per-note
curves, and what it offers for linked notes (phase 14).

## 1. Does JUCE host CLAP?

No. JUCE 9.0.4, the version DAWg pins (`cmake/Dependencies.cmake`), has no CLAP code at all, neither for building
plugins nor for hosting them:

* `modules/juce_audio_processors/format_types` holds the AU, LADSPA, LV2, VST3 and VST formats and nothing else. A
  search of the 9.0.4 source for "clap" finds only the General MIDI drum name "Hand Clap".
* `CHANGE_LIST.md` on JUCE's `develop` branch has no entry above 9.0.4 and does not mention CLAP.
* JUCE's roadmap updates (Q3 2024 and Q3 2025) promise the ability "to author CLAP plug-ins" in JUCE 9. Hosting is
  not mentioned in either. The JUCE 9 release announcement (21 July 2026) mentions CLAP once, as something
  "AudioProcessor v2" will be the foundation for; on 15 October 2026 the JUCE team said the first part of that work
  was "still some distance off".

Even if JUCE added a CLAP `AudioPluginFormat` later, it would host the plugin behind `juce::AudioPluginInstance`,
whose `processBlock` takes a `MidiBuffer` and whose parameters have one value each. That interface has no note ids,
no per-note parameter modulation and no modulation separate from the value: exactly the features that make CLAP
worth hosting for DAWg (section 5). The clap-juce-extensions maintainers made the same point in 2024: hosting CLAP
through JUCE would have to blur features like polyphonic modulation.

## 2. Options

| Option | What it is | Licence | Verdict |
|---|---|---|---|
| Wait for JUCE | Nothing on JUCE's roadmap for CLAP hosting | — | Not viable for 1.0; would lose per-note features anyway |
| clap-juce-extensions (free-audio) | Builds JUCE plugins as CLAP; its README says it does not host | MIT | Not applicable |
| juce_clap_hosting (Jatin Chowdhury) | A `CLAPPluginFormat` JUCE module | MIT | "Super-alpha": six commits, no GUI, no MIDI, unfinished state; its author calls it stale. Reference only |
| clap-host (free-audio) | The reference host: a Qt application | MIT | Reference for behaviour; not a library, and DAWg should not depend on Qt |
| CLAP SDK + clap-helpers (free-audio) | The C API headers, plus C++ helpers, including host-side ones: `host.hh` (a host base class), `plugin-proxy.hh` (calls into a plugin with misuse checks), `event-list.hh`, `reducing-param-queue.hh`, `note-end-queue.hh`, `misbehaviour-handler.hh` | MIT | **Recommended base** (C1) |
| Carla, Zrythm or another DAW's host | A full host to embed | GPL-2.0+ or AGPL | Far larger than the need, and built on its own engine |

MIT is compatible with AGPLv3; the CLAP SDK is header-only, and clap-helpers is header-only C++ with a CMake build,
so both come in through CPM like JUCE and Catch2.

**(C1) Write a CLAP host layer on the CLAP C API and clap-helpers.** Proposed. It is the only option that gives
DAWg CLAP's per-note features, and the API is small: a plugin is a struct of function pointers plus optional
extensions queried by name.

**(C2) Wrap it as a JUCE plugin format, with a native side channel.** Proposed. The layer implements
`juce::AudioPluginFormat` (`ClapPluginFormat`, format name "CLAP") and `juce::AudioPluginInstance`
(`ClapPluginInstance`). Everything DAWg already does with plugins then works unchanged: the scanner's child
processes, `KnownPluginList`, `PluginHost::load`, the state blob, the editor windows, latency changes, parameter
lists and automation gestures. Beside that, `ClapPluginInstance` offers a second, native entry point that
`PluginNode` uses when it finds one (by `dynamic_cast`): a process call that takes CLAP events (notes with ids, note
expressions, `PARAM_VALUE` and `PARAM_MOD` with sample offsets) instead of a `MidiBuffer` and `setValue`. The
alternative, a separate `ClapNode` beside `PluginNode` with its own scanner, loader, state and editor code, would
duplicate about a third of the work for nothing the side channel does not give.

If JUCE later ships its own CLAP hosting, DAWg keeps its layer: JUCE's would not carry the native features.

## 3. What hosting covers

### 3.1 Scanning

A `.clap` file (a bundle on macOS) exports `clap_entry`. Scanning calls `entry->init(path)`, gets the plugin
factory and reads each plugin's descriptor: id (reverse-DNS, e.g. "com.u-he.diva"), name, vendor, version and
feature tags ("instrument", "audio-effect", "note-effect" and so on). No plugin is instantiated, so a CLAP scan is
fast. One file can hold several plugins.

**(C3) Scan CLAP in the existing child process.** Proposed. `entry->init` loads the library and runs its static
initialisers, which can crash or hang, so `DAWg --scan-plugin CLAP <file> <out.xml>` runs it like VST3, with the
same timeout (`PluginScanner::perPluginTimeoutMs`). `ScanWorker` needs no change beyond registering the format.
Default locations follow the CLAP spec: `%COMMONPROGRAMFILES%\CLAP` and `%LOCALAPPDATA%\Programs\Common\CLAP` on
Windows, `/Library/Audio/Plug-Ins/CLAP` and `~/Library/Audio/Plug-Ins/CLAP` on macOS, `~/.clap` and `/usr/lib/clap`
on Linux, plus the `CLAP_PATH` environment variable everywhere. The `PluginDescription` keeps the file in
`fileOrIdentifier` and the CLAP plugin id in a field the format can read back when instantiating; `isInstrument`
comes from the "instrument" feature.

### 3.2 State

`clap.state` saves to and loads from a stream on the main thread. The bytes are the opaque blob DAWg already keeps
per node and saves as `plugins/<nodeId>.bin` in the project zip, so P3 (phase4-plugin-hosting.md) applies as it is:
the live instance is the source of truth and the blob is refreshed before removal, before saving and when the editor
closes. `clap.state-context` tells the plugin why it saves (project, preset or duplicate), so a duplicated node can
use the "duplicate" context. A CLAP plugin's state is not interchangeable with the same product's VST3 state.

### 3.3 Editors

`clap.gui` supports embedded views (the host gives a parent window: HWND, NSView or X11 window) and floating
windows. DAWg's editors are already native top-level windows owned by the app (P8), so the CLAP editor embeds into
a JUCE component inside that window (`HWNDComponent`, `NSViewComponent`, `XEmbedComponent`), with floating as the
fallback for plugins that only float. The host side implements `clap.gui`'s callbacks (`request_resize`,
`request_show`, `request_hide`, `closed`) and passes the display scale on Windows and Linux. On Linux the plugin's
GUI event loop needs `clap.timer-support` and `clap.posix-fd-support` from the host, driven from the JUCE message
loop.

### 3.4 Latency and tails

`clap.latency` is read on the main thread while the plugin is active. A plugin that changes its latency calls the
host's `latency.changed` during activation, or asks for a restart while active; either way DAWg re-reads it and
recompiles, which is the existing path (P7, `PluginNode::refreshLatency`). `clap.tail` reports a tail length, which
export (phase 9) can use to decide how long to render past the end, and `clap.render` lets export switch the plugin
to offline rendering.

### 3.5 Audio ports and sidechain

`clap.audio-ports` lists ports; DAWg uses the main stereo pair, and a second input port marked as a sidechain feeds
the node's key input (sidechain.md, K7). `clap.configurable-audio-ports` asks a plugin for stereo where it
supports it; otherwise the existing adaptation (mono duplicated, extra channels dropped) applies.

### 3.6 Parameters and automation

CLAP parameters have a stable `uint32` id, a plain-value range and flags. DAWg's automation lanes key a plugin
parameter by a stable id string (phase10-automation.md, A1); the CLAP id as decimal text is that string, so
`PluginNode::parameterList` and `parameterIndex` work through `HostedAudioProcessorParameter::getParameterID`.

**(C4) Automation goes in as sample-accurate `PARAM_VALUE` events.** Proposed. Lanes keep their normalised 0..1
values; the layer maps them to the plain range (rounding stepped and enum parameters). On the native path the value
goes into the block's input event list at its sample offset rather than through `setValue` before the block, which
removes the block-rate stepping for CLAP plugins. While the plugin is inactive, values go through `params.flush` on
the main thread. Changes made in the plugin's editor come back as output events: `PARAM_VALUE` with
`PARAM_GESTURE_BEGIN` and `PARAM_GESTURE_END`, which feed the touch of automation writing (A7). Values flagged
`CLAP_EVENT_DONT_RECORD` are not written. `params.rescan` with `CLAP_PARAM_RESCAN_ALL` (parameters added or
removed) refreshes the UI's parameter list; lanes on parameters that disappeared stay in the model, unused, like a
missing plugin's.

### 3.7 Threading

CLAP is strict about threads and tags every function. In DAWg's terms:

* **Main thread** is the JUCE message thread: `create_plugin`, `init`, `activate` and `deactivate` (which may
  allocate), state, GUI, parameter info and text, latency, `on_main_thread`, `destroy`.
* **Audio thread** is any engine thread that runs a track. DAWg renders tracks in parallel, so a plugin can see a
  different thread from block to block; CLAP allows that. `start_processing`, `stop_processing`, `process` and
  `reset` run there. `start_processing` is called lazily on the first `process` after activation.

**(C5) The host implements `clap.thread-check`.** Proposed. `is_main_thread` asks
`juce::MessageManager::existsAndIsCurrentThread()`; `is_audio_thread` reads a thread-local flag the engine sets on
its audio callback thread and its track workers (and on the export thread). Plugins use it to assert their own
threading, and clap-helpers' `plugin-proxy` uses it to catch DAWg's own mistakes in Debug builds.

**(C6) Host callbacks are queued to the message thread.** Proposed. `request_callback` (thread-safe) sets a flag
and triggers an `AsyncUpdater`, which calls `plugin->on_main_thread`. `request_restart` deactivates and
reactivates the plugin on the message thread, under the same suspension `PluginNode` already uses to re-prepare a
plugin (`suspendProcessing`), then re-reads latency, ports and parameters. `request_process` needs nothing, since
DAWg processes every plugin every block. Destruction stays on the message thread, as `SnapshotExchange` already
guarantees.

clap-helpers' `misbehaviour-handler` decides what happens when a plugin breaks a rule: log and carry on in Release,
assert in Debug. A hard assert is not an option because of the debug-assert dialogs that hang the tests.

### 3.8 Out-of-process hosting

Running plugins out of process is also a phase 15 item. CLAP's plain C API with explicit threads and an explicit
event list per block is easier to carry over a process boundary than `juce::AudioPluginInstance`, so C1 does not
make that harder. The design for it is separate.

## 4. Notes in: dialects

`clap.note-ports` says which note "dialects" a plugin accepts: CLAP's own note events (`CLAP`), plain MIDI
(`MIDI`), MPE over MIDI (`MIDI_MPE`) and MIDI 2.0 (`MIDI2`), with a preferred one.

**(C7) Use the CLAP dialect when offered, otherwise the existing MIDI and MPE encoders.** Proposed. A plugin that
supports `CLAP` notes gets native note events (section 5) and the node's MIDI settings (`setPluginMidi`) do not
apply; the inspector says so. A plugin that only takes MIDI or MPE gets what `MpeEncoder` produces today, wrapped in
`clap_event_midi`, with the existing per-node mode and bend range. MIDI 2.0 is not used in v1.

## 5. What sets DAWg apart, in CLAP

### 5.1 Note expression by note id

A CLAP note is addressed by `(port, channel, key, note_id)`. The host chooses the note id (a non-negative
`int32`). `NOTE_EXPRESSION` events then change one note: `TUNING` (relative, −120 to +120 semitones, as a double),
`VOLUME`, `PAN`, `VIBRATO`, `EXPRESSION`, `BRIGHTNESS` and `PRESSURE`. Values are absolute, not cumulative, and
sample-accurate; smoothing is the plugin's job.

**(C8) Per-note curves go out as note expressions on the note's id.** Proposed. Per note:

* `NOTE_ON` with `key = round(basePitch)`, channel 0, velocity 0..1, and a note id the node allocates from a
  per-instance counter and maps from the sequencer's note (live audition notes have negative DAWg ids, so DAWg ids
  cannot be passed through).
* `TUNING` = `basePitch + pitchCurve + vibrato − key`, the same quantity as today's MPE bend, but over ±120
  semitones in double precision, with no bend range to configure and no RPN set-up.
* `PRESSURE` from the pressure curve, `BRIGHTNESS` from timbre (today CC74).
* Values evaluated on the same 32-sample grid as `MpeEncoder`, sent only when they change.

There is no channel per note, so the 15-note MPE limit and the oldest-note release that comes with it go away;
polyphony is the plugin's. Two notes on the same key at once need `clap.voice-info` with
`CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES`; without it the node ends the earlier note first, as MIDI would.

### 5.2 Modulation that adds to the automated value

`PARAM_MOD` carries a modulation amount separate from the value: "the value heard is: param_value + param_mod",
with the amount in plain units. Parameters that accept it have `CLAP_PARAM_IS_MODULATABLE`. The plugin keeps the
two apart, so its own UI can show the automated value and the modulation around it.

Today the modulation pool (modulation.md, M5) writes `base + modulation` into the plugin's parameter and works out
the base by comparing the parameter with what it last wrote. That detection is what breaks per voice, and it makes
automation and modulation fight over one value.

**(C9) Modulation slots on CLAP parameters send `PARAM_MOD`.** Proposed. For a CLAP target with
`CLAP_PARAM_IS_MODULATABLE`, automation and manual edits send `PARAM_VALUE` and the slot sends
`PARAM_MOD = source × depth × (max − min)`, the same scaling as M5 in plain units. No base is tracked; a removed slot
sends an amount of 0. Slots on one parameter still sum. Because events carry sample offsets, the slot can be sent
on the 32-sample grid instead of once per block, removing the block-rate limit (modulation.md, section 3) for CLAP
plugins. Parameters without the flag, and all non-CLAP plugins, keep M5 as it is.

### 5.3 Per-voice modulation

`PARAM_MOD` and `PARAM_VALUE` can address one note: a parameter flagged `CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID`
accepts a modulation amount for one voice, matched by note id. CLAP's rule is that the voice uses the polyphonic
amount only, and that amount already includes the monophonic one.

Hosted plugins are not voice nodes (phase12-voice-scope.md, V6): a plugin instrument sits in the track graph and
is polyphonic itself. Per-voice modulation therefore means per-note modulation sources driving a track-level plugin.

**(C10) Per-note sources on a CLAP instrument's slots send per-note `PARAM_MOD`.** Proposed. When the source of a
slot on a CLAP instrument is note-scoped (Note Expression; an ADSR or a retriggered LFO driven by the instrument's
notes), the node evaluates it once per sounding note and sends one `PARAM_MOD` per note id, plus the monophonic sum
added in, as the rule above requires. Each note's own ADSR, pressure or timbre then reaches the plugin's filter
cutoff per voice, which is what the voice graph does for built-in nodes (V12). Whether this needs a per-note
evaluation of sources that today run once per track is the main design question left (section 10, question 3);
v1 could start with Note Expression only, which is already per note.

### 5.4 Note ends and release tails

A CLAP host assumes a voice per note id lives until the plugin says otherwise: after `NOTE_OFF` the plugin sends
`NOTE_END` with the same id once the voice has finished its release. `NOTE_CHOKE` from the host silences voices at
once.

**(C11) `NOTE_END` ends a note's per-note state.** Proposed. The node keeps sending a note's expressions and per-note
modulation after `NOTE_OFF` (a per-note ADSR's release, a pitch curve that continues into the release) until
`NOTE_END`, then frees the id. With per-note modulation this is required: without it a release would lose its
modulation. Plugins that never send `NOTE_END` are covered by a timeout. DAWg's `choke` and `chokeAll` map to
`NOTE_CHOKE` (by id, or with wildcards); `releaseAll` sends `NOTE_OFF` for every sounding note. `NOTE_END` also gives
the UI a sounding-voice count for plugin instruments, as the voice instrument reports today (V7).

## 6. Linked notes and glides (for phase 14)

CLAP has no legato, glide or portamento event; none of its headers mention them. A link can be expressed in two
ways:

* **One note id across the link.** The linked note sends no `NOTE_ON`. The first note's id carries on, and `TUNING`
  glides from the first pitch to the second over the link's time or curve, then follows the second note's own
  pitch curve. The voice, its envelope and any per-note modulation on that id continue. This is the same model as
  the phase 14 plan for MPE (keep the member channel and bend), without the bend range. What does not carry: the
  second note's velocity (there is no event for it; `VOLUME` is not the same thing) and its key, so key-tracked
  parts of the plugin stay at the first key, as with MPE. `NOTE_OFF` is sent once at the end of the chain, and
  `NOTE_END` arrives once.
* **Overlapping notes.** The second note's `NOTE_ON` is sent before the first note's `NOTE_OFF`, and the plugin's own
  mono or legato mode glides. This works only when the patch is mono or legato, and the glide is the plugin's.
  `clap.voice-info` with `voice_count == 1` says the current patch is mono.

**(C12) Linked notes share one CLAP note id and glide by `TUNING`.** Proposed, by default; the overlapping form
could be a per-node option for mono patches. For phase 14 it helps if the model gives a chain of linked notes one
identity at the plugin boundary ("plugin note"), used for the MPE member channel and the CLAP note id alike, so the
two paths stay one.

## 7. Size

Rough sizes, in lines of C++ without tests, for C1 with C2:

| Part | Lines |
|---|---|
| `ClapPluginFormat`: entry loading, factory, descriptors, default paths, scan | 300 |
| `ClapPluginInstance`: activate, process, audio ports, event lists, transport | 800 |
| Parameters, `flush`, rescan, gestures, automation mapping | 400 |
| State and state context | 100 |
| GUI: embedding on three platforms, resize, scale, Linux timers and fds | 400 |
| Threading: thread-check, `request_callback`, `request_restart` | 200 |
| `PluginNode` native path: note ids, note expressions, `PARAM_MOD`, `NOTE_END` | 500 |
| UI: format shown in the browser, MIDI settings hidden for CLAP notes | 100 |

About 2,800 lines, plus about 600 of tests. Tests need a CLAP plugin; a small test plugin built in-tree with
clap-helpers (a sine instrument that reports its events) avoids depending on installed plugins. Per-note sources
(C10) beyond Note Expression are extra and depend on question 3. In total roughly the size of phase 4.

Suggested steps: format and scan; instance with audio, notes as MIDI, state and latency; editors; parameters and
automation (C4); native notes and expressions (C8); `PARAM_MOD` (C9); per-note modulation and `NOTE_END` (C10,
C11); linked notes (C12) once phase 14 has landed.

## 8. Risks

* **Plugin behaviour varies.** Threading mistakes are common in young formats. Thread-check (C5) and the
  misbehaviour handler help find whose mistake it is; Bitwig, Reaper, FL Studio and Studio One already host CLAP,
  so most plugins have been tried against real hosts.
* **Editor embedding** is the platform-specific part: three windowing systems, DPI scaling on Windows, and the
  Linux event loop. Test on all three before calling it done.
* **JUCE changes.** AudioProcessor v2 will change JUCE's processor and parameter interfaces. C2 wraps the layer in
  JUCE's current ones, so it moves with VST3 when DAWg adopts v2.
* **Duplicates.** Many plugins install as both VST3 and CLAP, so the browser lists them twice. States do not
  transfer between the two (section 10, question 2).
* **Identity.** A CLAP plugin is identified by its id string, not by its file; moving the file must not make it
  missing. The stored `PluginRef::identifier` should be built from the id.
* **Per-note modulation cost.** One `PARAM_MOD` per note per slot per 32 samples is a lot of events for wide chords;
  send only on change, as the MPE encoder does.

## 9. Decisions

| # | Decision | Status |
|---|---|---|
| C1 | Own CLAP host layer on the CLAP SDK and clap-helpers (MIT) | Proposed |
| C2 | Wrapped as a JUCE `AudioPluginFormat`, with a native process path for `PluginNode` | Proposed |
| C3 | Scan in the existing child process; CLAP default paths and `CLAP_PATH` | Proposed |
| C4 | Automation as sample-accurate `PARAM_VALUE`; editor changes as gestures | Proposed |
| C5 | The host implements thread-check; every engine thread counts as an audio thread | Proposed |
| C6 | Host requests are queued to the message thread; restart under suspension | Proposed |
| C7 | CLAP note dialect when offered, otherwise the existing MIDI and MPE encoders | Proposed |
| C8 | Per-note curves as note expressions on host-allocated note ids | Proposed |
| C9 | Modulation slots send `PARAM_MOD` on modulatable CLAP parameters; no base tracking | Proposed |
| C10 | Note-scoped sources send per-note `PARAM_MOD` to CLAP instruments | Proposed |
| C11 | Per-note state lives until `NOTE_END`; chokes map to `NOTE_CHOKE` | Proposed |
| C12 | Linked notes keep one note id and glide by `TUNING` | Proposed |

## 10. Questions for the owner

1. Is C2 the right shape, a JUCE format with a native side path, or should CLAP have its own node type?
2. When a plugin is installed as both VST3 and CLAP, should the browser show one entry and prefer CLAP for new
   nodes, or list both?
3. Per-note modulation of a plugin (C10): v1 with Note Expression only, or should an ADSR or LFO wired to a CLAP
   instrument run per note as well? The second needs per-note evaluation of track-level sources, which does not
   exist yet.
4. Should note expressions also feed the CLAP dialect's `VOLUME` and `PAN` from new per-note curves, or stay with
   pitch, pressure and timbre as today?
5. Linked notes (C12): one note id with a `TUNING` glide by default, with the overlapping-notes form as an option for
   mono patches?

## Sources

* JUCE 9.0.4 source, `modules/juce_audio_processors/format_types`, and `CHANGE_LIST.md` on `develop`.
* JUCE roadmap updates, Q3 2024 and Q3 2025: https://juce.com/blog/juce-roadmap-update-q3-2025/
* "JUCE 9 is available now", JUCE forum, 21 July 2026: https://forum.juce.com/t/juce-9-is-available-now/69175
* "FR: Support CLAP for plugins (host & client)", JUCE forum:
  https://forum.juce.com/t/fr-support-clap-for-plugins-host-client/51860
* clap-juce-extensions, discussion 152, "CLAP host support" (June 2024):
  https://github.com/free-audio/clap-juce-extensions/discussions/152
* juce_clap_hosting: https://github.com/jatinchowdhury18/juce_clap_hosting
* CLAP SDK headers (`events.h`, `ext/params.h`, `ext/note-ports.h`, `ext/voice-info.h`):
  https://github.com/free-audio/clap
* clap-helpers: https://github.com/free-audio/clap-helpers
* clap-host: https://github.com/free-audio/clap-host
