import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { TempoMap } from '../time/tempoMap';
import { resizeEnds, resizeStarts, snappedTravel, type Grid } from './dragSnap';
import type { EditNote } from './edit';

const BEAT = TICKS_PER_BEAT;
const map = new TempoMap();
const beatGrid: Grid = (t, round) => map.snap(t, BEAT, round);

const note = (id: number, start: number, length: number): EditNote => ({ id, clipId: 1, start, length, pitch: 60, velocity: 0.8 });

describe('resizeEnds', () => {
  // Starts on beat 1, ends a third of a beat past beat 2: off the grid.
  const offGrid = note(1, BEAT, (4 * BEAT) / 3);

  it('puts an off-grid end on the nearest grid line, not its old length plus grid steps', () => {
    // Dragged right by about a beat: the end goes to beat 3, not to 3⅓.
    const [n] = resizeEnds([offGrid], offGrid, 0.9 * BEAT, BEAT, beatGrid);
    expect(n.start + n.length).toBe(3 * BEAT);
    // A small drag already lands on the grid line nearest the dragged end.
    const [m] = resizeEnds([offGrid], offGrid, 0.1 * BEAT, BEAT, beatGrid);
    expect(m.start + m.length).toBe(2 * BEAT);
  });

  it('stops at the first grid line after the start', () => {
    const [n] = resizeEnds([offGrid], offGrid, -10 * BEAT, BEAT, beatGrid);
    expect(n.start + n.length).toBe(2 * BEAT);
    const lateStart = note(2, 1.5 * BEAT, 2 * BEAT);
    const [m] = resizeEnds([lateStart], lateStart, -10 * BEAT, BEAT, beatGrid);
    expect(m.start + m.length).toBe(2 * BEAT);
  });

  it('moves the other notes by the grabbed note’s travel', () => {
    const other = note(2, 4 * BEAT, 2 * BEAT);
    const [a, b] = resizeEnds([offGrid, other], offGrid, 0.9 * BEAT, BEAT, beatGrid);
    expect(a.start + a.length).toBe(3 * BEAT);
    // The grabbed end went ⅔ of a beat, so the other note grew by the same.
    expect(b.length).toBe(2 * BEAT + (2 * BEAT) / 3);
  });

  it('drags freely without a grid', () => {
    const [n] = resizeEnds([offGrid], offGrid, 0.9 * BEAT, BEAT, null);
    expect(n.length).toBe(offGrid.length + Math.round(0.9 * BEAT));
  });
});

describe('resizeStarts', () => {
  const offGrid = note(1, 1.25 * BEAT, 3 * BEAT);

  it('puts an off-grid start on a grid line and keeps the end', () => {
    const [n] = resizeStarts([offGrid], offGrid, 0.6 * BEAT, BEAT, beatGrid);
    expect(n.start).toBe(2 * BEAT);
    expect(n.start + n.length).toBe(4.25 * BEAT);
    const [m] = resizeStarts([offGrid], offGrid, -0.2 * BEAT, BEAT, beatGrid);
    expect(m.start).toBe(BEAT);
  });

  it('stops at the last grid line before the end', () => {
    const [n] = resizeStarts([offGrid], offGrid, 10 * BEAT, BEAT, beatGrid);
    expect(n.start).toBe(4 * BEAT);
    expect(n.length).toBe(0.25 * BEAT);
  });

  it('never goes before zero', () => {
    const other = note(2, 0.5 * BEAT, BEAT);
    const [, b] = resizeStarts([offGrid, other], offGrid, -BEAT, BEAT, beatGrid);
    expect(b.start).toBe(0);
  });
});

describe('snappedTravel (moves)', () => {
  it('puts the grabbed note’s off-grid start on a grid line', () => {
    const start = 1.3 * BEAT;
    expect(start + snappedTravel(start, 0.9 * BEAT, beatGrid)).toBe(2 * BEAT);
    expect(start + snappedTravel(start, 0.1 * BEAT, beatGrid)).toBe(BEAT);
  });

  it('follows bar lines through the tempo map', () => {
    // A 7/8 bar, then 4/4: the quarter grid restarts at 3.5 beats.
    const odd = new TempoMap([], [
      { bar: 0, numerator: 7, denominator: 8 },
      { bar: 1, numerator: 4, denominator: 4 },
    ]);
    const grid: Grid = (t, round) => odd.snap(t, BEAT, round);
    expect(3.25 * BEAT + snappedTravel(3.25 * BEAT, 1.2 * BEAT, grid)).toBe(4.5 * BEAT);
  });
});
