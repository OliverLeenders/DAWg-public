import { describe, expect, it } from 'vitest';
import type { CurvePoint } from '../bridge/protocol';
import { evaluateCurve } from '../pianoroll/geometry';
import { pivotsOf, tiltShape, tiltSpan, withNodeAt } from './pitchShapes';

const flat: CurvePoint[] = [
  [0, 0, 4, 0],
  [1000, 0, 4, 0],
];

describe('note shapes', () => {
  it('pivots in the middle by default', () => {
    expect(pivotsOf([], 1000)).toEqual([500]);
    expect(pivotsOf([200, 700], 1000)).toEqual([200, 700]);
  });

  it('adds a node without changing the curve', () => {
    const curve: CurvePoint[] = [
      [0, -1, 4, 0],
      [500, 0, 4, 0],
      [1000, 0.5, 4, 0],
    ];
    const withNode = withNodeAt(curve, 250);
    expect(withNode).toHaveLength(4);
    for (const t of [100, 250, 400, 700]) expect(evaluateCurve(withNode, t, 0)).toBeCloseTo(evaluateCurve(curve, t, 0), 1);
    expect(withNodeAt(curve, 500)).toHaveLength(3);
  });

  it('tilts the side of a pivot, the edge by the whole drag', () => {
    const span = tiltSpan([500], 1000, 800);
    expect(span).toEqual({ fixed: 500, moving: 1000 });
    const tilted = tiltShape(flat, span.fixed, span.moving, 1);
    expect(evaluateCurve(tilted, 0, 0)).toBeCloseTo(0);
    expect(evaluateCurve(tilted, 500, 0)).toBeCloseTo(0);
    expect(evaluateCurve(tilted, 1000, 0)).toBeCloseTo(1);
    // Between the nodes the smooth curve eases from the pivot to the edge.
    expect(evaluateCurve(tilted, 750, 0)).toBeGreaterThan(0);
    expect(evaluateCurve(tilted, 750, 0)).toBeLessThan(1);

    // The left side tilts around the same pivot, the start moving.
    expect(tiltSpan([500], 1000, 100)).toEqual({ fixed: 500, moving: 0 });
  });

  it('tilts a span between two pivots from its nearer end', () => {
    expect(tiltSpan([200, 800], 1000, 300)).toEqual({ fixed: 800, moving: 200 });
    expect(tiltSpan([200, 800], 1000, 700)).toEqual({ fixed: 200, moving: 800 });
    const tilted = tiltShape(flat, 200, 800, 2);
    expect(evaluateCurve(tilted, 100, 0)).toBeCloseTo(0);
    expect(evaluateCurve(tilted, 800, 0)).toBeCloseTo(2);
  });
});
