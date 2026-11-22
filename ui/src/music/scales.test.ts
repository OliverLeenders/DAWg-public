import { describe, expect, it } from 'vitest';
import { SCALES, inScale, lockToScale } from './scales';

describe('scales', () => {
  // The masks of engine/include/dawg/Scales.h, written out.
  it('match the engine', () => {
    expect(SCALES.map((s) => s.mask)).toEqual([
      0xab5, 0x5ad, 0x9ad, 0xaad, 0x6ad, 0x5ab, 0xad5, 0x6b5, 0x56b, 0x295, 0x4a9, 0x4e9, 0x555, 0x6db, 0xfff,
    ]);
    expect(SCALES).toHaveLength(15);
  });

  it('lock keys like the engine', () => {
    const major = SCALES[0].mask;
    expect(inScale(61, 1, major)).toBe(true);
    expect(lockToScale(61, 0, major)).toBe(60);
    expect(lockToScale(61, 0, major, 'up')).toBe(62);
    expect(lockToScale(66, 0, major)).toBe(65);
  });
});
