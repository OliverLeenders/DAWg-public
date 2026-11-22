# Design: the graph's grid and editable cables

Status: **built** (2026-11-15), notes in section 6. Raised by the owner during the design-system discussion
(design-system.md, 6.3); the decisions are G1–G9, the owner's answers in section 5.

## 1. What exists

* The graph view (`ui/src/graph/GraphView.tsx`, React Flow) draws a dot background every 20 px
  (`<Background variant={Dots} gap={20} />`). Nodes go where they are dropped; `onNodeDragStop` stores the rounded
  position with `setNodePosition`. Nothing snaps.
* A node's ports are 18 px apart (`.graph-port` line height 14 px plus the 4 px gap of `.graph-ports`), and the first
  port's height depends on the header. So two nodes can't be lined up so that a cable runs straight, other than by eye.
* Positions are stored along the flow: `x` along it, `y` across. The Flow switch (left to right, top to bottom)
  transposes them for the view, so a graph keeps its shape in both directions.
* A cable (`RoutedEdge.tsx`) is a Bézier curve, or the route the last Layout (ELK) found while its nodes stay where
  Layout put them, or a route around nodes it would cross (`routing.ts`). Layout's routes are kept in a map in the
  view (`routeStore`), not in the project: they are gone after moving a node's end or reopening the project.
* A connection in the model (`Connection`, engine `Model.h`) is its two ends only: `fromNode`, `fromPort`, `toNode`,
  `toPort`.

## 2. Decisions

### The grid

**(G1) Nodes snap by their first port.** A dragged node snaps so that the centre of its first port (input side, or
output side for a node with no inputs) lies on a grid point. The header's height then doesn't matter, and two nodes
snapped at the same height have their first ports level.

