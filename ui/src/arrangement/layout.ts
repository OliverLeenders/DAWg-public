import type { AutomationLane, Clip, Project, Track } from '../bridge/protocol';
import { lanesOfTrack } from '../automation/automation';
import { tickToX, xToTick, type View } from '../pianoroll/geometry';
import { clipEnd } from '../timeline/clips';
import { audioTime } from '../timeline/audioTime';
import { playedClipsOf } from '../timeline/takes';
import type { TempoMap } from '../time/tempoMap';

// Lane layout and hit testing for the arrangement (docs/design/phase6-arrangement.md).

/** An automation lane shown under its track (docs/design/phase10-automation.md, section 5). */
export interface AutomationRow {
  track: Track;
  lane: AutomationLane;
  /** Top edge in content coordinates (before scrolling). */
  top: number;
  height: number;
}

/** A takes lane shown under its track (docs/design/takes-lanes.md, T5): the clips of one lane, the top row first. */
export interface TakeRow {
  track: Track;
  /** The lane's index on the track (Clip.lane). */
  lane: number;
  /** Top edge in content coordinates (before scrolling). */
  top: number;
  height: number;
}

/** What a lane shows: a track (or a folded group), the top or bottom arm of an open group's "C"
    (docs/design/groups-and-sends.md, R9), or the heading of the returns section (R10). */
export type LaneRole = 'track' | 'groupTop' | 'groupBottom' | 'returnsHeading';

export interface Lane {
  track: Track;
  index: number;
  role: LaneRole;
  /** The groups the track is in, outermost first (their colours draw the spines). */
  enclosing: Track[];
  /** Index among the lanes that hold clips (instrument and audio tracks), or -1. */
  clipIndex: number;
  /** A group's top arm or folded lane: where its members' clips are, as timeline spans. */
  summary?: Array<{ start: number; end: number }>;
  /** Top edge in content coordinates (before scrolling). */
  top: number;
  /** Height of the clip lane. */
  height: number;
  /** Takes lanes right below the clip lane (when the track shows them). */
  takes: TakeRow[];
  /** Automation lanes below those (when the track is expanded). */
  automation: AutomationRow[];
  /** Height including the automation lanes. */
  fullHeight: number;
}

export const CLIP_INSET = 2;
/** Heights of an open group's arms: name and switches on top, gain, pan and meter below. */
export const GROUP_TOP = 26;
export const GROUP_BOTTOM = 40; // 5 + 20, then the meter (3 px gap, 8 px, 4 px off the bottom)
/** Height of the "Returns" heading above the return tracks. */
export const RETURNS_HEADING = 22;
export const NAME_BAR = 15;
export const EDGE_PX = 6;
export const LOOP_HANDLE_PX = 9;
/** Height of a takes row. */
export const TAKE_ROW = 40;

/** One track's lane with its takes and automation rows below it, from `top`; `height` is the clip lane's. */
export function laneOfTrack(track: Track, index: number, top: number, height = track.height): Lane {
  const takes: TakeRow[] = [];
  const automation: AutomationRow[] = [];
  let rowTop = top + height;

  if (track.lanesExpanded && track.lanes > 1)
    for (let lane = 0; lane < track.lanes; lane++) {
      takes.push({ track, lane, top: rowTop, height: TAKE_ROW });
      rowTop += TAKE_ROW;
    }

  if (track.automationExpanded)
    for (const lane of lanesOfTrack(track)) {
      automation.push({ track, lane, top: rowTop, height: lane.height });
      rowTop += lane.height;
    }

  return { track, index, role: 'track', enclosing: [], clipIndex: -1, top, height, takes, automation, fullHeight: rowTop - top };
}

/** The arrangement's lanes, top to bottom. An open group is a "C" around its members: a top lane, the members'
    lanes and a bottom lane with the group's automation; a folded group is one lane and hides its members. */
