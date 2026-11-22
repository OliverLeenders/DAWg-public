import type { Project, Track } from '../bridge/protocol';

// The mixer's strips in signal-flow order (docs/design/mixer.md, X2): the arrangement's order, except that a group's
// strip comes after its members, which feed it; then the returns, and the master on its own. A track with output
// tracks has no strip: it is only their header, and its outputs, Main included, are the strips
// (docs/design/multi-output-instruments.md, MO12).

export interface Strip {
  track: Track;
  /** The open groups whose bracket spans this strip, outermost first; an open group's own strip ends its bracket. */
  brackets: Track[];
}

export interface MixerLayout {
  strips: Strip[];
  returns: Strip[];
  master: Track | null;
}

export function mixerLayout(project: Project): MixerLayout {
  const members = new Map<number, Track[]>();
  const roots: Track[] = [];
  const headers = new Set<number>();
  for (const track of Object.values(project.tracks)) if (track.kind === 'output' && track.source) headers.add(track.source);

  for (const id of project.trackOrder) {
    const track = project.tracks[String(id)];
    if (!track || headers.has(track.id)) continue;
    if (track.parent && project.tracks[String(track.parent)]) members.set(track.parent, [...(members.get(track.parent) ?? []), track]);
    else roots.push(track);
  }

  const strips: Strip[] = [];
  const returns: Strip[] = [];

  const visit = (track: Track, enclosing: Track[]) => {
    if (track.kind === 'group' && !track.folded) {
      const inside = [...enclosing, track];
      for (const member of members.get(track.id) ?? []) visit(member, inside);
      strips.push({ track, brackets: inside });
      return;
    }
    strips.push({ track, brackets: enclosing });
  };

  for (const track of roots) {
    if (track.kind === 'return') returns.push({ track, brackets: [] });
    else visit(track, []);
  }
  return { strips, returns, master: project.master };
}

/** All strips left to right, the master last: the order arrow keys move through (X10). */
export const stripOrder = (layout: MixerLayout) => [
  ...layout.strips.map((s) => s.track.id),
  ...layout.returns.map((s) => s.track.id),
  ...(layout.master ? [layout.master.id] : []),
];
