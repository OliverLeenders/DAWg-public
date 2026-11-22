# C++ Coding Guidelines

These rules apply to all C++ in `engine/`, `app/` and `tests/`. The goal is robust code: mistakes should
fail to compile, fail a test, or trip an assertion — not show up as a glitch in someone's mix.

The UI (`ui/src`) follows [design/ui-conventions.md](design/ui-conventions.md): which component, token and size to use
for a new panel or view.

---

## 1. Attributes and signatures

### 1.1 `[[nodiscard]]` on every non-void function

Every function that returns a value is declared `[[nodiscard]]`: free functions, member functions, static
functions, getters, factory functions and fluent builders.

```cpp
[[nodiscard]] bool canUndo() const noexcept;
[[nodiscard]] static std::unique_ptr<Graph> compile (const Project&);
```

Exceptions, where the return value is idiomatically ignored:

* copy/move assignment and compound assignment operators (`operator=`, `operator+=`, …)
* stream insertion operators (`operator<<`)
* overrides of third-party virtual functions whose signature we don't control, if adding the
  attribute causes warnings at existing call sites

If a caller genuinely doesn't need a result, it says so explicitly: `std::ignore = f();` (or
`[[maybe_unused]] const auto r = f();`) — never a silent drop.

### 1.2 `noexcept`

Mark functions `noexcept` when they cannot throw, and **always** for everything reachable from the audio
thread. Move constructors and move assignment are `noexcept`.

### 1.3 `const`, `explicit`, `override`, `final`

* Member functions that don't modify state are `const`.
* Single-argument constructors and conversion operators are `explicit`.
* Overrides use `override` (never repeat `virtual`).
* Classes are `final` unless designed for inheritance.
* Locals that never change are `const`. Prefer `const auto x = …`.

---

## 2. Documentation comments

### 2.1 Doxygen, Javadoc style

* Doc comments belong on the **declaration** (usually in the header), never on the implementation.
  Comments in `.cpp` files explain *how* or *why*, using `//`.
* A doc comment starts with `/**` **on its own line**, and ends with `*/` on its own line.
* Commands use `@`, never a backslash: `@param`, `@return`, `@throws`, `@see`, `@note`, `@tparam`.
* The first sentence is the brief description.

```cpp
/**
    Applies a command to the project and records it in the undo history.

    Consecutive commands with the same non-empty gesture id are merged into a single undo step.

    @param command  Name of the command, e.g. "setTempo".
    @param args     Command arguments as a JSON object.
    @param gesture  Optional gesture id used to merge continuous edits such as fader drags.
    @return         Whether the command succeeded, plus the new revision or an error message.
    @note           Message thread only.
*/
[[nodiscard]] Result apply (const juce::String& command, const juce::var& args, const juce::String& gesture = {});
```

### 2.2 What gets documented

* Every public class, function, type alias and enum in a header.
* `@param` for every parameter and `@return` for every non-void function, unless it's a trivial getter
  whose brief already says it all (`Returns the current tempo in BPM.`).
* `@throws` for every exception a function can throw.
* Every class or function used from more than one thread states its threading contract with `@note`
  (see section 4).

---

## 3. Ownership and resources

* No owning raw pointers and no naked `new`/`delete`. Use `std::unique_ptr` by default, and
  `std::make_unique` / `std::make_shared` to create objects.
  * Exception: JUCE APIs that take ownership of a raw pointer (e.g. `juce::var (new DynamicObject())`,
    `setContentOwned`). Wrap these in a helper where practical.
* Raw pointers and references are **non-owning** and must not outlive what they point at.
* Shared, immutable data uses `std::shared_ptr<const T>` (as the project model does).
* Every resource is RAII-managed. No `init()`/`shutdown()` pairs a caller can forget.
* Rule of zero: classes should not declare destructors or copy/move operations unless they manage a
  resource directly. Non-copyable classes use `JUCE_DECLARE_NON_COPYABLE` or deleted members.

---

## 4. Real-time safety

The audio callback and everything it calls is **real-time code**. Real-time code must never:

* allocate or free memory (no `new`, `std::vector::push_back` past capacity, `std::string`, `juce::String`
  building, `std::function` construction, `shared_ptr` release of the last reference)
* take a lock or wait (no `std::mutex`, `juce::CriticalSection`, condition variables, `MessageManager` calls)
* do I/O or logging (no files, no `DBG`, no `std::cout`)
* throw exceptions
* call into code whose cost is unbounded

Communication with the audio thread goes through the existing lock-free primitives (`SnapshotExchange`,
`SpscQueue`, atomics). Every function that runs on the audio thread is `noexcept` and documented with
`@note Audio thread.` Every cross-thread class documents which thread may call each member.

In Debug builds, an allocation guard asserts on any allocation inside the audio callback.

---

## 5. Error handling

| Situation | Mechanism |
|---|---|
| Programming error / broken invariant | `jassert` (Debug), plus defensive handling if the error could reach users |
| Invalid input at a boundary (UI command, file load, script) | Exception (e.g. `CommandError`) caught at the boundary and turned into a user-facing error |
| Expected failure (device unavailable, plugin failed to load) | Return a result type (`std::optional`, `juce::Result`, or a `Result` struct) |
| Audio thread | Never throws, never asserts in Release; degrades to silence |

