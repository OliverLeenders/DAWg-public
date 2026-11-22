import { describe, expect, it } from 'vitest';
import type { Project, Track } from '../bridge/protocol';
import { dropSlots, isInside, nearestSlot, tracksInside } from './groups';
import { clipLanes, GROUP_BOTTOM, GROUP_TOP, lanesOf } from './layout';

const track = (id: number, kind: Track['kind'], parent = 0, folded = false): Track =>
  ({
    id, name: String(id), colour: '#7c8cff', gainDb: 0, pan: 0, offsetMs: 0, drumMode: 'auto', automation: {}, automationOrder: [], automationMode: 1,
    automationExpanded: false,
  lanes: 1,
  lanesExpanded: false, height: 40, kind, parent, folded, sends: {}, mute: false, solo: false, inputChannel: -1,
    inputStereo: false, monitor: 1, clips: {}, graph: { nodes: {}, connections: {} },
  }) as Track;

/** 1, group 2 { 3, group 4 { 5 } }, 6 — with group 4 optionally folded. */
function project(folded = false): Project {
  const tracks = [track(1, 'instrument'), track(2, 'group'), track(3, 'audio', 2), track(4, 'group', 2, folded), track(5, 'instrument', 4), track(6, 'instrument')];
  return { trackOrder: tracks.map((t) => t.id), tracks: Object.fromEntries(tracks.map((t) => [String(t.id), t])) } as unknown as Project;
}

describe('lanesOf with groups', () => {
  it('draws an open group as a top lane, its members and a bottom lane', () => {
    const lanes = lanesOf(project());
    expect(lanes.map((l) => `${l.track.id}${l.role === 'track' ? '' : l.role === 'groupTop' ? '^' : 'v'}`)).toEqual(['1', '2^', '3', '4^', '5', '4v', '2v', '6']);
    expect(lanes[1].height).toBe(GROUP_TOP);
    expect(lanes[6].height).toBe(GROUP_BOTTOM);
    expect(lanes[4].enclosing.map((g) => g.id)).toEqual([2, 4]);
    // Clips go on tracks only, counted past the groups.
    expect(clipLanes(lanes).map((l) => l.track.id)).toEqual([1, 3, 5, 6]);
    expect(lanes[7].clipIndex).toBe(3);
  });

  it('folds a group into one lane and hides its members', () => {
    const lanes = lanesOf(project(true));
    expect(lanes.map((l) => `${l.track.id}${l.role === 'track' ? '' : l.role === 'groupTop' ? '^' : 'v'}`)).toEqual(['1', '2^', '3', '4', '2v', '6']);
    expect(lanes[3].clipIndex).toBe(-1);
  });
});

describe('group helpers', () => {
  it('knows what is inside what', () => {
    const p = project();
    expect(isInside(p, 5, 2)).toBe(true);
    expect(isInside(p, 5, 4)).toBe(true);
    expect(isInside(p, 3, 4)).toBe(false);
    expect(tracksInside(p, 2)).toBe(3);
  });

  it('offers drop places at each header, at the end of each group and at the end', () => {
    const p = project();
    const lanes = lanesOf(p);
    const slots = dropSlots(p, lanes, [6]);
    const at = (lane: number) => nearestSlot(slots, lanes[lane].top);
    expect(at(2)).toMatchObject({ parent: 2, before: 3 }); // before track 3, inside group 2
    expect(at(5)).toMatchObject({ parent: 4, before: 0 }); // the end of group 4
    expect(at(6)).toMatchObject({ parent: 2, before: 0 }); // the end of group 2
    expect(nearestSlot(slots, 1e6)).toMatchObject({ parent: 0, before: 0 });
  });

  it('never offers a place inside the moved group', () => {
    const p = project();
    const slots = dropSlots(p, lanesOf(p), [2]);
    expect(slots.every((s) => s.parent !== 2 && s.parent !== 4 && s.before !== 3 && s.before !== 5)).toBe(true);
  });
});

describe('the returns section', () => {
  /** 1, 2, then returns 7 and 8. */
  function withReturns(): Project {
    const tracks = [track(1, 'instrument'), track(2, 'audio'), track(7, 'return'), track(8, 'return')];
    return { trackOrder: tracks.map((t) => t.id), tracks: Object.fromEntries(tracks.map((t) => [String(t.id), t])) } as unknown as Project;
  }

  it('puts a heading above the returns, which hold no clips', () => {
    const lanes = lanesOf(withReturns());
    expect(lanes.map((l) => (l.role === 'returnsHeading' ? 'R:' : String(l.track.id)))).toEqual(['1', '2', 'R:', '7', '8']);
    expect(clipLanes(lanes).map((l) => l.track.id)).toEqual([1, 2]);
  });

  it('keeps returns and other tracks apart when dragging', () => {
    const p = withReturns();
    const lanes = lanesOf(p);
    const forTrack = dropSlots(p, lanes, [1]);
    expect(forTrack.some((s) => s.before === 7 || s.before === 8)).toBe(false);
    expect(nearestSlot(forTrack, 1e6)).toMatchObject({ parent: 0, before: 0 }); // the end of the tracks, at the heading
    const forReturn = dropSlots(p, lanes, [8]);
    expect(forReturn.map((s) => s.before)).toEqual([7, 0]);
  });
});
