import { describe, expect, it } from 'vitest';
import { curveAt, MIN_DOT_LEVEL, shaperDot } from './panelDraw';

describe('the level dot on a transfer curve', () => {
  const curve = { x: [-1, 0, 0.5, 1], y: [-0.8, 0, 0.4, 0.6] };

  it('reads a sampled curve between its samples, holding its ends', () => {
    expect(curveAt(curve, 0.25)).toBeCloseTo(0.2);
    expect(curveAt(curve, 0.75)).toBeCloseTo(0.5);
    expect(curveAt(curve, -2)).toBe(-0.8);
    expect(curveAt(curve, 2)).toBe(0.6);
  });

  it('puts the input level on the curve, clamped to the plot', () => {
    expect(shaperDot(curve, 0.5)).toEqual({ x: 0.5, y: 0.4 });
    expect(shaperDot(curve, 3)).toEqual({ x: 1, y: 0.6 });
  });

  it('hides when the input is too quiet or there is no curve yet', () => {
    expect(shaperDot(curve, MIN_DOT_LEVEL / 2)).toBeNull();
    expect(shaperDot(curve, undefined)).toBeNull();
    expect(shaperDot(null, 0.5)).toBeNull();
  });
});
