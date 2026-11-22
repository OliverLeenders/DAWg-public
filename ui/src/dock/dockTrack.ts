import type { Project, Track } from '../bridge/protocol';

/** The track the device dock shows: the selected track, or the first track when nothing is selected. The inspector,
    the graph and the headers already treat that first track as current, so the Channel tab must too. */
export function dockTrack(project: Project | null, selectedTrackId: number | null): Track | null {
  const id = selectedTrackId ?? project?.trackOrder[0];
  if (id === undefined || !project) return null;
  return project.master?.id === id ? project.master : (project.tracks[String(id)] ?? null);
}
