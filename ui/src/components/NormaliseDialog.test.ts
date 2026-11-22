import { describe, expect, it } from 'vitest';
import { parseTarget } from './NormaliseDialog';

describe('the Normalise target', () => {
  it('reads levels from -60 to 0 dBFS', () => {
    expect(parseTarget('0')).toBe(0);
    expect(parseTarget('-0.1')).toBe(-0.1);
    expect(parseTarget(' -6 dB ')).toBe(-6);
    expect(parseTarget('-3,5 dBFS')).toBe(-3.5);
    expect(parseTarget('-60')).toBe(-60);
  });

  it('refuses levels above full scale, below -60 and non-numbers', () => {
    expect(parseTarget('1')).toBeNull();
    expect(parseTarget('-61')).toBeNull();
    expect(parseTarget('')).toBeNull();
    expect(parseTarget('loud')).toBeNull();
  });
});
