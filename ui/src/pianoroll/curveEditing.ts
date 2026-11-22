import type { CurvePoint } from '../bridge/protocol';
import { evaluateCurve } from './geometry';

// Point editing for expression curves, shared by the lanes and the pitch curves in the roll
// (docs/design/phase3-piano-roll.md):
//   click a point        select it (Shift toggles); drag moves all selected points
//   Alt+click            add a point (snapped to the grid in time and, for pitch, to semitones; Ctrl/Cmd = free);
//                        on a note in the roll, where Alt+click splits it, Shift+Alt+click adds (snapped)
//   double-click point   delete it
//   drag a handle        bend the segment through the handle (makes it a free segment)
//   double-click handle  put it back in the middle: a straight segment
// Points always stay within the note (0 … note length).

const HIT_PX = 6;

/** CurveShape::free: a cubic Bezier segment whose handles point at its stored tension point (freeSegment). */
export const FREE_SHAPE = 3;

/** Maps curve time (ticks from the note start) and values to pixels and back. */
export interface CurveMapping {
  toX: (time: number) => number;
  toY: (value: number) => number;
  fromX: (x: number) => number;
  fromY: (y: number) => number;
  min: number;
  max: number;
  /** Latest allowed time: the note's length (points never leave the note). */
  maxTime: number;
  /** The lane's neutral value; the first point added to an empty curve anchors the note start at it. */
  neutral: number;
  /** Optional value snapping (whole semitones for pitch); bypassed when editing freely (Shift). */
  snapValue?: (value: number) => number;
  /** Optional time snapping to the roll's grid (note-relative ticks in and out); bypassed with Shift. */
  snapTime?: (time: number) => number;
  /** MPE lanes: the point at the note start is an anchor: it moves only vertically and can't be deleted (the
      engine keeps it). The note end has no anchor, so nothing sits on the note's resize edge unless added. */
  anchorStart?: boolean;
}

/** Whether a point is a lane's anchor (the note start). */
export const isAnchor = (time: number, anchorStart: boolean | undefined) => anchorStart === true && time === 0;

export type CurveTarget = { kind: 'point'; index: number } | { kind: 'handle'; segment: number };

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

const valueAt = (map: CurveMapping, raw: number, free: boolean) => {
  const v = clamp(raw, map.min, map.max);
  return free || !map.snapValue ? v : clamp(map.snapValue(v), map.min, map.max);
};

const timeAt = (map: CurveMapping, raw: number, free: boolean) => {
  const t = clamp(raw, 0, map.maxTime);
  return Math.round(free || !map.snapTime ? t : clamp(map.snapTime(t), 0, map.maxTime));
};

export type HandlePosition = (points: CurvePoint[], segment: number, map: CurveMapping) => { x: number; y: number };

export function handleValueFor(points: CurvePoint[], segment: number, time = handleTimeFor(points, segment)) {
  const [, startValue, shape, , , handleValue] = points[segment];
  return shape === FREE_SHAPE
    ? startValue + (points[segment + 1][1] - startValue) * (handleValue ?? 0.5)
    : evaluateCurve(points, time, 0);
}

/** Where a segment's tension handle is drawn: a free segment's control point, else on the curve mid-segment. */
export const handlePosition: HandlePosition = (points, segment, map) => {
  const time = handleTimeFor(points, segment);
  return { x: map.toX(time), y: map.toY(handleValueFor(points, segment)) };
};

/** The time range a free tension handle may occupy inside its segment. */
export function handleTimeBounds(points: CurvePoint[], segment: number) {
  const start = points[segment][0];
  const end = points[segment + 1][0];
  const span = end - start;
  const middle = (start + end) / 2;
  if (span <= 0) return { low: middle, high: middle };
  return { low: start, high: end };
}

/**
 * The time to draw a segment's handle at. A free segment stores its handle as a fraction of the segment, so it
 * stays between the same two nodes when either one moves; any other segment shows it at the midpoint until it is
 * bent (which makes it free).
 */
export function handleTimeFor(points: CurvePoint[], segment: number) {
  const { low, high } = handleTimeBounds(points, segment);
  const [start, , shape, , handleTime] = points[segment];
  const end = points[segment + 1][0];
  const time = shape === FREE_SHAPE && handleTime !== undefined
    ? start + clamp(handleTime, 0, 1) * (end - start)
    : (start + end) / 2;
  return clamp(time, low, high);
}

