import { describe, expect, it } from 'vitest';
import { differsInTime } from './MidiImportDialog';

const time = (bpm = 120, numerator = 4) => ({
  tempo: [{ tick: 0, bpm }],
  signatures: [{ bar: 0, numerator, denominator: 4 }],
});

describe('asking about a MIDI file\'s tempo', () => {
  it('has nothing to ask when it matches', () => {
    expect(differsInTime(time(), time())).toBe(false);
    expect(differsInTime(time(120.001), time())).toBe(false);
  });

  it('asks when the tempo or the signatures differ', () => {
    expect(differsInTime(time(90), time())).toBe(true);
    expect(differsInTime(time(120, 3), time())).toBe(true);
    expect(differsInTime({ ...time(), tempo: [...time().tempo, { tick: 100, bpm: 90 }] }, time())).toBe(true);
  });
});
