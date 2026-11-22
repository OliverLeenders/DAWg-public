import { describe, expect, it } from 'vitest';
import type { Clip, Project, Track } from '../bridge/protocol';
import { addedClipIds, selectionAfterDelete } from './clipSelection';

const clip = (id: number, start: number, lane = 0): Clip => ({ id, start, length: 100, lane }) as Clip;
const track = (...clips: Clip[]): Pick<Track, 'clips'> => ({ clips: Object.fromEntries(clips.map((c) => [String(c.id), c])) });

describe('selectionAfterDelete', () => {
  it('selects the next clip in time, whatever the id order', () => {
    expect(selectionAfterDelete([track(clip(3, 0), clip(1, 200), clip(2, 400))], [3])).toEqual([1]);
  });

  it('falls back to the previous clip at the end of the track', () => {
    expect(selectionAfterDelete([track(clip(1, 0), clip(2, 200), clip(3, 400))], [3])).toEqual([2]);
  });

  it('selects nothing when the track has no clips left', () => {
    expect(selectionAfterDelete([track(clip(1, 0), clip(2, 200))], [1, 2])).toEqual([]);
  });

  it('selects the clip after the last deleted one, skipping deleted clips', () => {
    const t = track(clip(1, 0), clip(2, 200), clip(3, 400), clip(4, 600), clip(5, 800));
    expect(selectionAfterDelete([t], [1, 3])).toEqual([4]);
    expect(selectionAfterDelete([t], [4, 5])).toEqual([3]);
  });

  it('selects one clip on each track that had a deletion', () => {
    const a = track(clip(1, 0), clip(2, 200));
    const b = track(clip(3, 0), clip(4, 200));
    const c = track(clip(5, 0));
    expect(selectionAfterDelete([a, b, c], [1, 4])).toEqual([2, 3]);
  });

  it('orders takes at the same start by lane', () => {
    expect(selectionAfterDelete([track(clip(1, 0, 0), clip(2, 0, 1), clip(3, 0, 2))], [2])).toEqual([3]);
  });
});

describe('addedClipIds', () => {
  const project = (...tracks: Array<Pick<Track, 'clips'>>) =>
    ({ tracks: Object.fromEntries(tracks.map((t, i) => [String(i + 1), t])) }) as unknown as Pick<Project, 'tracks'>;

  it('finds the clips a recording added, one per armed track', () => {
    const before = project(track(clip(1, 0)), track(), track(clip(2, 0)));
    const after = project(track(clip(1, 0), clip(3, 400)), track(clip(4, 400)), track(clip(2, 0)));
    expect(addedClipIds(before, after)).toEqual([3, 4]);
  });

  it('finds every loop pass as a take', () => {
    expect(addedClipIds(project(track()), project(track(clip(5, 0, 0), clip(6, 0, 1))))).toEqual([5, 6]);
  });

  it('finds nothing when no clip was added', () => {
    expect(addedClipIds(project(track(clip(1, 0))), project(track()))).toEqual([]);
  });
});
