# Phase 5 design: project workflow

Status: **accepted** (implementation notes in section 10). The engine and app decisions are numbered F1–F10 and summarised in section 9.
The UI decisions are in section 8.

## 1. Goals and non-goals

Goals:

* A native project format that is versioned, can be migrated, and is safe to write.
* New, open, save, save as and close project, plus templates.
* A list of recently opened projects.
* An unsaved-changes marker, and a prompt before unsaved work is discarded.
* Autosave and crash recovery. This matters most now that plugins run in-process and can crash DAWg.
* A welcome screen.

This replaces the temporary JSON save/open from phase 4 (`engine/src/ProjectFile.cpp`).

Non-goals:

* DAWproject import/export (phase 11).
* Audio files (phase 7). The layout below reserves a place for them.
* Several projects open at the same time.
* Cloud storage and collaboration.

## 2. Files on disk

**(F1) A project is a folder; the project file inside it is a zip.** Recommended.

```
My Song/
  My Song.dawg          zip: project.json, plugins/<trackId>-<nodeId>.bin
  Audio/                recorded and imported audio (phase 7)
  Backups/              previous versions of My Song.dawg (F3)
```

* **`project.json`** holds the model and replaces the base64 blobs of the temporary format. It contains:
  * the model in its UI JSON form;
  * `format`, `version`, `id` (a UUID, F7) and `nextId`;
  * the app version that saved it.
* **Plugin state** is stored as separate binary files in the zip. That keeps `project.json` small and
  readable, and is the same layout DAWproject uses.
* **The zip** uses deflate for JSON and stored (uncompressed) entries for binary state, so saving stays fast.
* **Audio** lives next to the `.dawg` file, not inside it. Rewriting a zip that holds gigabytes of audio on
  every save would be slow. Cubase and Studio One organise projects the same way.
  * A single-file "collect and export" for sharing can come later.
* **A `.dawg` file on its own** (without the folder) still opens. It just has no audio. Plugin state and
  notes are all inside it.

The alternative, a single `.dawg` file holding everything, is simpler to share. But it either rewrites the
audio on each save or needs an append-only container. Not worth it now.

**(F2) Atomic saves.** Recommended.

1. Write the zip to a temporary file in the same folder.
2. Flush it.
3. Move the previous version to `Backups/`.
4. Rename the temporary file into place.

A crash or a full disk during a save can never leave a broken project behind.

**(F3) Keep the last 5 saved versions in `Backups/`** (named with date and time). Recommended. It costs a few
kilobytes to megabytes each, and protects against "I saved over the good version".

## 3. Versioning

**(F4) An integer format version, with a chain of JSON → JSON migrations.** Recommended.

* Each version bump adds a function `migrate_N_to_N+1(json)` in `engine/src/ProjectMigrations.cpp`.
  Loading applies them in order, then parses.
* Files with a newer version than the app refuse to open, with a clear message.
* Every migration gets a test with a stored fixture file (`tests/fixtures/projects/vN.dawg`), so old projects
  keep opening.
* The phase 4 temporary format (`dawg-project-json`) is version 0 and gets a migration too. Nothing created
  during phase 4 is lost.

## 4. Session state: dirty, saved, recovered

**(F5) "Unsaved changes" means the current snapshot differs from the saved one.** Recommended.

* The session remembers the `ProjectPtr` that was last saved (or opened).
* The project is dirty when the current snapshot is a different object. Model snapshots are immutable and
  shared, so undoing back to the saved state makes the project clean again. Cubase and Logic behave the same
  way.
* Plugin parameter changes made in a plugin's own editor don't go through commands. They count as changes
  once a state stamp actually changes the stored state: when an editor closes, before a save, and before
  quitting.
  * Before quitting or closing a project, the app stamps all plugins. A tweak made in an editor that is still
  open is then caught by the prompt.

**(F6) Autosave to a separate place, never over the user's file.** Recommended.

* Every 2 minutes while dirty (configurable later), the app writes
  `<app data>/Autosave/<project id>.dawg` using the same atomic write.
