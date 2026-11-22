import { describe, expect, it } from 'vitest';
import { bitsText, holdInstants, levelCount, noteRate, quantise, rateText, scope, transfer } from './bitCrusher';

// The same cases as the engine's (tests/BitCrusherTests.cpp).

const defaults = { bits: 8, quantiser: 0, rateMode: 0, rate: 11000, note: 24, drive: 0, clip: 0, bias: 0 };

describe('quantise', () => {
  it('has 2^bits + 1 mid-tread and 2^bits mid-rise levels, 2^(1 - bits) apart', () => {
    for (const bits of [1, 2, 4, 8]) {
      const tread = new Set<number>();
      const rise = new Set<number>();
      for (let i = -1200; i <= 1200; i++) {
        tread.add(quantise(i / 1000, bits, false));
        rise.add(quantise(i / 1000, bits, true));
      }
      expect(tread.size).toBe(2 ** bits + 1);
      expect(rise.size).toBe(2 ** bits);
      expect(levelCount(bits, false)).toBe(2 ** bits + 1);
      expect(levelCount(bits, true)).toBe(2 ** bits);
      expect(Math.max(...tread)).toBe(1);
      expect(Math.max(...rise)).toBe(1 - 2 ** -bits);
    }
  });

  it('keeps silence silent mid-tread and never outputs 0 mid-rise', () => {
    expect(quantise(0, 4, false)).toBe(0);
    expect(quantise(0, 4, true)).toBe(0.0625);
    expect(quantise(-1e-9, 1, true)).toBe(-0.5);
    expect(quantise(-0.5, 1, false)).toBe(-1);
  });

  it('puts fractional depths between their neighbours and is off at 24 bits', () => {
    expect(levelCount(4.5, false)).toBeGreaterThan(levelCount(4, false));
    expect(levelCount(4.5, false)).toBeLessThan(levelCount(5, false));
    expect(quantise(0.123456789, 24, false)).toBe(0.123456789);
    expect(levelCount(24, true)).toBe(Infinity);
  });
});

describe('the transfer curve', () => {
  it('applies drive, the soft clip and the bias in half-steps', () => {
    expect(transfer(0.1, { ...defaults, drive: 20 })).toBe(1);
    expect(transfer(0.9, { ...defaults, bits: 24, drive: 6, clip: 1 })).toBeCloseTo(Math.tanh(0.9 * 10 ** 0.3), 12);
    // At 2 bits (steps of 1/2) a bias of 100 % moves silence half a step: to the boundary, which rounds up.
    expect(transfer(0, { ...defaults, bits: 2, bias: 100 })).toBe(0.5);
    expect(transfer(0, { ...defaults, bits: 2, bias: 60 })).toBe(0);
  });
});

describe('the hold', () => {
  it('takes every ratio-th sample, alternating neighbours at fractional ratios', () => {
    const lengths = (rate: number) => {
      const at = holdInstants(rate, 48000, 4800).flatMap((t, i) => (t ? [i] : []));
      return new Set(at.slice(1).map((v, i) => v - at[i]));
    };
    expect([...lengths(12000)]).toEqual([4]);
    expect([...lengths(19200)].sort()).toEqual([2, 3]);
    expect([...lengths(48000)]).toEqual([1]);
    // 0.1 and 1/3 aren't exact in binary; the holds stay whole.
    expect([...lengths(4800)]).toEqual([10]);
    expect([...lengths(16000)]).toEqual([3]);
    expect(holdInstants(12000, 48000, 1)).toEqual([true]);
  });

  it('runs the Note mode from C4', () => {
    expect(noteRate(0)).toBeCloseTo(261.6256, 4);
    expect(noteRate(12)).toBeCloseTo(523.2511, 4);
  });

  it('draws one cycle of the test tone, held', () => {
    const { dry, wet } = scope({ ...defaults, bits: 24, rate: 4800 }, 48000);
    expect(dry.length).toBe(218);
    expect(wet[0]).toBe(dry[0]);
    expect(wet[9]).toBe(dry[0]);
    expect(wet[10]).toBe(dry[10]);
  });
});

describe('the readouts', () => {
  it('name the rate, its ratio and the levels', () => {
    expect(rateText(11025, 44100)).toBe('11.0 kHz · ÷4.00');
    expect(rateText(48000, 48000)).toBe('Full rate');
    expect(rateText(500, 48000)).toBe('500 Hz · ÷96.00');
    expect(bitsText(8, false)).toBe('8.0 bits · 257 levels');
    expect(bitsText(24, false)).toBe('Bits off');
  });
});
