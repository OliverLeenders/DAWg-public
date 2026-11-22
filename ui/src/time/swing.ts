// Swing and quantise (docs/design/midi-effects.md, ME9, ME18). The engine's dawg/Swing.h does the same for the
// arpeggiator and note repeat; both are tested against the same table.

import type { TempoMap } from './tempoMap';

/** Straight: the second cell of each pair starts halfway through it. */
export const SWING_STRAIGHT = 0.5;
/** The most swing allowed: the second line on the pair's last quarter. */
export const SWING_MAX = 0.75;

/** Clamps a swing amount to 0.5..0.75. */
export const clampSwing = (swing: number) => Math.min(SWING_MAX, Math.max(SWING_STRAIGHT, swing));

/**
 * The position of grid line `index` within a stretch that starts on a line (a bar start): in each pair of cells the
 * second line moves from 50 % of the pair to `swing` of it.
 */
export function swungLine(index: number, step: number, swing: number) {
  const pair = Math.floor(index / 2);
  return index % 2 === 0 ? index * step : (2 * pair + 2 * clampSwing(swing)) * step;
}

/** Whether swing means anything on a grid: not for triplet grids, whose pairs don't make a beat, nor with no grid. */
export function swingApplies(gridBeats: number) {
  if (gridBeats <= 0) return false;
  const inverse = 1 / gridBeats;
  return Math.abs(Math.log2(inverse) - Math.round(Math.log2(inverse))) < 1e-9;
}

/**
 * Snaps a tick to the nearest swung grid line, the grid restarting at every bar line as TempoMap.snap's does. The
 * next bar's start counts as a line.
 */
export function swungSnap(map: TempoMap, tick: number, gridTicks: number, swing: number) {
  if (gridTicks <= 0) return Math.round(tick);
  const bar = map.barAt(tick).bar;
  const start = map.tickOfBar(bar);
  const next = map.tickOfBar(bar + 1);
  const step = Math.min(gridTicks, next - start);
  const cells = Math.ceil((next - start) / step - 1e-9);
  const k = Math.floor((tick - start) / step);
  let best = next;
  for (const index of [k - 1, k, k + 1, k + 2]) {
    if (index < 0 || index >= cells) continue;
    const line = start + Math.round(swungLine(index, step, swing));
    if (Math.abs(line - tick) < Math.abs(best - tick) || (Math.abs(line - tick) === Math.abs(best - tick) && line < best))
      best = line;
  }
  return best;
}

/** What Q does: moves a tick `strength` (0..1) of the way to its swung grid line. */
export function quantiseTick(map: TempoMap, tick: number, gridTicks: number, swing: number, strength: number) {
  const target = swungSnap(map, tick, gridTicks, swing);
  return Math.round(tick + (target - tick) * Math.min(1, Math.max(0, strength)));
}

/** The note fields quantise changes. */
export interface Quantisable {
  start: number;
  length: number;
}

/** Quantises a note's start, and with `lengths` its end, keeping it at least one grid step long when its end moves. */
export function quantiseNote<T extends Quantisable>(
  note: T,
  map: TempoMap,
  gridTicks: number,
  swing: number,
  strength: number,
  lengths: boolean,
): T {
  const start = Math.max(0, quantiseTick(map, note.start, gridTicks, swing, strength));
  if (!lengths) return { ...note, start };
  const end = quantiseTick(map, note.start + note.length, gridTicks, swing, strength);
  const minimum = Math.max(1, Math.round(gridTicks * Math.min(1, Math.max(0, strength))));
  return { ...note, start, length: Math.max(minimum, end - start) };
}
