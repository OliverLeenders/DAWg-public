import { describe, expect, it } from 'vitest';
import { decaySlopeDb, fadeDb, impulseName, keptRange, reverbTime, reverbTimeReadout, shapedEnvelope, tickStep } from './convolution';

describe('the convolution reverb panel', () => {
  it('keeps Length per cent of what follows Start', () => {
    expect(keptRange(4, 0, 100)).toEqual({ from: 0, to: 4 });
    expect(keptRange(4, 1000, 50)).toEqual({ from: 1, to: 2.5 });
    expect(keptRange(2, 5000, 100)).toEqual({ from: 2, to: 2 });
    expect(keptRange(4, 0, 0).to).toBeCloseTo(0.04);
  });

  it('adds the slope that makes the IR decay in Decay times its own decay time, as the engine does', () => {
    expect(decaySlopeDb(2, 100)).toBe(0);
    expect(decaySlopeDb(0, 50)).toBe(0);
    // Falling 60 dB in 1 s instead of 2 s: 30 dB per second more.
    expect(decaySlopeDb(2, 50)).toBeCloseTo(-30);
    expect(decaySlopeDb(1, 10)).toBeCloseTo(-540);
  });

  it('fades with a raised cosine', () => {
    expect(fadeDb(1)).toBeCloseTo(0);
    expect(fadeDb(0.5)).toBeCloseTo(-6.02, 1);
    expect(fadeDb(0)).toBeCloseTo(-120);
  });

  it('shapes the envelope over the kept part, reversed when asked', () => {
    const flat = Array.from({ length: 100 }, () => -10);
    const plain = shapedEnvelope(flat, 1, 1, { startMs: 0, lengthPercent: 100, decayPercent: 100, reverse: false });
    expect(plain).toHaveLength(100);
    expect(plain.every((p) => p.db === -10)).toBe(true);

    const decayed = shapedEnvelope(flat, 1, 1, { startMs: 500, lengthPercent: 100, decayPercent: 50, reverse: false });
    expect(decayed[0].t).toBeGreaterThanOrEqual(0.5);
    // 60 dB per second more, from the kept part's start.
    expect(decayed.at(-1)!.db - decayed[0].db).toBeCloseTo(-60 * (decayed.at(-1)!.t - decayed[0].t));

    const reversed = shapedEnvelope(flat, 1, 1, { startMs: 0, lengthPercent: 100, decayPercent: 50, reverse: true });
    expect(reversed[0].db).toBeLessThan(reversed.at(-1)!.db);
    expect(reversed.map((p) => p.t)).toEqual([...reversed.map((p) => p.t)].sort((a, b) => a - b));

    const shortened = shapedEnvelope(flat, 1, 1, { startMs: 0, lengthPercent: 50, decayPercent: 100, reverse: false });
    expect(shortened.at(-1)!.t).toBeLessThanOrEqual(0.5);
    expect(shortened.at(-1)!.db).toBeLessThan(-20);
    expect(shapedEnvelope([], 1, 1, { startMs: 0, lengthPercent: 100, decayPercent: 100, reverse: false })).toEqual([]);
  });

  it('reads out the reverb time after Decay and Stretch', () => {
    expect(reverbTime(2, 100, 100)).toBe(2);
    expect(reverbTime(2, 50, 150)).toBeCloseTo(1.5);
    expect(reverbTime(0, 50, 150)).toBe(0);
    expect(reverbTimeReadout(2.44)).toBe('RT 2.4 s');
    expect(reverbTimeReadout(0.35)).toBe('RT 350 ms');
    expect(reverbTimeReadout(0)).toBe('RT —');
  });

  it('names the default, a file and a missing file', () => {
    expect(impulseName(undefined, undefined, false)).toBe('Default room');
    expect(impulseName(0, 'hall', false)).toBe('Default room');
    expect(impulseName(3, 'hall', false)).toBe('hall');
    expect(impulseName(3, 'hall', true)).toBe('Missing file');
    expect(impulseName(3, undefined, false)).toBe('Missing file');
  });

  it('labels the time axis in steps of 1, 2 or 5', () => {
    expect(tickStep(2.5, 5)).toBe(0.5);
    expect(tickStep(10, 5)).toBe(2);
    expect(tickStep(0.3, 3)).toBeCloseTo(0.1);
  });
});
