# Phase 11 design: Faust scripting and node panels

Status: **implemented** (see section 7). Faust decisions are F1–F8, panel decisions P1–P5; all are summarised in
section 6. The UI decisions are in section 5.

## 1. Goals and non-goals

Goals:

* Write effects in Faust inside DAWg. A Faust node compiles its script live, gets its ports and parameters from
  the script, and can be swapped while playing without dropping out. Its parameters can be automated and MIDI
  learned like any node parameter.
* Graphical panels for the built-in nodes:
  * EQ: a response curve with draggable bands and a spectrum analyser;
  * compressor: a transfer curve and a gain-reduction meter;
  * filter: a response curve;
  * utility: a stereo width display.
* Generated controls for Faust nodes, laid out the way the script groups them.
* A small library of example scripts, and a place to keep your own.

Non-goals:

* Polyphonic Faust instruments (one instance per note). Those come with the voice-scope graph in phase 12, which
  is where per-note processing lives.
* Faust MIDI mapping metadata (`[midi:ctrl 7]`). DAWg's MIDI learn covers it.
* The Loop Reverb itself (docs/design/loop-reverb.md). It came after this phase, as a built-in node with its own panel.

## 2. Faust

**(F1) libfaust, interpreter backend first.** Recommended.

* libfaust compiles Faust source to an executable DSP at runtime. It has two backends:
  * **LLVM**: native-speed code, but it pulls in LLVM (tens of MB per platform, a heavy build).
  * **Interpreter**: compiles to bytecode that a small virtual machine runs, roughly 3–10× slower than native,
    with no LLVM.
* v1 builds libfaust from source with the interpreter backend through CPM, like JUCE. It is small, builds the
  same on all three platforms, and an effect with a few dozen operations per sample costs little.
* The LLVM backend can come later behind a CMake option. The node is written against libfaust's `dsp` interface,
  so the backend doesn't matter to it.
* **Licensing.** libfaust is LGPL 2.1 and the Faust libraries are mostly LGPL, MIT or STK-4.3. All of these
  combine with DAWg's AGPL-3.0. The bundled libraries' licences get listed in the third-party notices.

**(F2) A Faust node holds its script.**

* `GraphNode` gains `code` (the Faust source) for the new type `dawg.faust` ("Faust"). Its parameters live in
  `node.params` like any node's, keyed by the Faust path (`/Reverb/size`), so automation, MIDI learn and undo
  work unchanged.
* Its ports come from the compiled DSP:
  * 1 input or output becomes a mono port fed or played on both channels;
  * 2 become one stereo port;
  * more become one stereo port per pair.
* Inputs and outputs can differ: a generator has no input.

**(F3) Per-instance descriptors.**

* Built-in nodes have static descriptors. A Faust node's ports and parameters depend on its script, so the
  compiled DSP produces a descriptor: ports, and parameters with ranges, steps, defaults and units from the
  script's `hslider`, `vslider`, `nentry`, `checkbox` and `button`, plus metadata such as `[unit:dB]` and
  `[scale:log]`.
* The engine owns this descriptor, and the UI gets it with the project's node status, the way plugin status
  works today.
* Commands check Faust parameters against it.

**(F4) Compiling never blocks audio, and a swap doesn't click.**

* Editing the script is a command (`setNodeCode`). On commit, a background thread compiles the code: parse,
  bytecode, instance, `init (sampleRate)`.
* **On success:** the node swaps to the new DSP at the next block, crossfading old into new over 20 ms. Parameter
  values carry over by path.
* **On failure:** the old DSP keeps playing, and the errors go to the editor with line numbers.
* Compiled factories are cached by source hash, so undo and redo swap back instantly.

**(F5) The Faust libraries ship with DAWg.** `stdfaust.lib` and its library files are bundled as app resources,
so scripts can `import("stdfaust.lib");` with no internet access or Faust installation.

**(F6) Scripts are project data, with a library beside them.**

* A node's code is saved in the project, so a project opens anywhere.
* "Save to library" puts a script into `Documents/DAWg/Faust/` as a `.dsp` file. The library and a set of
  examples (a tremolo, a ping-pong delay, a chorus, a simple reverb) appear in the node picker and the Plugins
  tab like built-ins.

**(F7) Monaco is the editor.**

* Monaco is VS Code's editor component, used through `@monaco-editor/react`. It gets Faust syntax highlighting
  (a Monarch grammar), error markers from the compiler, completion of the standard libraries' function names,
  and Ctrl+Enter to compile.
* It is bundled with the UI, not loaded from a CDN, because DAWg works offline.

**(F8) Generated controls follow the script's groups.**

