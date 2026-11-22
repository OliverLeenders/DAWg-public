import { describe, expect, it } from 'vitest';
import { NODE_TYPES } from '../bridge/protocol';
import { lfoValue, phaserFrequency, sweepCurves, sweepRange, tapDelayMs, tapOffsets, REFERENCE_MS, MIN_DELAY_MS } from './modulationEffects';

// The same cases as tests/ModulationEffectsTests.cpp where the formulas meet.
describe('the modulation effects panels', () => {
  it('shapes the LFO as the engine does', () => {
    expect(lfoValue(0, 0.25)).toBeCloseTo(1, 12);
    expect(lfoValue(1, 0)).toBeCloseTo(0, 12);
    expect(lfoValue(1, 0.25)).toBeCloseTo(1, 12);
    expect(lfoValue(1, 0.75)).toBeCloseTo(-1, 12);
    expect(lfoValue(1, 0.125)).toBeCloseTo(0.5, 12);
  });

  it('sweeps the delay around Delay, or through zero around the reference', () => {
    expect(tapDelayMs(15, 15, 1)).toBeCloseTo(17.25, 9);
    expect(tapDelayMs(2, 100, -1)).toBeCloseTo(MIN_DELAY_MS, 9);
    expect(tapDelayMs(10, 100, 1, true)).toBeCloseTo(2 * REFERENCE_MS - MIN_DELAY_MS, 9);
    expect(tapDelayMs(2, 50, -1, true)).toBeCloseTo(REFERENCE_MS - 1, 9);
  });

  it('sweeps the phaser in octaves, clamped', () => {
    expect(phaserFrequency(700, 25, 1)).toBeCloseTo(1400, 6);
    expect(phaserFrequency(700, 100, 1)).toBeCloseTo(11200, 6);
    expect(phaserFrequency(4000, 100, 1)).toBeCloseTo(0.45 * 48000, 6);
    expect(phaserFrequency(50, 100, -1)).toBe(20);
  });

  it('offsets the right channel by Spread, over the voices for the Chorus', () => {
    expect(tapOffsets(NODE_TYPES.flanger, { spread: 90 }, 1)).toEqual([0.25]);
    expect(tapOffsets(NODE_TYPES.chorus, { spread: 180, voices: 2 }, 0)).toEqual([0, 0.5]);
    expect(tapOffsets(NODE_TYPES.chorus, { spread: 180, voices: 2 }, 1)).toEqual([0.25, 0.75]);
  });

  it('draws a curve per tap, left first', () => {
    const curves = sweepCurves(NODE_TYPES.chorus, { voices: 3, delay: 15, depth: 15, spread: 90, shape: 0 }, 16);
    expect(curves.map((c) => c.channel)).toEqual([0, 0, 0, 1, 1, 1]);
    expect(curves[0].values).toHaveLength(17);
    const [low, high] = sweepRange(NODE_TYPES.chorus, curves);
    expect(low).toBe(0);
    expect(high).toBeGreaterThan(17.25);
    const phaser = sweepCurves(NODE_TYPES.phaser, { centre: 700, depth: 25, spread: 0 }, 4);
    const [pLow, pHigh] = sweepRange(NODE_TYPES.phaser, phaser);
    expect(pLow).toBeCloseTo(350 / 1.5, 6);
    expect(pHigh).toBeCloseTo(1400 * 1.5, 6);
  });
});
