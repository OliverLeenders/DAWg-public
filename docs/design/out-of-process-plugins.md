# Design: plugins out of process (phase 15)

Status: **built on Windows and Linux**, on by default on Windows (section 0). macOS is still to come. The decisions are numbered O1–O11 and summarised in section 11. The owner accepted the proposed answers on 2026-11-02
(section 12).

## 0. As built so far

Steps 2 to 7 are in, on Windows only and off by default: the `pluginsOutOfProcess` user setting or `DAWG_PLUGIN_PROCESSES=1`, with no
UI yet. `RemotePluginInstance` from O1 is `RemoteProcessor` in engine/src/RemotePlugin.cpp. One helper per plugin file (O8a) is already
how it works. JUCE's `ChildProcessCoordinator` is the control pipe (O6), and `dawg_tests` is its own helper in the tests.

* **Works:** load, audio, notes as MIDI and by id, tuning and expressions, linked notes, parameters with their text, automation inside a
  block, whole-plugin and per-note modulation, sidechain, transport, state, latency, offline mode, and helpers going away with the app.
  Surge XT plays through it in the app, and DAWg keeps running when its helper is killed.
* **Differs from O5:** events go with each block rather than through rings, since only one block is ever on its way. They wait in a queue on
  the app's side, and with a block that couldn't go, MIDI too, so a late block loses no notes (O4). The plugin's own parameter changes and
  gestures come back over the pipe, so a value read on the app's side can be a few milliseconds behind. Since nothing shared holds them,
  no table of atomics is needed.
* **Step 4 (O4, O8):** a watch thread per helper waits on its process handle, so a crash is known at once rather than at JUCE's ten-second
  ping. It closes a helper whose blocks have all been late for 3 seconds, or whose message thread hasn't answered for 20, unless a load or
  prepare is holding that up. Every instance in the helper goes to `Status::crashed`, reports no latency, and offers Reload in place, in a new
  helper, with its last state; getting the state of a crashed plugin returns that too, so a save can't lose it. Snapshots are taken every 30
  seconds of a plugin whose parameters moved. A plugin that crashes while loading shows as crashed too, and Reload loads it again.
* **Differences from section 7:** the last output isn't faded, so a crash can click; and Reload blocks the UI while the plugin loads.
* **Step 5 (O7):** editor windows, as in section 5. A plugin with an open editor has its state kept every 30 seconds whether or not a
  parameter moved, since an editor can change what no parameter shows. A crash closes the window; Reload doesn't open it again.
* **Step 6 (O9):** on by default on Windows. "Separate processes" in the plugin panel switches it, and "Run inside DAWg" in a slot's menu makes
  an exception for one plugin, kept by identifier in user settings. Both apply at once: the plugin's state goes to the model, the compiler
  forgets the instance, and the next compile loads a new one. The audio thread goes on with the old engine until then. The CPU figure in the top
  bar turns amber for 10 seconds after a late block, and its tooltip names the plugins.
* **Step 7, on Windows, on 2026-11-02:** Vital, Kontakt 8, Serum 2, Element, MT-PowerDrumKit, MCompressor, MAutoPitch, MCharmVerb, MAnalyzer,
  ZL Equalizer 2 and OTT each loaded and opened their own editor in a helper, with no late blocks. Vital's and MCompressor's windows
  were looked at, not used. Plugin Alliance's plugins need a login, so none was tried. Kontakt's editor took 8.5 seconds to appear, and Kontakt was
  not played, having no instrument loaded.
  * Export of Vital, MAutoPitch (1024 samples of latency) and OTT: peak and LUFS equal to in-process, and the difference is at the level of
    the dither (-93 dB).
  * 12 Vitals playing chords, on the machine's audio settings: audio load 15.9% against 12.3% in process, peak 18.7% against 16.5%, 0 late blocks.
    The one helper held 1.7 GB.
  * Vital's volume, changed by automation, survived moving into DAWg and back, and a killed helper and Reload.
  * Killing Kontakt's helper left Vital playing in its own.
