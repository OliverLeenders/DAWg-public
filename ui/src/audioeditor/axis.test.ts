import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT, type AudioFile, type Clip } from '../bridge/protocol';
import { TempoMap } from '../time/tempoMap';
import { clipRangeOn, editorAxis, snapToOnset } from './axis';

const beat = TICKS_PER_BEAT;

const file = (musical: boolean, markers: AudioFile['timing']['markers'] = []): AudioFile => ({
  id: 1,
  path: 'a.wav',
  name: 'a',
  sampleRate: 48000,
  channels: 1,
  frames: 4 * 48000,
  timing: { mode: musical ? 'musical' : 'oneShot', musicalStyle: 'stretch', sourceBpm: 120, markers },
  suggestedBpm: 0,
  reverseOf: 0,
  pitch: { analyser: '', notes: [] },
});

const clip = (start: number, offset: number, length: number): Clip =>
  ({ id: 9, start, offset, length, loopStart: 0, loopLength: 0, fileId: 1 }) as unknown as Clip;

describe('editorAxis', () => {
  it('places a one-shot file in real time from where the clip plays it', () => {
    const map = new TempoMap([{ tick: 0, bpm: 60, ramp: false }]);
    // At 60 BPM a content tick lasts 1/15360 s; the clip starts at beat 4 with one beat of offset.
    const axis = editorAxis(clip(4 * beat, beat, 2 * beat), file(false), map);
    expect(axis.musical).toBe(false);
    expect(axis.frameToTick(0)).toBeCloseTo(3 * beat, 6);
    expect(axis.frameToTick(48000)).toBeCloseTo(4 * beat, 6);
    expect(axis.tickToFrame(5 * beat)).toBeCloseTo(96000, 6);
  });

  it('places a one-shot file through its markers, in seconds', () => {
    const map = new TempoMap([{ tick: 0, bpm: 60, ramp: false }]);
    // Content second 1 plays frame 24000: the first second holds half a second of audio, then it runs at its own speed.
    const axis = editorAxis(clip(4 * beat, beat, 2 * beat), file(false, [{ tick: 1, frame: 24000 }]), map);
    expect(axis.frameToTick(0)).toBeCloseTo(3 * beat, 6);
    expect(axis.frameToTick(24000)).toBeCloseTo(4 * beat, 6);
    expect(axis.tickToFrame(5 * beat)).toBeCloseTo(72000, 6);
    expect(axis.contentAt(4 * beat)).toBeCloseTo(1, 9);
    expect(axis.contentAt(axis.tickOfContent(1.234))).toBeCloseTo(1.234, 9);
  });

  it('places a musical file through its timing map', () => {
    const map = new TempoMap([{ tick: 0, bpm: 90, ramp: false }]);
    const axis = editorAxis(clip(8 * beat, 2 * beat, 2 * beat), file(true, [{ tick: beat, frame: 12000 }]), map);
    expect(axis.musical).toBe(true);
    // Content tick 0 sits at the clip's start minus its offset; the marker pins frame 12000 one beat later.
    expect(axis.frameToTick(0)).toBeCloseTo(6 * beat, 6);
    expect(axis.frameToTick(12000)).toBeCloseTo(7 * beat, 6);
    expect(axis.tickToFrame(8 * beat)).toBeCloseTo(12000 + 24000, 6);
    expect(axis.contentAt(axis.tickOfContent(1234))).toBeCloseTo(1234, 9);
  });

  it('shows other clips of the file where they play it', () => {
    const map = new TempoMap([{ tick: 0, bpm: 120, ramp: false }]);
    const axis = editorAxis(clip(0, 0, 2 * beat), file(true), map);
    expect(clipRangeOn(axis, clip(16 * beat, beat, beat), map)).toEqual([beat, 2 * beat]);
  });
});

describe('snapToOnset', () => {
  it('finds the nearest onset within reach', () => {
    const onsets = [
      [1000, 0.5],
      [1500, 1],
      [9000, 0.2],
    ] as const;
    expect(snapToOnset(1400, onsets, 200)).toBe(1500);
    expect(snapToOnset(1200, onsets, 250)).toBe(1000);
    expect(snapToOnset(5000, onsets, 200)).toBeNull();
  });
});
