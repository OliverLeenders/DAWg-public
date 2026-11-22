import { describe, expect, it } from 'vitest';
import type { Clip, Track } from '../bridge/protocol';
import { coveredEdges, crossfadeTails } from './crossfades';
import { playedClipsOf } from './takes';

const clip = (id: number, start: number, length: number, order: number, fades: Partial<Clip> = {}) =>
  ({ id, start, length, order, fileId: 7, lane: 0, offset: 0, loopStart: 0, loopLength: 0, fadeIn: 0, fadeOut: 0, fadeInShape: 0, fadeOutShape: 0, ...fades }) as Clip;
const track = (...clips: Clip[]) => ({ id: 1, clips: Object.fromEntries(clips.map((c) => [String(c.id), c])) }) as unknown as Track;

describe('crossfades', () => {
  it('finds the edges that lie over audio below, as the engine does', () => {
    const lower = clip(1, 0, 100, 1);
    const top = clip(2, 40, 100, 2);
    const t = track(lower, top);
    expect(coveredEdges(t, top)).toEqual({ start: true, end: false });
    expect(coveredEdges(t, lower)).toEqual({ start: false, end: false });
    const inside = clip(3, 10, 10, 3);
    expect(coveredEdges(track(lower, inside), inside)).toEqual({ start: true, end: true });
    // Note clips have none.
    expect(coveredEdges(track(lower, { ...inside, fileId: 0 }), { ...inside, fileId: 0 })).toEqual({ start: false, end: false });
  });

  it('pairs a top clip\'s fades with the clip they cover', () => {
    const lower = clip(1, 0, 100, 1);
    const top = clip(2, 40, 20, 2, { fadeIn: 5, fadeOut: 8, fadeInShape: 3, fadeOutShape: 1 });
    const t = track(lower, top);
    const part = playedClipsOf(t).find((p) => p.id === 2)!;
    expect(crossfadeTails(t, part)).toEqual([
      { edge: 'in', shape: 3, covered: lower },
      { edge: 'out', shape: 1, covered: lower },
    ]);
    // The covered clip's own parts draw no crossfade, and nor does a fade over nothing.
    for (const p of playedClipsOf(t).filter((p) => p.id === 1)) expect(crossfadeTails(t, p)).toEqual([]);
    const alone = clip(3, 200, 20, 3, { fadeIn: 5 });
    expect(crossfadeTails(track(lower, alone), alone)).toEqual([]);
  });
});
