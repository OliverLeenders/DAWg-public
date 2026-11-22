import ELK, { type ElkExtendedEdge, type ElkNode, type ElkPort } from 'elkjs/lib/elk.bundled.js';
import type { Connection, TrackGraph } from '../bridge/protocol';
import { feedbackChips, isLoopBack } from './validate';

// The graph view's Layout button: ELK's layered layout (the Sugiyama method), run only when asked for. It reserves a lane for each
// long cable and orders the columns to reduce crossings, and it knows where each node's ports are. Its cable routes become the
// cables' bends (docs/design/graph-grid-and-cables.md, G7).

export interface Point {
  x: number;
  y: number;
}

export type Side = 'left' | 'right' | 'top' | 'bottom';

/** A node as it's drawn: its size, and where its ports are, from its top left corner. */
export interface MeasuredNode {
  id: number;
  width: number;
  height: number;
  ports: Array<{ handle: string; side: Side; x: number; y: number }>;
}

export interface GraphLayout {
  positions: Map<number, Point>;
  /** The bends of each cable, by cable id, without its two ends. */
  routes: Map<string, Point[]>;
}

export const cableId = (c: Connection) => `${c.fromNode}:${c.fromPort}>${c.toNode}:${c.toPort}`;

const ELK_SIDE: Record<Side, string> = { left: 'WEST', right: 'EAST', top: 'NORTH', bottom: 'SOUTH' };
const MARGIN = 80;

const portId = (node: number, handle: string) => `${node}/${handle}`;

/** The nodes in signal order, with the way back of each loop left out, so that ELK takes those cables as the ones going backwards. */
export function signalOrder(graph: TrackGraph): number[] {
  const ids = Object.values(graph.nodes).map((n) => n.id).sort((a, b) => a - b);
  const forward = Object.values(graph.connections).filter((c) => !isLoopBack(graph, c));
  const waiting = new Map(ids.map((id) => [id, forward.filter((c) => c.toNode === id).length]));
  const order: number[] = [];

  while (order.length < ids.length) {
    const next = ids.find((id) => waiting.get(id) === 0) ?? ids.find((id) => !order.includes(id))!;
    waiting.set(next, -1);
    order.push(next);
    for (const c of forward) if (c.fromNode === next) waiting.set(c.toNode, (waiting.get(c.toNode) ?? 0) - 1);
  }

  return order;
}

/** What ELK is given. A Feedback chip isn't a node to it: the cable through it is one cable, which goes backwards. */
export function elkGraph(graph: TrackGraph, measured: MeasuredNode[], vertical: boolean): ElkNode {
  const chips = feedbackChips(graph);
  const byId = new Map(measured.map((m) => [m.id, m]));
  const children: ElkNode[] = [];

  for (const id of signalOrder(graph)) {
    const node = byId.get(id);
    if (!node || chips.has(id)) continue;
    const ports: ElkPort[] = node.ports.map((p) => ({
      id: portId(id, p.handle),
      x: p.x,
      y: p.y,
      width: 0,
      height: 0,
      layoutOptions: { 'elk.port.side': ELK_SIDE[p.side] },
    }));
    children.push({ id: String(id), width: node.width, height: node.height, ports, layoutOptions: { 'elk.portConstraints': 'FIXED_POS' } });
  }

  const edge = (id: string, from: Connection, to: Connection): ElkExtendedEdge => ({
    id,
    sources: [portId(from.fromNode, `out-${from.fromPort}`)],
    targets: [portId(to.toNode, `in-${to.toPort}`)],
  });
  const edges: ElkExtendedEdge[] = [];

  for (const c of Object.values(graph.connections)) {
    if (!chips.has(c.fromNode) && !chips.has(c.toNode)) edges.push(edge(cableId(c), c, c));
  }

  for (const [id, chip] of chips) edges.push(edge(`chip:${id}`, chip.into, chip.out));

  return {
    id: 'graph',
    children,
    edges,
    layoutOptions: {
      'elk.algorithm': 'layered',
      'elk.direction': vertical ? 'DOWN' : 'RIGHT',
      'elk.edgeRouting': 'ORTHOGONAL',
      'elk.layered.feedbackEdges': 'true',
      'elk.layered.cycleBreaking.strategy': 'MODEL_ORDER',
      'elk.layered.considerModelOrder.strategy': 'NODES_AND_EDGES',
      'elk.layered.nodePlacement.strategy': 'NETWORK_SIMPLEX',
      'elk.layered.spacing.nodeNodeBetweenLayers': '70',
      'elk.layered.spacing.edgeNodeBetweenLayers': '24',
      'elk.spacing.nodeNode': '50',
      'elk.spacing.edgeNode': '24',
      'elk.spacing.edgeEdge': '14',
    },
  };
}