export function hitCurve(
  points: CurvePoint[],
  x: number,
  y: number,
  map: CurveMapping,
  handleAt: HandlePosition = handlePosition,
): CurveTarget | null {
  for (let i = 0; i < points.length; i++) {
    if (Math.abs(map.toX(points[i][0]) - x) <= HIT_PX && Math.abs(map.toY(points[i][1]) - y) <= HIT_PX)
      return { kind: 'point', index: i };
  }

  for (let i = 0; i + 1 < points.length; i++) {
    if (points[i][1] === points[i + 1][1]) continue; // a flat segment has no visible bend
    const handle = handleAt(points, i, map);
    if (Math.abs(handle.x - x) <= HIT_PX && Math.abs(handle.y - y) <= HIT_PX) return { kind: 'handle', segment: i };
  }

  return null;
}

/**
 * Inserts a breakpoint at the pointer; returns the new points and the inserted index.
 * The first point of an empty curve also gets an anchor at the note start with the neutral value, so the
 * curve only changes from that point on (a lone point would otherwise hold its value over the whole note).
 */
export function addPoint(points: CurvePoint[], x: number, y: number, map: CurveMapping, free = false) {
  const time = timeAt(map, map.fromX(x), free);
  const existing = points.findIndex((p) => p[0] === time);
  if (existing >= 0) return { points, index: existing };

  const point: CurvePoint = [time, valueAt(map, map.fromY(y), free), 0, 0];
  const anchor: CurvePoint[] = points.length === 0 && time > 0 ? [[0, map.neutral, 0, 0]] : [];
  const next = [...anchor, ...points, point].sort((a, b) => a[0] - b[0]);
  return { points: next, index: next.indexOf(point) };
}

/**
 * Moves a group of points by the pointer's travel from where `grab` was picked up. The grabbed point lands
 * on the grid (unless free) and the others keep their distance to it. Points stay inside the note; where
 * moved points land on the time of an unmoved point, the moved one wins.
 *
 * @return  The new points and the new indices of the moved points.
 */
export function movePoints(
  points: CurvePoint[],
  indices: number[],
  grab: number,
  x: number,
  y: number,
  map: CurveMapping,
  free = false,
) {
  const [grabTime, grabValue] = points[grab];

  // Time: snap the grabbed point (the anchor keeps its time). Value: snapped deltas keep semitone curves on
  // semitones.
  const dt = isAnchor(grabTime, map.anchorStart) ? 0 : timeAt(map, map.fromX(x), free) - grabTime;
  let dv = map.fromY(y) - grabValue;
  if (!free && map.snapValue) dv = map.snapValue(dv);
  return shiftPoints(points, indices, dt, dv, map);
}

/** The limits a curve's points stay within (a mapping's, without its pixels). */
export type CurveBounds = Pick<CurveMapping, 'min' | 'max' | 'maxTime' | 'anchorStart'>;

/**
 * Shifts a group of points by a time and value delta (arrow keys, and the other notes of a multi-note drag),
 * limited so the whole group stays within the note and the value range. The anchor keeps its time and the other
 * points stay after it; where moved points land on the time of an unmoved point, the moved one wins.
 *
 * @return  The new points and the new indices of the moved points.
 */
export function shiftPoints(points: CurvePoint[], indices: number[], dt: number, dv: number, map: CurveBounds) {
  if (indices.length === 0) return { points, indices };
  const values = indices.map((i) => points[i][1]);
  const anchored = (i: number) => isAnchor(points[i][0], map.anchorStart);
  const movable = indices.filter((i) => !anchored(i)).map((i) => points[i][0]);

  const [low, high] = [map.anchorStart ? 1 : 0, map.maxTime];
  dt = movable.length > 0 ? clamp(dt, low - Math.min(...movable), high - Math.max(...movable)) : 0;
  dv = clamp(dv, map.min - Math.min(...values), map.max - Math.max(...values));

  const moved = indices.map((i): CurvePoint => {
    const point = [...points[i]] as CurvePoint;
    point[0] = anchored(i) ? point[0] : Math.round(point[0] + dt);
    point[1] += dv;
    return point;
  });
  const movedTimes = new Set(moved.map((p) => p[0]));
  const others = points.filter((p, i) => !indices.includes(i) && !movedTimes.has(p[0]));
  const next = [...others, ...moved].sort((a, b) => a[0] - b[0]);
  return { points: next, indices: moved.map((p) => next.indexOf(p)) };
}

/** Removes points, except a lane's anchor at the note start. */
export function deletePoints(points: CurvePoint[], indices: number[], anchorStart?: boolean) {
  return points.filter((p, i) => !indices.includes(i) || isAnchor(p[0], anchorStart));
}

/** Bends a segment by dragging its handle vertically from `startTension`. */
/**
 * Bends a segment by moving its tension point to (time, value), where the user dragged it; the Bezier handles point
 * at it (freeSegment). Its position is stored as a fraction of the segment in both directions, so it stays
 * between the same two nodes when either one moves.
 */
