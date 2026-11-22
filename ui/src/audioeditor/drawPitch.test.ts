import { describe, expect, it } from 'vitest';
import type { CurvePoint } from '../bridge/protocol';
import { notePitchAt, pitchLayout, pitchRange, tiltHandles, vibratoHandle, type ViewNote } from './drawPitch';

const note = (centre: number, points: CurvePoint[] = []): ViewNote => ({ id: 1, detectedCentre: centre, leadStart: 0, start: 1000, end: 5000, centre, points, pivots: [], edited: false, vibrato: 1 });

describe('pitch view', () => {
  it('shows the notes with a margin, at least an octave', () => {
    expect(pitchRange([note(60.2), note(62.7)], undefined)).toEqual([56, 68]);
    expect(pitchRange([note(50), note(70)], undefined)).toEqual([47, 73]);
  });

  it('falls back to the sung pitch, then to a default', () => {
    expect(pitchRange([], { ready: true, path: '', pitch: [null, 64, 65], notes: [] })).toEqual([59, 71]);
    expect(pitchRange([], undefined)).toEqual([54, 72]);
  });

  it('puts a note at its centre plus its shape', () => {
    const scoop = note(64, [
      [0, -1, 4, 0],
      [2000, 0, 4, 0],
      [4000, 0, 4, 0],
    ]);
    expect(notePitchAt(scoop, 1000)).toBeCloseTo(63);
    expect(notePitchAt(scoop, 3000)).toBeCloseTo(64);
    expect(notePitchAt(note(64), 2000)).toBe(64);
  });

  it('puts the vibrato handle below the middle of a note, the tilt handles above its ends', () => {
    const layout = pitchLayout({ width: 400, height: 300 }, [note(60)], undefined);
    const frameX = (frame: number) => frame / 10;
    const bar = vibratoHandle({ start: 1000, end: 5000, centre: 60 }, layout, frameX);
    const tilt = tiltHandles({ start: 1000, end: 5000, centre: 60 }, layout, frameX);
    expect(bar.x).toBe(300);
    expect(bar.y).toBeGreaterThan(layout.y(60) + layout.blobHeight / 2);
    expect(tilt.left.y).toBeLessThan(layout.y(60) - layout.blobHeight / 2);
  });
});