const pointsOf = (edge: ElkExtendedEdge): Point[] =>
  (edge.sections ?? []).flatMap((s) => [s.startPoint, ...(s.bendPoints ?? []), s.endPoint]);

/** Where on a route a chip sits: the middle of its longest stretch, preferring one that runs against the signal. */
export function chipSpot(route: Point[], vertical: boolean): { at: Point; segment: number } {
  let best = { at: route[0] ?? { x: 0, y: 0 }, segment: 0 };
  let score = -1;

  for (let i = 0; i + 1 < route.length; i++) {
    const [a, b] = [route[i], route[i + 1]];
    const along = vertical ? a.y - b.y : a.x - b.x;
    const length = Math.abs(a.x - b.x) + Math.abs(a.y - b.y);
    const value = along > 0 ? length * 4 : length;
    if (value > score) {
      score = value;
      best = { at: { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 }, segment: i };
    }
  }

  return best;
}

/** The side of a chip its input is on: towards the node feeding it, with its output on the other side, along whichever way the two nodes
    it sits between are further apart. So it reads the right way round wherever those nodes are. */
export function chipFacing(from: Point | undefined, to: Point | undefined, vertical: boolean): Side {
  const [dx, dy] = from && to ? [from.x - to.x, from.y - to.y] : vertical ? [0, 1] : [1, 0];
  if (Math.abs(dx) >= Math.abs(dy) && dx !== 0) return dx > 0 ? 'right' : 'left';
  return dy >= 0 ? 'bottom' : 'top';
}

/** Runs the layout. Everything comes back shifted to a margin from the origin. */
export async function layoutGraph(graph: TrackGraph, measured: MeasuredNode[], vertical: boolean): Promise<GraphLayout> {
  const result = await new ELK().layout(elkGraph(graph, measured, vertical));
  const positions = new Map<number, Point>();
  const routes = new Map<string, Point[]>();
  const byId = new Map(measured.map((m) => [m.id, m]));

  for (const child of result.children ?? []) positions.set(Number(child.id), { x: (child.x ?? 0) + MARGIN, y: (child.y ?? 0) + MARGIN });

  const shifted = (points: Point[]) => points.map((p) => ({ x: p.x + MARGIN, y: p.y + MARGIN }));
  const chips = feedbackChips(graph);

  for (const edge of (result.edges ?? []) as ElkExtendedEdge[]) {
    const route = shifted(pointsOf(edge));

    if (!edge.id.startsWith('chip:')) {
      routes.set(edge.id, route.slice(1, -1));
      continue;
    }

    // The cable through a chip: the chip sits on it, and it becomes the two cables it stands for.
    const id = Number(edge.id.slice('chip:'.length));
    const chip = chips.get(id)!;
    const spot = chipSpot(route, vertical);
    const size = byId.get(id) ?? { width: 0, height: 0 };
    positions.set(id, { x: spot.at.x - size.width / 2, y: spot.at.y - size.height / 2 });
    routes.set(cableId(chip.into), route.slice(1, spot.segment + 1));
    routes.set(cableId(chip.out), route.slice(spot.segment + 1, -1));
  }

  return { positions, routes };
}

/** A cable's path through its bends, with rounded corners. */
export function routedPath(from: Point, bends: Point[], to: Point, radius = 8): string {
  const points = [from, ...bends, to];
  let path = `M ${from.x} ${from.y}`;

  for (let i = 1; i + 1 < points.length; i++) {
    const [a, b, c] = [points[i - 1], points[i], points[i + 1]];
    const before = Math.hypot(b.x - a.x, b.y - a.y);
    const after = Math.hypot(c.x - b.x, c.y - b.y);
    const r = Math.min(radius, before / 2, after / 2);
    if (r <= 0) continue;
    const start = { x: b.x + ((a.x - b.x) * r) / before, y: b.y + ((a.y - b.y) * r) / before };
    const end = { x: b.x + ((c.x - b.x) * r) / after, y: b.y + ((c.y - b.y) * r) / after };
    path += ` L ${start.x} ${start.y} Q ${b.x} ${b.y} ${end.x} ${end.y}`;
  }

  return `${path} L ${to.x} ${to.y}`;
}
