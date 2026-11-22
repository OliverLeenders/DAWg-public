import { describe, expect, it } from 'vitest';
import { CUSTOM_SHAPE, chordNotes, invert, toggledNotes } from './chords';

describe('chords', () => {
  it('invert as the engine does', () => {
    // The same cases as tests/MidiEffectTests.cpp.
    expect(invert([0, 4, 7], 1)).toEqual([12, 4, 7]);
    expect(invert([0, 4, 7], 2)).toEqual([12, 16, 7]);
    expect(invert([0, 4, 7], -1)).toEqual([0, 4, -5]);
    expect(invert([0, 4, 7], 5)).toEqual([12, 16, 7]);
  });

  it('read a shape or the custom notes', () => {
    const params = (values: Record<string, number>) => (id: string) => values[id] ?? 0;
    expect(chordNotes(params({ shape: 1 }))).toEqual([0, 3, 7]);
    expect(chordNotes(params({ shape: CUSTOM_SHAPE, note2: 5, note3: 0, note4: -12 }))).toEqual([0, 5, -12]);
  });

  it('toggle keys, keeping the played note and at most six', () => {
    expect(toggledNotes([0, 4, 7], 4)).toEqual([7]);
    expect(toggledNotes([0, 4, 7], 10)).toEqual([4, 7, 10]);
    expect(toggledNotes([0, 1, 2, 3, 4, 5], 6)).toEqual([1, 2, 3, 4, 5]);
  });
});
