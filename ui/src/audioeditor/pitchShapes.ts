import type { CurvePoint } from '../bridge/protocol';
import { evaluateCurve, ROUNDED_SHAPE } from '../pianoroll/geometry';

// Editing a note's shape (docs/design/pitch-correction.md, sections 4.4 and 4.5, PC7 and PC8): pivots and tilting.
// One set of functions for the gestures and the tests.

/** The pivots a note tilts around: its own, or one in the middle when it has none. */
export const pivotsOf = (pivots: readonly number[], length: number) => (pivots.length > 0 ? [...pivots] : [Math.round(length / 2)]);

/** The shape with a node at `time` (the curve's value there, so the curve doesn't change), if it has none. */
export function withNodeAt(points: readonly CurvePoint[], time: number): CurvePoint[] {
  if (points.some((p) => p[0] === time)) return [...points];
  const node: CurvePoint = [time, evaluateCurve([...points], time, 0), ROUNDED_SHAPE, 0];
  return [...points, node].sort((a, b) => a[0] - b[0]);
}

/**
 * The span a drag at `time` tilts, between pivots or a pivot and the note's edge: `fixed` stays put and `moving` moves
 * by the whole drag. A pivot stays and the note's edge moves; between two pivots, the end nearer the drag moves.
 */
export function tiltSpan(pivots: readonly number[], length: number, time: number) {
  const before = [0, ...pivots.filter((p) => p <= time)];
  const after = [...pivots.filter((p) => p > time), length];
  const left = before[before.length - 1];
  const right = after[0];
  const leftIsPivot = pivots.includes(left);
  const rightIsPivot = pivots.includes(right);
  if (leftIsPivot && !rightIsPivot) return { fixed: left, moving: right };
  if (rightIsPivot && !leftIsPivot) return { fixed: right, moving: left };
  return time - left < right - time ? { fixed: right, moving: left } : { fixed: left, moving: right };
}

/**
 * Tilts a shape (PC8): every node between `fixed` and `moving` moves by k · (t − fixed) / (moving − fixed), so the node
 * at `moving` moves by exactly k and the pivot stays. Nodes are added at both ends of the span first, so the rest of
 * the curve doesn't move.
 *
 * @param points  The shape before the drag.
 * @param fixed   The pivot (frames from the note's start).
 * @param moving  The span's other end.
 * @param k       The drag in semitones.
 */
export function tiltShape(points: readonly CurvePoint[], fixed: number, moving: number, k: number): CurvePoint[] {
  const span = moving - fixed;
  if (span === 0) return [...points];
  const [low, high] = fixed < moving ? [fixed, moving] : [moving, fixed];
  return withNodeAt(withNodeAt(points, fixed), moving).map((p): CurvePoint => {
    if (p[0] < low || p[0] > high) return p;
    const moved = [...p] as CurvePoint;
    moved[1] = p[1] + (k * (p[0] - fixed)) / span;
    return moved;
  });
}