export function bendSegmentThrough(points: CurvePoint[], segment: number, time: number, value: number) {
  const [t0, v0] = points[segment];
  const [t1, v1] = points[segment + 1];
  const span = t1 - t0;
  if (span <= 0 || v1 === v0) return points;

  // Keep the handle just inside the rectangle.
  const handleTime = clamp((time - t0) / span, 0, 1);
  const handleValue = clamp((value - v0) / (v1 - v0), 0, 1);

  const next = points.slice();
  next[segment] = [t0, v0, FREE_SHAPE, 0, handleTime, handleValue];
  return next;
}

/** Puts a segment's tension point back in the middle of its rectangle, which makes the segment a straight line. */
export function straightenSegment(points: CurvePoint[], segment: number) {
  if (segment + 1 >= points.length) return points;
  const [t0, v0] = points[segment];
  const next = points.slice();
  next[segment] = [t0, v0, FREE_SHAPE, 0, 0.5, 0.5];
  return next;
}

/**
 * Bends a segment from a free handle drag. `grab` is the offset from the pointer to the handle when it was
 * picked up, so the handle follows the pointer without jumping. The handle stays inside the segment's
 * rectangle, and the returned `time` is where the handle was placed.
 */
export function bendSegmentToPointer(
  points: CurvePoint[],
  segment: number,
  map: CurveMapping,
  x: number,
  y: number,
  grab: { x: number; y: number },
) {
  const { low, high } = handleTimeBounds(points, segment);
  const time = clamp(map.fromX(x - grab.x), low, high);
  const [v0, v1] = [points[segment][1], points[segment + 1][1]];
  const lowValue = Math.min(v0, v1);
  const highValue = Math.max(v0, v1);
  const value = highValue > lowValue ? clamp(map.fromY(y - grab.y), lowValue, highValue) : lowValue;
  return { points: bendSegmentThrough(points, segment, time, value), time };
}

/** Indices of the points inside a screen rectangle. */
export function pointsInRect(points: CurvePoint[], map: CurveMapping, x0: number, y0: number, x1: number, y1: number) {
  const [left, right] = [Math.min(x0, x1), Math.max(x0, x1)];
  const [top, bottom] = [Math.min(y0, y1), Math.max(y0, y1)];
  return points
    .map((p, i) => ({ i, x: map.toX(p[0]), y: map.toY(p[1]) }))
    .filter((p) => p.x >= left && p.x <= right && p.y >= top && p.y <= bottom)
    .map((p) => p.i);
}

/** A note as box selection sees it: its pitch point count, and what the box covers of it. */
export interface BoxCandidate {
  id: number;
  pointCount: number;
  /** Indices of its pitch points inside the box. */
  inside: number[];
  /** Whether the box touches the note's block (used for notes without pitch points). */
  touched: boolean;
}

/**
 * Box selection in the roll, added to `base` (Shift, or empty). A note without pitch points is selected when the box
 * touches it. A bent note is selected once all of its pitch points are (with those already selected), and until
 * then only the points are; a note selected as a whole stays so.
 */
export function boxSelect(
  candidates: BoxCandidate[],
  base: { notes: ReadonlySet<number>; points: ReadonlyMap<number, number[]> },
) {
  const notes = new Set(base.notes);
  const points = new Map(base.points);
  for (const { id, pointCount, inside, touched } of candidates) {
    if (notes.has(id) && !points.has(id)) continue;
    if (pointCount === 0) {
      if (touched) notes.add(id);
      continue;
    }
    const all = new Set([...(points.get(id) ?? []), ...inside]);
    if (all.size === pointCount) {
      notes.add(id);
      points.delete(id);
    } else if (all.size > 0) points.set(id, [...all].sort((a, b) => a - b));
  }
  return { notes, points };
}

/**
 * Pastes copied points (times relative to the first copied point) into a curve at `at` (note-relative),
 * replacing the points in the pasted span. Points that would fall past the note end are dropped; an empty
 * target curve is anchored at its neutral value at the note start.
 */
export function pastePoints(points: CurvePoint[], copied: CurvePoint[], at: number, maxTime: number, neutral: number) {
  if (copied.length === 0) return points;
  // Like adding a first point: an empty curve keeps its neutral value until the pasted shape starts.
  if (points.length === 0 && at > 0) points = [[0, neutral, 0, 0]];
  const span = copied[copied.length - 1][0];
  const pasted = copied
    .map((p): CurvePoint => {
      const point = [...p] as CurvePoint;
      point[0] = Math.round(at + point[0]);
      return point;
    })
    .filter((p) => p[0] >= 0 && p[0] <= maxTime);
  const kept = points.filter((p) => p[0] < at || p[0] > at + span);
  return [...kept, ...pasted].sort((a, b) => a[0] - b[0]);
}