* A clean close deletes the autosave.
* **Crash detection:**
  * A `running.lock` file in app data records the open project's id and path while DAWg runs.
  * A clean exit removes it.
  * If it exists at startup, the last session crashed. If a matching autosave is newer than the project
    file, or the project was never saved, DAWg offers to recover it. How that offer looks is a UI question.
* **Save time:** autosaving stamps every plugin, which calls `getStateInformation()` on the message thread.
  For large sample-based instruments this can take tens of milliseconds. That is acceptable every 2 minutes.
  If it ever isn't, stamping can skip plugins whose editors are closed and whose state was stamped since the
  last autosave.

**(F7) Projects have a UUID**, created with the project and kept by "Save as" (an autosave belongs to the
open session, not the file). Templates get a new id for each project created from them.

## 5. Recent projects and templates

**(F8) The recent list lives in the app settings.**

* It holds up to 20 entries of path, name and last-opened time, newest first.
* Opening a project moves it to the top.
* Missing files stay in the list, marked missing, until the user removes them.
* A small JSON summary (tempo, track count, length, modified time) is cached per entry, so the welcome screen
  doesn't have to open every project.

**(F9) Templates are ordinary project files** in `<user documents>/DAWg/Templates/`.

* "New from template" opens the template, forgets its path, gives the project a new id and marks it
  untitled.
* "Save as template" writes the current project there.
* A built-in "Empty" template (one track with the MPE synth) always exists. It is generated, not a file.
* Which template "New" uses by default is a setting.

## 6. Commands and lifecycle (app side)

**(F10) A `ProjectManager` in `app/` owns the lifecycle.** Recommended.

* **Responsibilities:**
  * the current file;
  * the saved snapshot;
  * autosave and the lock file;
  * the recent list;
  * templates.
* **What it calls:** `Session::reset()` for new and open, `GraphCompiler::reset()` when the project
  changes, and the plugin stamps.
* **The flow for anything that would discard the current project** (new, open, close, quit):
  1. Stamp all plugins.
  2. If the project is dirty, ask save / don't save / cancel.
  3. Act.
* **Main window close** goes through the same flow. On macOS, Cmd+Q and the dock behave the same.
* **Bridge methods:**
  * `project.new { template? }`, `project.open { path? }` (a native dialog without a path),
    `project.save`, `project.saveAs`, `project.close`;
  * `project.recent`, `project.removeRecent`, `project.templates`, `project.saveAsTemplate`;
  * `project.recover { accept }`.
* **Events:** `dawg_projectState` carries `{ path, name, dirty, untitled }`.
* **File dialogs** are native (`juce::FileChooser`). "Save as" for a new project asks for a project name
  and a parent folder, and DAWg creates the folder.
* **Opening a project with missing plugins** opens it anyway. The plugins are placeholders (phase 4), and a
  summary lists what is missing.

## 7. Testing

* **Engine:**
  * round trips (model + plugin state) through the zip format;
  * migrations from every fixture version;
  * refusing newer versions;
  * the atomic write leaving the old file intact when the write fails (an injected failure);
  * the backup rotation.
* **App logic:** dirty tracking (edit, undo to saved, stamp change) as `Session` tests, with the saved
  snapshot kept in `Session` so it is testable without the app.
* **UI:** checked by hand over CDP, as before.

## 8. UI decisions (answered by the user)

1. **Startup: the welcome screen**, always. A preference to reopen the last project instead can come later.
2. **Welcome screen: a full-window start page.** It replaces the workspace until a project is open. The top
   bar stays, with the transport disabled.
   * The left column has "New project", "Open…" and the template cards.
   * The right column has the recent list with a search field.
3. **File commands: a "File" menu in the top bar**, left of undo, opening a web menu:
   * New
   * Open…
   * Open recent ▸
   * Save
   * Save as…
   * Save as template…
   * Close

   The project name sits next to the DAWg logo, with a ● dot when there are unsaved changes. The usual
   shortcuts apply: Ctrl/Cmd+N, O, S, Shift+S and W.
