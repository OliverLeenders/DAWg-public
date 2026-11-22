import type { KeyChange, ProjectKey } from '../bridge/protocol';
import type { TempoMap } from '../time/tempoMap';
import { SCALES, inScale } from '../music/scales';
import type { RollFold } from '../store/uiStore';
import { MAX_PITCH, MIN_PITCH } from './geometry';

// Folding the piano roll and the project's scale (docs/design/scale-highlighting.md, SH3 to SH5).

/** A key's root and scale mask, or null when the project has no key. */
export interface ScaleShown {
  root: number;
  mask: number;
}

export function scaleOf(key: ProjectKey | undefined): ScaleShown | null {
  if (!key?.enabled) return null;
  return { root: key.root, mask: (SCALES[key.scale] ?? SCALES[0]).mask };
}

/** The key change in effect on a bar (SH9): the last one starting on or before it. */
export function keyChangeAt(keys: readonly KeyChange[] | undefined, bar: number): KeyChange | undefined {
  let found: KeyChange | undefined;
  for (const change of keys ?? []) {
    if (change.bar > bar) break;
    found = change;
  }
  return found;
}

/** A stretch of the timeline in one key, in ticks; the last one ends at Infinity. */
export interface KeySpan {
  start: number;
  end: number;
  scale: ScaleShown | null;
}

/** The project's keys as spans of ticks, by the tempo map. */
export function keySpans(keys: readonly KeyChange[] | undefined, map: TempoMap): KeySpan[] {
  const list = keys && keys.length > 0 ? keys : [{ bar: 0, enabled: false, root: 0, scale: 0 }];
  return list.map((change, i) => ({
    start: i === 0 ? -Infinity : map.tickOfBar(change.bar),
    end: i + 1 < list.length ? map.tickOfBar(list[i + 1].bar) : Infinity,
    scale: scaleOf(change),
  }));
}

/** The scale at a tick, or null where no key is set. */
export function scaleAt(spans: readonly KeySpan[], tick: number): ScaleShown | null {
  for (const span of spans) if (tick >= span.start && tick < span.end) return span.scale;
  return null;
}

/** Every scale the spans use, without repeats, for folding to all of them. */
export function scalesOf(spans: readonly KeySpan[]): ScaleShown[] {
  const seen = new Map<string, ScaleShown>();
  for (const span of spans) if (span.scale) seen.set(`${span.scale.root}:${span.scale.mask}`, span.scale);
  return [...seen.values()];
}

/**
 * The rows a folded roll shows, ascending, or undefined for every row. Scale: the scales' keys, plus any pitch a note
 * uses outside them, so no note disappears. Used: the pitches the notes use. Scale folding needs a key and Used folding
 * a note; without one the roll stays unfolded.
 */
export function foldRows(fold: RollFold, pitches: Iterable<number>, scales: readonly ScaleShown[]): number[] | undefined {
  if (fold === 'off' || (fold === 'scale' && scales.length === 0)) return undefined;
  const shown = new Set<number>();
  for (const pitch of pitches) shown.add(Math.min(MAX_PITCH, Math.max(MIN_PITCH, Math.round(pitch))));
  // With key changes, the rows of every key the song uses (SH9), so the rows stay put while scrolling.
  if (fold === 'scale') {
    for (let pitch = MIN_PITCH; pitch <= MAX_PITCH; pitch++)
      if (scales.some((scale) => inScale(pitch, scale.root, scale.mask))) shown.add(pitch);
  }
  if (shown.size === 0) return undefined;
  return [...shown].sort((a, b) => a - b);
}

/** Whether a note's pitch lies outside the scale. */
export const outsideScale = (pitch: number, scale: ScaleShown | null) =>
  scale !== null && !inScale(Math.round(pitch), scale.root, scale.mask);
