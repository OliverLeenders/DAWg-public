// Humanising notes (docs/design/humanise-and-presets.md): Gaussian timing and velocity errors, shaped per drum piece
// or by the position in the bar, slow tempo drift across bars (a first-order autoregressive process), chord flams and
// a snare pocket.

import { TICKS_PER_BEAT } from '../bridge/protocol';
import { beatTicks, type TempoMap } from '../time/tempoMap';
import type { EditNote } from './edit';
import { getDrumPiece, type DrumMap } from '../store/drumMap';

/** Notes starting this close count as struck together: a chord, or limbs hitting at once. */
const CHORD_WINDOW_TICKS = TICKS_PER_BEAT / 40;
const DRUM_SLICE_TICKS = TICKS_PER_BEAT / 32;
/** The shortest length the length variation may leave (a shorter note keeps at least its own). */
const MIN_LENGTH_TICKS = TICKS_PER_BEAT / 30;

/** Fast 32-bit PRNG (Mulberry32) for reproducible, flicker-free previewing. */
export function mulberry32(seed: number) {
  let s = seed | 0;
  return function () {
    let t = (s += 0x6d2b79f5);
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/**
 * Standard Normal (Gaussian) distribution via Box-Muller transform.
 * Clamped to ±3.5 standard deviations to prevent musical glitches.
 */
export function randomGaussian(rng: () => number, mean = 0, stdev = 1): number {
  if (stdev <= 0) return mean;
  let u = 0;
  let v = 0;
  while (u === 0) u = rng();
  while (v === 0) v = rng();
  const z = Math.sqrt(-2.0 * Math.log(u)) * Math.cos(2.0 * Math.PI * v);
  const clamped = Math.max(-3.5, Math.min(3.5, z));
  return clamped * stdev + mean;
}

export const clampVelocity = (v: number) => Math.max(0.01, Math.min(1.0, v));

export interface MelodicHumaniseOptions {
  /** Timing deviation standard deviation in milliseconds (0 to 30 ms). */
  timingMs: number;
  /** Velocity variation strength (0.0 to 1.0, maps up to ±25% dynamic variance). */
  velocityVariance: number;
  /** Tempo drift strength across bars, 0.0 to 1.0. */
  drift: number;
  /** Spread in milliseconds for simultaneous polyphonic chord notes (0 to 25 ms). */
  chordFlamMs: number;
  /** Tighten downbeats (bar start and primary beats) to anchor timing. */
  downbeatAnchor: boolean;
}

export interface DrumHumaniseOptions {
  /** Drum map mapping pitches to drum roles and limbs. */
  drumMap: DrumMap;
  /** Base timing variance in milliseconds (0 to 30 ms). */
  tightnessMs: number;
  /** Multiplier on kick timing variance (0.1 = very tight anchor, 1.0 = loose). */
  kickTightness: number;
  /** Snare pocket in milliseconds: negative plays behind the beat (laid back), positive ahead of it (pushed). */
  snarePocketMs: number;
  /** Micro-stagger: how many ms the hi-hat hits before the snare on simultaneous strikes (0 to 10 ms). */
  hihatLeadMs: number;
  /** Dynamic velocity variance (0.0 to 1.0). */
  velocityDynamics: number;
  /** Alternating sticking accents (downstroke/tap) on 16th/8th hat subdivisions. */
  hihatAlternation: boolean;
  /** Ghost notes (velocity <= 0.35) receive 2x looser timing. */
  ghostLooseTiming: boolean;
}

export const DEFAULT_MELODIC_OPTIONS: MelodicHumaniseOptions = {
  timingMs: 8,
  velocityVariance: 0.35,
  drift: 0.3,
  chordFlamMs: 6,
  downbeatAnchor: true,
};

export const DEFAULT_DRUM_OPTIONS = (drumMap: DrumMap): DrumHumaniseOptions => ({
  drumMap,
  tightnessMs: 9,
  kickTightness: 0.3,
  snarePocketMs: -4, // classic subtle laid-back pocket
  hihatLeadMs: 4,    // hi-hat strikes 4ms ahead of snare
  velocityDynamics: 0.4,
  hihatAlternation: true,
  ghostLooseTiming: true,
});

/**
 * Humanises melodic/harmonic instrument notes with Gaussian motor noise,
 * bar-level tempo drift, downbeat anchoring, and polyphonic chord flams.
 */
export function humaniseMelodic(
  notes: EditNote[],
  options: MelodicHumaniseOptions,
  map: TempoMap,
  seed = 12345,
): EditNote[] {
  if (notes.length === 0) return [];
  const rng = mulberry32(seed);

  // Group simultaneous notes into chord clusters for chord flams
  const sorted = [...notes].sort((a, b) => a.start - b.start || a.pitch - b.pitch);
  const chords: EditNote[][] = [];
  let currentChord: EditNote[] = [];

  for (const n of sorted) {
    if (currentChord.length === 0) {
      currentChord.push(n);
    } else if (Math.abs(n.start - currentChord[0].start) <= CHORD_WINDOW_TICKS) {
      currentChord.push(n);
    } else {
      chords.push(currentChord);
      currentChord = [n];
    }
  }
  if (currentChord.length > 0) chords.push(currentChord);

  // Slow drift: each bar keeps most of the previous bar's offset, plus a small random change.
  const barDriftSec = new Map<number, number>();
  let prevDrift = 0;
  const getBarDrift = (bar: number) => {
    if (barDriftSec.has(bar)) return barDriftSec.get(bar)!;
    // AR(1) process: 0.75 * previous + normal random impulse
    const impulse = randomGaussian(rng, 0, (options.timingMs * 0.001 * options.drift * 0.6));
    const current = prevDrift * 0.75 + impulse;
    prevDrift = current;
    barDriftSec.set(bar, current);
    return current;
  };

  const results: EditNote[] = [];

  for (const chord of chords) {
    // Chord flam: lowest pitch struck first, higher pitches slightly staggered
    const isChord = chord.length > 1;
    chord.sort((a, b) => a.pitch - b.pitch);

    chord.forEach((note, index) => {
      const pos = map.barAt(note.start);
      const isDownbeat = pos.beat === 0 && pos.tickInBeat === 0;
      const isBeat = pos.tickInBeat === 0;

      // Downbeat tightening
      let effectiveTimingMs = options.timingMs;
      if (options.downbeatAnchor) {
        if (isDownbeat) effectiveTimingMs *= 0.45;
        else if (isBeat) effectiveTimingMs *= 0.7;
      }

      // Chord flam offset (lowest note = 0, highest note = chordFlamMs)
      const flamOffsetSec = isChord && options.chordFlamMs > 0
        ? (index / (chord.length - 1)) * (options.chordFlamMs / 1000)
        : 0;

      // Gaussian timing jitter
      const jitterMs = randomGaussian(rng, 0, effectiveTimingMs);
      const driftSec = options.drift > 0 ? getBarDrift(pos.bar) : 0;
      const totalOffsetSec = (jitterMs / 1000) + driftSec + flamOffsetSec;

      const origSec = map.secondsAt(note.start);
      const newSec = Math.max(0, origSec + totalOffsetSec);
      const newStart = Math.max(0, Math.round(map.tickAt(newSec)));

      // Dynamic velocity variation
      const velVarianceFactor = options.velocityVariance * 0.22;
      const velDelta = randomGaussian(rng, 0, velVarianceFactor);
      // Harder strikes arrive slightly earlier (positive velocity / negative timing correlation)
      const timingCorrelation = - (totalOffsetSec / 0.02) * 0.03 * options.velocityVariance;
      // Downbeat slight accent
      const accent = (isDownbeat && options.downbeatAnchor) ? 0.05 * options.velocityVariance : 0;
      const newVelocity = clampVelocity(note.velocity + velDelta + timingCorrelation + accent);

      // Subtle note length variation (±3%)
      const lengthRatio = 1 + randomGaussian(rng, 0, 0.03 * options.timingMs / 10);
      const newLength = Math.max(
        Math.min(MIN_LENGTH_TICKS, note.length),
        Math.round(note.length * Math.max(0.7, lengthRatio)),
      );

      results.push({
        ...note,
        start: newStart,
        length: newLength,
        velocity: newVelocity,
      });
    });
  }

  return results;
}

/**
 * Drum humanisation: Gaussian distributions per drum role,
 * snare pocket bias (laid-back / rushed), hi-hat vs snare micro-lead,
 * kick anchor rigidity, and alternating sticking velocity dynamics.
 */
export function humaniseDrums(
  notes: EditNote[],
  options: DrumHumaniseOptions,
  map: TempoMap,
  seed = 54321,
): EditNote[] {
  if (notes.length === 0) return [];
  const rng = mulberry32(seed);

  // Group notes with coincident start times to detect limb interactions
  const sorted = [...notes].sort((a, b) => a.start - b.start);
  const timeSlices: EditNote[][] = [];
  let currentSlice: EditNote[] = [];

  for (const n of sorted) {
    if (currentSlice.length === 0) {
      currentSlice.push(n);
    } else if (Math.abs(n.start - currentSlice[0].start) <= DRUM_SLICE_TICKS) {
      currentSlice.push(n);
    } else {
      timeSlices.push(currentSlice);
      currentSlice = [n];
    }
  }
  if (currentSlice.length > 0) timeSlices.push(currentSlice);

  const results: EditNote[] = [];

  for (const slice of timeSlices) {
    // Check for simultaneous snare + hi-hat
    const hasSnare = slice.some((n) => {
      const p = getDrumPiece(options.drumMap, n.pitch);
      return p?.role === 'snare' || p?.role === 'sidestick' || p?.role === 'clap';
    });

    for (const note of slice) {
      const piece = getDrumPiece(options.drumMap, note.pitch);
      const role = piece?.role ?? 'percussion';
      const pos = map.barAt(note.start);
      const isDownbeat = pos.beat === 0 && pos.tickInBeat === 0;

      let effectiveStdevMs = options.tightnessMs;
      let meanOffsetMs = 0;

      // 1. Role-specific timing variance & pocket bias
      if (role === 'kick') {
        // Kick anchors the timing
        effectiveStdevMs *= Math.max(0.1, options.kickTightness);
        if (isDownbeat) effectiveStdevMs *= 0.5; // downbeat of bar is tightest
      } else if (role === 'snare' || role === 'sidestick' || role === 'clap') {
        // Snare defines the pocket: laid back (negative, so later) or pushed (positive, so earlier)
        const isGhost = note.velocity <= 0.35;
        if (isGhost && options.ghostLooseTiming) {
          effectiveStdevMs *= 2.0;
        } else {
          meanOffsetMs = -options.snarePocketMs;
          effectiveStdevMs *= 0.8;
        }
      } else if (role === 'hihat_closed' || role === 'hihat_open' || role === 'ride' || role === 'ride_bell') {
        // When hi-hat and snare strike together, hi-hat hits slightly before snare
        if (hasSnare && options.hihatLeadMs > 0) {
          meanOffsetMs -= options.hihatLeadMs;
        }
        effectiveStdevMs *= 1.05;
      } else if (role.startsWith('tom_')) {
        effectiveStdevMs *= 1.2;
      } else if (role === 'crash') {
        // Crashes on downbeats hit slightly ahead of the beat (1-3ms)
        if (isDownbeat) meanOffsetMs -= 2;
        effectiveStdevMs *= 0.9;
      }

      // Compute Gaussian timing offset
      const jitterMs = randomGaussian(rng, meanOffsetMs, effectiveStdevMs);
      const origSec = map.secondsAt(note.start);
      const newSec = Math.max(0, origSec + jitterMs / 1000);
      const newStart = Math.max(0, Math.round(map.tickAt(newSec)));

      // 2. Velocity humanisation with sticking dynamics
      let velDelta = randomGaussian(rng, 0, options.velocityDynamics * 0.18);

      // Hi-hat / ride sticking alternation (downstroke vs tap stroke on 16th notes)
      if (options.hihatAlternation && (role === 'hihat_closed' || role === 'ride')) {
        // An 8th or 16th subdivision that isn't on a main beat is a tap stroke
        const beatFraction = pos.tickInBeat / beatTicks(map.signatureAt(note.start));
        if (beatFraction > 0.15 && beatFraction < 0.85) {
          // Offbeat tap stroke is naturally softer
          velDelta -= 0.08 * options.velocityDynamics;
        }
      }

      // Ghost notes stay low-velocity, do not blow up
      if (note.velocity <= 0.35) {
        velDelta = Math.min(0.08, velDelta);
      }

      const newVelocity = clampVelocity(note.velocity + velDelta);

      results.push({
        ...note,
        start: newStart,
        velocity: newVelocity,
      });
    }
  }

  return results;
}
