import { describe, expect, it } from 'vitest';
import { expanderGainChangeDb, expanderRatioThrough, gateCloseDb, gateFloorDb } from './dynamicsCurves';

// The same cases as the engine's (tests/GateExpanderTests.cpp, "the expander's ratio and range").

describe('expanderGainChangeDb', () => {
  it('is 0 above the threshold and (ratio - 1) dB per dB below it', () => {
    expect(expanderGainChangeDb(-30, -40, 2, -80)).toBe(0);
    expect(expanderGainChangeDb(-50, -40, 2, -80)).toBeCloseTo(-10, 5);
    expect(expanderGainChangeDb(-60, -40, 3, -80)).toBeCloseTo(-40, 5);
    expect(expanderGainChangeDb(-60, -40, 1, -80)).toBe(0);
  });

  it('stops at the range', () => {
    expect(expanderGainChangeDb(-60, -40, 4, -30)).toBeCloseTo(-30, 5);
  });

  it('has a soft knee', () => {
    expect(expanderGainChangeDb(-40, -40, 2, -80)).toBeCloseTo(-0.75, 5);
    expect(expanderGainChangeDb(-37, -40, 2, -80)).toBeCloseTo(0, 5);
    expect(expanderGainChangeDb(-43, -40, 2, -80)).toBeCloseTo(-3, 5);
  });
});

describe('the gate', () => {
  it('closes below threshold - hysteresis, to the range or to silence', () => {
    expect(gateCloseDb(-40, 6)).toBe(-46);
    expect(gateCloseDb(-40, -3)).toBe(-40);
    expect(gateFloorDb(-20)).toBe(-20);
    expect(gateFloorDb(-80)).toBe(-Infinity);
  });
});

describe('expanderRatioThrough', () => {
  it('inverts the curve below the knee', () => {
    const output = -60 + expanderGainChangeDb(-60, -40, 3, -80);
    expect(expanderRatioThrough(-60, output, -40)).toBeCloseTo(3, 5);
    expect(expanderRatioThrough(-30, -30, -40)).toBe(1);
  });
});
