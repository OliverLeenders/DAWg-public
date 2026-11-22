import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT, type Clip, type Note } from '../bridge/protocol';
import { audibleNotes, loopBoundaries } from './clips';

const BEAT = TICKS_PER_BEAT;

const note = (id: number, start: number, length = BEAT): Note => ({
  id,
  start,
  length,
  pitch: 60,
  velocity: 0.8,
  releaseVelocity: 0.5,
  expression: { pitch: [], pressure: [], timbre: [], vibrato: [] },
  vibrato: { depth: 1, rateHz: 5.5, syncBeats: 0, shape: 0, delay: 0, fadeIn: 0 },
});

const clip = (fields: Partial<Clip>, notes: Note[]): Clip => ({
  id: 1,
  start: 0,
  length: 4 * BEAT,
  offset: 0,
  loopStart: 0,
  loopLength: 0,
  name: '',
  colour: '',
  linkId: 0,
  fileId: 0,
  gainDb: 0,
  fadeIn: 0,
  fadeOut: 0,
  fadeInShape: 0,
  fadeOutShape: 0,
  transpose: 0,
  lane: 0,
  order: 0,
  sequencerPattern: -1,
  sequencerStartStep: 0,
  controllers: {},
  notes: Object.fromEntries(notes.map((n) => [String(n.id), n])),
  ...fields,
});

describe('audibleNotes', () => {
  it('applies the content offset (same rules as the engine)', () => {
    const played = audibleNotes(clip({ start: 4 * BEAT, offset: BEAT, length: 2 * BEAT }, [note(1, 0), note(2, BEAT), note(3, 2 * BEAT), note(4, 3 * BEAT)]));
    expect(played.map((p) => [p.note.id, p.start])).toEqual([
      [2, 4 * BEAT],
      [3, 5 * BEAT],
    ]);
  });

  it('repeats the loop range and cuts notes at each repeat', () => {
    const played = audibleNotes(clip({ length: 3 * BEAT, loopLength: BEAT }, [note(1, 0, 2 * BEAT)]));
    expect(played.map((p) => [p.start, p.end, p.repetition])).toEqual([
      [0, BEAT, 0],
      [BEAT, 2 * BEAT, 1],
      [2 * BEAT, 3 * BEAT, 2],
    ]);
    expect(loopBoundaries(clip({ length: 3 * BEAT, loopLength: BEAT }, []))).toEqual([BEAT, 2 * BEAT]);
  });
});
