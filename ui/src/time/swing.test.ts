import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { quantiseNote, quantiseTick, swingApplies, swungLine, swungSnap } from './swing';
import { TempoMap } from './tempoMap';

const beat = TICKS_PER_BEAT;
const sixteenth = beat / 4;

// The same table as tests/SwingTests.cpp, so the quantise and the arpeggiator agree.
const table: [index: number, swing: number, expected: number][] = [
  [0, 0.5, 0],
  [1, 0.5, 3840],
  [1, 0.6, 4608],
  [1, 2 / 3, 5120],
  [1, 0.75, 5760],
  [2, 0.75, 7680],
  [3, 0.75, 13440],
  [3, 0.9, 13440], // clamped to 75 %
  [1, 0.3, 3840], // clamped to 50 %
];

describe('swing', () => {
  it('moves every second grid line', () => {
    for (const [index, swing, expected] of table) expect(swungLine(index, sixteenth, swing)).toBeCloseTo(expected, 9);
  });

  it('applies to straight grids only', () => {
    expect(swingApplies(1 / 2)).toBe(true);
    expect(swingApplies(1 / 4)).toBe(true);
    expect(swingApplies(1)).toBe(true);
    expect(swingApplies(1 / 3)).toBe(false);
    expect(swingApplies(1 / 6)).toBe(false);
    expect(swingApplies(0)).toBe(false);
  });

  it('snaps to the nearest swung line, restarting at each bar', () => {
    const map = new TempoMap();
    const eighth = beat / 2;
    // 66 %: the off-beat 8th sits at 2/3 of the beat.
    expect(swungSnap(map, eighth, eighth, 2 / 3)).toBe(Math.round((2 * beat) / 3));
    expect(swungSnap(map, eighth - 100, eighth, 2 / 3)).toBe(Math.round((2 * beat) / 3));
    expect(swungSnap(map, 100, eighth, 2 / 3)).toBe(0);
    // A tick near the bar end goes to the next bar line.
    expect(swungSnap(map, 4 * beat - 10, eighth, 0.75)).toBe(4 * beat);

    // A 7/8 bar after a 4/4 one: the grid starts again on the bar line.
    const odd = new TempoMap([], [
      { bar: 0, numerator: 4, denominator: 4 },
      { bar: 1, numerator: 7, denominator: 8 },
    ]);
    expect(swungSnap(odd, 4 * beat + eighth, eighth, 0.75)).toBe(4 * beat + 0.75 * beat);
    // Its seventh 8th has no partner and stays straight.
    expect(swungSnap(odd, 4 * beat + 6 * eighth + 10, eighth, 0.75)).toBe(4 * beat + 6 * eighth);
  });

  it('moves notes by the strength', () => {
    const map = new TempoMap();
    expect(quantiseTick(map, 40, sixteenth, 0.5, 1)).toBe(0);
    expect(quantiseTick(map, 40, sixteenth, 0.5, 0.5)).toBe(20);
    expect(quantiseTick(map, 40, sixteenth, 0.5, 0)).toBe(40);
  });

  it('quantises lengths through the end', () => {
    const map = new TempoMap();
    const note = { id: 1, start: 50, length: sixteenth - 200 };
    expect(quantiseNote(note, map, sixteenth, 0.5, 1, false)).toEqual({ id: 1, start: 0, length: sixteenth - 200 });
    expect(quantiseNote(note, map, sixteenth, 0.5, 1, true)).toEqual({ id: 1, start: 0, length: sixteenth });
    // An end that would land on the start keeps one grid step.
    expect(quantiseNote({ start: 10, length: 20 }, map, sixteenth, 0.5, 1, true)).toEqual({ start: 0, length: sixteenth });
  });
});