4. **Prompts: in-app modals.**
   * Save / Don't save / Cancel is keyboard driven: Enter saves, Esc cancels.
   * The crash-recovery offer is a banner on the welcome screen with Recover / Discard.
5. **Recent list entries show:**
   * the name and a relative date ("2 h ago");
   * the parent folder in small dim text, with the full path on hover;
   * the cached summary ("120 BPM · 6 tracks · 3:12").

   There is no pinning. Missing projects are shown greyed out with a remove button, so they can be cleared
   from the list (F8).
6. **Templates: cards under "New project"**: "Empty" plus the user's templates. A card creates a new
   untitled project from that template. "New project" uses the default template.
7. **New projects start untitled.** The first save asks for a name and a parent folder and creates the
   project folder (F1). Until then, autosave protects the work (F6).

## 9. Decisions to review

| # | Decision | Recommendation |
|---|---|---|
| F1 | Project folder with a zipped `.dawg` file (`project.json` + plugin state files); audio in `Audio/` next to it | Yes |
| F2 | Atomic save via temporary file and rename | Yes |
| F3 | Keep the last 5 saved versions in `Backups/` | Yes |
| F4 | Integer format version with tested JSON → JSON migrations; the phase 4 format becomes version 0 | Yes |
| F5 | Dirty = current snapshot differs from the saved one; plugin edits count once stamped | Yes |
| F6 | Autosave every 2 minutes to app data, lock-file crash detection, recovery offer at startup | Yes |
| F7 | A UUID per project, for autosave and recovery | Yes |
| F8 | Recent list (20) in app settings with cached summaries | Yes |
| F9 | Templates are project files in the user's DAWg folder, plus a generated "Empty" template | Yes |
| F10 | `ProjectManager` owns the lifecycle; every discard goes through stamp → prompt → act | Yes |

---

## 10. Implementation notes (phase 5 as built)

* **Code locations.**
  * File format: `engine/include/dawg/ProjectFile.h` (`projectfile::write / read / migrate / summarise`).
  * Dirty tracking: `Session::markSaved / markUnsaved / isDirty`.
  * Lifecycle: `app/src/ProjectManager.cpp`.
  * UI: `ui/src/components/WelcomeScreen.tsx`, `FileMenu.tsx` and `ConfirmDiscard.tsx`.
* **Version 0.** Phase 4 `.dawg.json` files open as untitled projects. The next save asks for a place and
  writes the zip format.
* **Discarding unsaved changes over the bridge.** Each of `project.new / open / close / quit` takes a `mode`.
  1. The UI first calls with `ask`.
  2. If the result says `needsDecision`, the UI shows the prompt.
  3. The UI calls again with `save` or `discard`.

  `save` saves first (with the dialog if the project is untitled) and only then runs the action. A native
  window close emits `dawg_confirm { action: "quit" }`, because the prompt lives in the UI.
* **Default locations.**
  * Save and open dialogs start in `<Documents>/DAWg/Projects`.
  * Templates live in `<Documents>/DAWg/Templates`.
  * "New project" always uses the "Empty" template; choosing a default template is not exposed yet.
* **Save as.** If the chosen file is not already inside a folder of the same name, that folder is created
  (F1).
* **Autosave and recovery.**
  * The autosave interval is 2 minutes and not yet configurable.
  * Recovered projects open as unsaved and keep their original file path, if it still exists.
* **Window title and shortcuts.** The window title shows the project name, with " *" while unsaved. The
  shortcuts are Ctrl/Cmd+N, O, S, Shift+S and W.
* **Testing.**
  * Engine tests cover the round trip, the zip layout, version 0, refusing newer or foreign files, backup
    rotation, summaries and dirty tracking.
  * The app flow was checked by hand over CDP: welcome screen, new, the unsaved marker and prompt, save
    through the native dialog with folder creation, reopen from recent, a backup on the second save,
    autosave, recovery after a killed process, and the quit prompt.
