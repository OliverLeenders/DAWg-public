import { describe, expect, it } from 'vitest';
import type { Project, Track } from '../bridge/protocol';
import { dockTrack } from './dockTrack';

const track = (id: number, name: string): Track => ({ id, name } as Track);

describe('the device dock track', () => {
  const first = track(1, 'First');
  const second = track(2, 'Second');
  const master = track(99, 'Master');
  const project = {
    trackOrder: [1, 2],
    tracks: { '1': first, '2': second },
    master,
  } as unknown as Project;

  it('uses the selected track', () => {
    expect(dockTrack(project, 2)).toBe(second);
    expect(dockTrack(project, 99)).toBe(master);
  });

  it('falls back to the first track, as the inspector, graph and headers do', () => {
    expect(dockTrack(project, null)).toBe(first);
  });

  it('shows nothing without a project', () => {
    expect(dockTrack(null, null)).toBeNull();
  });
});
