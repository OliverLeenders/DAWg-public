# Design: DAWg's design language and design system

Status: **agreed** (2026-11-14); all ten steps built (section 8); the checklist for new work is [ui-conventions.md](ui-conventions.md). The survey is sections 2 and 3, the decisions DS1–DS17 section 4,
the owner's answers section 6. DS17 (note names in frequency fields) came from the owner after the first draft. No UI
code has changed for this document.

## 1. Why

DAWg's views were built one at a time: the arrangement, the node graph with components, split nodes and mono rings,
the piano roll and mixer in their own windows, the dock with a panel per device, the inspectors. They share a colour
scheme and most of a look, because `ui/src/styles.css` starts from Typewright's dark tokens
(`FontEditor/apps/editor/src/styles/tokens.css`). But only the colours were taken over. Sizes, type, states, signal
colours and canvas drawing were decided per view, and the reviews (docs/ui-review-2026-11-10.md,
docs/design/ux-review.md) keep finding the same kinds of drift. This document writes down the language the UI already
speaks, names where it contradicts itself, and proposes a small system (tokens, components, rules) so the next view
and the next device panel come out consistent without a review.

Goals:

* One set of tokens, used by CSS and by every canvas, with a name per *role* rather than per colour.
* One component per job (a switch, a choice, a number, a section, a menu, a dialog), each with its states written
  down.
* Rules for what can't be a component: canvases, density, layout of the dock, mixer and editors.
* A staged migration that never leaves a view half-changed, in small commits.

Non-goals:

* A light theme. The tokens are built so that one could be added (as in Typewright), but nothing here adds it.
* A visual redesign. The look stays; the system makes it consistent.
* A component library outside `ui/src`, CSS-in-JS, or a CSS framework.

Screenshots taken for this survey (a template project in a separate instance) are in the session notes folder,
`design-system-shots/`: welcome, arrangement, graph, a component with a Band Split, the dock, the piano roll, the
mixer, a clip menu, a dialog.

## 2. What exists

### 2.1 Views

| View | Where | Look today |
|---|---|---|
| Top bar | `components/TopBar.tsx`, `WindowControls.tsx` | Brand, project name, menus, undo/redo, transport, position LCD, BPM, signature, view switch (whole-accent when on), mixer, CPU, MIDI, window buttons. Crowded at 1400 px: the MIDI indicator is clipped to "Nt". |
| Welcome | `components/WelcomeScreen.tsx` | Large brand, action rows, template tiles, recent list. The "missing" tag and the row's × overlap. |
| Arrangement | `arrangement/` (canvas: `draw.ts`) | Track headers with a 3 px track-colour bar on the left, M/S/arm/monitor letter switches, gain and pan fields; canvas clips in track colour; strips (signature, key, markers) with uppercase labels. |
| Graph | `graph/` (React Flow) | Nodes with a 3 px top border by category (instrument accent, effect green, utility yellow, I/O grey); ports by signal (audio green circle, events orange square, modulation purple circle, mono a ring); Band Split with a tone per band on its ports and a band bar. |
| Piano roll | `pianoroll/` (own window, `?window=roll`) | Toolbar in label/value pairs that wraps to a second row; canvas notes in track colour by velocity; near-white keyboard; lane tabs below. |
| Mixer | `mixer/` (own window, `?window=mixer`) | Strips with a track-colour bar at the bottom, insert rows with power icons, send knobs, pan, M/S/arm, fader with meter, gain box. Toolbar with soft-tint toggles (I/O, Inserts, Sends, Offset) and Wide/Narrow. |
| Dock | `dock/` | Modulation column, device cards (power, name, presets, collapse; selected: 2 px accent top line), panels of `Section`s with knobs, segmented choices, readouts and graphs, the track fader on the right. |
| Inspectors | `components/Inspector.tsx`, `TrackInspector`, `NodeInspector`, `ClipInspector` | Uppercase headings, list rows for instrument, inserts and sends (power icon and ×), knob grid, folded Timing. |
| Browser | `components/RightPanel.tsx`, `SoundExplorer.tsx`, `PluginBrowser.tsx` | Uppercase tabs (SOUNDS, MIDI FILES, PLUGINS), search field, lists. |
| Menus and dialogs | `MenuItem.tsx`, `.context-menu`, `.modal` | Menus: icon column, label, shortcut, uppercase heading; dialogs: 15 px title, fields, primary button in whole accent. |

### 2.2 Tokens

`styles.css:4-32` defines 29 custom properties: surfaces (`--bg`, `--bg-raised`, `--bg-panel`, `--bg-hover`), rules
(`--border`, `--border-strong`, `--frame`), ink (`--text`, `--text-dim`, `--text-faint`), `--accent`,
`--accent-ink`, `--accent-soft`, `--accent-2` (green: "levels, playing, enabled"), `--warn`, `--danger`, `--mod`,
`--mod-ink`, `--mod-point`, `--eq-mid`, `--eq-side`, `--lcd`, `--on-accent`, `--shadow`, `--scrim`, `--disabled`,
`--radius`, `--font`, `--mono`. Each colour notes the Typewright token it follows.

Typewright has more than DAWg took: a control height (`--control: 1.75rem`), an icon-only button size (`--tool`),
four icon sizes, a six-step type scale, `--hover` and `--selected`, warning surface/ink/rule, an inset shadow, and a
written rule for pressed states (section 3.2). DAWg uses several of those *values* as literals instead:

