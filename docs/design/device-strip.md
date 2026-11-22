# Design: the device strip

Status: **accepted, being built**. The owner accepted this on 2026-11-03. It replaces the bottom dock's Channel and Device tabs (channel-strip.md, C4; phase11-faust-panels.md, section 5).

## 1. What this is

One horizontal strip across the bottom of the window shows the devices of one graph, in signal order, and scrolls sideways when they don't fit. The track's fader stays fixed at its right.

```
 ┌ route line ─────┬──────────────┐        ↺──────┐
 │ Vital │ EQ      │ Saturator    │ Comp   │ Dub  │ Gain ┃ LFO  ┃▐ Fader
 │ [Open]│ ~curve~ │ ─ knobs ─    │ meter  │ Echo │      ┃ rate ┃▐
 └───────┴─────────┴──────────────┴────────┴──────┴──────┸──────┸▐
 ◄──────────────── scrolls horizontally ────────────────►
```

## 2. Decisions

**(D1) One row, in signal order.** A graph can't be shown faithfully in a line, so order and wiring are kept apart. Devices are sorted depth first (`stripOrder` in `ui/src/dock/strip.ts`), leaving out each loop's way back, so a loop can't break the order, and modulation cables, which aren't the signal's route. Parallel branches follow one another in the row: of the nodes whose inputs are all placed, the one fed by the most recently placed comes next, and a device before the Gain that ends the track. `signalOrder` in `graph/layout.ts` was the first idea, but it takes ready nodes by id, which interleaves branches. Stacking them was rejected, as the dock is too short for an EQ to share its height. Showing only the selected node's path was rejected, as the strip would rearrange itself as you work.

**(D2) Arrows and a route line show the wiring.** On a custom graph the cards stand 14 px apart, and a card feeding the one right after it has an arrow filling the gap between them, level with their headers: it touches both cards' borders and goes into neither, and neither does anything else of the wiring. A cable that runs around other cards, and a loop's way back, are drawn in a lane above the cards, 13 px, on three levels 4 px apart, the shortest lowest: each leaves near its card's right edge and drops into the card it feeds, and a way back is dashed, in the loop colour, from centre to centre. The lane is there only when such a cable is, as every pixel of it comes off the panels: with it, the card header is 20 px so that the Compressor, Feedback and Delay still fit the dock's default height. On a simple chain neither is drawn, as the order says it all. Hovering a card marks the cards it connects to, modulation included, and fades the other cables (`graph/focus.ts`). On a simple chain only modulation links are marked: the order already shows the audio route, and marking the neighbours looked like a stray highlight. Clicking the lane opens the graph view.

**(D3) What each node shows.**

| Node | Card |
|---|---|
| Built-in effect | Its panel, resizable |
| Plugin (VST, CLAP) | A narrow card: name, Open, bypass, crash and latency badges |
| Faust | Its script's controls |
| Component | Its name, pinned knobs and Open |
| Instruments | In the inspector and their own window since 2026-11-09 (macros-and-modulation-column.md, MC14); before, a card first in the row |
| Feedback | Its panel, marked ↺, after what feeds it |
| LFO, ADSR, Note Expression, Macros | In the dock's modulation column since 2026-11-09 (macros-and-modulation-column.md, MC12); before, a section after a divider labelled Modulation |
| Sequencer, Mix Input, Track Output | Not shown |
| Gain | Hidden on a simple chain unless modulated, as the fader sits beside it; a card on a custom graph |

Plugin GUIs aren't embedded: they have fixed sizes and run in helper processes. Open uses the plugin's own window, as now.

**(D4) The strip shows the same level as the graph view.** Open on a component or Voice Instrument takes the graph view, and with it the strip, inside. A narrow bar at the strip's left edge, kept there as it scrolls, has the way up a level and the path from the track, written upwards like a collapsed card's name; each name in it goes back to that level. In the arrangement it shows the selected track's top level, even while the graph view is still inside something.

**(D5) Width and collapse are the user's layout, not the project's.** A width belongs to a node type, is set by dragging a card's right edge, and is remembered across projects, with a default and a minimum for each type. The type's minimum is only a floor: a card is never narrower than its panel needs at the dock's current height, which is measured live (the panel's min-content width, `ui/src/dock/panelMinWidth.ts`) or reported by a panel that trades space itself, such as the compressor shrinking its curve. Resizing stops there and a narrower remembered width is raised to it, so controls can never be pushed over a graph or out of the card. Square graphs (the Saturator's curve, the Utility's goniometer) prefer the body's height as their width, through container query units, and narrow with the card down to a small floor. Collapsing, to a 28 px card of its power switch over its name written upwards, belongs to a node: it's kept by project file and node, the latest 500. A collapsed card runs no analyser. Neither is in undo or marks a project as changed.

