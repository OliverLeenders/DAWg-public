# UI conventions: a checklist for new panels and views

The short form of [design-system.md](design-system.md), for anyone adding a device panel, a dialog or a view to
`ui/src`. Each line says what to use; the design doc says why. When something here doesn't fit, change this list and
the design doc together rather than working around them.

## Controls (`ui/src/ui/controls.tsx`, `dock/Knob.tsx`)

* **A parameter** goes through `ParamSlot`: a knob for a number, a stepper for a whole number with 16 values or fewer,
  a segmented row for a few short choices (or pictures, `Glyphs.tsx`), a menu for more, a toggle for on/off. Don't
  pick the control by hand; give the parameter the right kind.
* **A value to read and type** is a `ValueText` (`ParamValueText` for a parameter); values under a graph go in
  `Readouts`. A frequency field takes note names by itself (`takesNoteNames`); pass `paramHint` to show the note.
* **Buttons:** `Button` (words) or `IconButton` (an icon, with a `title`), at `s` 18 px (card and track headers), `m`
  24 px (the default) or `l` 28 px (top bar, dialog buttons). `variant="primary"` once per dialog, `danger` for
  removing, `ghost` in dense rows.
* **Pressed states:** something on or off is a `Toggle` (the accent's tint and border); one of several is a
  `Segmented` (the whole accent). Its words or icon say what it does.
* **Power** for a device's, a send's or a band's on/off; **TrackSwitch** for mute, solo, arm.
* **Inactive controls** (a mode makes them irrelevant): `paramActive` dims them; they stay draggable.

## Overlays and structure (`ui/src/ui/`)

* **Menus and popovers:** `Menu` (a list of actions; arrows, Home, End) or `Popover` (anything else), with `at` when
  opened at a point (a right-click, a button's corner), `anchor="end"` to open upwards. Never a hand-built backdrop.
* **Dialogs:** `Dialog`, with an `h3` title or `title`; `onClose` for Escape; `closeOnBackdrop` only for reading
  dialogs. Buttons in `.modal-buttons`, the primary one last.
* **Bars:** `Bar` with `fold` priorities (higher folds first); never a bar that wraps.
* **Tabs:** `Tabs` with `view` (switches what a panel shows) or `lane` (picks a lane under an editor).
* **Sections:** `Section` for a group of controls in a panel, with a short heading.

## Colour, type and space (`ui/src/tokens.css`)

* **No colour literals**, in CSS or on a canvas. Pick the token by role: surfaces (`--bg`, `--bg-raised`,
  `--bg-panel`, `--bg-card`, `--bg-float`), ink (`--text`, `--text-dim`, `--text-faint`), accent, status (`--ok`,
  `--warn`, `--danger`), signals (`--signal-audio`, `--signal-events`, `--mod`, `--signal-key`), tones, track switches,
  meters, timelines. A new role gets a new token, and a contrast pair in `tokens.test.ts` if text sits on it.
* **Green** is audio and level only; "on" is `--on`. A track's colour is a bar on the left of what is the track (the
  mixer's at the bottom) and the fill of its content; selection is the accent, never the track colour.
* **Canvases** draw with `readTheme()`, `font()`, `mix()` and `alpha()` (`canvas/theme.ts`) and `useCanvas`; text at
  `xs` (11 px) or more, Plex Sans or Plex Mono, and names that would collide left out.
* **Type:** `--text-xs` (11 px) for labels and small capitals, `--text-s` (12 px, mono) for values, `--text-m` for
  body and rows. Nothing smaller, except a glyph inside a handle. No text written upwards.
* **Space:** gaps and paddings from `--space-1` to `--space-7` (2 to 24 px); heights `--control-s`, `--control`,
  `--control-l`; radii `--radius-s`, `--radius`, `--radius-l`, `--radius-round`.
* **Whole pixels:** bars, rows and fields are whole numbers of pixels tall, and icons in a control are even sizes
  (10, 12, 14, 16), so nothing renders on a half pixel.

## Depth and areas

* Lighter is higher: a card (`--bg-card`, `--shadow-card`) on a working area, a menu or dialog on `--bg-float` with
  `--shadow`. Outlines only between neighbours of one level, on controls, on floating things, and as one rule between
  regions.
* **A working area** (where the work happens and content scrolls) gets `.working-area` (the inset); **a scroll area**
  gets `.scroll-fade` and `useScrollFade`.
* Hover is the overlay (`--hover-overlay`); keyboard focus keeps the ring.

## Before it's done

* Every icon-only button has a `title`; every control is reachable by Tab; Escape closes what opened.
* Typecheck and tests pass (`npx tsc --noEmit`, `npx vitest run`, including `tokens.test.ts`).
* Screenshots before and after in the running app, with the window narrow and wide.
