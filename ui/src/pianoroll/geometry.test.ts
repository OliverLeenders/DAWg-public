import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT, type CurvePoint, type Vibrato } from '../bridge/protocol';
import {
  adjacentNote,
  evaluateCurve,
  NO_VIBRATO,
  noteInPitchDirection,
  ROUNDED_SHAPE,
  secondsPerTick,
  SMOOTH_SHAPE,
  smoothSlope,
  vibratoOffset,
  type PlacedNote,
} from './geometry';

// Mirrors tests/VibratoTests.cpp so the roll draws exactly what the engine plays.
const spt = secondsPerTick(120); // one beat = 0.5 s
const at = (seconds: number) => seconds / spt;
const vibrato = (patch: Partial<Vibrato>): Vibrato => ({ ...NO_VIBRATO, depth: 1, rateHz: 2, ...patch });

describe('noteInPitchDirection', () => {
  const placed = (id: number, start: number, end: number, pitch: number) =>
    ({ start, end, note: { id, pitch } }) as unknown as PlacedNote;
  // A C major chord at 100, a held bass under it, and a melody note far later, above everything.
  const notes = [
    placed(1, 100, 200, 60),
    placed(2, 100, 200, 64),
    placed(3, 100, 200, 67),
    placed(4, 0, 400, 48),
    placed(5, 5000, 5100, 80),
    placed(6, 300, 400, 72),
  ];
  const ids = (from: number[], direction: 1 | -1) => noteInPitchDirection(notes, new Set(from), direction)?.note.id;

  it('walks through a chord by pitch', () => {
    expect(ids([1], 1)).toBe(2);
    expect(ids([2], 1)).toBe(3);
    expect(ids([3], -1)).toBe(2);
  });

  it('counts a note sounding at the same time as nearest', () => {
    expect(ids([1], -1)).toBe(4);
    expect(ids([4], 1)).toBe(1); // nearest pitch among those it sounds with
  });

  it('prefers the note nearest in time above a closer pitch further away', () => {
    expect(ids([3], 1)).toBe(6);
    expect(ids([6], 1)).toBe(5);
  });

  it('stops when nothing lies that way, and leaves a selection from its outer note', () => {
    expect(ids([5], 1)).toBeUndefined();
    expect(ids([4], -1)).toBeUndefined();
    expect(ids([1, 2], 1)).toBe(3);
    expect(ids([2, 3], -1)).toBe(1);
  });

  it('starts at an end without a selection', () => {
    expect(ids([], 1)).toBe(4);
    expect(ids([], -1)).toBe(5);
  });
});

describe('adjacentNote', () => {
  const placed = (id: number, start: number, pitch: number) =>
    ({ start, end: start + 10, note: { id, pitch } }) as unknown as PlacedNote;
  // Deliberately out of order: the chord at 100 plays 3 (C4), then 1 (E4).
  const notes = [placed(1, 100, 64), placed(2, 200, 60), placed(3, 100, 60), placed(4, 0, 72)];
  const ids = (from: number[], direction: 1 | -1) => adjacentNote(notes, new Set(from), direction)?.note.id;

  it('goes by start, then lowest pitch first', () => {
    expect(ids([4], 1)).toBe(3);
    expect(ids([3], 1)).toBe(1);
    expect(ids([1], 1)).toBe(2);
    expect(ids([2], -1)).toBe(1);
    expect(ids([1], -1)).toBe(3);
  });

  it('starts at an end without a selection, and stops at the ends', () => {
    expect(ids([], 1)).toBe(4);
    expect(ids([], -1)).toBe(2);
    expect(ids([2], 1)).toBeUndefined();
    expect(ids([4], -1)).toBeUndefined();
    expect(adjacentNote([], new Set(), 1)).toBeNull();
  });

  it('leaves a selection of several notes from its far end', () => {
    expect(ids([4, 3], 1)).toBe(1);
    expect(ids([1, 2], -1)).toBe(3);
  });
});

