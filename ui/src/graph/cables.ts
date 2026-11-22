import type { Point } from './layout';
import { fitBends, type End } from './grid';

// Editing a cable's bends (docs/design/graph-grid-and-cables.md, G5–G8). A cable with bends runs in straight stretches from its output
// through its bends to its input; everything here is in the canvas's own coordinates.

const across = (end: End) => end.side === 'left' || end.side === 'right';

/** Whether a stretch runs left and right (rather than up and down). */
export const flat = (a: Point, b: Point) => Math.abs(b.y - a.y) <= Math.abs(b.x - a.x);

/** The whole route: the output, the bends lined up with the ends, the input. */
export const routeOf = (bends: Point[], from: End, to: End): Point[] => [from.at, ...fitBends(bends, from, to), to.at];

/** Two bends that make an unbent cable a step at a point: out of its output, across at the point, into its input (G6). */
export function stepAt(from: End, to: End, at: Point): Point[] {
  return across(from) ? [{ x: at.x, y: from.at.y }, { x: at.x, y: to.at.y }] : [{ x: from.at.x, y: at.y }, { x: to.at.x, y: at.y }];
}

/** Four bends that pull an unbent cable aside at a point, across the flow: out of its output, aside before the point, back after it, into
    its input. The stretch aside is the middle one, `width` long, which a drag across the flow then moves. */
export function detourAt(from: End, to: End, at: Point, width: number): Point[] {
  if (across(from)) {
    const w = (Math.sign(to.at.x - from.at.x) || 1) * width / 2;
    return [{ x: at.x - w, y: from.at.y }, { x: at.x - w, y: at.y }, { x: at.x + w, y: at.y }, { x: at.x + w, y: to.at.y }];
  }
  const w = (Math.sign(to.at.y - from.at.y) || 1) * width / 2;
  return [{ x: from.at.x, y: at.y - w }, { x: at.x, y: at.y - w }, { x: at.x, y: at.y + w }, { x: to.at.x, y: at.y + w }];
}

/** Whether a move from `a` to `b` goes more across the flow than along it, for a cable leaving its output at `from`. */
export const acrossFlow = (from: End, a: Point, b: Point) => (across(from) ? Math.abs(b.y - a.y) > Math.abs(b.x - a.x) : Math.abs(b.x - a.x) > Math.abs(b.y - a.y));

/** The stretches that can be dragged: not the two at the ports, which run straight out of them. As indices into the route. */
export const movable = (route: Point[]) => route.map((_, i) => i).filter((i) => i >= 1 && i + 2 < route.length);

/** The bends with the route's stretch `index` (from route[index] to route[index + 1]) moved sideways to `to`: a flat one up or down, an
    upright one left or right. Its neighbours stretch. */
export function moveStretch(bends: Point[], route: Point[], index: number, to: number): Point[] {
  const isFlat = flat(route[index], route[index + 1]);
  // Route index i is bend i - 1.
  return bends.map((p, k) => (k === index - 1 || k === index ? (isFlat ? { ...p, y: to } : { ...p, x: to }) : p));
}

/** The bends with a pair added where a stretch is double-clicked, at the point on it nearest `at`: dragging a half that is not at a port
    then makes a step there (G6). */
export function splitAt(bends: Point[], route: Point[], index: number, at: Point): Point[] {
  const [a, b] = [route[index], route[index + 1]];
  const point = flat(a, b) ? { x: clamp(at.x, a.x, b.x), y: a.y } : { x: a.x, y: clamp(at.y, a.y, b.y) };
  return [...bends.slice(0, index), point, { ...point }, ...bends.slice(index)];
}

const clamp = (v: number, a: number, b: number) => Math.min(Math.max(a, b), Math.max(Math.min(a, b), v));

/** The bends without bend `k`. Taking one bend out of a route of straight stretches would leave a slanted one, so the bend next to it goes
    too and the stretch after them moves onto the line of the one before (G6). With two or fewer the cable is a curve again. */
export function removeBend(bends: Point[], k: number): Point[] {
  if (bends.length <= 2) return [];
  const pair = k + 1 < bends.length ? k : k - 1;
  const rest = [...bends.slice(0, pair), ...bends.slice(pair + 2)].map((p) => ({ ...p }));
  const [before, after] = [bends[pair - 1], rest[pair]];
  // The stretch from the bend before the pair into it ran along one axis: the one after now continues it.
  // When the pair came first, the next bend lines up with the output as it is drawn (fitBends).
  if (before && after) {
    if (flat(before, bends[pair])) after.y = before.y;
    else after.x = before.x;
  }
  return rest;
}

/** Bends moved along with their nodes (G8, a selection moved together). */
export const translate = (bends: Point[], dx: number, dy: number) => bends.map((p) => ({ x: p.x + dx, y: p.y + dy }));

/** Model coordinates (along and across the flow) to the canvas's, and back: the same swap both ways. */
export const facing = (p: Point, vertical: boolean): Point => (vertical ? { x: p.y, y: p.x } : { x: p.x, y: p.y });