export function lanesOf(project: Project): Lane[] {
  const lanes: Lane[] = [];
  const members = new Map<number, Track[]>();
  const roots: Track[] = [];
  let top = 0;
  let clips = 0;

  for (const id of project.trackOrder) {
    const track = project.tracks[String(id)];
    if (!track) continue;
    if (track.parent && project.tracks[String(track.parent)]) members.set(track.parent, [...(members.get(track.parent) ?? []), track]);
    else roots.push(track);
  }

  const add = (lane: Lane) => {
    lanes.push(lane);
    top += lane.fullHeight;
  };

  // Where the clips inside a group are, nested groups included.
  const spans = (group: Track): Array<{ start: number; end: number }> =>
    (members.get(group.id) ?? []).flatMap((member) =>
      member.kind === 'group' ? spans(member) : Object.values(member.clips).map((clip) => ({ start: clip.start, end: clipEnd(clip) })),
    );

  const visit = (track: Track, enclosing: Track[]) => {
    if (track.kind === 'group' && !track.folded) {
      const lane = laneOfTrack({ ...track, automationExpanded: false, lanesExpanded: false }, lanes.length, top, GROUP_TOP);
      add({ ...lane, track, role: 'groupTop', enclosing, summary: spans(track) });
      for (const member of members.get(track.id) ?? []) visit(member, [...enclosing, track]);
      add({ ...laneOfTrack(track, lanes.length, top, GROUP_BOTTOM), role: 'groupBottom', enclosing });
      return;
    }
    const holdsClips = track.kind === 'instrument' || track.kind === 'audio';
    const summary = track.kind === 'group' ? spans(track) : undefined;
    add({ ...laneOfTrack(track, lanes.length, top), enclosing, clipIndex: holdsClips ? clips++ : -1, summary });
  };

  for (const track of roots) {
    // The returns come last, under a heading of their own (R10).
    if (track.kind === 'return' && !lanes.some((lane) => lane.role === 'returnsHeading'))
      add({ ...laneOfTrack({ ...track, automationExpanded: false, lanesExpanded: false }, lanes.length, top, RETURNS_HEADING), role: 'returnsHeading' });
    visit(track, []);
  }
  return lanes;
}

/** The lanes that hold clips, in order (Lane.clipIndex indexes this). */
export const clipLanes = (lanes: Lane[]) => lanes.filter((lane) => lane.clipIndex >= 0);

export const totalHeight = (lanes: Lane[]) =>
  lanes.length === 0 ? 0 : lanes[lanes.length - 1].top + lanes[lanes.length - 1].fullHeight;

/** The automation lane at a canvas y (scroll applied), if any. */
export function automationAt(lanes: Lane[], y: number, scrollY: number): AutomationRow | undefined {
  const contentY = y + scrollY;
  for (const lane of lanes)
    for (const row of lane.automation) if (contentY >= row.top && contentY < row.top + row.height) return row;
  return undefined;
}

/** The takes row at a canvas y (scroll applied), if any. */
export function takeRowAt(lanes: Lane[], y: number, scrollY: number): TakeRow | undefined {
  const contentY = y + scrollY;
  for (const lane of lanes) for (const row of lane.takes) if (contentY >= row.top && contentY < row.top + row.height) return row;
  return undefined;
}

/** A takes row as a lane, for the clip drawing and hit testing that take one. */
export const rowAsLane = (lane: Lane, row: TakeRow): Lane => ({ ...lane, top: row.top, height: row.height, takes: [], automation: [] });

/** The clip under a point in a takes row: the lane's own clips, whole. */
export function hitTake(view: View, lanes: Lane[], x: number, y: number, scrollY: number): { clip: Clip; row: TakeRow; lane: Lane } | undefined {
  const row = takeRowAt(lanes, y, scrollY);
  const lane = row && lanes.find((l) => l.takes.includes(row));
  if (!row || !lane) return undefined;
  const tick = xToTick(view, x);
  const clip = Object.values(row.track.clips).find((c) => c.lane === row.lane && c.start <= tick && clipEnd(c) > tick);
  return clip ? { clip, row, lane } : undefined;
}

/** The lane at a canvas y (scroll applied), or undefined below the last lane. */
export function laneAt(lanes: Lane[], y: number, scrollY: number): Lane | undefined {
  const contentY = y + scrollY;
  return lanes.find((l) => contentY >= l.top && contentY < l.top + l.height);
}

export interface ClipBox {
  x0: number;
  x1: number;
  y0: number;
  y1: number;
}

