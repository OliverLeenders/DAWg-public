import { TICKS_PER_BEAT, type CurvePoint, type GraphNode } from '../bridge/protocol';

// The waveshaper panel's helpers (docs/design/waveshaper.md, S6, S12).

/** The Shape parameter's Drawn choice. */
export const DRAWN = 6;

/** The plot's inset from the square's edges, in pixels, so points and the curve stay clear of the border. */
export const PLOT_PAD = 6;

const cycle = TICKS_PER_BEAT;

/** The plot's side: the largest square the plot's box holds, in whole pixels. */
export function plotSize(width: number, height: number): number {
  return Math.max(0, Math.floor(Math.min(width, height)));
}

/** The range of the shaper's input u = g x + b a full-scale input reaches (S12), within the drawn curve's -1..1. */
export function inputRange(driveDb: number, biasPercent: number): [number, number] {
  const g = Math.pow(10, driveDb / 20);
  const b = biasPercent / 100;
  return [Math.max(-1, Math.min(1, b - g)), Math.max(-1, Math.min(1, b + g))];
}

/** The Symmetric parameter's value for a node: symmetric unless switched off (S6). */
export const isSymmetric = (params: Record<string, number>) => (params.symmetric ?? 1) >= 0.5;

/** The shaper's input u (-1..1) at a drawn curve's time (0..TICKS_PER_BEAT), and back (S6). */
export const inputAt = (time: number) => (time / cycle) * 2 - 1;
export const timeAt = (input: number) => ((input + 1) / 2) * cycle;

/**
 * Where a symmetric curve's input range ends: the largest |u| a full-scale input reaches (S6), within 0..1; the
 * symmetric editor's band runs from 0 to it.
 */
export function symmetricReach(driveDb: number, biasPercent: number): number {
  const g = Math.pow(10, driveDb / 20);
  return Math.min(1, Math.abs(biasPercent / 100) + g);
}

/**
 * The positive half of a drawn curve, as the symmetric editor shows it: times stretched from TICKS_PER_BEAT / 2..
 * TICKS_PER_BEAT to 0..TICKS_PER_BEAT, starting with a point at (0, 0), the origin a symmetric curve passes through.
 */
export function positiveHalf(full: CurvePoint[]): CurvePoint[] {
  const half = full.filter(([time]) => time > cycle / 2).map((p): CurvePoint => {
    const point = [...p] as CurvePoint;
    point[0] = Math.round((p[0] - cycle / 2) * 2);
    return point;
  });
  const start = full.find(([time]) => time === cycle / 2);
  const origin: CurvePoint = start ? ([...start] as CurvePoint) : [0, 0, 0, 0];
  origin[0] = 0;
  origin[1] = 0;
  return [origin, ...half.filter(([time]) => time > 0)];
}

/** Keeps a symmetric curve's first point at the origin, as the editor pins it. */
export function pinOrigin(half: CurvePoint[]): CurvePoint[] {
  if (half.length === 0 || half[0][0] !== 0) return [[0, 0, 0, 0], ...half];
  if (half[0][1] === 0) return half;
  const origin = [...half[0]] as CurvePoint;
  origin[1] = 0;
  return [origin, ...half.slice(1)];
}

/** A segment's shape seen from its other end, flipped through the origin: free handles mirror, power eases reverse. */
function flipped(point: CurvePoint): [number, number, number, number] {
  const [, , shape, tension, handleTime = 0.5, handleValue = 0.5] = point;
  if (shape === 3) return [3, 0, 1 - handleTime, 1 - handleValue];
  if (shape === 2) return [2, -tension, 0.5, 0.5];
  return [shape, 0, 0.5, 0.5];
}

/**
 * The whole curve that a symmetric curve's positive half plays (S6): the half moved to TICKS_PER_BEAT / 2..
 * TICKS_PER_BEAT and its mirror image through the origin below, with a control point at (0, 0). Linear, hold and free
 * segments mirror exactly; a hold after a sloped segment gets a point one tick later for its step. Power segments
 * (which the editor doesn't make) mirror approximately.
 */
