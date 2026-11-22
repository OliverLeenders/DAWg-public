import { Power, X } from 'lucide-react';
import { useState } from 'react';
import type { LinkMapping, ParamLink, Track } from '../bridge/protocol';
import { DragNumber } from '../components/DragNumber';
import { dispatch, useDawg } from '../store/store';
import { IconButton, Select, Toggle } from '../ui/controls';
import { describeMapping, endName, groupByNodes, linksOfTrack, mappingUnit, removeLinks, setLink, trackOf } from './links';
import { modulationUi } from './modulationStore';

// The track's parameter links in the modulation column (docs/design/parameter-linking.md, L16): grouped by the two
// devices they join, each with its mapping, a switch and a remove button.

const KINDS: Array<{ value: LinkMapping['kind']; label: string }> = [
  { value: 'identity', label: 'Same' },
  { value: 'inverted', label: 'Inverted' },
  { value: 'offset', label: 'Offset' },
  { value: 'scaled', label: 'Scaled' },
  { value: 'range', label: 'Range' },
];

export function LinksEditor({ track }: { track: Track }) {
  const project = useDawg((s) => s.project);
  const links = linksOfTrack(project, track.id);

  if (links.length === 0)
    return <p className="mod-hint">Right-click a knob and choose Link to another parameter, or a device's menu to mirror it on another.</p>;

  return (
    <div className="mod-links">
      {groupByNodes(links).map((group) => (
        <LinkGroup key={`${group[0].source.nodeId}>${group[0].target.nodeId}`} group={group} trackId={track.id} />
      ))}
    </div>
  );
}

/** Whether a link only copies a parameter to the same one: a mirrored device's many such links fold into a count. */
const plain = (link: ParamLink) => link.active && link.mapping.kind === 'identity' && link.source.param === link.target.param;

function LinkGroup({ group, trackId }: { group: ParamLink[]; trackId: number }) {
  const project = useDawg((s) => s.project);
  const [all, setAll] = useState(false);
  const first = group[0];
  const from = endName(project, first.source).split(' · ')[0];
  const to = endName(project, first.target).split(' · ')[0];
  const elsewhere = first.source.trackId !== first.target.trackId;
  const otherTrack = trackOf(project, first.source.trackId === trackId ? first.target.trackId : first.source.trackId);
  const allOn = group.every((l) => l.active);
  const folded = group.length > 3 && !all ? group.filter(plain) : [];
  const shown = group.filter((l) => !folded.includes(l));
  return (
    <section className="mod-link-group">
      <header className="mod-link-header">
        <span className="mod-target-name" title={elsewhere ? `With ${otherTrack?.name ?? 'another track'}: follows a block later` : undefined}>
          {from} → {to}
          {elsewhere && ` (${otherTrack?.name ?? 'other track'})`}
        </span>
        <Toggle
          size="s"
          icon
          on={allOn}
          title={allOn ? 'Switch these links off' : 'Switch these links on'}
          onChange={() =>
            dispatch({
              command: 'batch',
              args: { label: 'Switch links', commands: group.map((l) => ({ command: 'setLink' as const, args: { id: l.id, active: !allOn } })) },
            })
          }
        >
          <Power size={10} />
        </Toggle>
        {group.length > 1 && (
          <IconButton size="s" className="tiny" title="Remove these links" onClick={() => removeLinks(group.map((l) => l.id))}>
            <X size={10} />
          </IconButton>
        )}
      </header>
      <ul className="mod-targets">
        {shown.map((link) => (
          <LinkRow key={link.id} link={link} single={group.length === 1} />
        ))}
      </ul>
      {(folded.length > 0 || all) && group.length > 3 && (
        <button className="mod-link-more" onClick={() => setAll(!all)}>
          {all ? 'Show only the differences' : `${folded.length} more, the same on both`}
        </button>
      )}
    </section>
  );
}

function LinkRow({ link, single }: { link: ParamLink; single: boolean }) {
  const project = useDawg((s) => s.project);
  const unit = mappingUnit(link, project);
  const name = (address: ParamLink['source']) => endName(project, address).split(' · ').slice(1).join(' · ');
  const label = link.source.param === link.target.param ? name(link.source) : `${name(link.source)} → ${name(link.target)}`;
  const mapping = link.mapping;
  const amount = mapping.amount ?? (mapping.kind === 'scaled' ? 1 : 0);
  const percent = unit === '%';

  return (
    <li
      className={`mod-target ${link.active ? '' : 'inactive'}`}
      onPointerEnter={() => modulationUi.mark([link.source, link.target])}
      onPointerLeave={() => modulationUi.mark([])}
    >
      <span className="mod-target-name" title={`${endName(project, link.source)} → ${endName(project, link.target)}: ${describeMapping(mapping, unit)}`}>
        {label}
      </span>
      <Select
        size="s"
        value={mapping.kind}
        title="How the follower takes the value"
        onChange={(e) => setLink(link.id, { mapping: kindChange(mapping, e.target.value as LinkMapping['kind']) })}
      >
        {KINDS.map((k) => (
          <option key={k.value} value={k.value}>
            {k.label}
          </option>
        ))}
      </Select>
      {(mapping.kind === 'offset' || mapping.kind === 'scaled') && (
        <DragNumber
          className="graph-mod-depth"
          value={amount}
          min={mapping.kind === 'scaled' ? -4 : percent ? -1 : -100}
          max={mapping.kind === 'scaled' ? 4 : percent ? 1 : 100}
          quantum={0.01}
          format={(v) => (mapping.kind === 'scaled' ? `× ${v.toFixed(2)}` : describeMapping({ kind: 'offset', amount: v }, unit))}
          title={mapping.kind === 'scaled' ? 'The factor, around the middle of the range' : 'What is added'}
          onChange={(value, gesture) => dispatch({ command: 'setLink', args: { id: link.id, mapping: { ...mapping, amount: value } } }, gesture)}
        />
      )}
      {mapping.kind === 'range' && (
        <>
          <RangeField link={link} field="outMin" title="What the follower is at the bottom of its source's range" />
          <RangeField link={link} field="outMax" title="What the follower is at the top of its source's range" />
        </>
      )}
      {single && (
        <IconButton size="s" className="tiny" title="Remove this link" onClick={() => removeLinks([link.id])}>
          <X size={10} />
        </IconButton>
      )}
    </li>
  );
}

function RangeField({ link, field, title }: { link: ParamLink; field: 'outMin' | 'outMax'; title: string }) {
  const value = link.mapping[field] ?? (field === 'outMin' ? 0 : 1);
  return (
    <DragNumber
      className="graph-mod-depth"
      value={value}
      min={0}
      max={1}
      quantum={0.01}
      format={(v) => `${Math.round(v * 100)} %`}
      title={title}
      onChange={(next, gesture) => dispatch({ command: 'setLink', args: { id: link.id, mapping: { ...link.mapping, [field]: next } } }, gesture)}
    />
  );
}

/** A mapping of another kind, with that kind's starting amount. */
function kindChange(mapping: LinkMapping, kind: LinkMapping['kind']): LinkMapping {
  if (kind === 'scaled') return { kind, amount: 1 };
  if (kind === 'offset') return { kind, amount: 0 };
  if (kind === 'range') return { kind, inMin: 0, inMax: 1, outMin: 0, outMax: 1 };
  return { kind, centre: mapping.centre };
}
