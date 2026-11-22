import type { Connection, GraphNode, NodeTypeInfo, TrackGraph } from '../bridge/protocol';

// Putting a node in a cable's path and taking it out again (docs/design/phase8-node-graph.md, section 7). Dropping a node
// without connections on a cable makes A -> B into A -> node -> B and moves what is to the right out of its way; shaking
// a node while dragging it takes it out, and A -> B closes again.

type InfoFor = (node: GraphNode) => NodeTypeInfo | undefined;

/**
 * The two cables that replace `cable` when `node` is inserted into it, or null if it can't be: the node must have no
 * connections, and an input and an output of the kind the cable carries (its first of each, so a sidechain input stays
 * free). Modulation cables have no signal path to continue through a node. No further checks are needed: a node
 * without connections can't close a loop, and each cable's far end keeps the one connection it had.
 */
export function insertionCables(
  graph: TrackGraph,
  node: GraphNode,
  cable: Connection,
  infoFor: InfoFor,
): [Connection, Connection] | null {
  if (Object.values(graph.connections).some((c) => c.fromNode === node.id || c.toNode === node.id)) return null;

  const from = graph.nodes[String(cable.fromNode)];
  const kind = from ? infoFor(from)?.outputs[cable.fromPort]?.type : undefined;
  if (!kind || kind === 'modulation') return null;

  const info = infoFor(node);
  if (info?.userEditable === false) return null;
  const input = info?.inputs.findIndex((port) => port.type === kind) ?? -1;
  const output = info?.outputs.findIndex((port) => port.type === kind) ?? -1;
  if (input < 0 || output < 0) return null;

  return [
    { fromNode: cable.fromNode, fromPort: cable.fromPort, toNode: node.id, toPort: input },
    { fromNode: node.id, fromPort: output, toNode: cable.toNode, toPort: cable.toPort },
  ];
}

/** Taking a node out of the graph's wiring: the cables that go, and those that close the gap. */
export interface Detachment {
  remove: Connection[];
  add: Connection[];
}

const sameCable = (a: Connection, b: Connection) =>
  a.fromNode === b.fromNode && a.fromPort === b.fromPort && a.toNode === b.toNode && a.toPort === b.toPort;

/**
 * Unplugs a node, or returns null if it has no cables or is a fixed node. What came into its first audio or event input
 * goes on to what its first output of that kind fed, the reverse of inserting it. Other cables (a sidechain key,
 * modulation) just go. A cable that already exists isn't added twice, and an event input keeps its one connection.
 */
export function detachment(graph: TrackGraph, node: GraphNode, infoFor: InfoFor): Detachment | null {
  const info = infoFor(node);
  if (info?.userEditable === false) return null;

  const all = Object.values(graph.connections);
  const remove = all.filter((c) => c.fromNode === node.id || c.toNode === node.id);
  if (remove.length === 0) return null;

  const remaining = all.filter((c) => !remove.includes(c));
  const add: Connection[] = [];

  for (const kind of ['audio', 'events'] as const) {
    const input = info?.inputs.findIndex((port) => port.type === kind) ?? -1;
    const output = info?.outputs.findIndex((port) => port.type === kind) ?? -1;
    if (input < 0 || output < 0) continue;

    for (const before of remove.filter((c) => c.toNode === node.id && c.toPort === input)) {
      for (const after of remove.filter((c) => c.fromNode === node.id && c.fromPort === output)) {
        const healed = { fromNode: before.fromNode, fromPort: before.fromPort, toNode: after.toNode, toPort: after.toPort };
        const taken = (c: Connection) => c.toNode === healed.toNode && c.toPort === healed.toPort;
        const present = [...remaining, ...add];
        if (present.some((c) => sameCable(c, healed)) || (kind === 'events' && present.some(taken))) continue;
        add.push(healed);
      }
    }
  }

  return { remove, add };
}

/** The graph as it would be after a detachment. */
export function graphAfter(graph: TrackGraph, detached: Detachment | null): TrackGraph {
  if (!detached) return graph;
  const kept = Object.values(graph.connections).filter((c) => !detached.remove.some((r) => sameCable(r, c)));
  const connections = Object.fromEntries([...kept, ...detached.add].map((c, i) => [String(i), c]));
  return { ...graph, connections } as TrackGraph;
}

const SHAKE_REVERSALS = 5; // changes of direction that make a shake
const SHAKE_MIN_TRAVEL_PX = 60; // how far each stroke must go, so a steady drag's jitter doesn't count
const SHAKE_WINDOW_MS = 1000; // how recent the changes of direction must be

/** Recognises a shake in a dragged pointer's path: quick strokes back and forth along either axis. */
export class ShakeDetector {
  private axes = [
    { turn: 0, direction: 0, reversals: [] as number[] },
    { turn: 0, direction: 0, reversals: [] as number[] },
  ];

  private started = false;

  /** Takes a pointer position (pixels) and its time (ms). Returns whether this completes a shake. */
  feed(x: number, y: number, time: number): boolean {
    const values = [x, y];
    if (!this.started) {
      this.started = true;
      this.axes.forEach((axis, i) => (axis.turn = values[i]));
      return false;
    }

    return this.axes.some((axis, i) => {
      const value = values[i];
      const direction = Math.sign(value - axis.turn);
      if (direction === axis.direction || axis.direction === 0) {
        // Still going the same way: the turning point follows it, so a stroke is measured from where it began.
        if (axis.direction === 0 && Math.abs(value - axis.turn) >= SHAKE_MIN_TRAVEL_PX) axis.direction = direction;
        if (axis.direction !== 0) axis.turn = value;
        return false;
      }

      // Going back: it counts once it has gone far enough to be a stroke.
      if (Math.abs(value - axis.turn) < SHAKE_MIN_TRAVEL_PX) return false;
      axis.direction = direction;
      axis.turn = value;
      axis.reversals = [...axis.reversals, time].filter((t) => time - t <= SHAKE_WINDOW_MS);
      return axis.reversals.length >= SHAKE_REVERSALS;
    });
  }
}

const ROOM_PADDING = 40;

/** A node's place on the canvas, along the signal's direction (x) and across it (y). */
export interface Box {
  x: number;
  y: number;
  width: number;
  height: number;
}

const acrossOverlaps = (a: Box, b: Box) => a.y < b.y + b.height + ROOM_PADDING && b.y < a.y + a.height + ROOM_PADDING;

/**
 * Makes room for a node inserted between `from` and `to`, with no layout beyond that: the node keeps its place unless it
 * would sit on `from`, and everything from `to` onwards (all nodes starting at or after it, so the shape of what is there
 * stays as it is) moves along by what it takes to clear the node. A node it doesn't touch needs no room.
 *
 * @return  Where the inserted node goes along x, how far the others must move, and which of them do.
 */
export function makeRoom(node: Box, from: Box, to: Box, others: Box[]): { x: number; shift: number; moved: Box[] } {
  const x = acrossOverlaps(node, from) ? Math.max(node.x, from.x + from.width + ROOM_PADDING) : node.x;
  const placed = { ...node, x };
  const moved = others.filter((n) => n !== from && n.x >= to.x);
  const shift = Math.max(0, ...moved.filter((n) => acrossOverlaps(placed, n)).map((n) => x + node.width + ROOM_PADDING - n.x));
  return { x, shift, moved: shift > 0 ? moved : [] };
}
