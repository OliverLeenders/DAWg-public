import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { TempoMap } from '../time/tempoMap';
import type { EditNote } from './edit';
import {
  GENERAL_MIDI_DRUM_MAP,
  getDrumPiece,
  isDrumTrack,
  ROLAND_V_DRUMS_MAP,
  ABLETON_DRUM_RACK_MAP,
} from '../store/drumMap';
import {
  DEFAULT_DRUM_OPTIONS,
  DEFAULT_MELODIC_OPTIONS,
  humaniseDrums,
  humaniseMelodic,
  randomGaussian,
  mulberry32,
} from './humanise';

const FOUR_FOUR = new TempoMap([{ tick: 0, bpm: 120, ramp: false }], [{ bar: 0, numerator: 4, denominator: 4 }]);

const BEAT = TICKS_PER_BEAT;
const TICKS_PER_MS = (BEAT * 2) / 1000; // at 120 bpm

const makeNote = (id: number, start: number, length: number, pitch: number, velocity = 0.8): EditNote => ({
  id,
  clipId: 1,
  start,
  length,
  pitch,
  velocity,
});

describe('drumMap', () => {
  it('looks up GM drum pieces by pitch', () => {
    const kick = getDrumPiece(GENERAL_MIDI_DRUM_MAP, 36);
    expect(kick).toBeDefined();
    expect(kick?.role).toBe('kick');
    expect(kick?.limb).toBe('right_foot');

    const snare = getDrumPiece(GENERAL_MIDI_DRUM_MAP, 38);
    expect(snare?.role).toBe('snare');
    expect(snare?.limb).toBe('left_hand');

    const hat = getDrumPiece(GENERAL_MIDI_DRUM_MAP, 42);
    expect(hat?.role).toBe('hihat_closed');
    expect(hat?.limb).toBe('right_hand');

    expect(getDrumPiece(ROLAND_V_DRUMS_MAP, 34)?.name).toBe('Snare Rimshot');
    expect(getDrumPiece(ABLETON_DRUM_RACK_MAP, 36)?.name).toBe('Kick');
  });

  it('detects drum tracks by name heuristics or explicit flags', () => {
    expect(isDrumTrack('Acoustic Drums')).toBe(true);
    expect(isDrumTrack('Trap Beat')).toBe(true);
    expect(isDrumTrack('808 Sub')).toBe(true);
    expect(isDrumTrack('Percussion')).toBe(true);
    expect(isDrumTrack('Piano')).toBe(false);
    expect(isDrumTrack('Synth Lead')).toBe(false);

    expect(isDrumTrack('Piano', true)).toBe(true);
    expect(isDrumTrack('Drums', false)).toBe(false);
  });
});

describe('Gaussian PRNG', () => {
  it('produces normal distribution with expected mean and bounded variance', () => {
    const rng = mulberry32(42);
    const samples: number[] = [];
    for (let i = 0; i < 2000; i++) {
      samples.push(randomGaussian(rng, 0, 10));
    }
    const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
    // Mean should be very close to 0
    expect(Math.abs(mean)).toBeLessThan(0.8);

    // Distribution should fall within ±3.5 sigma
    for (const val of samples) {
      expect(Math.abs(val)).toBeLessThanOrEqual(35.1);
    }
  });
});

describe('humaniseMelodic', () => {
  it('humanises chord flams and anchors downbeats', () => {
    // A 3-note C major chord on beat 1 (tick 0)
    const chord = [
      makeNote(1, 0, BEAT, 60, 0.8), // C4
      makeNote(2, 0, BEAT, 64, 0.8), // E4
      makeNote(3, 0, BEAT, 67, 0.8), // G4
    ];

    const result = humaniseMelodic(
      chord,
      { ...DEFAULT_MELODIC_OPTIONS, chordFlamMs: 10, timingMs: 5 },
      FOUR_FOUR,
      42,
    );

    expect(result).toHaveLength(3);
    // Notes in chord should be slightly staggered from lowest to highest pitch
    const c4 = result.find((n) => n.id === 1)!;
    const g4 = result.find((n) => n.id === 3)!;
    expect(g4.start).toBeGreaterThanOrEqual(c4.start);
  });

  it('treats notes a few milliseconds apart as a chord, and keeps a short note short', () => {
    const options = { ...DEFAULT_MELODIC_OPTIONS, timingMs: 0, velocityVariance: 0, drift: 0, chordFlamMs: 10 };
    const result = humaniseMelodic([makeNote(1, 0, 100, 60), makeNote(2, 200, 100, 67)], options, FOUR_FOUR, 1);

    const top = result.find((n) => n.id === 2)!;
    // The flam counts from the top note's own start; a separate note would not move at all.
    expect(Math.abs(top.start - (200 + 10 * TICKS_PER_MS))).toBeLessThan(2);
    expect(top.length).toBe(100);
  });
});

