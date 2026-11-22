import { describe, expect, it } from 'vitest';
import type { Project, Track } from '../bridge/protocol';
import { mixerLayout, stripOrder } from './order';

const track = (id: number, kind: Track['kind'], parent = 0, folded = false): Track =>
  ({ id, name: String(id), kind, parent, folded, sends: {}, clips: {}, graph: { nodes: {}, connections: {} } }) as unknown as Track;

/** 1, group 2 { 3, group 4 { 5 } }, return 7, 6, and the master 9 — with group 4 optionally folded. */
function project(folded = false): Project {
  const tracks = [
    track(1, 'instrument'),
    track(2, 'group'),
    track(3, 'audio', 2),
    track(4, 'group', 2, folded),
    track(5, 'instrument', 4),
    track(7, 'return'),
    track(6, 'instrument'),
  ];
  return {
    trackOrder: tracks.map((t) => t.id),
    tracks: Object.fromEntries(tracks.map((t) => [String(t.id), t])),
    master: track(9, 'master'),
  } as unknown as Project;
}

describe('mixerLayout', () => {
  it('puts a group after its members, at every level, and the returns and master apart', () => {
    const layout = mixerLayout(project());
    expect(layout.strips.map((s) => s.track.id)).toEqual([1, 3, 5, 4, 2, 6]);
    expect(layout.strips.map((s) => s.brackets.map((g) => g.id))).toEqual([[], [2], [2, 4], [2, 4], [2], []]);
    expect(layout.returns.map((s) => s.track.id)).toEqual([7]);
    expect(stripOrder(layout)).toEqual([1, 3, 5, 4, 2, 6, 7, 9]);
  });

  it('hides a folded group\'s members, and its strip has no bracket of its own', () => {
    const layout = mixerLayout(project(true));
    expect(layout.strips.map((s) => s.track.id)).toEqual([1, 3, 4, 2, 6]);
    expect(layout.strips[2].brackets.map((g) => g.id)).toEqual([2]);
  });

  it('gives a track with output tracks no strip: its outputs, Main included, are the strips', () => {
    const base = project();
    const output = (id: number, source: number) => ({ ...track(id, 'output'), source }) as Track;
    const outputs = [output(10, 1), output(11, 1)];
    const withOutputs = {
      ...base,
      trackOrder: [1, 10, 11, ...base.trackOrder.slice(1)],
      tracks: { ...base.tracks, ...Object.fromEntries(outputs.map((t) => [String(t.id), t])) },
    } as Project;
    expect(mixerLayout(withOutputs).strips.map((s) => s.track.id)).toEqual([10, 11, 3, 5, 4, 2, 6]);
  });
});
