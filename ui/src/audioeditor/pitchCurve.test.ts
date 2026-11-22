import { describe, expect, it } from 'vitest';
import type { PitchNote } from '../bridge/protocol';
import { pitchCorrection } from './pitchCurve';

const note = (id: number, start: number, end: number, detectedCentre: number, targetCentre: number): PitchNote => ({
  id,
  leadStart: start,
  start,
  end,
  detectedCentre,
  targetCentre,
  detectedShape: [],
  shape: [],
  pivots: [],
  vibrato: 1,
  transitionMs: 20,
});

describe('pitch correction twin', () => {
  // The same table as "pitch correction table shared with the UI's twin" in tests/PitchNotesTests.cpp.
  it('matches the engine on its table', () => {
    const notes = [note(1, 0, 48000, 60, 61), note(2, 48000, 96000, 62, 62)];
    const table: Array<[number, number]> = [
      [0, 0],
      [240, 0.5],
      [24000, 1],
      [47520, 1],
      [48000, 0.5],
      [48240, 0.1464466],
      [60000, 0],
    ];
    for (const [frame, expected] of table) expect(pitchCorrection(notes, 48000, frame, 1, 1)[0]).toBeCloseTo(expected, 6);
  });

  it('removes vibrato when its amount is 0', () => {
    const track = { start: 0.005, hop: 0.01, pitch: Array.from({ length: 200 }, (_, i) => 60 + 0.3 * Math.sin(2 * Math.PI * 6 * (0.005 + i * 0.01))) };
    const vibratoless = { ...note(1, 0, 96000, 60, 60), vibrato: 0 };
    let worst = 0;
    for (let t = 0.4; t < 1.6; t += 0.01) {
      const sung = 60 + 0.3 * Math.sin(2 * Math.PI * 6 * t);
      worst = Math.max(worst, Math.abs(sung + pitchCorrection([vibratoless], 48000, Math.round(t * 48000), 1, 1, track)[0] - 60));
    }
    expect(worst).toBeLessThan(0.1);
  });

  it('corrects by the change of the shape, and a recentred note sounds the same', () => {
    const shape: PitchNote['shape'] = [
      [0, -0.8, 4, 0],
      [9600, 0.2, 4, 0],
      [38400, -0.2, 4, 0],
      [48000, -0.6, 4, 0],
    ];
    const sung = { ...note(1, 0, 48000, 60, 60), detectedShape: shape, shape };
    const level = shape.map(([t, v, ...rest], i): PitchNote['shape'][number] => [t, i === 1 ? 0 : v, ...rest]);
    expect(pitchCorrection([{ ...sung, shape: level }], 48000, 9600, 1, 1)[0]).toBeCloseTo(-0.2, 6);
    expect(pitchCorrection([sung], 48000, 9600, 1, 1)[0]).toBeCloseTo(0, 6);
    // The engine's recentring moves the centre up and the shape down by as much: the same correction.
    const recentred = { ...sung, targetCentre: 60.5, shape: shape.map(([t, v, ...rest]): PitchNote['shape'][number] => [t, v - 0.5, ...rest]) };
    expect(pitchCorrection([recentred], 48000, 9600, 1, 1)[0]).toBeCloseTo(0, 6);
  });
});
