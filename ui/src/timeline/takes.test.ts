import { describe, expect, it } from 'vitest';
import type { Clip } from '../bridge/protocol';
import { playedParts } from './takes';

const clip = (id: number, start: number, length: number, order: number) =>
  ({ id, start, length, order, lane: 0, offset: 0, loopStart: 0, loopLength: 0, fadeIn: 0, fadeOut: 0 }) as Clip;
const spans = (clips: Clip[]) => playedParts(clips).map((c) => [c.id, c.start, c.start + c.length]);

describe('playedParts', () => {
  it('leaves clips that don\'t overlap as they are', () => {
    const clips = [clip(1, 0, 100, 5), clip(2, 100, 50, 1)];
    expect(playedParts(clips)).toEqual(clips);
  });

  it('plays the top clip where clips stack, as the engine does', () => {
    expect(spans([clip(1, 0, 100, 1), clip(2, 40, 20, 2)])).toEqual([[1, 0, 40], [2, 40, 60], [1, 60, 100]]);
    expect(spans([clip(1, 0, 100, 2), clip(2, 40, 20, 1)])).toEqual([[1, 0, 100]]);
    expect(spans([clip(1, 0, 100, 1), clip(2, 20, 100, 2), clip(3, 50, 20, 3)])).toEqual([
      [1, 0, 20],
      [2, 20, 50],
      [3, 50, 70],
      [2, 70, 120],
    ]);
  });

  it('moves a cut part\'s content with it', () => {
    const [left, top, right] = playedParts([{ ...clip(1, 0, 100, 1), offset: 10 }, clip(2, 40, 20, 2)]);
    expect(left.offset).toBe(10);
    expect(top.id).toBe(2);
    expect(right.offset).toBe(70);
  });
});
