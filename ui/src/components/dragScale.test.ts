import { describe, expect, it } from 'vitest';
import { centredTaper, dragTo, logTaper, normOf, powerTaper, rangePixels, wheelTo, type DragSpec } from './dragScale';

const pixelsPerStep = (spec: DragSpec, fine = false) => rangePixels(spec, fine) / (spec.max - spec.min);

describe('dragging a number field', () => {
  it('moves a whole number at least 10 px a step, a short range up to 20, and none slower than 300 px', () => {
    const unison: DragSpec = { min: 1, max: 16, integer: true };
    expect(pixelsPerStep(unison)).toBeCloseTo(13.3, 1);
    expect(pixelsPerStep({ min: 1, max: 8, integer: true })).toBe(20);
    expect(pixelsPerStep({ min: 1, max: 64, integer: true })).toBeCloseTo(4.8, 1);
    expect(pixelsPerStep({ min: 0, max: 127, integer: true })).toBeCloseTo(2.4, 1);
  });

  it('is ten times finer with Shift, but no finer than 10 px a step for a whole number', () => {
    expect(pixelsPerStep({ min: 0, max: 127, integer: true }, true)).toBe(10);
    expect(pixelsPerStep({ min: 1, max: 16, integer: true }, true)).toBeCloseTo(13.3, 1);
    expect(rangePixels({ min: -1, max: 1 }, true)).toBe(2000);
  });

  it('lands on whole voices of Unison as the drag goes', () => {
    const unison: DragSpec = { min: 1, max: 16, integer: true };
    const start = normOf(unison, 1);
    expect(dragTo(unison, start, 6, false)).toBe(1);
    expect(dragTo(unison, start, 14, false)).toBe(2);
    expect(dragTo(unison, start, 200, false)).toBe(16);
  });

  it('crosses a modulation depth from -100 % to +100 % in 200 px, rounded to its step', () => {
    const depth: DragSpec = { min: -10, max: 10, quantum: 0.01 };
    const fromMinus = normOf(depth, -10);
    expect(dragTo(depth, fromMinus, 200, false)).toBe(10);
    expect(dragTo(depth, fromMinus, 100, false)).toBeCloseTo(0, 6);
    expect(dragTo(depth, fromMinus, 1, false)).toBeCloseTo(-9.9, 6);
  });

  it('drags a tempo along a log curve, a couple of pixels a BPM around 120', () => {
    const bpm: DragSpec = { min: 5, max: 2000, quantum: 0.01, taper: logTaper(5, 2000), rangePixels: 1200 };
    const moved = dragTo(bpm, normOf(bpm, 120), 10, false);
    expect(moved).toBeGreaterThan(125);
    expect(moved).toBeLessThan(127);
  });

  it('gives the short end of a time more travel, and an offset fine travel either side of zero', () => {
    const crossfade = powerTaper(0, 480000, 3);
    expect(crossfade.fromNorm(0.1)).toBeCloseTo(480, 6);
    expect(crossfade.toNorm(crossfade.fromNorm(0.37))).toBeCloseTo(0.37, 9);
    const offset = centredTaper(1000, 2);
    expect(offset.fromNorm(0.5)).toBe(0);
    expect(offset.fromNorm(0.6)).toBeCloseTo(40, 6);
    expect(offset.fromNorm(0.4)).toBeCloseTo(-40, 6);
    expect(offset.toNorm(-250)).toBeCloseTo(0.25, 9);
  });

  it('turns a wheel notch into a step of a whole number or a hundredth of a continuous travel', () => {
    expect(wheelTo({ min: 1, max: 16, integer: true }, 4, 1)).toBe(5);
    expect(wheelTo({ min: 0, max: 100 }, 50, -1)).toBeCloseTo(49, 9);
    // A quantum coarser than the hundredth still moves a quantum.
    expect(wheelTo({ min: 0, max: 10, quantum: 1 }, 3, 1)).toBe(4);
    expect(wheelTo({ min: 5, max: 2000, quantum: 0.01 }, 120, 1, 1)).toBe(121);
  });
});
