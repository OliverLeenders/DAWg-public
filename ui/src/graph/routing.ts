import { getBezierPath, Position } from '@xyflow/react';
import type { Point, Side } from './layout';

// Cables that would run behind a node, between Layouts: such a cable goes around, along the node's edges. Everything here works from
// where the nodes are now, so it needs nothing saved in the project, and holds after moving a node or opening a project.

export interface Box {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface CableEnd {
  at: Point;
  side: Side;
}

/** How far a cable keeps away from a node. */
const CLEARANCE = 16;
/** A turn is worth this much length, as a cable with few of them reads better. */
const TURN_COST = 80;

const STEP: Record<Side, Point> = { left: { x: -1, y: 0 }, right: { x: 1, y: 0 }, top: { x: 0, y: -1 }, bottom: { x: 0, y: 1 } };
const POSITION: Record<Side, Position> = { left: Position.Left, right: Position.Right, top: Position.Top, bottom: Position.Bottom };

const inside = (p: Point, b: Box, margin = 0) =>
  p.x > b.x - margin && p.x < b.x + b.width + margin && p.y > b.y - margin && p.y < b.y + b.height + margin;

/** Whether a straight stretch passes through a box grown by a margin. */
const stretchHits = (a: Point, b: Point, box: Box, margin = 0) =>
  Math.max(a.x, b.x) > box.x - margin &&
  Math.min(a.x, b.x) < box.x + box.width + margin &&
  Math.max(a.y, b.y) > box.y - margin &&
  Math.min(a.y, b.y) < box.y + box.height + margin;

/** Whether a route, from one end through its bends to the other, passes through any box. */
export function routeHits(points: Point[], boxes: Box[]): boolean {
  return boxes.some((box) => points.some((p, i) => i + 1 < points.length && stretchHits(p, points[i + 1], box)));
}

/** Whether the usual curve between two ends passes through a box. A cable may touch its own nodes where it meets them. */
export function curveHits(from: CableEnd, to: CableEnd, boxes: Box[]): boolean {
  const [path] = getBezierPath({
    sourceX: from.at.x,
    sourceY: from.at.y,
    sourcePosition: POSITION[from.side],
    targetX: to.at.x,
    targetY: to.at.y,
    targetPosition: POSITION[to.side],
  });
  const [x0, y0, x1, y1, x2, y2, x3, y3] = path.match(/-?\d+(\.\d+)?(e-?\d+)?/g)!.map(Number);
  const samples = 32;

  for (let i = 0; i <= samples; i++) {
    const t = i / samples;
    const u = 1 - t;
    const [a, b, c, d] = [u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t];
    const p = { x: a * x0 + b * x1 + c * x2 + d * x3, y: a * y0 + b * y1 + c * y2 + d * y3 };
    if (boxes.some((box) => inside(p, box, -2))) return true;
  }

  return false;
}

/** Where a cable can start turning: straight out from its end, clear of its own node, which a port's centre sits a little inside. Null if
    another node is in the way. */
function wayOut(end: CableEnd, boxes: Box[]): Point | null {
  const step = STEP[end.side];
  const own = boxes.filter((box) => inside(end.at, box, 2));
  let reach = CLEARANCE;

  for (const box of own) {
    const edge = step.x > 0 ? box.x + box.width - end.at.x : step.x < 0 ? end.at.x - box.x : step.y > 0 ? box.y + box.height - end.at.y : end.at.y - box.y;
    reach = Math.max(reach, edge + CLEARANCE);
  }

  const out = { x: end.at.x + step.x * reach, y: end.at.y + step.y * reach };
  const others = boxes.filter((box) => !own.includes(box));
  return others.some((box) => stretchHits(end.at, out, box, CLEARANCE - 1)) ? null : out;
}

/** The bends of the shortest way between two ends that stays clear of the boxes, or null if there isn't one. */
export function routeAround(from: CableEnd, to: CableEnd, boxes: Box[]): Point[] | null {
  const start = wayOut(from, boxes);
  const end = wayOut(to, boxes);
  if (!start || !end) return null;

  // The lines a cable can run along: beside each node, and straight out of each end.
  const lines = (pick: (p: Point) => number, near: (b: Box) => number, far: (b: Box) => number) =>
    [...new Set([pick(start), pick(end), ...boxes.flatMap((b) => [near(b) - CLEARANCE, far(b) + CLEARANCE])])].sort((a, b) => a - b);
  const xs = lines((p) => p.x, (b) => b.x, (b) => b.x + b.width);
  const ys = lines((p) => p.y, (b) => b.y, (b) => b.y + b.height);

  // A shortest path over those lines' crossings, one state per crossing and direction of travel.
  const horizontal = 0;
  const vertical = 1;
  const stateOf = (i: number, j: number, axis: number) => (j * xs.length + i) * 2 + axis;
  const costs = new Map<number, number>();
  const cameFrom = new Map<number, number>();
  const queue: Array<[number, number]> = [];

  const push = (state: number, cost: number, previous?: number) => {
    if (cost >= (costs.get(state) ?? Infinity)) return;
    costs.set(state, cost);
    if (previous !== undefined) cameFrom.set(state, previous);
    queue.push([cost, state]);
    for (let i = queue.length - 1; i > 0; ) {
      const parent = (i - 1) >> 1;
      if (queue[parent][0] <= queue[i][0]) break;
      [queue[parent], queue[i]] = [queue[i], queue[parent]];
      i = parent;
    }
  };

  const pop = () => {
    const top = queue[0];
    const last = queue.pop()!;
    if (queue.length > 0) {
      queue[0] = last;
      for (let i = 0; ; ) {
        const [l, r] = [2 * i + 1, 2 * i + 2];
        let least = i;
        if (l < queue.length && queue[l][0] < queue[least][0]) least = l;
        if (r < queue.length && queue[r][0] < queue[least][0]) least = r;
        if (least === i) break;
        [queue[least], queue[i]] = [queue[i], queue[least]];
        i = least;
      }
    }
    return top;
  };

  const [startI, startJ, endI, endJ] = [xs.indexOf(start.x), ys.indexOf(start.y), xs.indexOf(end.x), ys.indexOf(end.y)];
  const startAxis = from.side === 'left' || from.side === 'right' ? horizontal : vertical;
  push(stateOf(startI, startJ, startAxis), 0);

  // Stays just inside the lines beside each box, which are themselves clear.
  const blocked = (a: Point, b: Point) => boxes.some((box) => stretchHits(a, b, box, CLEARANCE - 1));

  while (queue.length > 0) {
    const [cost, state] = pop();
    if (cost > (costs.get(state) ?? Infinity)) continue;

    const axis = state % 2;
    const index = (state - axis) / 2;
    const [i, j] = [index % xs.length, Math.floor(index / xs.length)];
    if (i === endI && j === endJ) return bendsOf(state, cameFrom, xs, ys, from.at, to.at);

    const here = { x: xs[i], y: ys[j] };
    for (const [ni, nj, nextAxis] of [
      [i - 1, j, horizontal],
      [i + 1, j, horizontal],
      [i, j - 1, vertical],
      [i, j + 1, vertical],
    ]) {
      if (ni < 0 || nj < 0 || ni >= xs.length || nj >= ys.length) continue;
      const there = { x: xs[ni], y: ys[nj] };
      if (blocked(here, there)) continue;
      push(stateOf(ni, nj, nextAxis), cost + Math.abs(there.x - here.x) + Math.abs(there.y - here.y) + (nextAxis === axis ? 0 : TURN_COST), state);
    }
  }

  return null;
}

/** A path's corners, leaving out the points along its straight stretches. */
function bendsOf(last: number, cameFrom: Map<number, number>, xs: number[], ys: number[], from: Point, to: Point): Point[] {
  const walked: Point[] = [];

  for (let state: number | undefined = last; state !== undefined; state = cameFrom.get(state)) {
    const index = (state - (state % 2)) / 2;
    walked.unshift({ x: xs[index % xs.length], y: ys[Math.floor(index / xs.length)] });
  }

  const all = [from, ...walked, to];
  return all.filter((p, i) => {
    if (i === 0 || i === all.length - 1) return false;
    const [a, b] = [all[i - 1], all[i + 1]];
    return !((a.x === p.x && p.x === b.x) || (a.y === p.y && p.y === b.y));
  });
}