* `#6b5520`, `#f2e3c4` (Typewright's `--warn-rule`, `--warn-ink`) in the output-mute banner and the missing-file bar;
* `#d49af5` (Typewright's `--syntax-keyword`) for sidechain sends and the input meter, 6 times;
* `#d6a05a` (`--warn`) written out 16 times in CSS and 4 in TS, as the mute switch, the events port and cable, MIDI
  learn and a frozen track's stale icon;
* `#e0c372` for solo and for the utility category; `#e8c36a` and `#e6c35a` (two more yellows) in meters and band tones.

### 2.3 Measures (counted in `styles.css` and `ui/src`)

* **Type:** 11 px 180 times, 12 px 19, 13 px (root) once, 14, 15, 18 and 40 px a few times, plus `0.78rem`,
  `0.72rem` and `0.9em`. Canvases draw text at 7 to 12 px, in IBM Plex Sans, Inter and JetBrains Mono (the last two
  aren't the UI's faces and fall back to Segoe UI and Consolas).
* **Uppercase labels:** letter-spacing 0.04, 0.06, 0.08 and 0.12 em, at 11 or 12 px, weight 400 or 600.
* **Control heights:** 14, 16, 18, 20, 22, 24, 26, 28 and 32 px all occur; `.text-button` is `1.75rem` (22.75 px).
* **Gaps:** 1, 2, 3, 4, 5, 6, 8, 10, 12, 16 and 20 px; 4, 6 and 8 dominate.
* **Radii:** `--radius` (3 px) 83 times; literal 1, 2, 3, 6, 8, 10, 13 px and 999 px elsewhere.
* **Shadows:** `--shadow` 7 times and 6 other drop shadows.
* **Icons:** lucide at 9, 10, 11, 12, 13, 14, 15, 16, 18 and 22 px (12 and 13 most); stroke widths 1, 1.4, 1.6, 2.
* **z-index:** 2, 4, 5, 10, 11, 20, 40, 45, 50, 60, 100, with no list of layers.
* **Inline styles:** 108 `style={{…}}`, 35 of them in `pianoroll/HumaniseDialog.tsx`.
* **Canvas colours:** `pianoroll/draw.ts` reads 8 tokens (`readTheme`), but the keyboard uses Tailwind slate literals
  (`#94a3b8`, `#334155`, `#d9dce3`, `#1b1e25`); `arrangement/draw.ts` has 12 literals and five ad-hoc `rgba`s.
* **Media queries:** none. Container queries: 11 (dock panels, the Sampler, track headers).
* **Motion:** six transitions (100–200 ms), one keyframe animation (MIDI learn); no `prefers-reduced-motion`.

### 2.4 Shared components

| Component | Where | Used by |
|---|---|---|
| `Section`, `SectionHeader` | `dock/Section.tsx` | Every dock panel |
| `ParamSlot` → `Knob` (rotary or stepper), `ChoiceSlot` (segmented or select), `SlotToggle` | `dock/Knob.tsx` | Dock panels, generic panel, node inspector, instrument section |
| `ParamSwitch` | `dock/Knob.tsx` | Rows with room for a word |
| `ParamField`, `DragNumber`, `ValueText` | `dock/Knob.tsx`, `components/` | Number fields everywhere (one drag rule, `dragScale.ts`) |
| `Readouts`, `Readout` | `components/Readouts.tsx` | Values under graphs |
| `MenuItem` | `components/MenuItem.tsx` | Every right-click menu |
| `DeviceMenu` | `components/DeviceMenu.tsx` | Inspector, strip, mixer, graph |
| `TrackSwitches`, `PanSlider`, `FaderWithMeter`, `GainReadout` | `dock/TrackFader.tsx` | Dock fader, mixer strip |
| `Meter` | `components/Meter.tsx` | Top bar, headers |
| Glyphs | `components/Glyphs.tsx` | Waveform, filter and direction pictures in segments |
| `drawWave` | `timeline/waveform.ts` | Arrangement, audio editor (not yet Sampler, browser) |
| `useCanvas`, `readTheme`, `mix` | `pianoroll/useCanvas.ts`, `draw.ts` | All canvases (from the piano roll's module) |

Not shared yet: buttons (`.icon-button`, `.text-button`, `.switch`, `.slot-toggle`, `.toggle-button`, plus a dozen
one-off button classes) are class names, not components; dialogs are `.modal` markup repeated per dialog; tabs exist
three times (`.panel-tab`, `.lane-tab`, the browser's); popovers four times (`.context-menu`, `.submenu-popup`,
`.preset-menu-popup`, `.toolbar-popover`).

## 3. The implicit design language

### 3.1 Principles the UI already follows

1. **Dark, quiet and flat.** Three close surfaces (`--bg`, `--bg-raised`, `--bg-panel`) separated by thin rules,
   one shadow for floating things, colour kept for meaning. Content (clips, curves, meters) is the brightest thing on
   screen. Mostly true; the piano keyboard (near-white) is the exception.
2. **One accent for "chosen".** Blue marks selection, focus, the current view and edited values; nothing decorative
   is blue.
3. **Colour is signal.** Green is audio and "on", purple is modulation, orange is events, and track colours identify
   tracks. A user can follow a colour from a port to a cable to a knob's ring.
4. **Every value is direct and exact.** Knobs and graphs for the gesture, a boxed or underlined number to drag or
   type for precision, one drag rule for all of them (`dragScale.ts`).
5. **The control fits the data.** Choices are segments or menus, switches are switches, small integers are steppers
   (ParamSlot, review 1.1–1.6).
6. **Labels are short, words over jargon.** Section headings in small capitals, names in sentence case, tooltips that
   explain the gesture.
7. **Dense like a DAW.** 11 px labels, 18–24 px controls, cards that fill the dock height, container queries instead
   of scroll.
8. **The same thing looks the same everywhere** (the goal behind the shared fader parts, `DeviceMenu` and `ParamSlot`)
   — the principle the reviews enforce most, and the one this document is mostly about.

### 3.2 Where it contradicts itself

**Pressed states.** Typewright's rule: one of several (a mode, a tool) is the accent *whole* (`--accent` fill,
`--on-accent` text); on/off and "picked out of a row" is the accent's *tint* (`--accent-soft` fill, `--accent-ink`
text). DAWg has both styles but not by that rule:

* `.segment.active` (`styles.css:1529`) is whole: the Band Split's solo, Filter types, Saturator oversampling in
  a slot.
* `.segmented button.active` (`styles.css:1210`) is tint: the mixer's I/O/Inserts/Sends (on/off: right), but also
  Wide/Narrow and the export dialog's render mode (one of several: should be whole).
* The view switch is whole (right); the play button is whole *green*; power is green ink in the inspector and
  mixer rows but accent in the dock card's header.
* `--accent-soft` against `--bg-raised` is 1.14:1, so a tinted segment is told apart only by its text colour.

**One parameter, two controls.** The Filter's oversampling is a `<select>` ("2x"); the Saturator's is a segmented
row ("Off 2x 4x") — same parameter kind, same values, in two panels side by side (screenshot `12-dock.png`).

**Signal colours collide with state colours.**

* Green (`--accent-2`) is audio ports and cables, the effect category, "power on", the play button, meters and the
  ok state. A Band Split's mid band (`#6cc49a`) is indistinguishable from a plain audio port (`#74c08a`).
* The events colour (`#d6a05a`) is also `--warn`, the mute switch and MIDI learn.
* Solo (`#e0c372`) is also the utility category's top border.
* Left and right tones reuse the low and high band tones.
* Sidechain is a seventh colour (`#d49af5`) without a token, close to `--mod` (`#b58cff`).

**Track colour has four placements.** A 3 px bar on the left in headers and the dock fader, a 3 px bar at the bottom
of a mixer strip, a fill in clips and notes, and the accent (not the track colour) on the selected device card. The
piano roll window's title is in the track colour; the main window's project name isn't.

**Type and size drift.** See 2.3: eleven control heights, seven radii, four letter-spacings for small capitals, three
font families in canvases, canvas text down to 7 px though CSS keeps an 11 px floor.

**Many button and switch styles.** `.icon-button` (28 px, 24 small, 18 in card headers), `.text-button` (22.75 px),
`.switch` (M/S/R letters; 18 px in headers, 22 in the mixer toolbar), `.slot-toggle`, `.param-switch`,
`.toggle-button`, `.corner-toggle` (14 px), `.preset-selector-button`, `.stepper-button`, plus power icons drawn three
ways (dock card, inspector row, mixer row).

**Tabs three ways.** The browser's uppercase tabs, the inspector's `.panel-tab`, the roll's `.lane-tab` (sentence
case, raised active tab, controller lanes in italics).

**Rotated text.** The dock's scope path ("MPE Synth › Multiband Compressor") is vertical and overlaps itself; the roll's
lane label ("Velo city") wraps vertically. Review 3.13 removed rotated text from the dock's add strips, but not these.

