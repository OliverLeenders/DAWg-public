import { describe, expect, it } from 'vitest';
import type { Clip } from '../bridge/protocol';
import { clipPasses } from '../pianoroll/controllers';
import { gainToPosition, positionToGain } from './automation';

describe('automation values', () => {
  it('maps the fader curve both ways, with 0 dB near 0.71', () => {
    expect(gainToPosition(0)).toBeCloseTo(Math.SQRT1_2, 6);
    expect(positionToGain(gainToPosition(-12))).toBeCloseTo(-12, 6);
    expect(positionToGain(0)).toBe(-96);
    expect(positionToGain(1)).toBeCloseTo(6.02, 2);
  });
});

describe('clip passes', () => {
  const clip = (fields: Partial<Clip>) => ({ start: 1000, length: 400, offset: 0, loopStart: 0, loopLength: 0, ...fields }) as Clip;

  it('is one pass without a loop', () => {
    expect(clipPasses(clip({ offset: 50 }))).toEqual([{ start: 1000, end: 1400, contentAt: 50 }]);
  });

  it('repeats the loop range', () => {
    expect(clipPasses(clip({ loopStart: 0, loopLength: 150, offset: 100 }))).toEqual([
      { start: 1000, end: 1050, contentAt: 100 },
      { start: 1050, end: 1200, contentAt: 0 },
      { start: 1200, end: 1350, contentAt: 0 },
      { start: 1350, end: 1400, contentAt: 0 },
    ]);
  });
});