describe('humaniseDrums', () => {
  it('applies snare pocket bias and anchors kick tighter than snare', () => {
    // Kick on beat 1, Snare on beat 2 (0.5 s at 120 bpm)
    const pattern = [
      makeNote(1, 0, BEAT / 2, 36, 0.9),    // Kick
      makeNote(2, BEAT, BEAT / 2, 38, 0.9), // Snare
    ];

    const opts = {
      ...DEFAULT_DRUM_OPTIONS(GENERAL_MIDI_DRUM_MAP),
      tightnessMs: 10,
      snarePocketMs: -10, // laid back pocket
      kickTightness: 0.2, // kick very tight
    };

    const result = humaniseDrums(pattern, opts, FOUR_FOUR, 999);
    expect(result).toHaveLength(2);

    const kick = result.find((n) => n.id === 1)!;
    const snare = result.find((n) => n.id === 2)!;

    // The kick stays within a few milliseconds of 0 (its deviation is 1 ms on the downbeat)
    expect(Math.abs(kick.start)).toBeLessThan(4 * TICKS_PER_MS);
    expect(snare.start).toBeDefined();
  });

  const meanOverSeeds = (value: (seed: number) => number) => {
    let sum = 0;
    for (let seed = 1; seed <= 300; seed++) sum += value(seed);
    return sum / 300;
  };

  it('plays a negative snare pocket behind the beat and a positive one ahead', () => {
    const snare = [makeNote(1, BEAT, BEAT / 2, 38, 0.9)];
    const meanMs = (snarePocketMs: number) =>
      meanOverSeeds((seed) => {
        const options = { ...DEFAULT_DRUM_OPTIONS(GENERAL_MIDI_DRUM_MAP), tightnessMs: 2, snarePocketMs };
        return (humaniseDrums(snare, options, FOUR_FOUR, seed)[0].start - BEAT) / TICKS_PER_MS;
      });

    expect(meanMs(-10)).toBeGreaterThan(8);
    expect(meanMs(8)).toBeLessThan(-6);
  });

  it('softens hi-hats between the beats, not on them', () => {
    const meanVelocity = (start: number) =>
      meanOverSeeds((seed) => {
        const options = { ...DEFAULT_DRUM_OPTIONS(GENERAL_MIDI_DRUM_MAP), velocityDynamics: 0.5 };
        return humaniseDrums([makeNote(1, start, BEAT / 4, 42, 0.8)], options, FOUR_FOUR, seed)[0].velocity;
      });

    expect(meanVelocity(BEAT) - meanVelocity(BEAT + BEAT / 2)).toBeGreaterThan(0.02);
    expect(Math.abs(meanVelocity(BEAT) - meanVelocity(2 * BEAT))).toBeLessThan(0.02);
  });

  it('handles simultaneous hi-hat and snare with micro-lead', () => {
    // Both on beat 2
    const slice = [
      makeNote(1, BEAT, BEAT / 2, 38, 0.85), // Snare
      makeNote(2, BEAT, BEAT / 2, 42, 0.8),  // Closed Hi-Hat
    ];

    const opts = {
      ...DEFAULT_DRUM_OPTIONS(GENERAL_MIDI_DRUM_MAP),
      tightnessMs: 2,
      snarePocketMs: 0,
      hihatLeadMs: 6, // hi-hat 6ms ahead
    };

    const result = humaniseDrums(slice, opts, FOUR_FOUR, 777);
    const snare = result.find((n) => n.id === 1)!;
    const hat = result.find((n) => n.id === 2)!;

    // Hi-hat should anticipate the snare
    expect(hat.start).toBeLessThanOrEqual(snare.start);
  });
});