* `hgroup`, `vgroup` and `tgroup` lay out the controls in rows, columns and tabs.
* Sliders show as knobs or sliders by their style metadata (`[style:knob]`), `checkbox` as a toggle, and
  `button` as a momentary button.
* Bargraphs (`hbargraph` / `vbargraph`) become meters, fed through telemetry.

## 3. Node panels

**(P1) Response curves come from the engine.**

* The EQ's and filter's response curves are computed by the engine from their actual coefficients, not
  re-derived in the UI. That keeps one source of truth, which matters once the de-cramped filters land.
* A bridge call returns the magnitude at the fixed 256 log-spaced frequencies plus extra points clustered
  around each running band's peak or knee, so a high-Q peak is neither flattened between samples nor drawn as
  a visible corner. The panel asks again while a band is dragged, and draws the samples as a smooth curve.

**(P2) Analysers are taps, on only while a panel shows them.**

* A node with an open panel gets a tap: its output is copied into a lock-free ring buffer. The message thread
  runs an 8192-point FFT at 30 Hz, averages each bin's power over time (rising within about 30 ms, falling at
  about 12 dB per second), and sends 256 log-spaced bands with telemetry: a band two or more bins wide takes its
  loudest bin, a narrower one is interpolated between bins, and all are tilted by 3 dB per octave about 1 kHz so
  that pink noise reads flat. The same goes for the side, (L - R) / 2, beside the mid, (L + R) / 2. The panels
  draw the bands as a line with a fading fill below it; the EQ in mid and side draws both, quieter, in its mid's
  and side's colours (channel-strip.md, C8).
* The compressor keeps a history of 10 ms slots (the loudest input and output and the most gain reduction of
  each) in a lock-free ring, which the 30 Hz analysis drains, so nothing between two frames is lost. The utility
  publishes the correlation and width of its output.
* Nothing is measured for closed panels.

**(P3) Panels are canvases in the theme.** Each panel is a React component drawing on a canvas with the theme's
tokens, like the arrangement and the piano roll. They edit through the same `setNodeParam` commands, with
gestures, so a drag is one undo step and writes automation in Touch mode. A dragged handle shows its values at
once instead of waiting for the engine's round trip, and sends at most one batch of commands per frame (the newest
values); it keeps showing them until the model has caught up.

**(P4) The panels:**

* **EQ:** a handle on the curve for each band in use (frequency on x, gain on y; the wheel or a vertical drag
  with Alt sets a bell's or shelf's Q, the wheel an HP's or LP's slope from 6 to 48 dB/oct), the summed curve, and the spectrum behind it. Bands are added
  by double-clicking the curve (the band depends on where) or from its context menu, bypassed by double-clicking
  their handle and removed from its menu, which also sets a band's type and puts it on the mid or the side only; the hovered band
  shows its own curve and its values, and the mid's and side's curves show when they differ (channel-strip.md,
  C1, C5 and C8).
* **Filter:** the curve with one handle (cutoff and resonance), and the mode as a segmented control.
* **Compressor:** the transfer curve with threshold and ratio handles, an input and output level dot moving on
  the curve, and attack, release, makeup and mix as knobs. Beside the curve, a level history scrolls right to left
  over 2, 4 or 8 seconds (remembered per user):
  * the input as a filled area and the output as a line, on the curve's dB scale;
  * the gain reduction hanging from the top on a 0 to −24 dB scale, with a live bar at the right edge;
  * the threshold as a dashed line that can be dragged up and down;
  * it keeps scrolling, with silence, when no audio comes in.

  When the dock is narrower than 680 px, a gain-reduction meter takes the history's place.
* **Utility:** a goniometer (stereo scatter) with a correlation meter, and gain, pan and width as knobs.