**Canvases outside the tokens.** The keyboard's Tailwind greys, the arrangement's literal `#0d1218`/`#e5ebf2`, the
frequency axis at 10 px with "10k" and "20k" overprinting at narrow widths (`dock/panelDraw.ts`), notes at low
velocity nearly invisible (`mix(bg, colour, 0.35 + 0.65 · velocity)`).

**Accessibility.**

* `--text-faint` is 3.98:1 on `--bg-panel` (under 4.5:1 for small text) and is used for real labels.
* Control borders (`--border` 1.35:1, `--border-strong` 1.48:1 against their surfaces) fall short of the 3:1 asked
  of a control's boundary, so empty fields and off switches are hard to see (review 1.7's "empty square").
* Focus rings exist now (`styles.css:2020`) but there's no layer order for popovers and dialogs to trap focus in, and
  only 14 places set `role="dialog"`/`aria-modal`.
* No reduced motion setting; few animations, so cheap to add.

## 4. Decisions

### Tokens

**(DS1) Tokens in their own file, named by role.** `ui/src/tokens.css` holds every token; `styles.css` and canvases
only read them. Names keep DAWg's current ones where they exist (no churn for 83 `var(--radius)` uses), and add
roles:

| Group | Tokens |
|---|---|
| Surfaces | `--bg` (level 0), `--bg-raised` (1), `--bg-panel` (2), `--bg-float` (3, new), `--hover-overlay` (replaces `--bg-hover`), `--lcd` (wells, canvases), `--selected` (= `--accent-soft`, rows picked) |
| Ink | `--text`, `--text-dim`, `--text-faint` (raised to ≥ 4.5:1 on `--bg-panel`, DS15) |
| Rules | `--border`, `--border-strong`, `--frame`, `--control-border` (new, ≥ 3:1, for fields and off switches) |
| Accent | `--accent`, `--accent-ink`, `--accent-soft`, `--on-accent` |
| Status | `--ok`, `--warn`, `--danger`, plus `--warn-surface`, `--warn-ink`, `--warn-rule`, `--danger-surface`, `--danger-ink`, `--danger-rule` (the banners and toasts) |
| Signals | `--signal-audio`, `--signal-events`, `--mod` (+ `--mod-ink`, `--mod-point`), `--signal-key` (sidechain) |
| Tones | `--tone-1`…`--tone-4` (bands), `--tone-left`, `--tone-right`, `--tone-mid`, `--tone-side`, `--tone-dry` |
| Track switches | `--mute`, `--solo`, `--arm`, `--automation-read`, `--automation-write` |
| Categories | `--category-instrument`, `--category-effect`, `--category-utility`, `--category-io` |
| Meters | `--meter-low`, `--meter-mid`, `--meter-clip`, `--meter-input` |
| Timelines | `--playhead`, `--marker`, `--punch`, `--warp-marker`, `--transient`, `--key-white`, `--key-black`, `--key-label` |
| EQ bands | `--eq-cut`, `--eq-band-1`…`--eq-band-6` |

`--accent-2` stays as an alias of `--ok` until its uses are moved.

**(DS2) Each meaning gets its own colour, and each colour one meaning per context.** Proposed separations:

* Audio stays green; "on" (power, enabled) moves to `--accent-ink` on a tinted power icon, as the dock card already
  does, so green means audio and level only. The effect category's border becomes neutral (`--border-strong`) or a
  desaturated green; the category is already in the node's name and ports.
* Events get their own orange, apart from `--warn`: warn moves a step towards amber-yellow, events keep `#d6a05a`.
  Mute gets its own token, a value between, so a muted track never reads as a warning.
* Band tones are chosen against the signal colours: none within ΔE 10 of audio green or mod purple. Proposed: blue,
  teal-cyan, yellow, coral (the mid band leaves green).
* Sidechain (`--signal-key`) stays pink-purple and moves further from `--mod` (6.1, 4).

**(DS3) Type scale, five steps in px.** The root stays 13 px.

| Token | Size | For |
|---|---|---|
| `--text-xs` | 11 px | Control labels, small capitals headings, axis and scale labels, meter scales, kbd hints |
| `--text-s` | 12 px | Values (in `--mono`: fields, knob values, readouts), dense lists |
| `--text-m` | 13 px | Body, menus, inspector rows, track names, buttons with words |
| `--text-l` | 15 px | Dialog titles, the position display |
| `--text-xl` | 18 px | Brand, welcome heading (40 px stays a one-off) |

One small-capitals style (`.caps`): `--text-xs`, weight 600, `letter-spacing: 0.08em`, `--text-dim`. Values in
`--mono` with tabular figures. Canvases use the same two families and the same sizes, read from the tokens (DS9).

**(DS4) Spacing on a 2 px base: 2, 4, 6, 8, 12, 16, 24.** `--space-1`…`--space-7`. Odd values (1, 3, 5, 10, 20) go
except where a pixel-exact line needs them (a 1 px gap between keys).

**(DS5) Three control heights.** `--control-s: 18px` (in card headers, track headers, chips), `--control: 24px`
(fields, selects, segmented rows, text buttons, icon buttons in panels and toolbars), `--control-l: 28px` (top bar
transport, dialog buttons). Icon-only buttons are square at the same heights. A row of controls is one height.

**(DS6) Radii: `--radius` 3 px for controls and cards, `--radius-s` 2 px for chips inside controls and handles,
`--radius-l` 6 px for popovers and dialogs, `--radius-round` for knobs, dots and pills.** Nothing else.

**(DS7) Elevation: lighter is higher.** What sits on top of something is a step lighter than it. Four surface
levels:

| Level | Token | What sits there |
|---|---|---|
| 0 ground | `--bg` | Working areas: arrangement lanes, graph, roll, audio editor, the dock's strip, the mixer's strips |
| 1 raised | `--bg-raised` | Panels and bars: inspector, browser, dock background, track headers, top bar, toolbars |
| 2 card | `--bg-panel`; `--bg-card` (`#212b37`) for device cards | Things on a panel or a working area: device cards, mixer strips, list rows, control fills |
| 3 floating | `--bg-float` (new, a step above `--bg-panel`, about `#232e3a`) | Menus, popovers, dialogs, toasts, a card being dragged |

Today menus and dialogs are `--bg-raised`, darker than the dock cards they open over; they move to level 3. The
surface values otherwise stay (shared with Typewright); whether the steps (about 1.1:1 between neighbours) need to
be wider is judged from before-and-after screenshots in step 3.

* **Hover is a state, not a level:** a light overlay (`--hover-overlay`, about 4 % white) over the element's own
  level, so it lifts any level. `--bg-hover` goes once its uses have moved.
* **Two shadows.** `--shadow` for floating things (level 3). `--shadow-card` for a device card on its working area: a
  1 px lit top edge, which carries most of it on a dark ground, and a short, close shadow below (owner, 2026-11-14:
  cards needed a shadow and more contrast). The six ad-hoc shadows go.