**(G2) The grid is 20 px, and ports are 20 px apart.** `.graph-port` grows from 14 to 16 px line height (gap stays
4 px), so every port of a snapped node lies on a grid line, and a cable between two ports at the same height on two
snapped nodes is straight. The dots stay every 20 px; every fifth one is a little brighter (DS13's grid weights).

**(G3) Snapping is on by default, with a toggle.** A "Snap" toggle in the graph toolbar (DS11: tint when on),
remembered per user. Holding Shift during a drag skips snapping for that drag (not Alt: Linux window managers take
Alt-drag). Layout's results are snapped as well. Existing projects keep their positions until a node is moved.

**(G4) Align and distribute.** With two or more nodes selected, the toolbar's "Align" menu offers: align inputs
(the selection's first input ports on one line), align outputs, align along the flow (same `x`), and distribute
evenly across the flow. Each is one undo step (a batch of `setNodePosition`).

### Cables

**(G5) A cable can have bends, stored in the project.** `Connection` gets an optional `bends`: a list of points in
flow coordinates (as node positions are), so they follow the Flow switch. A cable without bends is drawn as now (a
curve, or around what it would cross). A cable with bends is drawn as straight segments between its port, its bends
and the other port, with rounded corners (4 px radius), so a hand-routed cable reads as deliberate.

**(G6) Editing a cable.**

* **Drag a segment** sideways to move it: a horizontal segment moves up or down, a vertical one left or right; its
  neighbours stretch. Dragging a cable that has no bends gives it two (a step), at the point grabbed.
* **Double-click a cable** to add a bend there; **double-click a bend** to remove it.
* Bends snap to the grid when snapping is on (G3).
* A cable's right-click menu has **Reset cable** (removes its bends), and the graph toolbar's menu **Reset all cables**
  for the selection or the whole graph.
* Selection and hover as cables have today; the bends show as small handles (DS13) only on a hovered or selected cable.

**(G7) Layout writes its routes as bends.** Layout stores ELK's routes in the cables' `bends`, in the same undo step as
the positions, so a laid-out graph survives a reload. `routeStore` and `routeStillHolds` go; the routing around
nodes (`routing.ts`) stays for cables without bends.

**(G8) Moving a node keeps its cables' bends.** When an end moves, only the segment touching that port stretches (and
its neighbour, to keep right angles); the bends in between stay where they are. Moving several selected nodes
together moves the bends of cables between them along with them. Reconnecting a cable to another port keeps its
bends; disconnecting drops them.

**(G9) One command, no format bump.** `setConnectionBends` (trackId, scope, the connection's four ends, `bends`)
sets or clears them, as one undo step per gesture. The project file stores `bends` only where a cable has them; old
projects load as they are (DAWg has no projects to stay compatible with yet). Components and voice graphs use the
same `Connection` and get the same editing.

## 3. Not now

* Reroute dots as nodes of their own (Blender's, Unreal's): they would put nodes without audio in the model; bends do
  the same job.
* A cable style per graph (all straight, all right-angled): bends and Layout cover it.
* Snapping to other nodes' edges (smart guides): G4's align commands do it on request.

## 4. Steps

1. **Ports and grid (UI).** Port pitch 20 px (G2), snap by the first port (G1), the Snap toggle and Shift (G3),
   Layout's positions snapped. Tests for the snap arithmetic in both flow directions.
2. **Align and distribute (UI)** (G4).
3. **Model and command.** `Connection::bends` in the engine model, its serialisation, `setConnectionBends`, and
   reconnecting and disconnecting as G8 says; engine tests (round trip, undo, components, voice graphs).
4. **Drawing and editing (UI).** Cables drawn through their bends with rounded corners (G5); segment drag, double-click
   to add and remove, handles, Reset (G6); end segments stretching when a node moves, bends moving with a selection
   (G8). Tests for the segment arithmetic.
5. **Layout into the model (UI)** (G7): Layout writes bends; `routeStore` goes.
6. **Help.** The graph's part of the keyboard and mouse reference (`components/shortcuts.ts`): Shift-drag, cable drag,
   double-clicks, Reset.

## 5. Owner's answers (2026-11-14)

Every recommendation accepted:

* Snap by the first port's centre, with ports 20 px apart (G1, G2).
* Snap on by default, a toolbar toggle, Shift to skip (G3).
* Align and distribute for a selection (G4).
* Bends per cable stored in the project, edited by dragging segments and double-clicking (G5, G6); not reroute dots
  or a per-graph cable style.
* Layout saves its routes as bends (G7).
* Moving a node stretches only the end segments (G8).
* An optional `bends` on the connection without a file-format bump (G9).

## 6. Notes from building (2026-11-15)

* **Grid (steps 1 and 2):** `ui/src/graph/grid.ts` (snap arithmetic, `snapLayout`, `fitBends`) and `align.ts`, with tests.
  The Snap toggle sits beside Flow in the graph header and is remembered with the window layout (`graphSnap`). A drag
  snaps the node under the pointer and moves the rest of the selection by the same amount; a snapped position is
  stored as it is, as a port's centre can sit half a pixel into its node. Align lines up with the node furthest
  upstream and ends on the grid with snapping on. A node dropped from the browser or added from the menu (a patch's
  nodes together) moves onto the grid once it has been measured, in the same undo step as the adding.
* **A fix on the way:** a node's output column stretched to its input column's height, so its first output sat 10 to
  20 px below its first input and Layout left every cable a small step. `.graph-ports` starts at the top now, so a
  node's first input and output are on one row and Layout's chain is straight.
* **Top to bottom:** ports there are spread across the node's width, not a grid step apart, so only the first port is
  on the grid; Layout's bends are still lined up with the ports.
* **Model (step 3):** `Connection::bends` (`CablePoint`, at most `limits::maxCableBends`, 64), saved only where a cable
  has them, and `setConnectionBends`, undone as "Edit cable". Connections compare by their ports only, so a cable with
  bends is the same cable to `connect`, `disconnect` and the graph compiler, which doesn't rebuild for a bend.
  Grouping into a component and ungrouping make new cables, without bends.
* **Editing (step 4):** `ui/src/graph/cables.ts` and `RoutedEdge.tsx`. The bends are drawn lined up with the ports
  (`fitBends`), so moving one node only stretches the stretches at its ports (G8) with nothing stored. Besides a step,
  dragging an unbent cable across the flow pulls out a detour of four bends, whose stretch aside follows the pointer.
  A double-clicked stretch gets a pair of bends at that point; dragging the half not at a port makes a step there.
  Taking a bend out takes its neighbour too, so no stretch is left slanted. The stretches at the ports can't be
  dragged. "Reset cables" in the canvas bar clears the selection's cables, or all of them.
* **Moving a cable's end (G8):** a cable's input end can be dragged to another port, its output end once the cable is
  selected (a drag from an output otherwise makes a new cable). It is one undo step and keeps its bends, unless the
  cable now closes a loop and gets a Feedback node; let go on empty canvas, the cable comes out.
* **Checked in the app:** ports 20 px apart, Layout's chain on one grid row with cables straight, a dragged node on
  the grid (and off it with Shift), a detour pulled out and moved, undo, Align inputs, Reset cables, top to bottom.
