import { describe, expect, it } from 'vitest';
import { SCALES } from '../music/scales';
import { foldRows, keyChangeAt, keySpans, outsideScale, scaleAt, scaleOf, scalesOf } from './fold';
import { TempoMap } from '../time/tempoMap';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { DEFAULT_VIEW, pitchAtY, pitchToRow, pitchToY, rowToPitch, stepRows, visibleRowPitches, withRows, yToPitch, type View } from './geometry';

// Folding the piano roll (docs/design/scale-highlighting.md, SH4 and SH5).

const cMajor = scaleOf({ enabled: true, root: 0, scale: 0 });

describe('scaleOf', () => {
  it('is null without a key', () => {
    expect(scaleOf(undefined)).toBeNull();
    expect(scaleOf({ enabled: false, root: 2, scale: 0 })).toBeNull();
  });

  it('gives the root and the scale mask', () => {
    expect(scaleOf({ enabled: true, root: 9, scale: 1 })).toEqual({ root: 9, mask: SCALES[1].mask });
  });
});

describe('foldRows', () => {
  it('shows every row unfolded, and without a key for Scale', () => {
    expect(foldRows('off', [60], [cMajor!])).toBeUndefined();
    expect(foldRows('scale', [60], [])).toBeUndefined();
  });

  it('folds to the scale, keeping notes outside it', () => {
    const rows = foldRows('scale', [61], [cMajor!])!;
    expect(rows.filter((p) => p >= 60 && p < 72)).toEqual([60, 61, 62, 64, 65, 67, 69, 71]);
    expect(rows[0]).toBe(0);
    expect(rows[rows.length - 1]).toBe(127);
  });

  it('folds to the pitches in use, rounding bent ones', () => {
    expect(foldRows('used', [64, 60.3, 64, 67], [])).toEqual([60, 64, 67]);
    expect(foldRows('used', [], [])).toBeUndefined();
  });

  it('tells notes outside the scale', () => {
    expect(outsideScale(61, cMajor)).toBe(true);
    expect(outsideScale(62, cMajor)).toBe(false);
    expect(outsideScale(61, null)).toBe(false);
  });
});

describe('key changes', () => {
  const map = new TempoMap([{ tick: 0, bpm: 120, ramp: false }], [{ bar: 0, numerator: 4, denominator: 4 }]);
  const keys = [
    { bar: 0, enabled: true, root: 0, scale: 0 },
    { bar: 4, enabled: true, root: 2, scale: 1 },
    { bar: 8, enabled: false, root: 2, scale: 1 },
  ];
  const bar = (n: number) => n * 4 * TICKS_PER_BEAT;

  it('finds the change in effect on a bar', () => {
    expect(keyChangeAt(keys, 3)?.bar).toBe(0);
    expect(keyChangeAt(keys, 4)?.bar).toBe(4);
    expect(keyChangeAt(keys, 20)?.bar).toBe(8);
    expect(keyChangeAt(undefined, 2)).toBeUndefined();
  });

  it('turns them into spans of ticks, with no scale where no key is set', () => {
    const spans = keySpans(keys, map);
    expect(scaleAt(spans, bar(4) - 1)).toEqual(cMajor);
    expect(scaleAt(spans, bar(4))).toEqual({ root: 2, mask: SCALES[1].mask });
    expect(scaleAt(spans, bar(9))).toBeNull();
    expect(scaleAt(spans, -5)).toEqual(cMajor);
    expect(scalesOf(spans)).toHaveLength(2);
  });

  it('folds to every key the song uses', () => {
    const rows = foldRows('scale', [], scalesOf(keySpans(keys, map)))!;
    // C major and D minor together: everything but C sharp, E flat, F sharp and A flat.
    expect(rows.filter((p) => p >= 60 && p < 72)).toEqual([60, 62, 64, 65, 67, 69, 70, 71]);
  });
});

describe('folded geometry', () => {
  const rows = [60, 62, 64, 67];
  const view: View = { ...DEFAULT_VIEW, rows, topPitch: 3.5, keyHeight: 10 };

  it('maps pitches to rows and back, continuously between rows', () => {
    expect(pitchToRow(rows, 62)).toBe(1);
    expect(pitchToRow(rows, 65.5)).toBe(2.5);
    expect(pitchToRow(rows, 58)).toBe(-2);
    expect(pitchToRow(rows, 69)).toBe(5);
    for (const row of [-1, 0, 0.25, 1.5, 2.75, 3, 4]) expect(pitchToRow(rows, rowToPitch(rows, row))).toBeCloseTo(row, 9);
    expect(pitchToRow(undefined, 61.5)).toBe(61.5);
  });

  it('places rows top to bottom and finds the row under the pointer', () => {
    expect(pitchToY(view, 67)).toBe(0);
    expect(pitchToY(view, 60)).toBe(30);
    expect(pitchAtY(view, 15)).toBe(64);
    expect(pitchAtY(view, 1000)).toBe(60); // below the last row: the lowest
    expect(yToPitch(view, 20)).toBeCloseTo(63, 9); // half way between 62 and 64
  });

  it('moves notes by rows', () => {
    expect(stepRows(view, 62, 2)).toBe(67);
    expect(stepRows(view, 62, -5)).toBe(60);
    expect(stepRows({ ...view, rows: undefined }, 62, 2)).toBe(64);
  });

  it('lists the rows in sight', () => {
    expect(visibleRowPitches(view, 20)).toEqual([60, 62, 64, 67]); // a row of margin below, as unfolded
    expect(visibleRowPitches({ ...DEFAULT_VIEW, topPitch: 64.5, keyHeight: 10 }, 20)).toEqual([61, 62, 63, 64, 65]);
  });

  it('keeps the pitch at the top edge when folding and unfolding', () => {
    const unfolded: View = { ...DEFAULT_VIEW, topPitch: 64 };
    const folded = withRows(unfolded, rows);
    expect(folded.topPitch).toBe(2);
    expect(withRows(folded, undefined).topPitch).toBe(64);
    expect(withRows(folded, rows)).toBe(folded);
  });
});
