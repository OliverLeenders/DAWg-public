import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT, type MidiFileContents } from '../bridge/protocol';
import { previewEvents } from '../components/midiPreview';
import { clipFromFile } from './importAudio';

const note = (start: number, pitch: number, length = 100) => ({ start, length, pitch, velocity: 0.5 });

const file: MidiFileContents = {
  name: 'song',
  tempo: [{ tick: 0, bpm: 120 }],
  signatures: [{ bar: 0, numerator: 4, denominator: 4 }],
  tracks: [
    { name: 'Keys', drums: false, length: 400, notes: [note(0, 60)], controllers: { '1': [[0, 0.5, 0]] } },
    { name: 'Bass', drums: false, length: 800, notes: [note(10, 36)], controllers: { '64': [[0, 1, 1]] } },
  ],
};

describe('dropping a MIDI file on a track', () => {
  it('merges its parts into one clip at the drop', () => {
    const command = clipFromFile(file, 7, 960);
    expect(command.command).toBe('addClip');
    if (command.command !== 'addClip') throw new Error('expected addClip');
    expect(command.args).toMatchObject({ trackId: 7, start: 960, length: 800, name: 'song' });
    expect(command.args.notes).toHaveLength(2);
    expect(Object.keys(command.args.controllers!)).toEqual(['1', '64']);
  });
});

describe('previewing a MIDI file', () => {
  it('plays every note at the tempo, a note off before a note on where they meet', () => {
    const quarter = { ...file, tracks: [{ ...file.tracks[0], notes: [note(0, 60, TICKS_PER_BEAT), note(TICKS_PER_BEAT, 60, TICKS_PER_BEAT)] }] };
    const events = previewEvents(quarter, 120);
    expect(events.map((e) => [Math.round(e.ms), e.on])).toEqual([
      [0, true],
      [500, false],
      [500, true],
      [1000, false],
    ]);
    expect(previewEvents(quarter, 60)[1].ms).toBeCloseTo(1000);
  });
});