* **Linux, on 2026-11-14:** the same code, with the platform's pieces swapped: the block's memory is POSIX shared memory in
  /dev/shm (removed by the app when the instance goes), the two sides wake each other with futexes on the block's own request and
  done counters (so no events need sharing), a helper's end is watched through a pidfd (without one, a kernel before 5.3, only the
  ping notices it), and a hung helper is killed with SIGKILL. The helper's block thread gets real-time scheduling from JUCE's
  real-time thread where the system allows it; there is no MMCSS. All 19 helper tests pass in WSL (Ubuntu 24.04). It is **off by
  default** on Linux until step 7 has been done there with real plugins; `DAWG_PLUGIN_PROCESSES=1` or "Separate processes" turns it
  on. Not done on Linux: whether a helper's window has the focus (`remoteHelperIsForeground` says no), and timing on a real audio
  device (WSL has none).
* **Step 7 on Linux, begun on 2026-11-14:** Surge XT 1.3.4 (CLAP) in WSL, by the hand-run test `[.real-remote]`
  (`DAWG_REAL_PLUGIN=<file>`): 775 parameters, plays (peak 0.27), no late blocks, a moved parameter survives its state into a
  second instance, and after its helper is killed with SIGKILL it shows as crashed, reloads, plays again and keeps the value.
  Editors, more plugins and a real audio device are still to be tried before it goes on by default.
* **Still open:** macOS has no helper yet, so plugins run inside DAWg there. Long sessions and real mouse and keyboard use in editors
  are untested. Reload and a switch block the UI as long as a plugin takes to load.

## 1. Goals and non-goals

Goals:

* **A plugin that crashes or hangs doesn't take DAWg down.** The project stays open, other tracks keep playing, and the plugin
  can be reloaded.
* **Nothing else changes for the user.** No added latency in normal use, the same editors, automation, modulation, per-note
  notes (CLAP), sidechains, state and export.
* **The engine's guarantee holds.** The audio thread never allocates, locks or waits without a limit, whatever a plugin does.
* VST3 and CLAP alike, on Windows, macOS and Linux.

Non-goals for v1:

* Bridging 32-bit plugins or other architectures.
* Isolating built-in nodes and Faust. They stay in process: they are ours, and a voice graph runs thousands of them.
* Protection from malicious plugins. A child process runs as the same user. This is about faults, not security.
* Saving what a plugin did since its last state snapshot (O8).

## 2. What exists

* `PluginNode` holds a `juce::AudioProcessor` and uses it through `processBlock`, its callback lock, parameters and latency.
  `dynamic_cast` finds `ClapNativeInput` (notes by id, per-note modulation, events) and `ClapEditorAccess`.
* Almost everything else reaches a plugin through that processor: `PluginHost::load`, state in `MainComponent`, `PluginWindows`,
  automation names in `AutomationControl`, and export's `setNonRealtime`.
* **Editors are already separate top-level windows** (phase4-plugin-hosting.md, P8). Nothing embeds a plugin in the web UI.
* The scanner already runs plugins in a child process: `DAWg --scan-plugin`, with `ScanWorker`, so the executable can already act
  as a helper.
* Tracks render on a small worker pool, and the audio thread spins until they're done. A wait on a plugin happens on one of
  those threads, and only one block is ever in flight.
* State is a blob per node, refreshed at defined moments (P3). Autosave and crash recovery exist for the project itself.
* `Status` already has loading and failed states, in which an effect passes audio through and an instrument is silent.

## 3. Shape

**(O1) A stand-in processor, so `PluginNode` barely changes.** `RemotePluginInstance` derives from `juce::AudioPluginInstance` and
implements `ClapNativeInput`; editors go their own way (O7). Its `processBlock` hands the block to the helper and
waits for the result. `PluginHost::load` creates one in place of a real instance. Everything above it keeps working, and the
compiler, the modulation pool and the encoders don't learn that a process boundary exists.

```
DAWg                                            DAWg --plugin-host <name>
PluginNode -> RemotePluginInstance  <-shared memory->  HostedInstance -> VST3 or ClapPluginInstance
              (stand-in processor)  <-control pipe-->  (and its editor window)
```

The helper is the same executable with another switch, as for scanning, and it uses the same format code. Plugin formats stay on
the helper's side, except that the app still registers them to scan and list.

## 4. Audio and events across the boundary

**(O2) Synchronous hand-off in the same block, if it measures well enough.** Per instance, one block of shared memory holds the
input and output audio, an event ring and the transport. The stand-in writes the input, signals the helper's thread for that
instance and waits; the helper runs the plugin, writes the output and signals back. No latency is added, and sample offsets mean
what they mean today.

