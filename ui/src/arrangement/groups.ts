import type { Project, Track } from '../bridge/protocol';
import { GROUP_TOP, type Lane } from './layout';

// Group tracks in the arrangement (docs/design/groups-and-sends.md): what is inside what, and where dragged
// tracks can land.

/** Whether `trackId` lies inside the group `groupId` (at any depth). */
export function isInside(project: Project, trackId: number, groupId: number): boolean {
  for (let parent = project.tracks[String(trackId)]?.parent ?? 0; parent !== 0; parent = project.tracks[String(parent)]?.parent ?? 0)
    if (parent === groupId) return true;
  return false;
}

/** How many output tracks a track has (docs/design/multi-output-instruments.md, MO4): deleted and duplicated with it. */
export const outputTracksOf = (project: Project, trackId: number) =>
  project.trackOrder.filter((id) => project.tracks[String(id)]?.kind === 'output' && project.tracks[String(id)]?.source === trackId).length;

/** Whether a track is only the header of its output tracks (MO12): it plays nothing itself, so it takes no effects. */
export const isOutputsHeader = (project: Project | null | undefined, trackId: number) =>
  !!project && outputTracksOf(project, trackId) > 0;

/** Whether an output track sits right under its track, in its group, after only that track's other output tracks. */
export function underSource(project: Project | null | undefined, track: Track): boolean {
  if (!project || track.kind !== 'output') return false;
  const order = project.trackOrder;
  for (let i = order.indexOf(track.id) - 1; i >= 0; --i) {
    const above = project.tracks[String(order[i])];
    if (above?.id === track.source) return above.parent === track.parent;
    if (above?.kind !== 'output' || above.source !== track.source || above.parent !== track.parent) return false;
  }
  return false;
}

/** How many tracks a group holds, nested groups and their tracks included. */
export const tracksInside = (project: Project, groupId: number) =>
  project.trackOrder.filter((id) => isInside(project, id, groupId)).length;

/** A place between headers where dragged tracks can land: into `parent`, before `before` (0: at its end). */
export interface DropSlot {
  /** Content y of the drop line. */
  y: number;
  parent: number;
  before: number;
  /** Nesting depth of the landing place, for indenting the drop line. */
  depth: number;
}

/** The drop slots for moving `ids`: one at the top of each header, one at the end of each open group and one at
    the very end. Slots inside the moved tracks themselves are left out. */
export function dropSlots(project: Project, lanes: Lane[], ids: number[]): DropSlot[] {
  const moving = (id: number) => ids.includes(id) || ids.some((moved) => isInside(project, id, moved));
  // Returns move only among themselves, other tracks never into their section (R10).
  const returns = ids.length > 0 && ids.every((id) => project.tracks[String(id)]?.kind === 'return');
  const slots: DropSlot[] = [];

  for (const lane of lanes) {
    const depth = lane.enclosing.length;
    const track: Track = lane.track;
    if (lane.role === 'returnsHeading') {
      if (!returns) slots.push({ y: lane.top, parent: 0, before: 0, depth: 0 }); // the end of the tracks
    } else if ((track.kind === 'return') !== returns) {
      continue;
    } else if (lane.role === 'groupBottom') {
      if (!moving(track.id)) slots.push({ y: lane.top, parent: track.id, before: 0, depth: depth + 1 });
    } else if (!moving(track.id)) {
      slots.push({ y: lane.top, parent: track.parent, before: track.id, depth });
    }
  }

  const hasReturns = lanes.some((lane) => lane.role === 'returnsHeading');
  const end = lanes.length === 0 ? GROUP_TOP : lanes[lanes.length - 1].top + lanes[lanes.length - 1].fullHeight;
  if (returns || !hasReturns) slots.push({ y: end, parent: 0, before: 0, depth: 0 });
  return slots.filter((slot) => slot.parent === 0 || !moving(slot.parent));
}

/** The slot nearest to a content y. */
export const nearestSlot = (slots: DropSlot[], y: number) =>
  slots.reduce((best, slot) => (Math.abs(slot.y - y) < Math.abs(best.y - y) ? slot : best), slots[0]);