**(D6) The tabs go.** The strip covers both. A selected device is a highlighted card: selecting a node elsewhere scrolls to it, and clicking a card selects its node. The dock's own header goes with them, to give the panels its height; the View menu hides the dock.

**(D7) Bypassed devices stay full size,** dimmed, so nothing shifts when bypass is toggled.

**(D8) Only what's near the screen runs.** A card mounts its panel while on screen or within 400 px of it, and runs its analyser only while on screen, as a long chain can't run every spectrum at once: each redraws 30 times a second. Cards of plain knobs and plugins watch nothing.

**(D9) Scrolling.** The wheel scrolls the strip sideways, and so does the scrollbar (2026-11-12; it took Shift before).
A plain wheel goes to what's under it instead when that takes the wheel (a field clicked to nudge it, the EQ's graph)
or is a list in a card that can still scroll up or down. Shift+wheel scrolls the strip over anything.

**(D10) Editing.** On a simple chain, add between cards and drag to reorder. Each effect has a 14 px gap before it, the same as on a custom graph, whose + (shown on hover) opens the effect picker for that place, and a narrow Add effect slot follows the last; a track without an instrument starts with a Choose instrument slot. An effect is dragged by its header, or a collapsed one as a whole, and lands in the gap it's dropped on, or before or after the card it's dropped on, by which half; the gap or slot it would land in is marked. The instrument and the Gain stay where they are. On a custom graph the strip adjusts sound and doesn't rewire, the same rule as the inspector (phase8-node-graph.md, G6). A device from the browser (the Plugins tab) dropped in a gap or on a card goes in there; dropped anywhere else in the strip it goes where it belongs, an instrument in the instrument slot and an effect after the others. A right-click on any card opens the device menu the inspector, the mixer and the graph view share (`components/DeviceMenu.tsx`): open or reload a plugin, open a component or voice graph, edit a script, replace, move, bypass, remove. Delete removes the selected card, unless a control inside it has the focus (an EQ curve deletes its band).

## 3. Steps

1. Check every panel at narrow and wide sizes, and set default and minimum widths (section 4).
2. The strip for simple chains, replacing the tabs, with plugin cards, widths and lazy analysers. Done: a custom graph is shown in the inspector's order until step 3.
3. Graph order, the route line and hover focus. Done.
4. Nesting, the modulation section and collapse. Done.
5. Insert and reorder on chains. Done.

## 4. Panel widths

Measured in the app at the dock's default height of 220 px, by narrowing each panel until something scrolled or stuck out, then looking at the result: a panel that doesn't overflow can still be unusable, as the Filter is at 160 px. The values are in `ui/src/dock/deviceWidths.ts`.

| Device | Default | Minimum | Note |
|---|---|---|---|
| EQ | 420 | 240 | Its curve takes any width; below 240 the band handles crowd |
| Filter | 360 | 280 | Overflow-free from 160, but its labels collide there |
| Compressor | 320 | 280 | The curve, meter and knobs just fit |
| Saturator | 300 | 280 | |
| Delay | 300 | 300 | Narrower, its Sync and Link switches and note value are cut off |
| Limiter, Utility | 260 | 200 | |
| Feedback | 340 | 320 | Narrower, its Clear button runs into the Time knob |
| ADSR | 320 | 200 | |
| LFO | 420 | 320 | |
| Loop Reverb | 660 | 640 | Its knob groups don't reflow, so it has little give |
| Sampler | 880 | 824 | A fixed 800 px inside: wider than the whole dock on a small window, so the strip's scrolling matters here |
| Component | 320 | 200 | Grows with its knobs |
| Plugin, Voice Instrument | 150 | 150 | A name and a button |
| Others, and Faust | by knobs | by knobs | Plain knobs: as many columns as two rows need |

Found while measuring, for step 2:

* All panels stretch to whatever they're given, so none needs rewriting.
* The LFO overflows its height by 23 px at every width, and the Compressor, Delay, Saturator, Utility and Loop Reverb by 8 px when narrow, at the dock's 220 px. The card's header has to be smaller than today's dock header, which sits on its own row with the tabs.
* A preset selector, a name and a power button don't fit in a 160 px header: the header needs to give way first.
* EQ, Delay, Limiter, Utility, Feedback and ADSR minimums are overflow limits plus a margin, and were judged less closely than the rest. They may move once they're seen side by side. Delay and Feedback did, in step 3: without overflowing, their controls ran into each other.
