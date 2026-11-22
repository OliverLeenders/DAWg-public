import { NODE_TYPES } from '../bridge/protocol';

// The sweeps the Chorus, Flanger and Phaser panels draw (docs/design/modulation-effects.md, X3, X5, X7, X8, X10), with
// the engine's formulas (EffectNodes.cpp, dawg/ModulationEffects.h).

export type ModulationType = typeof NODE_TYPES.chorus | typeof NODE_TYPES.flanger | typeof NODE_TYPES.phaser;

export const SAMPLE_RATE = 48000;
/** The shortest delay a tap reads, in ms: two samples. */
export const MIN_DELAY_MS = (2 / SAMPLE_RATE) * 1000;
/** The Flanger's through-zero reference, in ms (X8). */
export const REFERENCE_MS = 5;
const PHASER_OCTAVES = 4;
const PHASER_MIN_HZ = 20;
const PHASER_MAX_RATIO = 0.45;

/** The LFO's value at a phase: 0 Sine, 1 Triangle; -1..1, 0 at phase 0 and rising. */
export function lfoValue(shape: number, phase: number): number {
  const fraction = phase - Math.floor(phase);
  if (Math.round(shape) === 1) {
    const shifted = fraction + 0.25 - Math.floor(fraction + 0.25);
    return 1 - 4 * Math.abs(shifted - 0.5);
  }
  return Math.sin(2 * Math.PI * fraction);
}

/** The Chorus's and Flanger's tap delay in ms for an LFO value (X5, X7), or through zero around the reference (X8). */
export function tapDelayMs(delayMs: number, depthPercent: number, m: number, throughZero = false): number {
  const depth = depthPercent / 100;
  if (throughZero) return REFERENCE_MS + Math.min(delayMs * depth, REFERENCE_MS - MIN_DELAY_MS) * m;
  return Math.max(MIN_DELAY_MS, delayMs * (1 + depth * m));
}

/** The Phaser's allpass frequency for an LFO value (X10), clamped as the engine clamps it. */
export function phaserFrequency(centre: number, depthPercent: number, m: number, sampleRate = SAMPLE_RATE): number {
  const f = centre * Math.pow(2, PHASER_OCTAVES * (depthPercent / 100) * m);
  return Math.min(PHASER_MAX_RATIO * sampleRate, Math.max(PHASER_MIN_HZ, f));
}

/** How many taps (voices) a channel has. */
export function voiceCount(type: string, params: Record<string, number>): number {
  return type === NODE_TYPES.chorus ? Math.min(4, Math.max(1, Math.round(params.voices ?? 2))) : 1;
}

/** The phase offsets of a channel's taps from the left phase, in cycles: voice k at k / V, and the right channel
    Spread / 360 further (over V for the Chorus, so 180 degrees puts its voices halfway between the left ones). */
export function tapOffsets(type: string, params: Record<string, number>, channel: 0 | 1): number[] {
  const voices = voiceCount(type, params);
  const spread = channel === 0 ? 0 : (params.spread ?? 90) / 360 / (type === NODE_TYPES.chorus ? voices : 1);
  return Array.from({ length: voices }, (_, k) => spread + k / voices);
}

/** What a tap sweeps at an LFO value: ms for the Chorus and the Flanger, Hz for the Phaser. */
export function sweepValue(type: string, params: Record<string, number>, m: number): number {
  if (type === NODE_TYPES.phaser) return phaserFrequency(params.centre ?? 700, params.depth ?? 60, m);
  const defaults = type === NODE_TYPES.chorus ? { delay: 15, depth: 15 } : { delay: 2, depth: 70 };
  return tapDelayMs(params.delay ?? defaults.delay, params.depth ?? defaults.depth, m, (params.throughZero ?? 0) >= 0.5);
}

export interface SweepCurve {
  channel: 0 | 1;
  /** The swept value at `points` evenly spaced phases over one cycle, from the left phase 0. */
  values: number[];
}

/** Every tap's sweep over one cycle, left channel first. */
export function sweepCurves(type: string, params: Record<string, number>, points = 128): SweepCurve[] {
  const shape = params.shape ?? (type === NODE_TYPES.flanger ? 1 : 0);
  const curves: SweepCurve[] = [];
  for (const channel of [0, 1] as const)
    for (const offset of tapOffsets(type, params, channel))
      curves.push({
        channel,
        values: Array.from({ length: points + 1 }, (_, i) => sweepValue(type, params, lfoValue(shape, i / points + offset))),
      });
  return curves;
}

/** The range a sweep graph shows: [low, high] in ms (from 0) or in Hz (an octave around the sweep). */
export function sweepRange(type: string, curves: SweepCurve[]): [number, number] {
  const all = curves.flatMap((c) => c.values);
  const low = Math.min(...all);
  const high = Math.max(...all);
  if (type === NODE_TYPES.phaser) return [Math.max(PHASER_MIN_HZ, low / 1.5), Math.min(SAMPLE_RATE / 2, high * 1.5)];
  return [0, Math.max(high * 1.1, type === NODE_TYPES.flanger ? 1 : 5)];
}