export function clipBox(view: View, lane: Lane, clip: Pick<Clip, 'start' | 'length'>, scrollY: number): ClipBox {
  return {
    x0: tickToX(view, clip.start),
    x1: tickToX(view, clip.start + clip.length),
    y0: lane.top - scrollY + CLIP_INSET,
    y1: lane.top - scrollY + lane.height - CLIP_INSET,
  };
}

export type ClipZone = 'body' | 'start' | 'end' | 'loop' | 'fadeIn' | 'fadeOut' | 'gain';

/** Size of the fade and gain handles of audio clips (drawn just below the name bar). */
export const HANDLE_PX = 7;

/** Where an audio clip's handles are: fade in, fade out and gain (phase 7, section 9). */
/** Where an audio clip's fade and gain handles are. Fades are content ticks (timeline/audioTime.ts). */
export function audioHandles(view: View, box: ClipBox, clip: Clip, map?: TempoMap) {
  const y = box.y0 + NAME_BAR + 1;
  const time = audioTime(clip, map);
  const fadeIn = Math.min(box.x1 - HANDLE_PX, tickToX(view, time.tickAt(clip.fadeIn)));
  const fadeOut = Math.max(box.x0, tickToX(view, time.tickAt(time.localLength - clip.fadeOut)) - HANDLE_PX);
  return { y, fadeIn, fadeOut, gain: (box.x0 + box.x1) / 2 - HANDLE_PX / 2 };
}

export interface ClipHit {
  clip: Clip;
  lane: Lane;
  zone: ClipZone;
}

/** The clip under a point and which part of it: edges resize, the top-right corner loops (section 7). */
export function hitClip(view: View, lanes: Lane[], x: number, y: number, scrollY: number, map?: TempoMap): ClipHit | undefined {
  const lane = laneAt(lanes, y, scrollY);
  if (!lane) return undefined;

  // The track's row shows what plays (stacked takes cut around the top one); later clips draw on top, so search from
  // the end. A hit names the whole clip, so edits act on it.
  const clips = playedClipsOf(lane.track);

  for (let i = clips.length - 1; i >= 0; i--) {
    const clip = clips[i];
    const box = clipBox(view, lane, clip, scrollY);
    if (x < box.x0 || x > box.x1 || y < box.y0 || y > box.y1) continue;

    const edge = Math.min(EDGE_PX, (box.x1 - box.x0) / 4);
    let zone: ClipZone = 'body';

    if (clip.fileId !== 0 && box.x1 - box.x0 > 4 * HANDLE_PX) {
      const h = audioHandles(view, box, clip, map);
      const inHandle = (hx: number) => x >= hx - 2 && x <= hx + HANDLE_PX + 2 && y >= h.y - 2 && y <= h.y + HANDLE_PX + 2;
      if (inHandle(h.gain)) return { clip, lane, zone: 'gain' };
      if (inHandle(h.fadeIn)) return { clip, lane, zone: 'fadeIn' };
      if (inHandle(h.fadeOut)) return { clip, lane, zone: 'fadeOut' };
    }

    if (x >= box.x1 - LOOP_HANDLE_PX && y <= box.y0 + LOOP_HANDLE_PX) zone = 'loop';
    else if (x <= box.x0 + edge) zone = 'start';
    else if (x >= box.x1 - edge) zone = 'end';
    return { clip: lane.track.clips[String(clip.id)] ?? clip, lane, zone };
  }

  return undefined;
}

/** Clips whose boxes intersect a rectangle (canvas coordinates). */
export function clipsInRect(view: View, lanes: Lane[], scrollY: number, x0: number, y0: number, x1: number, y1: number) {
  const [left, right] = [Math.min(x0, x1), Math.max(x0, x1)];
  const [top, bottom] = [Math.min(y0, y1), Math.max(y0, y1)];
  const fromTick = xToTick(view, left);
  const toTick = xToTick(view, right);
  const ids: number[] = [];

  for (const lane of lanes) {
    const laneTop = lane.top - scrollY;
    if (laneTop + lane.height < top || laneTop > bottom) continue;

    for (const clip of Object.values(lane.track.clips))
      if (clip.start < toTick && clipEnd(clip) > fromTick) ids.push(clip.id);
  }

  return ids;
}