* No exception may cross the bridge, the audio callback, or a thread boundary.
* Don't catch `...` except at those top-level boundaries.
* Validate every value that comes from outside (UI, files, plugins): types, ranges, finiteness (NaN/inf).

---

## 6. Types and values

* `enum class` instead of plain `enum`.
* No C-style casts. Use `static_cast`; `reinterpret_cast` only with a comment saying why.
* Fixed units in names or types: `tempoBpm`, `gainDb`, `timeBeats`, `lengthSamples`. Time in the model
  is `double` beats. Sample data is `float`.
* No magic numbers: name them as `constexpr` constants near their use, or in a `limits` namespace.
* Prefer `std::span`, `std::string_view`, `std::optional` and `std::variant` over pointer/size pairs,
  sentinel values and unions.
* Use `std::` containers and algorithms internally. Use `juce::String` for text, and `juce::var` only at
  serialisation boundaries (bridge, files).

---

## 7. Concurrency

* Each class has one owning thread unless documented otherwise.
* Prefer message passing (immutable snapshots, queues) over shared mutable state.
* Atomics default to sequentially consistent ordering. Relaxed/acquire/release only with a comment
  explaining why it is correct.
* Never hold a lock while calling unknown code (callbacks, plugins, listeners).

---

## 8. Naming and formatting

| Thing | Style | Example |
|---|---|---|
| Types, classes, enums | `PascalCase` | `AudioEngine`, `EngineState` |
| Functions, locals, parameters | `camelCase` | `applyCommand`, `positionBeats` |
| Non-static data members of classes | `m_` + `camelCase` | `m_session`, `m_undoStack` |
| Fields of plain data structs | `camelCase`, no prefix | `track.name`, `state.tempo` |
| Constants (`constexpr`, `static constexpr`) and enumerators | `camelCase` | `maxUndoSteps`, `Backend::webview2` |
| Namespaces | lowercase | `dawg`, `dawg::limits` |
| Files | match the main type | `AudioEngine.h` / `AudioEngine.cpp` |

* The `m_` prefix tells member state apart from locals and parameters at a glance. A **plain data struct**
  (all members public, no invariants, no member functions beyond trivial helpers — e.g. `Track`,
  `EngineState`) is exempt, because its fields are always accessed through an object (`track.name`).
* No other prefixes or suffixes (`_x`, `x_`, `s_`, `g_`) and no Hungarian notation.
* Allman braces, 4-space indent, a space before call parentheses (`f (x)`), a 120-column limit.
  Enforced by `.clang-format`, which CI checks.
* `#pragma once` in every header.
* Include order: own header, project headers, JUCE module headers, standard library. Include specific JUCE
  module headers (`<juce_core/juce_core.h>`), never `JuceHeader.h`.
* No `using namespace` in headers.

---

## 9. Functions and classes

* A function does one thing. If it needs section comments, it should probably be several functions.
* Early return over deep nesting.
* Prefer pure functions over member functions with hidden state; the command layer is the model for this.
* Default member initialisers (`int x = 0;`) over constructor initialiser lists for fixed defaults.
* No global mutable state. Function-local statics only for immutable data.
* No macros except JUCE's, and `#if` platform guards.

---

## 10. Testing

* Every command, every engine behaviour, and every bug fix has a Catch2 test. A bug fix's test fails
  before the fix.
* Engine code stays GUI-free so it can be tested without a window or audio device.
* DSP is tested offline with `renderOffline` against known output (exact, or within tolerance).
* Tests are deterministic: no sleeps, no real devices, no network.

---

## 11. Tooling and enforcement

Everything below runs in CI (`.github/workflows/ci.yml`) and can be run locally.

| Check | Where | Run locally |
|---|---|---|
| **Warnings as errors** — `/W4 /WX` (MSVC), `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror` (GCC/Clang), DAWg sources only; JUCE and Catch2 headers are treated as system headers | every build (`DAWG_WARNINGS_AS_ERRORS=ON`) | normal build |
| **Formatting** — `.clang-format` | `lint` job | `python tools/lint.py format --fix` |
| **Static analysis** — `.clang-tidy` (`bugprone-*`, `performance-*`, `modernize-use-nodiscard`, `cppcoreguidelines-owning-memory`, `readability-*` incl. naming and the `m_` prefix, …) | `lint` job | configure with Ninja and `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then `python tools/lint.py tidy <build-dir>` |
| **Documentation** — `Doxyfile` with `WARN_IF_UNDOCUMENTED` and warnings as errors | `docs` job | `doxygen Doxyfile` |
| **Sanitizers** — tests under AddressSanitizer + UndefinedBehaviorSanitizer, and under ThreadSanitizer | `sanitizers` job | `-DDAWG_SANITIZE=address,undefined` (GCC/Clang) |
| **Audio-thread allocations** — `ScopedNoAllocation` counts heap use inside the audio callback | Debug builds (`DAWG_ALLOCATION_GUARD=ON`); tests assert zero, the app asserts on the message thread | Debug build |

Tool versions are pinned in `tools/requirements.txt` (clang-format, clang-tidy). Install them with
`pip install -r tools/requirements.txt`, ideally into a `.tools` virtual environment, which
`tools/lint.py` finds automatically.

A justified exception to a clang-tidy rule is marked inline with `// NOLINT(check-name): reason` or
`// NOLINTNEXTLINE(check-name): reason`. Never suppress without a reason.
