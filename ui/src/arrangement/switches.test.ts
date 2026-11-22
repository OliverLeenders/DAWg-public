import { describe, expect, it } from 'vitest';
import type { Project, Track } from '../bridge/protocol';
import { restorable, switchClick, tracksWith } from './switches';

const track = (id: number, mute = false, solo = false): Track => ({ id, kind: 'instrument', mute, solo }) as unknown as Track;

function project(tracks: Track[]): Project {
  return {
    trackOrder: tracks.map((t) => t.id),
    tracks: Object.fromEntries(tracks.map((t) => [String(t.id), t])),
    master: track(9),
  } as unknown as Project;
}

describe('tracksWith', () => {
  it('lists the tracks with the switch on, in track order', () => {
    const p = project([track(3, true), track(1, false, true), track(2, true, true)]);
    expect(tracksWith(p, 'mute')).toEqual([3, 2]);
    expect(tracksWith(p, 'solo')).toEqual([1, 2]);
  });

  it('is empty without a project', () => {
    expect(tracksWith(null, 'mute')).toEqual([]);
  });
});

describe('switchClick', () => {
  it('turns off only that switch, on every track that has it, as one batch, and remembers them', () => {
    const p = project([track(1, true, true), track(2), track(3, true)]);
    expect(switchClick(p, 'mute', [])).toEqual({
      command: {
        command: 'batch',
        args: {
          label: 'Unmute all tracks',
          commands: [
            { command: 'setTrackSwitches', args: { id: 1, mute: false } },
            { command: 'setTrackSwitches', args: { id: 3, mute: false } },
          ],
        },
      },
      remember: [1, 3],
    });
    expect(switchClick(p, 'solo', [])).toEqual({
      command: {
        command: 'batch',
        args: { label: 'Unsolo all tracks', commands: [{ command: 'setTrackSwitches', args: { id: 1, solo: false } }] },
      },
      remember: [1],
    });
  });

  it('restores the remembered tracks when none has the switch on, then forgets them', () => {
    // Track 4 was removed since.
    const p = project([track(1), track(2), track(3)]);
    expect(switchClick(p, 'mute', [1, 3, 4])).toEqual({
      command: {
        command: 'batch',
        args: {
          label: 'Restore mutes',
          commands: [
            { command: 'setTrackSwitches', args: { id: 1, mute: true } },
            { command: 'setTrackSwitches', args: { id: 3, mute: true } },
          ],
        },
      },
      remember: [],
    });
  });

  it('clears instead of restoring once a track has the switch on again, remembering the new set', () => {
    const p = project([track(1), track(2, true)]);
    expect(switchClick(p, 'mute', [1]).remember).toEqual([2]);
  });

  it('does nothing with nothing on and nothing to restore', () => {
    expect(switchClick(project([track(1), track(2)]), 'solo', [])).toEqual({ command: null, remember: [] });
    expect(switchClick(project([track(1)]), 'solo', [5])).toEqual({ command: null, remember: [] });
  });
});

describe('restorable', () => {
  it('keeps the remembered tracks that still exist and are off', () => {
    const p = project([track(1), track(2, false, true)]);
    expect(restorable(p, 'solo', [1, 2, 7])).toEqual([1]);
  });
});
