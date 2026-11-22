import { describe, expect, it } from 'vitest';
import { outputRange } from './SaturatorPanel';

describe('the saturator panel', () => {
  it('scales the output axis to the curve, in steady steps', () => {
    expect(outputRange([-0.1, 0, 0.1])).toBeCloseTo(0.1);
    expect(outputRange([-0.93, 0.95])).toBeCloseTo(0.95);
    expect(outputRange([-0.96, 0.96])).toBeCloseTo(1);
    expect(outputRange([-1.6, 1.2])).toBeCloseTo(2);
    expect(outputRange([0, 0])).toBeCloseTo(0.05);
  });
});