The cost is two thread wake-ups per plugin per block. That is the design's main unknown: a plugin-heavy project at a 64 or 128
sample block has little time to spare. **Step 1 is a benchmark with no plugins in it**: round trips across N processes on the
real worker pool, at 64 to 512 samples, on Windows first, reporting the 99.9th percentile rather than the average. Proposed bar:
under 50 µs per plugin at the 99.9th percentile, and 50 instances at 128 samples without a dropout at otherwise idle load. If it
misses, the fallback is an optional **one block of added latency** with the helper running ahead, reported through the existing
delay compensation, which needs no waiting at all but can't be used for live monitoring without being felt.

### 4.1 What step 1 measured (2026-11-02)

`tools/ipcbench`, on an 8-core, 16-thread i7-10875H under Windows 11: real child processes with a thread per instance, shared memory,
events for the wake-up, a 30 µs spin on the waiting side, MMCSS "Pro Audio" on both sides, blocks paced as a device would, and the
audio thread plus seven workers taking instances like the engine's pool. The helper only copies the audio unless noted, so these are
the boundary's cost alone. Runs last 10 seconds unless noted; the machine was in ordinary use, not quiet.

| Case | Round trip p50 / p99.9 / max (µs) | Block p99.9 (µs) of its length | Overruns |
|---|---|---|---|
| 1 plugin, 128 samples | 13 / 96 / 218 | 117 of 2667 | 0 of 3750 |
| 50 plugins, 64 samples | 9 / 69 / 929 | 320 of 1333 | 0 of 7500 |
| **50 plugins, 128 samples** | 9 / 83 / 269 | 253 of 2667 | 0 of 3750 |
| 50 plugins, 128 samples, 3 minutes | 8 / 88 / 372 | 354 of 2667 | 0 of 67500 |
| 100 plugins, 64 samples | 8 / 80 / 306 | 351 of 1333 | 0 of 7500 |
| 50 instances in 10 processes, 128 samples | 8 / 78 / 259 | 287 of 2667 | 0 of 3750 |
| 50 plugins, 128 samples, 20 µs of work each | 29 / 111 / 335 | 523 of 2667 | 0 of 3750 |
| Chain on one thread, 16 plugins, 64 samples | 4 / 56 / 239 | 323 of 1333 | 0 of 7500 |
| Chain on one thread, 50 plugins, 64 samples | 4 / 47 / 316 | 626 of 1333 | 0 of 7500 |
| 50 plugins, 128 samples, all cores busy | 11 / 27 / 277 | 439 of 2667 | 0 of 3750 |
| The same, **without real-time priority** | 9 / 88 / 72740 | 37450 of 2667 | **50 of 3750** |

What it says:

* **The second bar passed, the first did not.** 50 instances at 128 samples never overran, also over three minutes. The per-plugin bar of
  50 µs at the 99.9th percentile was missed in the parallel runs, at 70 to 95 µs. That bar was stricter than what matters: parallel
  plugins overlap, so the block's total is the real limit, and it stayed near 10 % of the block. The design goes on with no added latency,
  so the fallback of one block of latency isn't needed on this machine.
* **Real-time priority is not optional** (O3). Without it, with the cores busy, a round trip reached 73 ms and blocks overran.
* **Long chains at the smallest blocks are the tight case.** 50 plugins in a row at 64 samples spent up to 47 % of the block on the boundary
  alone, before any plugin does anything. Typical chains are far shorter, but this overhead is real and adds up on one thread.
* Spinning longer didn't help (200 µs: p99.9 77), and not spinning made the typical trip slower but not the tail (p50 16, p99.9 69), so
  the spin only saves the typical case some time. Grouping processes made no visible difference.

What it doesn't say: no real plugin ran, so cold caches after the switch aren't in these figures, and they would matter for large plugins.
One machine, Windows only, no audio device involved, with the blocks timed by the program itself. macOS and Linux need their own runs.

**(O3) Short spin, then an OS wait, with real-time priority on the other side.** The waiting thread spins briefly, since the pool
already spins, then waits on a named semaphore or futex. Without priority the helper's thread would lose to everything else, so
it takes MMCSS "Pro Audio" on Windows, joins the device's audio workgroup on macOS where the OS allows that across processes, and
requests real-time scheduling on Linux. The macOS part is the least certain, and step 1 includes it when that platform is reached.

