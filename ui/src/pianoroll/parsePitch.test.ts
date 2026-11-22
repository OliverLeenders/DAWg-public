import { describe, expect, it } from 'vitest';
import { parsePitch, pitchName } from './geometry';

describe('typed pitches', () => {
  it('reads note names with C4 at 60, and MIDI numbers', () => {
    expect(parsePitch('C4')).toBe(60);
    expect(parsePitch('c#3')).toBe(49);
    expect(parsePitch('Eb4')).toBe(63);
    expect(parsePitch('C-1')).toBe(0);
    expect(parsePitch(' 61 ')).toBe(61);
    expect(parsePitch(pitchName(49))).toBe(49);
  });

  it('refuses what is not a pitch', () => {
    expect(parsePitch('H2')).toBeNull();
    expect(parsePitch('')).toBeNull();
    expect(parsePitch('200')).toBeNull();
    expect(parsePitch('G9')).toBe(127);
    expect(parsePitch('A9')).toBeNull();
  });
});
