import type { Clip, Track } from '../bridge/protocol';
import { clipEnd } from './clips';
import { playedClipsOf } from './takes';

// Crossfades (docs/design/audio-features.md, AU5-AU8): a crossfade is an audio clip's fade over a clip below it in
// the stack. The engine plays the covered clip on under the fade with the mirrored fade (crossfadedParts in
// engine/src/ClipOps.cpp); these find the same edges for drawing and the menus.

const above = (a: Clip, b: Clip) => a.order > b.order || (a.order === b.order && a.id > b.id);

/** Which edges of an audio clip lie over audio of a clip lower in the stack (coveredEdges in the engine). */
export function coveredEdges(track: Track, clip: Clip): { start: boolean; end: boolean } {
  const edges = { start: false, end: false };
  if (clip.fileId === 0) return edges;
  for (const other of Object.values(track.clips)) {
    if (other.id === clip.id || other.fileId === 0 || !above(clip, other)) continue;
    if (other.start < clip.start && clipEnd(other) > clip.start) edges.start = true;
    if (other.start < clipEnd(clip) && clipEnd(other) > clipEnd(clip)) edges.end = true;
  }
  return edges;
}

/** A covered clip playing on under a fade of the clip above it. */
export interface CrossfadeTail {
  /** The top clip's fade it plays under. */
  edge: 'in' | 'out';
  /** The fade's shape (the covered clip's fade mirrors it). */
  shape: number;
  /** The covered clip. */
  covered: Clip;
}

/** The crossfades at the edges of a played part of a track (the parts are what the track's row draws). */
export function crossfadeTails(track: Track, part: Clip): CrossfadeTail[] {
  const own = track.clips[String(part.id)];
  if (!own || own.fileId === 0) return [];
  const played = playedClipsOf(track);
  const tails: CrossfadeTail[] = [];
  const coveredAt = (tick: number, ending: boolean) =>
    played.find((p) => {
      if (p.id === own.id || p.fileId === 0) return false;
      const whole = track.clips[String(p.id)];
      if (!whole || !above(own, whole)) return false;
      // Cut there: the covered clip itself goes on past the tick (or started before it).
      return ending ? clipEnd(p) === tick && clipEnd(whole) > tick : p.start === tick && whole.start < tick;
    });

  if (part.start === own.start && own.fadeIn > 0) {
    const covered = coveredAt(own.start, true);
    if (covered) tails.push({ edge: 'in', shape: own.fadeInShape, covered: track.clips[String(covered.id)] });
  }
  if (clipEnd(part) === clipEnd(own) && own.fadeOut > 0) {
    const covered = coveredAt(clipEnd(own), false);
    if (covered) tails.push({ edge: 'out', shape: own.fadeOutShape, covered: track.clips[String(covered.id)] });
  }
  return tails;
}
