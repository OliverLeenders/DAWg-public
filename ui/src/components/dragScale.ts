// How far a number field moves for a drag or a wheel notch, from its range (docs/ui-review-2026-11-10.md, "Dragging
// number fields"). One rule for every field, so none needs a speed of its own:
//
// - a continuous value crosses its whole range in 200 px, along its parameter's curve where it has one (a skewed Hz or
//   ms knob gives the low end more travel), as a knob turns in 160 px;
// - a whole number moves at least 10 px per step, and at most 20, crossing its range in no more than 300 px: Unison
//   (1 to 16) 13 px a voice, a row's 64 steps 5 px each, a velocity (0 to 127) 2.4 px;
// - Shift is ten times finer, as long as that is finer than 10 px per step for a whole number;
// - a wheel notch is one step of a whole number, or a hundredth of a continuous value's travel.

/** A value's travel: 0 at the minimum, 1 at the maximum, along its curve. */
export interface Taper {
  toNorm: (value: number) => number;
  fromNorm: (norm: number) => number;
}

export interface DragSpec {
  min: number;
  max: number;
  /** Pixels for the whole range instead of 200, for a range much wider than what's used (BPM from 5 to 2000). */
  rangePixels?: number;
  /** Whole steps of `quantum` only (a key, a count). */
  integer?: boolean;
  /** What values round to: 1 for a whole number by default; none for a continuous value unless given. */
  quantum?: number;
  /** The value's curve; a straight line from min to max by default. */
  taper?: Taper;
}

export const CONTINUOUS_PIXELS = 200;
const PIXELS_PER_STEP = 10;
const MOST_PIXELS_PER_STEP = 20;
const MOST_PIXELS = 300;
const FINE = 10;
const WHEEL_TRAVEL = 0.01;

export const linearTaper = (min: number, max: number): Taper => ({
  toNorm: (v) => (max > min ? (v - min) / (max - min) : 0),
  fromNorm: (n) => min + n * (max - min),
});

const clamp01 = (n: number) => Math.min(1, Math.max(0, n));
const quantumOf = (spec: DragSpec) => spec.quantum ?? (spec.integer ? 1 : 0);
const stepsOf = (spec: DragSpec) => Math.max(1, Math.round((spec.max - spec.min) / quantumOf(spec)));

/** The pixels of drag that cross the whole range, normally or with Shift held. */
export function rangePixels(spec: DragSpec, fine = false): number {
  if (!spec.integer) return (spec.rangePixels ?? CONTINUOUS_PIXELS) * (fine ? FINE : 1);
  const steps = stepsOf(spec);
  const normal = Math.min(MOST_PIXELS, Math.max(steps * PIXELS_PER_STEP, Math.min(CONTINUOUS_PIXELS, steps * MOST_PIXELS_PER_STEP)));
  // Finer only while a step is shorter than 10 px.
  return fine ? Math.max(normal, Math.min(normal * FINE, steps * PIXELS_PER_STEP)) : normal;
}

/** A value rounded to its quantum and kept in its range. */
export function settle(spec: DragSpec, value: number): number {
  const quantum = quantumOf(spec);
  const rounded = quantum > 0 ? spec.min + Math.round((value - spec.min) / quantum) * quantum : value;
  return Math.min(spec.max, Math.max(spec.min, rounded));
}

const taperOf = (spec: DragSpec) => spec.taper ?? linearTaper(spec.min, spec.max);

/** Where a value is along its travel, 0..1. */
export const normOf = (spec: DragSpec, value: number) => clamp01(taperOf(spec).toNorm(value));

/** The value a drag of `pixels` (up or right positive) from `startNorm` along the travel lands on. */
export function dragTo(spec: DragSpec, startNorm: number, pixels: number, fine: boolean): number {
  return settle(spec, taperOf(spec).fromNorm(clamp01(startNorm + pixels / rangePixels(spec, fine))));
}

/** The value a number of wheel notches (up positive) from `value` lands on; `wheelStep` overrides the notch. */
export function wheelTo(spec: DragSpec, value: number, notches: number, wheelStep?: number): number {
  if (wheelStep !== undefined) return settle(spec, value + notches * wheelStep);
  if (spec.integer) return settle(spec, value + notches * quantumOf(spec));
  const next = settle(spec, taperOf(spec).fromNorm(clamp01(normOf(spec, value) + notches * WHEEL_TRAVEL)));
  // A quantum coarser than the notch would leave the value where it was: then a quantum per notch.
  return next === value && quantumOf(spec) > 0 ? settle(spec, value + notches * quantumOf(spec)) : next;
}

/** A curve for a range spanning orders of magnitude (BPM, Hz): equal travel for equal ratios. */
export const logTaper = (min: number, max: number): Taper => ({
  toNorm: (v) => Math.log(Math.max(min, v) / min) / Math.log(max / min),
  fromNorm: (n) => min * Math.pow(max / min, n),
});

/** A curve giving the low end more travel (a time in ms, a crossfade): value grows with travel to the given power. */
export const powerTaper = (min: number, max: number, exponent: number): Taper => ({
  toNorm: (v) => Math.pow(Math.max(0, (v - min) / (max - min)), 1 / exponent),
  fromNorm: (n) => min + Math.pow(n, exponent) * (max - min),
});

/** The same for a range around zero (an offset from -1000 to 1000 ms): fine near zero on both sides. */
export const centredTaper = (limit: number, exponent: number): Taper => ({
  toNorm: (v) => 0.5 + 0.5 * Math.sign(v) * Math.pow(Math.min(1, Math.abs(v) / limit), 1 / exponent),
  fromNorm: (n) => Math.sign(n - 0.5) * Math.pow(Math.abs(n - 0.5) * 2, exponent) * limit,
});