describe('vibratoOffset', () => {
  it('is silent at zero intensity (the default) and defaults to one semitone', () => {
    expect(vibratoOffset(NO_VIBRATO, at(0.125), spt, 0)).toBe(0);
    expect(NO_VIBRATO.depth).toBe(1);
    expect(vibratoOffset({ ...NO_VIBRATO, rateHz: 2 }, at(0.125), spt, 1)).toBeCloseTo(1);
  });

  it('peaks a quarter cycle in and scales with the depth lane', () => {
    expect(vibratoOffset(vibrato({}), at(0.125), spt, 1)).toBeCloseTo(1);
    expect(vibratoOffset(vibrato({}), at(0.375), spt, 1)).toBeCloseTo(-1);
    expect(vibratoOffset(vibrato({}), at(0.125), spt, 0.5)).toBeCloseTo(0.5);
  });

  it('waits for the delay and fades in', () => {
    const v = vibrato({ shape: 2, delay: TICKS_PER_BEAT, fadeIn: TICKS_PER_BEAT });
    expect(vibratoOffset(v, at(0.4), spt, 1)).toBe(0);
    expect(vibratoOffset(v, at(0.6), spt, 1)).toBeCloseTo(0.2);
    expect(vibratoOffset(v, at(1.1), spt, 1)).toBeCloseTo(1);
  });

  it('follows beats when tempo-synced', () => {
    const v = vibrato({ shape: 2, syncBeats: 0.5 });
    expect(vibratoOffset(v, 0.2 * TICKS_PER_BEAT, 1, 1)).toBeCloseTo(1);
    expect(vibratoOffset(v, 0.3 * TICKS_PER_BEAT, 1, 1)).toBeCloseTo(-1);
  });
});

describe('evaluateCurve: smooth segments', () => {
  // The same curve and values as engine/tests CurveTests.cpp ("smooth segments are a monotone cubic ...").
  const S = SMOOTH_SHAPE;
  const curve: CurvePoint[] = [
    [0, 0, S, 0],
    [100, 1, S, 0],
    [200, 3, S, 0],
    [300, 2, S, 0],
    [400, 2, S, 0],
  ];

  it('matches the engine', () => {
    expect(evaluateCurve(curve, 50, 0)).toBeCloseTo(0.3958333333333333, 12);
    expect(evaluateCurve(curve, 150, 0)).toBeCloseTo(2.166666666666667, 12);
    expect(evaluateCurve(curve, 250, 0)).toBeCloseTo(2.5, 12);
    expect(evaluateCurve(curve, 350, 0)).toBeCloseTo(2, 12);
  });

  it('passes through its nodes and never past them', () => {
    for (const [t, v] of curve) expect(evaluateCurve(curve, t, 0)).toBeCloseTo(v, 12);
    for (let t = 0; t <= 400; t++) {
      expect(evaluateCurve(curve, t, 0)).toBeLessThanOrEqual(3 + 1e-12);
      expect(evaluateCurve(curve, t, 0)).toBeGreaterThanOrEqual(-1e-12);
    }
  });

  it('is a straight line on its own', () => {
    expect(evaluateCurve([[0, 0, S, 0], [100, 2, 0, 0], [200, 0, 0, 0]], 25, 0)).toBeCloseTo(0.5, 12);
  });
});

describe('rounded curves', () => {
  // Same knee as "rounded curves round a knee that smooth ones keep" in tests/CurveTests.cpp.
  it('round a knee that smooth ones keep', () => {
    const knee = (shape: number): CurvePoint[] => [
      [0, 0, shape, 0],
      [100, 1, shape, 0],
      [1000, 1, shape, 0],
    ];
    expect(smoothSlope(knee(SMOOTH_SHAPE), 1)).toBe(0);
    expect(smoothSlope(knee(ROUNDED_SHAPE), 1)).toBeCloseTo(0.001, 12);
    expect(evaluateCurve(knee(SMOOTH_SHAPE), 250, 0)).toBeLessThanOrEqual(1);
    const passed = evaluateCurve(knee(ROUNDED_SHAPE), 250, 0);
    expect(passed).toBeGreaterThan(1);
    expect(passed).toBeLessThan(1.15);
  });

  it('are level at their ends', () => {
    const rise: CurvePoint[] = [
      [0, 0, ROUNDED_SHAPE, 0],
      [100, 0, ROUNDED_SHAPE, 0],
      [200, 1, ROUNDED_SHAPE, 0],
    ];
    expect(smoothSlope(rise, 0)).toBe(0);
    expect(smoothSlope(rise, 2)).toBe(0);
    expect(evaluateCurve(rise, 199, 0)).toBeCloseTo(1, 3);
  });
});
