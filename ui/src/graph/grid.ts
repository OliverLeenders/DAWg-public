import type { Connection } from '../bridge/protocol';
import { cableId, type GraphLayout, type MeasuredNode, type Point, type Side } from './layout';

// The graph canvas's grid (docs/design/graph-grid-and-cables.md, G1–G3): a node snaps so that its first port's centre is on a grid point,
// and as ports are a grid step apart, so are all of a node's ports along its side.

/** The grid's step, in flow units: the dots' spacing and the ports' pitch. */
export const GRID = 20;
/** Every so many steps a dot is a little brighter (DS13's grid weights). */
export const MAJOR = 5;

const snap = (value: number) => Math.round(value / GRID) * GRID;

/** A point moved to the nearest grid point. */
export const snapPoint = (p: Point): Point => ({ x: snap(p.x), y: snap(p.y) });

/** Where a node goes so that its first port, at `port` from its top left corner, is on the nearest grid point. */
export function snapNode(position: Point, port: Point): Point {
  const at = snapPoint({ x: position.x + port.x, y: position.y + port.y });
  return { x: at.x - port.x, y: at.y - port.y };
}

/** The port a node snaps by, from its ports' centres: its first input, or its first output for a node without inputs (G1). The lowest
    number is the first one drawn, and on a Feedback chip the signal's input rather than its modulation input. */
export function firstPortOf(ports: Array<{ handle: string; x: number; y: number }>): Point | null {
  const number = (handle: string) => Number(handle.split('-')[1]);
  for (const prefix of ['in-', 'out-']) {
    const first = ports.filter((p) => p.handle.startsWith(prefix)).sort((a, b) => number(a.handle) - number(b.handle))[0];
    if (first) return { x: first.x, y: first.y };
  }
  return null;
}

/** The same, from React Flow's handle bounds. */
export function firstPort(handles: { target?: HandleBox[] | null; source?: HandleBox[] | null } | undefined): Point | null {
  const centres = [...(handles?.target ?? []), ...(handles?.source ?? [])].flatMap((h) =>
    h.id ? [{ handle: h.id, x: h.x + h.width / 2, y: h.y + h.height / 2 }] : [],
  );
  return firstPortOf(centres);
}

/** A cable's end: where its port is and which side of the node it leaves from. */
export interface End {
  at: Point;
  side: Side;
}

const across = (side: Side) => side === 'left' || side === 'right';

/** Bends with the first and last ones lined up with the cable's ends, so that the stretch out of a port runs straight out of it however
    its node has moved (G8): only the two end stretches and their neighbours change. */
export function fitBends(bends: Point[], from: End, to: End): Point[] {
  if (bends.length === 0) return bends;
  const fitted = bends.map((p) => ({ ...p }));
  const line = (p: Point, end: End) => (across(end.side) ? (p.y = end.at.y) : (p.x = end.at.x));
  line(fitted[0], from);
  line(fitted[fitted.length - 1], to);
  return fitted;
}

/** Layout's result on the grid (G3): each node moved so its first port is on a grid point, and its routes' bends on grid lines and lined
    up with the moved ends again. */
export function snapLayout(result: GraphLayout, measured: MeasuredNode[], connections: Connection[], on: boolean): GraphLayout {
  if (!on) return result;
  const byId = new Map(measured.map((m) => [m.id, m]));
  const positions = new Map<number, Point>();
  for (const [id, at] of result.positions) {
    const port = firstPortOf(byId.get(id)?.ports ?? []);
    positions.set(id, port ? snapNode(at, port) : at);
  }

  const endOf = (id: number, handle: string): End | null => {
    const [at, port] = [positions.get(id), byId.get(id)?.ports.find((p) => p.handle === handle)];
    return at && port ? { at: { x: at.x + port.x, y: at.y + port.y }, side: port.side } : null;
  };
  const routes = new Map<string, Point[]>();
  for (const c of connections) {
    const id = cableId(c);
    const route = result.routes.get(id);
    const [from, to] = [endOf(c.fromNode, `out-${c.fromPort}`), endOf(c.toNode, `in-${c.toPort}`)];
    if (route && from && to) routes.set(id, fitBends(route.map(snapPoint), from, to));
  }
  return { positions, routes };
}

export interface HandleBox {
  id?: string | null;
  x: number;
  y: number;
  width: number;
  height: number;
}

/** The offset that puts a dot pattern's dots on the grid points: React Flow draws them half a step from its tiles' corners. */
export const dotOffset = (gap: number, size: number) => size / 2 - gap / 2;
