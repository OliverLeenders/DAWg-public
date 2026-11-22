import { describe, expect, it } from 'vitest';
import { MAX_SLICES, startKeyFor } from './SliceDialog';

describe('the slices\' keys', () => {
  it('start at the chosen key, lower when they would pass the top key, as sliceToSampler does', () => {
    expect(startKeyFor(36, 16)).toBe(36);
    expect(startKeyFor(36, 92)).toBe(36);
    expect(startKeyFor(36, 100)).toBe(28);
    expect(startKeyFor(120, 16)).toBe(112);
    expect(startKeyFor(36, MAX_SLICES)).toBe(0);
  });
});
