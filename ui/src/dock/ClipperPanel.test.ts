import { describe, expect, it } from 'vitest';
import { ceilingLevel, meterFraction, METER_RANGE_DB, overReadout } from './ClipperPanel';

describe('the clipper panel', () => {
  it('draws the ceiling scaled by Output', () => {
    expect(ceilingLevel(0, 0)).toBeCloseTo(1);
    expect(ceilingLevel(-6, 0)).toBeCloseTo(0.501, 3);
    expect(ceilingLevel(-6, 6)).toBeCloseTo(1);
    expect(ceilingLevel(-12, -6)).toBeCloseTo(0.126, 3);
  });

  it('fills the meter in proportion to the clip amount, within its range', () => {
    expect(meterFraction(0)).toBe(0);
    expect(meterFraction(-3)).toBe(0);
    expect(meterFraction(METER_RANGE_DB / 2)).toBeCloseTo(0.5);
    expect(meterFraction(METER_RANGE_DB * 2)).toBe(1);
  });

  it('reads out the clip amount, or a dash when nothing clips', () => {
    expect(overReadout(0)).toBe('Clip —');
    expect(overReadout(0.01)).toBe('Clip —');
    expect(overReadout(2.44)).toBe('Clip 2.4 dB');
    expect(overReadout(12)).toBe('Clip 12.0 dB');
  });
});
