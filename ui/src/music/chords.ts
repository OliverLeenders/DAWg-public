// The Chord's shapes (docs/design/midi-effects.md, ME11), a copy of chordShapes in engine/include/dawg/MidiEffects.h,
// for the Chord panel's keyboard.

/** Semitones above the played note, the played note (0) first, by the Shape choice; Custom comes after them. */
export const CHORD_SHAPES: number[][] = [
  [0, 4, 7],
  [0, 3, 7],
  [0, 7],
  [0, 2, 7],
  [0, 5, 7],
  [0, 4, 7, 9],
  [0, 3, 7, 9],
  [0, 4, 7, 10],
  [0, 4, 7, 11],
  [0, 3, 7, 10],
  [0, 3, 6, 10],
  [0, 3, 6],
  [0, 3, 6, 9],
  [0, 4, 8],
  [0, 4, 7, 14],
  [0, 12],
];

/** The Shape choice's index of Custom. */
export const CUSTOM_SHAPE = CHORD_SHAPES.length;

/** The note parameters Custom uses, after the played note. */
export const CHORD_NOTE_PARAMS = ['note2', 'note3', 'note4', 'note5', 'note6'];

/** Moves the lowest notes up an octave (positive) or the highest down (negative), as the engine's invert(). */
export function invert(notes: number[], inversion: number): number[] {
  const result = [...notes];
  const count = notes.length;
  const steps = Math.max(-(count - 1), Math.min(count - 1, Math.round(inversion)));
  const order = notes.map((_, i) => i).sort((a, b) => notes[a] - notes[b] || a - b);
  for (let i = 0; i < Math.abs(steps); i++) {
    if (steps > 0) result[order[i]] += 12;
    else result[order[count - 1 - i]] -= 12;
  }
  return result;
}

/** The notes a Chord plays above the played note, from its parameters. */
export function chordNotes(params: (id: string) => number): number[] {
  const shape = Math.round(params('shape'));
  const notes =
    shape < CHORD_SHAPES.length
      ? CHORD_SHAPES[shape]
      : [0, ...CHORD_NOTE_PARAMS.map((id) => Math.round(params(id))).filter((n) => n !== 0)];
  return invert(notes, params('inversion'));
}

/**
 * The Custom note parameters after a click on a key `semitones` above the played note: the note comes in if it wasn't
 * there, or goes. The played note stays. At most five notes join it; a sixth replaces nothing and is ignored.
 */
export function toggledNotes(current: number[], semitones: number): number[] {
  if (semitones === 0) return current.filter((n) => n !== 0);
  const others = current.filter((n) => n !== 0);
  const next = others.includes(semitones) ? others.filter((n) => n !== semitones) : [...others, semitones];
  return next.length > CHORD_NOTE_PARAMS.length ? others : next.sort((a, b) => a - b);
}
