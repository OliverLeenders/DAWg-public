import type { Clip, Track } from '../bridge/protocol';
import { clipEnd } from './clips';

// Takes lanes (docs/design/takes-lanes.md): where a track's clips stack, the one with the highest order plays. This
// is playedClips() in engine/src/Model.cpp, for drawing: the track's own row shows these parts, and its takes rows dim
// what they cover.

/** The part of a clip between two timeline positions inside it, as playedClips() cuts it (clipPart in the engine). */
export function clipPart(clip: Clip, from: number, to: number): Clip {
  let offset = clip.offset + (from - clip.start);
  if (clip.loopLength > 0) {
    const r = (offset - clip.loopStart) % clip.loopLength;
    offset = clip.loopStart + (r < 0 ? r + clip.loopLength : r);
  }
  return {
    ...clip,
    start: from,
    length: to - from,
    offset,
    fadeIn: from > clip.start ? 0 : clip.fadeIn,
    fadeOut: to < clipEnd(clip) ? 0 : clip.fadeOut,
  };
}

/** What a list of clips plays: the clips themselves where nothing overlaps, else the top clip's parts first. Sorted by
    start. */
export function playedParts(clips: readonly Clip[]): Clip[] {
  // The usual case: nothing overlaps, and every clip plays whole.
  const sorted = [...clips].sort((a, b) => a.start - b.start || a.id - b.id);
  let end = -Infinity;
  let overlapping = false;
  for (const clip of sorted) {
    if (clip.start < end) overlapping = true;
    end = Math.max(end, clipEnd(clip));
  }
  if (!overlapping) return sorted;

  const byOrder = [...clips].sort((a, b) => b.order - a.order || b.id - a.id);
  let taken: Array<[number, number]> = []; // disjoint, sorted
  const result: Clip[] = [];

  for (const clip of byOrder) {
    const [start, stop] = [clip.start, clipEnd(clip)];
    const piece = (from: number, to: number) => result.push(from === start && to === stop ? clip : clipPart(clip, from, to));
    let cursor = start;
    for (const [a, b] of taken) {
      if (b <= cursor) continue;
      if (a >= stop) break;
      if (a > cursor) piece(cursor, a);
      cursor = Math.max(cursor, b);
    }
    if (cursor < stop) piece(cursor, stop);

    // Merge the clip's range into what's taken.
    const merged: Array<[number, number]> = [];
    let range: [number, number] = [start, stop];
    for (const span of taken) {
      if (span[1] < range[0] || span[0] > range[1]) merged.push(span);
      else range = [Math.min(range[0], span[0]), Math.max(range[1], span[1])];
    }
    merged.push(range);
    taken = merged.sort((x, y) => x[0] - y[0]);
  }

  return result.sort((a, b) => a.start - b.start || a.id - b.id);
}

const playedCache = new WeakMap<Track, Clip[]>();

/** What a track plays, kept per track object (tracks are immutable snapshots). */
export function playedClipsOf(track: Track): Clip[] {
  let parts = playedCache.get(track);
  if (!parts) {
    parts = playedParts(Object.values(track.clips));
    playedCache.set(track, parts);
  }
  return parts;
}

/** The parts of a clip that something on top of it covers, as timeline spans. */
export function coveredSpans(track: Track, clip: Clip): Array<[number, number]> {
  const played = playedClipsOf(track)
    .filter((part) => part.id === clip.id)
    .map((part): [number, number] => [part.start, clipEnd(part)])
    .sort((a, b) => a[0] - b[0]);
  const spans: Array<[number, number]> = [];
  let cursor = clip.start;
  for (const [a, b] of played) {
    if (a > cursor) spans.push([cursor, a]);
    cursor = Math.max(cursor, b);
  }
  if (cursor < clipEnd(clip)) spans.push([cursor, clipEnd(clip)]);
  return spans;
}

/** "Take 3": lanes are numbered from the bottom, so the newest take has the highest number. */
export const takeName = (track: Track, lane: number) => `Take ${track.lanes - lane}`;