* **Working areas are sunk (Typewright's inset).** The areas where the work happens and content scrolls under fixed
  parts of the window (the arrangement's lanes, the graph, the roll, the dock's strip, the mixer's strips) sit on the
  ground with `--shadow-inset` on an overlay of their own (`.working-area::after`): a dark rim, a short shadow under the
  top edge and a weaker one over the bottom. Typewright's dark values vanish on DAWg's ground, so these are stronger.
  Panels (inspector, browser, bars, the dock's frame) aren't working areas.
* **Hidden content fades at the edge.** A scroll area whose content runs on past an edge fades it out over
  `--scroll-fade` (48 px) on that edge only, while more is hidden there (`useScrollFade`, `.scroll-fade`): the dock's
  strip, the mixer's strips, the roll's lane tabs. A fade was chosen over an edge shadow.
* **Stacking order** with z-index tokens: sticky parts of a view (`--z-sticky`: playhead, headers), popovers and menus
  (`--z-popover`), dialogs (`--z-dialog`, `--scrim` behind), toasts (`--z-toast`).

**Outlines where lightness can't separate, not around everything:**

1. **Neighbours at the same level** (device cards in a row, mixer strips, inspector rows) have no contrast between
   them, so something must part them: a gap showing the level below where there is room (cards, strips), a hairline
   (`--border`) where there isn't (list rows).
2. **A thing on a different level with nothing like it beside it** (a lone card in the dock) needs no outline: fill
   and gap do it.
3. **Controls** (fields, off toggles, selects) keep a border at `--control-border`, for DS15's 3:1 boundary; ghost
   buttons in dense rows have none.
4. **Floating things** have the lighter fill, the shadow and a hairline border, which carries the edge where the
   shadow vanishes against dark content.
5. **Regions** (inspector beside the arrangement, the dock under it) are parted by one `--frame` rule, not boxed.

**(DS8) Motion: 120 ms ease-out for hover, press and fold, 200 ms for things that enter (popover, toast); nothing
animates a value the user drags.** `prefers-reduced-motion` turns transitions off; the MIDI learn pulse becomes a
steady outline.

**(DS9) Icons: lucide at three sizes, `--icon-s` 12 px (chevrons, ×, inside chips), `--icon` 14 px (beside words, in
small buttons), `--icon-l` 16 px (alone in a 24–28 px button), stroke 2 (1.5 at 16 px).** DAWg's own glyphs
(`Glyphs.tsx`, `BrandMark.tsx`) use the same box and stroke. Every icon-only button keeps a `title` and an
`aria-label`.

### Components

**(DS10) One component per job, with written states.** Turn the class-name families into components in
`ui/src/ui/` (a new folder for the system's pieces; device-specific parts stay in `dock/`):

| Component | Variants | Replaces |
|---|---|---|
| `Button` | `text`, `icon`, `ghost` (no border, in dense rows), `primary` (whole accent, one per dialog), `danger`; sizes s/m/l (DS5) | `.text-button`, `.icon-button`, one-off button classes |
| `Toggle` | on/off: tint when on (DS11); with an icon, a word or a letter | `.slot-toggle`, `.param-switch`, `.toggle-button`, `.corner-toggle`, the mixer's section toggles |
| `TrackSwitch` | mute, solo, arm, monitor, automation; colour plus a shape when on (DS15) | `.switch.*` |
| `Power` | on, off, failed; same in card header, inspector row, mixer row, graph node | Three power drawings and the mixer dot |
| `Segmented` | one of several: whole accent (DS11); words or glyphs; horizontal or vertical | `.segmented`, `.segment` |
| `Select` | the one native `<select>` style with a drawn chevron, at the three heights | Eight per-place select rules |
| `NumberField` | drag-number and click-to-type, boxed or underlined (in text), with unit | `DragNumber`, `ValueText`, `ParamField` look |
| `Knob`, `Stepper` | as now; states below | — |
| `Section` | as now, plus `actions` | — |
| `Readouts` | as now | — |
| `Tabs` | view tabs (underline) and lane tabs (raised); nothing else | Three tab styles |
| `Popover`, `Menu`, `MenuItem` | anchored, context (at pointer), submenu | Four popover styles |
| `Dialog` | title, body, buttons right-aligned, primary last; focus trap, Escape, Enter | `.modal` repeated per dialog, `HumaniseDialog`'s inline styles |
| `Banner`, `Toast` | info, warn, error | The two hard-coded warning bars, error toast |
| `Meter` | horizontal, vertical; stereo; clip | `.meter`, the fader's meter, header meters |

**(DS11) Pressed states follow Typewright's rule.** One of several (a mode, a tool, a view, Wide/Narrow, a segmented
choice): `--accent` fill, `--on-accent` text. On/off (a toggle, a section shown, Sync, Listen, the metronome): tint,
`--accent-soft` fill with `--accent-ink` text *and* a 1 px `--accent` border (so the tint is visible at 1.14:1).
Exceptions that keep their own colour: play (green), record and arm (`--danger`/`--arm`), mute, solo, automation
modes.

**(DS12) Every control has the same states, drawn the same way.**

| State | Look |
|---|---|
| Rest | `--bg-panel` fill, `--control-border` border (fields, toggles), or none (ghost) |
| Hover | `--hover-overlay` over its own fill, `--border-strong` border |
| Pressed / on | DS11 |
| Focus (keyboard) | 2 px `--accent` outline, 1 px offset (as now) |
| Disabled | `opacity: var(--disabled)`, no hover, `cursor: default` |
| Inactive (doesn't apply in this mode, review 1.8) | label and value in `--text-faint`, arc dimmed, still draggable (6.1, 6) |
| Modulated | `--signal-mod` ring on a knob's arc, `--mod-ink` value; a mapped macro adds a dot |
| Automated | value in `--automation-read` colour while playing back; the parameter name underlined |
| MIDI learn | `--warn` outline, steady (pulsing only without reduced motion) |
| Bypassed (device) | body at 0.55 opacity, power off; name not struck through |
| Missing / failed | `--danger` badge, never colour alone |

### Canvases

**(DS13) One canvas theme, from the tokens.** Move `readTheme`, `mix`, `useCanvas` and the shared drawing helpers to
`ui/src/canvas/` and extend `Theme` to all tokens a canvas needs (signals, tones, meters, track switch colours,
fonts, the type scale). Rules for every canvas (arrangement, roll, lanes, waveforms, EQ, filter and dynamics graphs,
step grid, LFO and ADSR editors, waveshaper curve):

* **No colour literals.** Every colour is a theme entry or a `mix()` of entries. The keyboard's slate greys become
  `--bg-panel`/`--text-dim` mixes; the white keys drop to a light grey under `--text`, so content stays brightest.
* **Text.** IBM Plex Sans and Plex Mono only, at `--text-xs` (11 px) minimum, `--text-dim` for scales, and labels
  that collide are dropped (the frequency axis keeps 100, 1k, 10k at narrow widths).
* **Grid.** Three weights: `--border` at 45 % for minor lines, `--border` for beats and decades, `--border-strong` for
  bars and the 0 dB line.
* **Curves.** 1.5 px lines, the accent for the main curve, tones or signals for others; handles 8–10 px circles with a
  `--lcd` fill and a 1.2 px ring, filled on hover; the selected handle filled in the accent.
* **Content in track colour.** Clips and notes use one pair of helpers: `trackFill(colour, strength)` and
  `trackStroke(colour)`. A note's velocity changes its fill between 45 % and 100 % (not from 35 %), so a soft note
  stays visible.
* **Selection** is a `--text` outline (as now) in arrangement and roll; on graphs, the accent.
* **Sharp at any scale.** Every canvas goes through `useCanvas` (device pixel ratio, resize).

### Density and layout

**(DS14) Density rules per region.**

* **Bars** (top bar, mixer toolbar, roll toolbar, view headers): one row at `--control` height, groups separated by
  `--space-5` and a 1 px rule; groups in a fixed order (file, edit, transport, time, view, window). When a bar is too
  narrow, whole groups fold into a `⋯` overflow menu from the right; a bar never wraps to a second row (the roll's
  toolbar does today). The top bar's status group (CPU, MIDI, audio device) folds first.
* **Dock cards:** header at `--control-s`; body padding `--space-4`/`--space-6`; `Section`s laid out in rows of
  knob-sized slots 58 px wide; graphs take the room left and shrink first; container queries decide between knobs
  and fields, never a scroll bar inside a card at the default dock height. A card states its minimum width
  (`panelMinWidth.ts`) and never clips a control.
* **Mixer strips:** as mixer.md X3/X5 (96 and 56 px). Every strip lines up section by section; the track colour bar
  stays at the bottom, by the name (DS16).
* **Inspectors:** a two-column grid (label, control) at `--text-m`, sections in small capitals with folding for
  rarely used ones (Timing).
* **Editors (arrangement, roll, audio editor):** the canvas fills; rulers and lane tabs at `--control`; no rotated
  text — a lane's name is in its tab, the dock's scope path is horizontal in the strip header.
* **Windows:** every window's top bar is its title bar (window-chrome.md); window buttons at the right edge,
  `--control-l` high, the same in all four windows.

**(DS16) Track colour is identity, in one place per view.** A 3 px bar on the *left* edge of anything that is a track
in a list or a panel (track header, inspector title, dock fader), a fill for its content (clips, notes, waveforms). A
selected thing is marked by the accent, never by the track colour. The mixer strip keeps its bar at the bottom, by the
track's name: the eye goes to the names and the faders first, so that is where the colour belongs (6.1, 5).

### Accessibility

**(DS15) Contrast, colour and keyboard.**

* Text ≥ 4.5:1 on its surface (`--text-faint` lifted to about `#8592a0`); boundaries of controls ≥ 3:1
  (`--control-border` about `#4a5868`).
* Colour is never the only signal: arm is a filled circle, record a filled dot with a ring, automation read and write
  are letters (R, W, T, L) in their colours, mute and solo are letters, bypass is the power icon off, a missing file
  has a badge.
* Every control is reachable by Tab inside its panel; F6 moves between panels (as now); arrow keys move inside a
  segmented control, a list or a menu; Escape closes the top popover or dialog; dialogs trap focus.
* The UI honours `prefers-reduced-motion` (DS8).
* A test (`tokens.test.ts`) computes the contrast of the defined pairs, so a token change can't silently break them.

### Entering values

**(DS17) A frequency field takes a note name.** Any field for a frequency parameter accepts a note name as well as a
number: the Filter's cutoff, the EQ's band frequencies (on the band's field and its menu), crossovers, and every other
audio-range frequency, in the dock, the inspector, the graph node's exposed controls and the readouts. It is one
change in `parseParam` (`graph/params.ts`), which every typed parameter goes through, reusing the piano roll's
`parsePitch` grammar:

* `A4`, `C#3`, `C♯3`, `Eb4`, `E♭4`, with C4 = MIDI 60 as in the roll; lower case works (`a4`);
* an optional offset in cents: `A4+20`, `A4 -15c`, `A4+20ct`;
* converted at A4 = 440 Hz (12-TET), then clamped to the parameter's range like a typed number;
* numbers keep working as now (`1000`, `1k`, `2.5 kHz`).

* the fields that take it: every parameter in Hz whose range reaches 1 kHz or more (cutoffs, band frequencies,
  crossovers, a resonator's tuning), not LFO or tremolo rates;
* the field keeps showing the number (`440 Hz`); its tooltip, and the field while being edited, add the nearest note
  and its offset (`A4 +0 ct`).

## 5. Summary

| # | Decision |
|---|---|
| DS1 | Tokens in `ui/src/tokens.css`, named by role; current names kept, roles added |
| DS2 | One meaning per colour: audio, events, modulation, sidechain, status and switches apart; band tones clear of signals |
| DS3 | Type scale of five px steps; one small-capitals style; two families in CSS and canvases |
| DS4 | Spacing 2, 4, 6, 8, 12, 16, 24 px |
| DS5 | Control heights 18, 24, 28 px |
| DS6 | Radii 2, 3, 6 px and round |
| DS7 | Lighter is higher: four surface levels, menus and dialogs on the lightest; hover an overlay; a shadow only on floating things; outlines only between same-level neighbours, on controls, on floating things, and as rules between regions |
| DS8 | Motion 120/200 ms, none on dragged values, reduced motion honoured |
| DS9 | Lucide at 12, 14, 16 px; own glyphs on the same grid |
| DS10 | Components in `ui/src/ui/`: Button, Toggle, TrackSwitch, Power, Segmented, Select, NumberField, Tabs, Popover/Menu, Dialog, Banner/Toast, Meter |
| DS11 | Pressed: whole accent for one of several, tint plus border for on/off |
| DS12 | One state table for every control (hover, focus, disabled, inactive, modulated, automated, learn, bypassed, failed) |
| DS13 | One canvas theme from the tokens; no literals; 11 px floor; grid, curve and handle rules |
| DS14 | Density per region: bars never wrap, dock cards never scroll at default height, no rotated text |
| DS15 | Contrast ≥ 4.5:1 text and ≥ 3:1 boundaries, never colour alone, full keyboard, reduced motion, a contrast test |
| DS16 | Track colour as a left bar for identity and a fill for content (the mixer's at the bottom, by the name); selection is the accent |
| DS17 | Frequency fields accept note names (`A4`, `C#3`, `Eb4+20`), at A4 = 440 Hz |

## 6. Questions for the owner

### 6.1 Answered (2026-11-14)

Every recommendation was accepted except the mixer's colour bar. Elevation (DS7) was settled afterwards: lighter is
higher, as the dock's cards already are, with outlines only where lightness can't part things.

1. **Typewright:** DAWg owns its token names (its current ones), shares values where they overlap and the pressed-state
   rule (DS1, DS11).
2. **Text size:** values at 12 px mono, labels at 11 px; knob slots stay 58 px wide (DS3).
3. **"On":** one `Power` look everywhere, lit in `--accent-ink`; green means audio and level only (DS2).
4. **Sidechain:** its own pink, `--signal-key`, moved further from `--mod` (DS2).
5. **Mixer track colour:** stays at the bottom of the strip, by the name. The eye goes to the names and the faders
   first, so the colour belongs there (DS16).
6. **Inactive controls:** dimmed but still draggable; hidden only where a mode puts another control in its place
   (DS12).
7. **Where things live:** `ui/src/tokens.css`, components in `ui/src/ui/`, `styles.css` split by area as each area
   migrates, plain global CSS, hand-formatted.
8. **Piano keyboard:** grey, so notes are the brightest thing in the roll (DS13).
9. **Light theme:** not for 1.0; token names stay free of "dark".
10. **Bars:** fold whole groups into a `⋯` menu instead of wrapping; the roll's toolbar first (DS14).

### 6.2 Answered: note names in frequency fields (DS17)

The owner accepted every recommendation below.

1. **Which fields.** *Recommendation:* every parameter in Hz whose range reaches 1 kHz or more (cutoffs, band
   frequencies, crossovers, a resonator's tuning), not LFO or tremolo rates, where `A1` (55 Hz) means nothing.
2. **What the field shows afterwards.** *Recommendation:* the number, as now (`440 Hz`); the tooltip and the field
   while it is being edited add the nearest note and its offset (`A4 +0 ct`). Showing note names in the field itself
   would need a per-field switch and costs width.
3. **Tuning reference.** *Recommendation:* fixed A4 = 440 Hz. DAWg has no project tuning; when it gets one, this follows
   it.
4. **Keyboard entry without typing.** A small "note" button or a scroll of semitones on the field? *Recommendation:*
   no; Shift+wheel and Shift-drag already give fine steps, and a key-tracking knob is a different feature.

### 6.3 Raised, for a design of its own: the graph's grid and cable shapes

The owner asked for a graph grid to align nodes on and for cables whose shapes can be edited. Being graph-editing
features rather than design-system rules, they have their own document, graph-grid-and-cables.md (built 2026-11-15).
The look of the grid and of cables follows DS13.

## 7. Steps

Each step is one or a few commits that leave every view working; screenshots before and after over CDP.

1. **Tokens (no visible change).** Add `ui/src/tokens.css` with every DS1 token at today's values; point the 16
   `#d6a05a`, the yellows, the warning surfaces and the sidechain purple at tokens; add the type, space, height,
   radius, z-index and icon tokens. Add the contrast test. *Shared by all later steps.*
2. **Canvas theme.** Move `readTheme`, `mix` and `useCanvas` to `ui/src/canvas/`, widen `Theme`, replace the canvas
   literals (keyboard, arrangement, step grid, EQ, compressor history, sound explorer), one font helper. Fixes the
   axis overprint and the 7–10 px canvas text. *Views: arrangement, roll, lanes, dock graphs, browser preview.*
3. **Colour separations and elevation (visible).** DS2: power look, mute/warn/events apart, band tones, sidechain,
   category borders, contrast lifts (DS15); DS7: menus and dialogs on `--bg-float`, hover overlay, outlines dropped
   where rule 2 says, with before-and-after screenshots to judge the surface steps. One commit, so the colours change
   once. *Views: graph, dock, mixer, inspector, headers.*
4. **Buttons and toggles.** `Button`, `Toggle`, `TrackSwitch`, `Power`, `Segmented`, `Select` in `ui/src/ui/` with the
   DS11 and DS12 states; replace the class families view by view: dock (cards, `ParamSlot`'s toggles and segments),
   then mixer, track headers, inspector, top bar, dialogs. Filter's oversampling becomes the Saturator's segmented row.
5. **Popovers, menus and dialogs.** `Popover`, `Menu`, `Dialog`, `Banner`, `Toast`; focus trap and Escape; move the
   dialogs over (export, normalise, slice, MIDI import, humanise, key and signature dialogs, confirmations).
6. **Bars.** Top bar groups and overflow; the roll toolbar on one row (review 3.12's grouping) with overflow; the
   mixer toolbar; window buttons the same in all windows.
7. **Tabs and rotated text.** One `Tabs`; the dock's scope path horizontal; the roll's lane labels in tabs only.
8. **Note names in frequency fields (DS17):** `parseParam` with tests, then the tooltip and editing hint. Independent
   of the other steps; can go first.
9. **Type and spacing sweep.** Per area, replace literal sizes, gaps, heights and radii with tokens; values at 12 px
   mono, labels 11 px. `styles.css` splits by area as each is swept.
10. **Docs.** A short `docs/design/ui-conventions.md` checklist for new panels (which component for which parameter,
    which token for which colour), linked from CODING_GUIDELINES.md.

Order: 1 and 2 are invisible and unblock everything; 3 is the one visible colour change; 4 and 5 are the bulk; 6, 7
and 9 follow per view and can run in parallel sessions once 4 has landed, each touching one area's files.

## 8. Notes from building

### Steps 1 and 2 (2026-11-14)

* **Tokens:** `ui/src/tokens.css`, imported before `styles.css`, holds every token; `styles.css` no longer defines any
  and has no colour literals. Every use of the old `--accent-2` went to its role (`--signal-audio`, `--ok`, `--on`,
  `--meter-low`, `--monitor`, `--automation-read`, `--category-effect`, `--playhead`), so step 3 changes values, not
  uses. Uses of `#d6a05a` went to `--warn`, `--mute`, `--signal-events`, `--learn`, `--automation-touch` or
  `--marker`. `--eq-mid` and `--eq-side` became `--tone-mid` and `--tone-side`.
* **Kept or changed from DS1:** the modulation colour keeps its name, `--mod` (28 uses; `--signal-mod` bought nothing).
  The track palette stays in `colours.ts` only, as the engine's order follows it and no stylesheet needs it. Meters
  have low, mid, clip and an input's low end (`--meter-input`) rather than four steps. Timeline and EQ band tokens
  were added for the canvases.
* **Not used yet**, defined for step 3: `--bg-float`, `--hover-overlay`, `--control-border` (`#64717f`, the lowest grey
  at 3:1 on `--bg-panel`), the type, space, height, radius, icon and motion scales.
* **Layers:** menus and popovers are `--z-popover` (30), over dialogs (`--z-dialog`, 20); banners and toasts
  `--z-toast` (50). Before, a toolbar popover (60) and the MIDI popup (60) sat over dialogs but context menus (11)
  under them. Stacking inside a view (2 to 5) stays local.
* **Two fixes on the way:** `var(--warning, …)` named a token that never existed, so the frozen icon and the CPU
  warning used their fallbacks; they are `--warn` now. The humanise dialog used four tokens that don't exist
  (`--bg-sunken`, `--text-muted` and fallbacks from another palette); they are `--lcd`, `--text-dim`, `--border` and
  `--accent`.
* **Contrast test:** `ui/src/tokens.test.ts` reads `tokens.css` (Vitest is told to keep that one stylesheet,
  `vite.config.ts`) and checks 40 pairs. `--text-faint` on `--bg-raised` and `--bg-panel` is a `todo` until step 3
  lifts it.
* **Canvases:** `ui/src/canvas/theme.ts` (`readTheme`, read once and cached; `font`, `mix`, `alpha`, `LIGHT`, `SHADE`)
  and `ui/src/canvas/useCanvas.ts` (moved from `pianoroll/`). No canvas has a colour literal or a face of its own any
  more: Inter and JetBrains Mono are gone, text is 11 px Plex Sans or Plex Mono from the type scale, and 12 px for
  the controller lane's empty message. The one exception is a glyph inside a handle (a band's number, T and R on the
  compressor), sized to fit the handle.
* **Visible changes from step 2:** canvas text at 11 px instead of 9–10 px; the piano keyboard grey (`--key-white`,
  `--key-black`), its labels only on rows tall enough for them (12 px, the default zoom has 14); a note's name only on
  rows of 12 px or more; the frequency axis drops names that would overlap (100, 1k, 10k, 20k first), so "10k" and
  "20k" no longer overprint; the EQ's M and S marks at 11 px. Checked in the app: arrangement, roll, EQ, Filter.
* **Left for later:** the Monaco editor's theme (`faust/monaco.ts`) still has its own colour list, as Monaco wants
  literal colours when its theme is defined; it could read `readTheme()` there. Two-channel curves (the modulation
  effects' L and R) are the accent and `--signal-audio`; DS2's `--tone-left` and `--tone-right` are for step 3.

### Step 3 (2026-11-14)

* **Colours (DS2):** power and other "on" states in `--accent-ink` (`--on`), so green is audio and level only;
  `--warn` amber (`#e8b84e`), events keep their orange, mute redder (`#e0904f`); the second band tone cyan
  (`#40b8c8`), as the green read as a plain audio port; sidechain pink (`#ee8fc6`), apart from `--mod`; the effect
  category a quiet green (`#6e9a7b`).
* **Contrast (DS15):** `--text-faint` `#8592a0`, 4.5:1 on every surface up to `--bg-hover`; fields, selects,
  checkboxes, switches, segmented controls, steppers and number boxes have `--control-border`. The contrast test
  covers all three surfaces for the faint text now, and pink on a fill.
* **Depth (DS7, added after the first screenshots):** device cards on `--bg-card` with `--shadow-card`; the dock's
  strip (`.device-strip-area`, wrapped in `DeviceDock`) and the mixer's strips (`.mixer-strips-area`) on the ground;
  the inset on those two, the arrangement's lanes, the graph canvas and the roll's note area; the scroll fade on the
  dock's strip, the mixer's strips and the roll's lane tabs (`ui/src/ui/useScrollFade.ts`, which sets
  `data-more-start` and `data-more-end` as the area scrolls or its content changes size; the fade's ends are
  registered properties, so they ease in and out, and not with reduced motion). The audio editor has no inset yet:
  its canvas has no wrapper to carry the overlay.
* **Elevation (DS7):** menus, popovers, the plugin picker, dialogs, banners, toasts and tooltips on `--bg-float` with a
  `--border-strong` hairline and `--shadow`; their own drop shadows went. Every `:hover` that used `--bg-hover` is the
  overlay (a gradient image over the element's own fill); `--bg-hover` stays for states that aren't hover (a picked
  shortcut area, a selected group strip, a filled pad). Device cards lost their outline (rule 1: the gap parts them);
  mixer strips keep theirs, as only 2 px part them.
* **Pressed (DS11):** `.segmented` choices are the whole accent; groups of on/off toggles are `.segmented.toggles`
  (the mixer's sections, the Delay's Sync, Link and Ping-pong, the modulation effects' Sync), tinted with an accent
  border.
* Screenshots of the same scene before and after are in the session notes (`step3-compare/`).

### Step 4 (2026-11-14)

* **Components** in `ui/src/ui/controls.tsx`, drawn by `ui/src/ui/controls.css` (loaded before `styles.css`, so a
  view's placement of a control wins over its defaults): `Button` and `IconButton` (sizes `s` 18, `m` 24, `l` 28 px;
  variants `primary`, `danger`, `ghost`), `Toggle` (on or off: `aria-pressed`, the tint with an accent border),
  `Power`, `TrackSwitch`, `Segmented` and `Select`. They keep the old class names (`text-button`, `icon-button`,
  `switch`, `small`) as their hooks, so the views' placement rules still apply; the sizes are now the three heights.
* **Buttons:** all 190 `icon-button` and `text-button` buttons are `IconButton` and `Button`, moved by a script: a
  plain icon button (28 px before) is `l`, a small one (24 px) `m`, and the per-view sizes (16, 20 and 22 px in card,
  track, automation, take, send and returns headers, the EQ's band menu, the mixer's group fold) are `s` or `m`, their
  rules gone. Text buttons are 24 px with 11 px words (they were 22.75 px with 10 px). Buttons in a `<form>` that
  submitted by default say `type="submit"`; everywhere else a button is `type="button"`.
* **Power:** one switch for devices (dock, inspector, node inspector, graph nodes, mixer inserts), the EQ's bands,
  sends and automation lanes: a borderless icon, lit in `--on`, its tooltip naming what it switches. The mixer's
  insert rows keep it at 14 px.
* **Toggles:** the slot toggle, the node inspector's and graph nodes' `ParamSwitch`, Faust's toggle and momentary
  button and the arrangement's Tempo corner button are `Toggle`; `.toggle-button`, `.slot-toggle.active` and
  `.param-switch.active` are gone. Slot toggles still say On or Off where they have no icon.
* **Track switches:** mute, solo and arm in the track headers, the dock's fader and the mixer, and the global M and S,
  are `TrackSwitch`. The headers' other small buttons (instrument, expand, automate, takes) keep the switch's class
  for now.
* **Segmented:** the Filter's oversampling is the Saturator's segmented choice (Off, 2x, 4x), both through
  `Segmented`. The other `.segmented` rows look the same already and move to the component as their panels are
  touched.
* **Not yet:** `Select` exists but the views' selects stay native `<select>`s with the shared style; the header
  switches above; the `.active` class on buttons (kept as an alias of the two pressed states).
* **Track headers (owner, after the first screenshots):** the switches stand in fixed columns from the right edge:
  mute and solo last, as every track has them, so they line up down the arrangement; the track's own switches (arm,
  then monitoring or the instrument's editor) in two columns to their left; the automation expander and mode under
  mute and solo, with the add button before them. The tall tier uses the same two rows; the compact tier puts the
  expander before the track's switches. Lit track switches (mute, solo, arm, monitoring on, touch and latch) are their
  colour's tint (18 %), border and letter, at least 4.5:1 for the letters; the write mode stays filled, as it records.
  The master column has the track headers' right border, so its switches stay in the columns.
* **Whole pixels:** the top bar was 51.5 px, which put everything under it on half pixels and blurred every glyph. It
  is 52 px with an even 36 px inside and 20 px lines (the position display 22 px); the status bar is 24 px, so the
  dock isn't on a half pixel either. Track header rows, gain and pan fields are 20 px; switch icons are 10 or 12 px, so
  they land on whole pixels in an 18 px inside; the pan is centred. Measured from pixels (each switch captured at 4x,
  its ink located): every icon within 0.25 px of its box's centre, letters within 0.13 px, the rest being the glyphs'
  own odd sizes.
* **Segmented icons:** a row of icon-only segments is tighter (5 px sides), and a segmented row never shrinks below its
  content: the Delay's Sync, Link and Ping-pong were cut off in their 81 px column.

### Step 5 (2026-11-14)

* **Components** in `ui/src/ui/overlays.tsx`: `Popover` (a box that closes on Escape or a press outside), `Menu` (a
  Popover with `role="menu"` that the arrow keys, Home and End move through) and `Dialog` (`role="dialog"`,
  `aria-modal`, its title, Escape to cancel, Tab kept inside, a press outside closing it only where asked). All give
  the focus back to what had it when they close, and Escape closes only the innermost one (a stack), caught before
  the window's shortcuts.
* **Drawn into the body:** a Popover or Menu placed at a point (`at`, a right-click or a button's corner) is drawn
  into the document's body through a portal and moved back inside the window when it would hang over an edge. A
  device card is a size container, which makes it the containing block for anything `position: fixed` inside it: its
  menu was clipped by the dock. The piano roll's note menu now takes window coordinates for the same reason. Dialogs
  are always drawn into the body; their backdrop keeps presses from reaching the views that opened them (the top bar
  would drag the window).
* **Moved over:** the twelve dialogs (key, signature, text prompt, save changes, confirmation, export, MIDI import,
  normalise, slice, humanise, the shortcut reference, the update dialog), nine by a script; all 27 menus and popovers
  (clip and lane menus, track header and colour menus, device menu, the top bar's menus, MIDI learn and MIDI settings,
  the plugin and automation pickers, send menus, the strip's and the EQ's menus, the modulation column's, presets,
  the sampler's, the code view's script menu and save box, the mixer's sends, the roll's note menu, the vibrato
  popover). Checked in the app: Escape now closes all eleven overlays tried (it closed four of them before), Tab stays
  inside a dialog, and the arrows move through a menu.
* **Not changed:** toasts and banners already carry `role="alert"` or `"status"` and step 3's look, so they keep their
  components. The sampler's mapping popover is placed from its bottom right corner and stays where it is drawn.

### Steps 6, 7 and 9 (2026-11-15)

* **Bars (step 6):** `ui/src/ui/Bar.tsx` lays a bar out as items in one row that never wraps: it measures each group,
  and the ones that don't fit fold into a "⋯" menu at the bar's end, the highest `fold` first and among equals the
  rightmost. The top bar folds the status first, then undo and redo, the views and the mixer, then the tempo; the
  transport stays longest. The piano roll's toolbar (which wrapped onto two rows) folds the MIDI effects note, then the
  vibrato and links, the scale and tools, the help and zoom, and keeps the grid (Snap, Swing, Strength) longest. The
  mixer's toolbar folds its global switches, the transport, the sections and the width. A portaled popover stops its
  presses from reaching the bar, so a press in the "⋯" menu never drags the window.
* **Tabs (step 7):** `ui/src/ui/Tabs.tsx`, two kinds: view tabs (words with an accent underline: the browser's Sounds,
  MIDI files, Plugins, no longer in capitals) and lane tabs (raised into their lane: the roll's). Both are a
  `tablist` the arrow keys move through. `.panel-tab` and `.lane-tab` are gone.
* **No text written upwards (step 7):** the dock's scope path inside a component is a row over the strip, read left to
  right; the strip's dividers (MIDI effects, modulation) and the mixer's Returns gap are icons with tooltips; the
  roll's lane name reads across at the top of its corner. The one exception is a collapsed device card's name: the card
  is 24 px wide.
* **The sweep (step 9):** values people read and set are 12 px mono (knob values, fields, readouts, gain, pan, send
  levels, the fader readout, steppers, number boxes, the EQ's tooltip and menu fields); the inspector's knob slots are
  58 px like the dock's, so a value stays on one line. Every literal font size is a token (`--text-xs` to `--text-xl`)
  except the welcome heading (40 px), a relative 0.9em and the 9 px glyph in a port's width marker; the selects'
  10 px rem sizes are 11 px. Radii are the four tokens (1 px bars and 50 % dots stay). Every gap is on the spacing
  scale, the odd ones moved to the nearest step (3 to 4, 5 to 6, 10 to 8, 14, 18 and 20 to 16), except 1 px lines and
  the welcome screen's 48 px. Paddings and control heights in the views are not swept yet.
* **The stylesheet split (step 9):** `styles.css` is fifteen files in `ui/src/styles/` (base, topbar, workspace, roll,
  inspector, project, arrangement, browser, mixing, graph, editing, dock, mixer, devices, depth), each a stretch of the
  old file in its old order, loaded in that order by `styles/index.css`, so the cascade is unchanged. Rules for one
  area that sat apart in the old file (the dock's panels in `dock.css` and `devices.css`) stay apart for now.

### Step 8 (2026-11-15)

* **Parsing:** `parseParam` (`graph/params.ts`) reads a note name for any parameter that `takesNoteNames`: in Hz with a
  range reaching 1 kHz, so cutoffs, band frequencies, crossovers and a resonator's tuning do, LFO and tremolo rates
  don't. `parseNote` takes a letter, an optional sharp or flat (`#`, `♯`, `b`, `♭`), an octave (C4 is MIDI 60) and an
  optional offset in cents (`+20`, `-15c`, `+20ct`, `−50 cents`), at A4 = 440 Hz, and the result is clamped like a
  typed number. Every place that types a parameter goes through it, so the dock's knobs and readouts, the node
  inspector, a component's exposed controls and the EQ's band menu all take notes.
* **Showing it:** the value stays a number (`440 Hz`). `noteOf` names the nearest note and how far off it is
  (`A4 +0 ct`, `B5 +21 ct`); it is in the value's tooltip, and while a field is typed in it shows over the field and
  follows what is typed (`ValueText`'s `hint`, `paramHint`). The EQ's band menu field has it as its tooltip. A
  `ParamField` (a number box to drag) parses notes but shows no hint yet.
* **Checked in the app:** typing `A4`, `C#3+20`, `eb6` and `2k` into the Filter's cutoff set 440 Hz, 140 Hz, 1.24 kHz
  and 2.00 kHz, with the hint showing `A4 +0 ct`, `C#3 +20 ct`, `D#6 +0 ct` (flats are named as sharps) and
  `B6 +21 ct` while typing. Tests in `graph/params.test.ts`.

### Step 10 and the loose ends (2026-11-15)

* **The checklist:** `docs/design/ui-conventions.md`, linked from `CODING_GUIDELINES.md`: which component for which
  parameter, overlay and structure, which token for colour, type and space, depth, and what to check before a view is
  done.
* **Paddings and heights:** paddings in `ui/src/styles/` are on the spacing scale (2 to 4 px to `--space-1` and
  `--space-2`, 5 and 6 to `--space-3`, 8 and 9 to `--space-4`, 11 and 12 to `--space-5`, 16 and 18 to `--space-6`,
  24 to `--space-7`); 18, 24 and 28 px heights in rules for controls are `--control-s`, `--control` and `--control-l`.
  Left literal: the top bar's `8px 14px 7px` (52 px with an even inside), 1 px insets, 7, 10 and 14 px where a
  measured row depends on them, and layout sizes of 20 px and more. Two rows moved by the sweep were put back: the
  Delay's icon choices get a 2/4 px inset so the row fits its card, and a track header's pan field sits on a whole
  pixel again (its row is centred, the gain field 68 px wide).
* **Loose ends:** the Monaco editor's theme reads the tokens (`--syntax-*` added for its token colours); the audio
  editor's canvas sits in a `.working-area`; the mixer's arm circle is the track header's (10 px, 2.5 px stroke); a
  `ParamField` shows a frequency's note in its tooltip; inactive controls are dimmed but still draggable (answer 6);
  `Popover` and `Menu` take `anchor="end"` to open upwards from a point, used by the Sampler's mapping menu.
* **Not checked in the app:** the Sampler's mapping menu opening upwards (the templates have no sample on a pad) and the
  graph's "not connected" chip.
