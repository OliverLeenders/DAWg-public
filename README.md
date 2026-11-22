# DAWg

An open-source digital audio workstation built around an MPE piano roll with expression curves,
node-based effects routing, and scriptable effects. C++20 / JUCE 9 engine with a React UI in an embedded WebView.

See [docs/PLAN.md](docs/PLAN.md) for the architecture and roadmap.

**Status:** phases 1–13 are done (among the latest: the voice-scope graph with polyphonic Faust, group tracks,
effect sends, sidechain inputs, the mixer window, the tempo map with time signatures, and per-track timing offsets).
The app arranges audio and MIDI clips, records, hosts VST3 instruments and effects, automates parameters, scripts
Faust nodes, plays per-note voice graphs, mixes in a mixer window and exports to WAV or FLAC. See the roadmap in
[docs/PLAN.md](docs/PLAN.md) for what is next (linked notes and release readiness).

On Windows the audio settings offer ASIO drivers as well as WASAPI and DirectSound. ASIO uses the ASIO SDK that JUCE
bundles (under its GPLv3 licence), so no separate SDK download is needed.

## Requirements

* CMake ≥ 3.25
* A C++20 compiler: Visual Studio 2022, Xcode 15+, or GCC 12+ / Clang 15+
* Node.js ≥ 20 with npm
* Linux only: the packages listed in the CI workflow (`.github/workflows/ci.yml`), LLVM's among them (`llvm-18-dev`)
* macOS only: LLVM from Homebrew (`brew install llvm`), with `LLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm`

All other C++ dependencies (JUCE, Catch2, the WebView2 SDK, libfaust, the Faust libraries and Rubber Band) are
downloaded at configure time; on Windows LLVM's release is too (943 MB). libfaust is compiled from source, so the first
build takes a while. Faust scripts run on libfaust's LLVM backend, compiled to native code; `-DDAWG_FAUST_LLVM=OFF` builds the
interpreter instead, which needs no LLVM and runs scripts about 9× slower.

## Build

```bash
cmake -S . -B build
```

```bash
cmake --build build --config Debug
```

The build also runs `npm ci` and `npm run build` in `ui/` and copies the result next to the executable.

Run the tests:

```bash
ctest --test-dir build -C Debug --output-on-failure
```

### Options

| Option | Default | Meaning |
|---|---|---|
| `DAWG_BUILD_UI` | `ON` | Build the web UI as part of the app build. |
| `DAWG_BUILD_TESTS` | `ON` | Build the unit tests. |
| `DAWG_ENABLE_VST2` | `OFF` | Host VST2 plugins. Requires `DAWG_VST2_SDK_PATH` to point at your own copy of the legacy VST2 SDK (not distributed with DAWg). |
| `DAWG_WARNINGS_AS_ERRORS` | `ON` | Treat warnings in DAWg's own sources as errors. |
| `DAWG_ALLOCATION_GUARD` | `ON` | In Debug builds, count heap allocations on the audio thread. |
| `DAWG_FAUST_LLVM` | `ON` | Compile Faust scripts to native code with libfaust's LLVM backend, about 9× faster than its interpreter (`OFF`). With MSVC, optimised builds then use the static C runtime, as LLVM's release does, and Debug builds keep the interpreter. |
| `DAWG_FAUST_LLVM_DIR` | empty | The LLVM install to use (it has `lib/cmake/llvm`). Empty: on Windows LLVM's release, downloaded; elsewhere the system's. |
| `DAWG_SANITIZE` | empty | GCC/Clang sanitizers, e.g. `address,undefined` or `thread`. |

### Checks

C++ code follows [docs/CODING_GUIDELINES.md](docs/CODING_GUIDELINES.md). Install the pinned tools once:

```bash
python -m venv .tools
```

```bash
.tools/Scripts/pip install -r tools/requirements.txt
```

(On macOS/Linux use `.tools/bin/pip`.) Then format the code before committing:

```bash
python tools/lint.py format --fix
```

CI also runs clang-tidy, Doxygen and the sanitizers; see the guidelines for running those locally.

## UI development with hot reload

Start the Vite dev server:

```bash
npm --prefix ui run dev
```

Then start the app with `--dev`, which loads `http://localhost:5173` instead of the bundled UI
(on Windows the Debug build is at `build/app/DAWg_artefacts/Debug/DAWg.exe`):

```bash
build/app/DAWg_artefacts/Debug/DAWg.exe --dev
```

Edits to `ui/src` show up instantly. Right-click → Inspect opens the WebView dev tools.

For repeatable live UI checks, expose the WebView2 page through CDP and run the repository helper:

```powershell
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = '--remote-debugging-port=9222'
build/app/DAWg_artefacts/Debug/DAWg.exe --dev http://127.0.0.1:5173
node tools/dawg-cdp.mjs verify-graph
```

The helper can also inspect the current graph or focused LFO editor:

```powershell
node tools/dawg-cdp.mjs inspect
node tools/dawg-cdp.mjs inspect-lfo
```

It uses Node's built-in WebSocket client and requires Node 22 or newer.

## Layout

```
engine/   GUI-free core: model, commands, undo, patch diff, realtime handoff, audio engine
app/      JUCE app: window, WebView, JS bridge, plugin scanner, audio settings
ui/       React + TypeScript front end (Vite)
tests/    Catch2 unit tests for the engine
docs/     Plan and design notes
```

## Licence

AGPL-3.0-or-later. See [LICENSE](LICENSE).

Third-party components:

* JUCE: AGPL-3.0 (or the JUCE commercial licence).
* libfaust: LGPL-2.1-or-later, linked statically. DAWg's full source is available, so it can be relinked against a
  modified libfaust.
* The Faust libraries, bundled next to the app: mostly LGPL with exceptions, MIT and STK-4.3. Each library file
  states its licence; the STK licence text is in `faust/licenses`.
* Rubber Band Library (time stretching): GPL-2.0-or-later, used under GPLv3, which the AGPL allows. Copyright
  2007–2024 Particular Programs Ltd. Its single-file build is compiled into the engine.
* Monaco Editor: MIT.
