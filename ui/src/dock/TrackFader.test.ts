import { describe, expect, it } from 'vitest';
import { dbToFader, faderToDb } from './TrackFader';

describe('the dock fader curve', () => {
  it('follows the gain lanes up to +6 dB and runs linearly to +12 above', () => {
    expect(dbToFader(0)).toBeCloseTo(0.9 * Math.SQRT1_2, 6);
    expect(dbToFader(6)).toBeCloseTo(0.9, 2);
    expect(dbToFader(12)).toBe(1);
    expect(dbToFader(-96)).toBe(0);
    expect(dbToFader(-200)).toBe(0);
  });

  it('inverts', () => {
    for (const db of [-60, -24, -6, 0, 3, 6, 9, 12]) expect(faderToDb(dbToFader(db))).toBeCloseTo(db, 4);
    expect(faderToDb(0)).toBe(-96);
  });
});