**(O4) Every wait has a deadline, and a late plugin is treated like a dropout.** The audio thread gives up at a deadline derived
from the block's length. It then passes audio through (an effect) or writes silence (an instrument), as a loading plugin does,
and counts a late block for the UI. The plugin's result is dropped when it does arrive, tagged by a sequence number, and no new
block is posted until the helper is free again. Events that couldn't go keep waiting in the ring, so a note-off is never lost. A
plugin that is late for a long time is reported as hung (O8). During export there is no real-time deadline, only a long limit
that detects a hang.

**(O5) Events and parameters use lock-free rings in the shared memory, fixed in size at prepare.** Notes as MIDI bytes or as the
`ClapNativeInput` calls, serialised with sample offsets. Parameter changes go both ways in rings: automation and modulation one
way, editor changes and gestures the other. A table of atomics holds current values, so `parameterValue` on the audio thread
stays a plain read and modulation's base detection still works. The stand-in's parameter objects mirror the list sent at load.
Full rings drop as they do today, and the drop is counted.

**(O6) Everything else goes over a control pipe, off the audio thread.** Load, prepare, state, parameter text, latency changes,
restart requests and editor commands are messages on a named pipe or socket. The stand-in's calls that JUCE requires to be
synchronous, such as parameter text and `getStateInformation`, wait on the message thread with a timeout and return nothing when
it passes, so a hung plugin can't freeze the UI for long. `prepare` is asynchronous as far as the engine is concerned: the node
stays in its loading state until the helper answers.

## 5. Editors

**(O7) The editor window belongs to the helper, as a window of its own.** Today's editors are top-level windows already, so the
helper opens the same kind of window, with `createClapEditor` or the plugin's own editor inside, and the app only asks for it to
open and close and remembers its position. No window crosses a process boundary on the inside: reparenting a child window across
processes joins their input queues on Windows, so a hung plugin would freeze DAWg's UI, which is what this work is meant to end.

The window is not owned by the main window either. An owner in another process joins the two input queues just as a parent does, so a
hung plugin could still freeze DAWg's UI. The app instead does what it does for its own plugin windows (phase4-plugin-hosting.md, P8): it
tells the helper to float the window while DAWg is the active app, counting a helper's window as DAWg, and to hide it while the main window
is minimised. This also answers X11 and macOS without a separate mechanism, to be tried there.

Keys go to the plugin's window and no further. Space for play doesn't reach the app from a plugin window, the same as an in-process one.
`ClapEditor`'s embedding runs in the helper unchanged, and the macOS and Linux embedding it still lacks is needed either way.

## 6. Which plugins share a process

**(O8a) One helper per plugin file by default.** All instances from one file share a process, with one audio thread per instance
since tracks render in parallel. A crash takes down only that plugin's instances, memory is shared between instances of one plugin
(samples, wavetables), and products whose instances talk to each other keep working. The other choices:

| Grouping | A crash takes down | Cost | |
|---|---|---|---|
| One helper for all plugins | every plugin | lowest | Little better than now |
| **One per plugin file** | that plugin's instances | moderate | **Proposed** |
| One per instance | one instance | highest; multi-instance products break | A later per-node option |

A helper's memory and load time come on top of the plugin's, and some plugins are slow to start. Whether a helper can be
kept warm is left until real numbers exist.

## 7. A plugin that fails

**(O8) A crash becomes a state of the node, and the user decides about the rest.**

* **Detecting it:** the pipe closing, or the process handle signalling, is a crash. Late blocks going on for a few seconds, or an
  unanswered ping, is a hang, and the app kills the helper.
* **Sound:** every instance in that helper goes to the existing pass-through or silence, from the next block on. Its last output
  is faded where one is at hand, to limit the click; output protection still stands behind it.
* **UI:** a new `Status::crashed` on the node, with the plugin's name in a banner and a Reload button in the inspector and the
  mixer. The node stays in the model, with its blob, lanes and connections.
* **Reload:** a new helper, the last state blob, the same parameter ids. What changed since the last snapshot is lost, so a snapshot
  is added: after an editor closes, on save (both exist), and every 30 seconds for a plugin whose parameters moved, on the helper's
  message thread so nothing else waits. The snapshot is opt-out per plugin family if some prove slow.
* **Restarts are never automatic in a loop.** A plugin that crashes on load or on a certain note would crash again. I propose one
  button and no automatic restart in v1. A failed plugin in a saved project opens as crashed, not as a dialog.
