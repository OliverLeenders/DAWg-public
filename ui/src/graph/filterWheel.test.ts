import { describe, expect, it } from 'vitest';
import { filterSlopeSteps, stepFilterSlope } from './filterWheel';

describe('the filter slope wheel', () => {
  it('steps once per wheel notch and keeps partial movement for a touchpad', () => {
    expect(filterSlopeSteps(0, -100)).toEqual({ steps: 1, accumulated: 0 });
    expect(filterSlopeSteps(0, 100)).toEqual({ steps: -1, accumulated: 0 });
    expect(filterSlopeSteps(0, -50)).toEqual({ steps: 0, accumulated: 50 });
    expect(filterSlopeSteps(50, -50)).toEqual({ steps: 1, accumulated: 0 });
    expect(filterSlopeSteps(50, -120)).toEqual({ steps: 1, accumulated: 70 });
  });

  it('clamps the slope to its choices', () => {
    expect(stepFilterSlope(0, -1, 0, 3)).toBe(0);
    expect(stepFilterSlope(3, 1, 0, 3)).toBe(3);
    expect(stepFilterSlope(0, 1, 0, 3)).toBe(1);
    expect(stepFilterSlope(1, 2, 0, 3)).toBe(3);
  });
});