**(P5) The inspector keeps its sliders.** They still list every parameter (and are what MIDI learn and "Show
automation" hang off). The panel is the graphical view of the same parameters.

## 4. Implementation order

1. libfaust (interpreter) through CPM; a Faust node that compiles in the background, swaps with a crossfade,
   carries per-instance descriptors, and has parameters by path. Tests: compile, errors, swap, parameters.
2. `setNodeCode`, the descriptor in node status, project persistence; Faust nodes in the picker; the bundled
   libraries and examples.
3. The code editor (Monaco, highlighting, errors, completion) and the generated controls.
4. Engine response curves and analyser taps; the four panels.
5. The script library ("Save to library").

## 5. UI decisions (answered by the user)

1. **Code editor:** opened only from a Faust node, not a view of its own (revised after phase 12; it was a fourth
   centre view in the Tab cycle).
   * A click on a Faust node, in the graph or the inspector, shows its controls in the device dock, like any
     other device.
   * The dock's "Code" button for that node opens the code editor in the centre. So do double-clicking the node
     in the graph and the code button on its inspector slot.
   * The editor's close button, or Tab when the editor isn't focused, returns to the view it was opened from.
   * A node picker at its top switches between the project's Faust nodes.
2. **Node panels:** a device dock at the bottom of the centre area, resizable and hidable (View menu), showing the
   selected node's panel (Ableton-style): the EQ, filter, compressor and utility panels, and a Faust node's
   generated controls.
3. **Compiling:** automatically about 600 ms after typing stops (live coding). Errors show inline while the old
   DSP keeps playing, and Ctrl+Enter compiles at once.
4. **Faust controls:** laid out by the script's groups (F8) in the dock. The inspector still lists them as
   sliders.

## 6. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| F1 | libfaust with the interpreter backend through CPM; LLVM later as an option (the LLVM backend is the default since 2026-10-28) | Yes |
| F2 | `dawg.faust` nodes hold their code; parameters by Faust path; ports from the DSP | Yes |
| F3 | Per-instance descriptors from the compiled DSP | Yes |
| F4 | Background compile, 20 ms crossfade swap, errors keep the old DSP, factory cache | Yes |
| F5 | Faust libraries bundled | Yes |
| F6 | Code in the project; a script library and examples in the picker | Yes |
| F7 | Monaco, bundled, with Faust highlighting, error markers, completion | Yes |
| F8 | Generated controls follow Faust groups and styles | Yes |
| P1 | Response curves computed by the engine | Yes |
| P2 | Analyser taps only for open panels; FFT on the message thread | Yes |
| P3 | Canvas panels using the theme tokens; edits through `setNodeParam` with gestures | Yes |
| P4 | EQ, filter, compressor and utility panels as listed | Yes |
| P5 | Inspector sliders stay alongside the panels | Yes |

## 7. Implementation notes

* **libfaust** (cmake/Faust.cmake).
  * Faust 2.88.0's compiler sources are built as the static library `dawg_faust` with the interpreter backend,
    the way Faust's own "staticlib" target does. Faust's top-level CMake project isn't used.
  * The LLVM backend is the default since 2026-10-28 (F1's option; the owner: performance comes first); the
    interpreter remains with `-DDAWG_FAUST_LLVM=OFF` (the sanitizer builds use it). The LLVM comes from
    `DAWG_FAUST_LLVM_DIR`, or else on Windows from LLVM's release for MSVC (21.1.8, downloaded through CPM with its
    SHA-256; Windows has no LLVM package with libraries) and elsewhere from the system (apt `llvm-18-dev`, Homebrew
    `llvm`; CI sets `LLVM_DIR`). The build adds Faust's `generator/llvm` sources with `LLVM_BUILD` and the
    `LLVM_<major>0` define, links LLVM's core, MCJIT, native-target, passes and IR-reader libraries, and defines
    `DAWG_FAUST_LLVM`, with which FaustNode.cpp creates `llvm_dsp_factory` instead of `interpreter_dsp_factory`. With
    MSVC, Debug builds keep the interpreter (LLVM's release has no debug libraries). Details that took finding: LLVM's
    Windows release uses the static C runtime, so optimised builds do too (CMakeLists.txt), and its LLVMConfig.cmake
    overwrites `CMAKE_MSVC_RUNTIME_LIBRARY` for every configuration, so Faust.cmake restores it; its compile
    definitions (`UNICODE` among them) are not applied to libfaust with MSVC, as in Faust's own build (elsewhere its
    LLVM glue gets them, with `-fno-rtti`); `LLVM_BUILD_UNIVERSAL` stays off, so only the host target is initialised;
    its PDB library names the DIA SDK by the release machine's path, which is replaced with this Visual Studio's.
  * Measured (Release, 16 voices of the Voice synth example, docs/design/phase12-voice-scope.md): 30 % of real time
    with the interpreter, 3.1–3.6 % with LLVM 21.1.8. Compiling an example script takes 30–130 ms with LLVM, on the
    background compiler thread.
  * The Faust libraries come from the faustlibraries commit Faust 2.88.0 pins.
  * One patch is applied at configure time. The interpreter's peephole optimiser reads `*(cur + 1)` past a block's
    last instruction. Release builds read that harmlessly, but MSVC's checked iterators stop Debug builds with an
    assertion (and a dialog). Indexing the raw pointer instead keeps the release behaviour.
* **Engine** (FaustNode.h/.cpp).
  * `FaustProgram::compile` builds a factory and a descriptor from one probe instance:
    * one port per channel pair;
    * parameters by path, with `[unit]`, `[scale:log]` (a geometric mid value) and `menu` / `radio` styles (choice
      parameters);
    * whole-number steps become integer parameters, checkboxes and buttons toggles;
    * the control layout as a tree of groups and controls.
  * libfaust's global state is guarded by one mutex.
  * `FaustCompiler::shared()` caches results by source text (32 unused programs at most) and compiles on a worker
    thread when background compiling is on (the app), or before returning (tests).
* **Graph compiler.**
  * The planner takes ports from the instance's descriptor, so a Faust node's ports follow its script.
  * `faustInstance` runs the newest script that compiled. While a new one compiles, or if it fails, the last good
    program keeps playing.
  * A different program gets a new `FaustNode` that holds the old one and crossfades from it over 20 ms. Parameter
    values carry over by path from the model.
  * `releaseFinishedFade` drops replaced nodes on the next compilation, so they don't pile up.
  * A finished background compilation makes the app call `invalidate()` and update.
* **Commands.**
  * `setNodeCode` replaces a script.
  * `addNode` and `insertNode` take `code` (default: a stereo pass-through).
  * Parameter commands check a Faust node against its compiled program. A node whose script hasn't compiled accepts
    any parameter unclamped; unknown paths are ignored once it compiles.
* **App** (FaustControl.cpp, PanelControl.cpp).
  * The libraries, their licences and the example scripts are copied next to the executable (`faust/`).
  * Node status gains Faust entries: `compiling` / `ready` / `failed`, the error, and the running program
    (descriptor, channels, layout).
  * Other bridge calls:
    * `faust.scripts` and `faust.saveScript` (examples, and the library in Documents/DAWg/Faust);
    * `faust.completions` (the standard libraries' definitions, read from stdfaust.lib's bindings);
    * `faust.revealLibrary`.
  * Telemetry carries bargraph values (`faustMeters`).
* **Panels.**
  * `effectResponse` computes curves from the code the nodes run:
    * the EQ's biquad magnitudes, per band and summed;
    * the filter's exact digital response (the TPT SVF is the bilinear transform of the analog prototype, so it is
      the prototype's response at Ω = tan(ω/2)/g);
    * the compressor's static curve with makeup.
  * Any node can carry an `AnalyserTap` (a lock-free ring of stereo frames). The engine fills it after the node's
    block while it is on.
  * `nodes.watch` turns it on for the node the dock shows. At 30 Hz the app sends `dawg_analysis`: a 4096-point
    FFT in 256 log bands, the correlation and width, goniometer points, and the node's `meters()` (the compressor's
    gain reduction, input and output levels, and its 10 ms slots since the last frame).
