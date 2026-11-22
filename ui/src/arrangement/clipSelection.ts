import type { Clip, Project, Track } from '../bridge/protocol';

/** Clip order within a track: by start, then takes lane, then id, so overlapping takes have a fixed order. */
function byTime(a: Clip, b: Clip): number {
  return a.start - b.start || a.lane - b.lane || a.id - b.id;
}

/**
 * The clips to select after deleting `deleted` (docs/design/phase6-arrangement.md, section 7). For each track that
 * loses clips: the first remaining clip after the last deleted one in time, else the last remaining clip before it,
 * else none. So deleting clips on several tracks selects one clip on each of them.
 */
export function selectionAfterDelete(tracks: Iterable<Pick<Track, 'clips'>>, deleted: Iterable<number>): number[] {
  const gone = new Set(deleted);
  const next: number[] = [];
  for (const track of tracks) {
    const clips = Object.values(track.clips).sort(byTime);
    const lastDeleted = clips.findLastIndex((c) => gone.has(c.id));
    if (lastDeleted < 0) continue;
    const after = clips.slice(lastDeleted + 1).find((c) => !gone.has(c.id));
    const before = clips.slice(0, lastDeleted).findLast((c) => !gone.has(c.id));
    const chosen = after ?? before;
    if (chosen !== undefined) next.push(chosen.id);
  }
  return next;
}

/**
 * The ids of clips in `after` that `before` does not have, on any track: after a recording, the takes it made
 * (docs/design/takes-lanes.md, T3).
 */
export function addedClipIds(before: Pick<Project, 'tracks'>, after: Pick<Project, 'tracks'>): number[] {
  const known = new Set(Object.values(before.tracks).flatMap((t) => Object.values(t.clips).map((c) => c.id)));
  return Object.values(after.tracks)
    .flatMap((t) => Object.values(t.clips).map((c) => c.id))
    .filter((id) => !known.has(id));
}
