import type { Track } from '../bridge/protocol';

/** Width of one level of group spine at the left of the headers. */
export const SPINE = 8;

/** How far a header's own content, and its separator line, start from the left: past the spines of its groups. */
export const indentStyle = (groups: Track[]) => ({ ['--indent' as string]: `${groups.length * SPINE}px` });

/** The colours of the groups a header sits in, one bar per level: together they draw the groups' "C"s
    (docs/design/groups-and-sends.md, R9). `own` adds a group's own bar on its arms, rounded at the arm's end. */
export function Spines({ groups, own, end }: { groups: Track[]; own?: Track; end?: 'top' | 'bottom' }) {
  const bars = own ? [...groups, own] : groups;
  if (bars.length === 0) return null;
  return (
    <div className="track-spines" style={{ width: bars.length * SPINE }}>
      {bars.map((group, depth) => (
        <span
          key={group.id}
          className={`track-spine ${group === own ? `own ${end ?? ''}` : ''}`}
          style={{ left: depth * SPINE, background: group.colour }}
        />
      ))}
    </div>
  );
}
