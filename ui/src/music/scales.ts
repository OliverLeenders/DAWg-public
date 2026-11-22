// The scale table Scale Lock uses (docs/design/midi-effects.md, ME12), a copy of engine/include/dawg/Scales.h, so the
// panel's keyboard and, later, the piano roll's highlighting agree with the engine. scales.test.ts checks the masks.

export interface Scale {
  name: string;
  /** Bit i is set when the pitch class i semitones above the root is in the scale. */
  mask: number;
}

const maskOf = (degrees: number[]) => degrees.reduce((mask, degree) => mask | (1 << degree), 0);

/** In the order of Scale Lock's Scale choice. */
export const SCALES: Scale[] = [
  { name: 'Major', mask: maskOf([0, 2, 4, 5, 7, 9, 11]) },
  { name: 'Natural minor', mask: maskOf([0, 2, 3, 5, 7, 8, 10]) },
  { name: 'Harmonic minor', mask: maskOf([0, 2, 3, 5, 7, 8, 11]) },
  { name: 'Melodic minor', mask: maskOf([0, 2, 3, 5, 7, 9, 11]) },
  { name: 'Dorian', mask: maskOf([0, 2, 3, 5, 7, 9, 10]) },
  { name: 'Phrygian', mask: maskOf([0, 1, 3, 5, 7, 8, 10]) },
  { name: 'Lydian', mask: maskOf([0, 2, 4, 6, 7, 9, 11]) },
  { name: 'Mixolydian', mask: maskOf([0, 2, 4, 5, 7, 9, 10]) },
  { name: 'Locrian', mask: maskOf([0, 1, 3, 5, 6, 8, 10]) },
  { name: 'Major pentatonic', mask: maskOf([0, 2, 4, 7, 9]) },
  { name: 'Minor pentatonic', mask: maskOf([0, 3, 5, 7, 10]) },
  { name: 'Blues', mask: maskOf([0, 3, 5, 6, 7, 10]) },
  { name: 'Whole tone', mask: maskOf([0, 2, 4, 6, 8, 10]) },
  { name: 'Diminished', mask: maskOf([0, 1, 3, 4, 6, 7, 9, 10]) },
  { name: 'Chromatic', mask: 0xfff },
];

/** The pitch class of a key above a root, 0..11. */
export const degreeOf = (key: number, root: number) => (((key - root) % 12) + 12) % 12;

/** Whether a key is in a scale. */
export const inScale = (key: number, root: number, mask: number) => ((mask >> degreeOf(key, root)) & 1) !== 0;

export type ScaleDirection = 'nearest' | 'up' | 'down';

/** Moves a key into a scale as the engine does: ties go down. */
export function lockToScale(key: number, root: number, mask: number, direction: ScaleDirection = 'nearest') {
  if (inScale(key, root, mask)) return key;
  for (let distance = 1; distance < 12; distance++) {
    if (direction !== 'up' && inScale(key - distance, root, mask)) return key - distance;
    if (direction !== 'down' && inScale(key + distance, root, mask)) return key + distance;
  }
  return key;
}