* **UI.**
  * The code view (faust/CodeView.tsx) is loaded lazily, as Monaco is large. It has:
    * the Monarch grammar, the theme and completion (faust/monaco.ts);
    * compiling 600 ms after typing stops, or at once with Ctrl+Enter;
    * error markers and a clickable error list;
    * examples and library loading, and "Save to library".
  * `ui.editCode` opens the code view and remembers the view it came from (`codeReturn`); `ui.closeCode` goes back.
    The Tab cycle and the top bar's view switch leave it out.
  * The device dock (dock/) shows the selected node, or the code view's node:
    * the EQ, filter, compressor and utility panels on canvases;
    * Faust's generated controls by group (the script's implicit top group as a row);
    * knobs for other nodes.
  * Panels edit through `setNodeParam` with one gesture per drag and touch automation.
  * Faust examples and library scripts appear in the node picker and the Plugins tab.
* **Not done in v1:**
  * Buttons in Faust scripts are commands like any parameter change, so a press is an undo step.
  * Faust nodes have no response curve in the dock (only their controls).
  * The panels show the model's values, not automation as it plays (the inspector's sliders do).
* **Tests.**
  * tests/FaustTests.cpp:
    * descriptors, errors with lines, library imports, controls and layout;
    * processing with parameters by path, bypass;
    * the crossfade swap, failure keeping the old DSP, the cache on undo;
    * persistence, background compiling, and the examples compiling.
  * tests/NodeGraphTests.cpp: response curves against measured sines, and the tap and compressor meters.
  * UI tests: error parsing, the compressor's level history (dock/levelHistory.test.ts).

- macOS with the LLVM backend (2026-11-10, CI): the test runner aborts with "recursive_mutex lock failed" while the
  process's statics are destroyed, after the Faust tests have passed. The shared compiler is never destroyed, which
  wasn't enough, so the cause is in LLVM's own teardown. The test runner skips static destructors on macOS
  (tests/TestMain.cpp). The app may do the same when it quits on macOS; not checked, as there is no Mac to try it on.