* **Latency:** the node reports none while it is down, which recompiles as bypass does today, and its own on the way back.

## 8. Threads and real-time rules

* The app's audio thread and workers never allocate or lock for this. They write to rings, signal and wait with a deadline.
* The helper's audio thread is the plugin's audio thread for `clap.thread-check` and VST3 alike; the helper's main thread runs its
  JUCE message loop.
* Allocation counting moves to the helper's side, reported over the pipe, so the existing plugin allocation readout keeps working.

## 9. What the user sees and sets

**(O9) One setting, on by default once it passes step 1.** Settings get "Run plugins in separate processes", and a project doesn't
store it, since it changes nothing about the sound. A per-plugin "Run inside DAWg" exception covers products that don't cope,
kept with the plugin list in user settings rather than the project. Nothing about the project file changes.

The status line reports plugins that are late, with a tooltip naming them. A plugin that is merely too slow shows there, not as a
crash.

**(O10) Costs that stay.** Some memory per helper, one more context switch per plugin per block, a slower first load, and editor
windows that are less tightly tied to the main window on macOS. Some copy protection may object to a helper process; this is what
the in-process exception is for.

## 10. Testing and steps

**(O11) Bad plugins are built in.** The test plugin in tests/clap gets switches through a parameter: crash now, crash on a note,
hang, sleep for a block, write out of bounds on another thread, and allocate on the audio thread. Tests start a real helper, using a
small helper target beside `dawg_tests`, and check what the engine does: other tracks unaffected, no thread blocked past its
deadline, a note-off delivered after a late block, reload restoring state, and no allocation in the parent. `[.real-clap]` runs
Surge XT through it by hand.

Steps, each with tests and approved before it starts:

1. **Benchmark** (O2, O3): shared memory and wake-ups only. It decides synchronous or one block of latency, and whether this goes on.
2. **Helper and stand-in**: process start, load, audio, notes as MIDI, state and latency, behind the setting, off by default.
3. **Rings** (O5): parameters both ways, gestures, native notes, per-note modulation, sidechain.
4. **Failure** (O4, O8): deadlines, crash and hang detection, `Status::crashed`, reload, snapshots, UI.
5. **Editors** (O7): the helper's windows, floating, positions.
6. **Grouping and settings** (O8a, O9): one helper per file, the in-process exception, export.
7. **Real plugins**: a wide set by hand on Windows, then macOS and Linux, before it goes on by default.

Rough size: about 3,000 lines beside about 800 of tests, similar to phase 4 and the CLAP host together, with step 1 only a few days.

## 11. Decisions

| # | Decision | Status |
|---|---|---|
| O1 | A stand-in `juce::AudioPluginInstance`, so `PluginNode` and everything above it barely change | Accepted |
| O2 | Synchronous hand-off with no added latency (section 4.1) | Measured on Windows, accepted |
| O3 | Spin, then wait, with real-time priority in the helper, which measuring showed is required | Accepted |
| O4 | Every wait has a deadline; a late plugin passes audio or is silent for that block, and loses no events | Accepted |
| O5 | Events and parameters in lock-free rings in shared memory, with a table of current values | Accepted |
| O6 | Everything else on a control pipe, with timeouts on the message thread | Accepted |
| O7 | Editors are the helper's own windows, floated and hidden on the app's say; no parent or owner across processes | Accepted |
| O8 | A crash is a node state with a Reload button; no automatic restart; state snapshots | Accepted |
| O8a | One helper per plugin file | Accepted |
| O9 | One setting, on by default once proven, with a per-plugin in-process exception; no project format change | Accepted |
| O10 | The remaining costs are accepted, as listed | Accepted |
| O11 | Misbehaving plugins built into the tests | Accepted |

## 12. The owner's answers (2026-11-02)

1. **Default:** on for everyone once proven, with the per-plugin exception.
2. **If the benchmark missed:** it didn't call for added latency, so the question fell away on Windows. It comes back if macOS or Linux measure worse.
3. **Grouping:** one helper per plugin file.
4. **After a crash:** a Reload button, no automatic restart.
5. **macOS editors:** a floating window is acceptable, to be tried on a Mac.

## 13. Not verified

Beyond section 4.1 nothing here has been built or timed. Priority behaviour on macOS and Linux, and the macOS workgroup and window claims,
come from general knowledge of those platforms, not from tests on them, and copy protection's behaviour is a guess until real products
are tried.