export function mirrored(half: CurvePoint[]): CurvePoint[] {
  const points = pinOrigin(half);
  const at = (time: number) => Math.round(cycle / 2 + time / 2);
  const positive = points.map((p): CurvePoint => {
    const point = [...p] as CurvePoint;
    point[0] = at(p[0]);
    return point;
  });

  // From the far end inwards: the image of point i starts the image of the segment from point i - 1 to i.
  const negative: CurvePoint[] = [];
  for (let i = points.length - 1; i >= 1; i--) {
    const time = cycle - at(points[i][0]);
    const [shape, tension, handleTime, handleValue] = flipped(points[i - 1]);
    const hold = shape === 1;
    // A hold keeps its start value, which for the image is the value at the segment's inner end.
    const value = hold ? -points[i - 1][1] : -points[i][1];
    const stepAfterSlope = hold && (i === points.length - 1 || points[i][2] !== 1) && value !== -points[i][1];
    if (stepAfterSlope) {
      negative.push([time, -points[i][1], 0, 0]);
      negative.push([time + 1, value, 1, 0]);
    } else {
      negative.push(shape === 3 ? [time, value, shape, tension, handleTime, handleValue] : [time, value, shape, tension]);
    }
  }
  return [...negative, ...positive];
}

/** A drawn curve's positive half with straight segments through points given as [u, value], u from 0 to 1. */
const half = (points: Array<[number, number]>, segment = 0): CurvePoint[] =>
  points.map(([u, v]) => [Math.round(u * cycle), v, segment, 0]);

/**
 * The drawn curve's presets (S12), as positive halves for the symmetric editor: the identity, a soft curve, a fold and
 * a staircase. The asymmetric editor plays them mirrored, and also offers a half-wave curve.
 */
export const SYMMETRIC_PRESETS: Record<string, CurvePoint[]> = {
  Identity: half([[0, 0], [1, 1]]),
  Soft: [
    [0, 0, 3, 0, 0.2, 0.8],
    [cycle, 1, 0, 0],
  ],
  Fold: half([[0, 0], [0.5, 1], [1, 0]]),
  Steps: half([[0, 0], [0.25, 1 / 3], [0.5, 2 / 3], [0.75, 1], [1, 1]], 1),
};

/** The asymmetric editor's presets: the symmetric ones mirrored, and a half-wave curve. */
export const ASYMMETRIC_PRESETS: Record<string, CurvePoint[]> = {
  ...Object.fromEntries(Object.entries(SYMMETRIC_PRESETS).map(([name, points]) => [name, mirrored(points)])),
  Half: [
    [0, -0.25, 0, 0],
    [cycle / 2, 0, 0, 0],
    [cycle, 1, 0, 0],
  ],
};

/** The points of the whole drawn curve: the node's, or the identity for a new node. */
export const drawnPoints = (node: GraphNode): CurvePoint[] =>
  node.modulationShape && node.modulationShape.length > 0 ? node.modulationShape : ASYMMETRIC_PRESETS.Identity;

/** The waveshaper's meters: the input level (linear), the shaper's input u it gives, and the running curve at u. */
export interface WaveshaperMeters {
  input?: number;
  shaperInput?: number;
  shaped?: number;
}

/**
 * Where the level dot sits on the drawn curve's plot, which is drawn over u: at u, clamped to -1..1, and the curve
 * there; the symmetric plot's positive quadrant shows a negative u at its mirror image. Null when the input is too
 * quiet to show.
 */
export function drawnDot(meters: WaveshaperMeters | null | undefined, symmetric: boolean, minLevel: number): { x: number; y: number } | null {
  if (!meters || !(meters.input !== undefined && meters.input >= minLevel)) return null;
  const u = meters.shaperInput ?? 0;
  const y = meters.shaped ?? 0;
  const flip = symmetric && u < 0 ? -1 : 1;
  return { x: Math.max(-1, Math.min(1, flip * u)), y: flip * y };
}
