import { NODE_TYPES, type GraphNode, type LinkMapping, type ParamAddress, type ParamDescriptor, type ParamLink, type Project, type Track } from '../bridge/protocol';
import { infoOf } from '../graph/nodeInfo';
import { fromPosition, toPosition } from '../graph/params';
import { findNode } from '../graph/voice';
import { evaluateCurve } from '../pianoroll/geometry';
import { dispatch, useDawg } from '../store/store';

// Parameter links in the UI (docs/design/parameter-linking.md): finding them, the same mapping arithmetic as
// engine/src/ParamLinks.cpp (so a linked knob shows what the engine plays), and the commands that make them.

/** Ticks across a range mapping's curve, as modulationShapeTicks in engine/include/dawg/Model.h. */
export const LINK_CURVE_TICKS = 960;

export const sameAddress = (a: ParamAddress, b: ParamAddress) => a.trackId === b.trackId && a.nodeId === b.nodeId && a.param === b.param;

/** The link driving a parameter, if one does. */
export const linkDriving = (project: Project | null | undefined, target: ParamAddress) =>
  project?.links?.find((l) => sameAddress(l.target, target));

/** The links a parameter drives. */
export const linksFrom = (project: Project | null | undefined, source: ParamAddress) =>
  project?.links?.filter((l) => sameAddress(l.source, source)) ?? [];

/** The links with an end on a track. */
export const linksOfTrack = (project: Project | null | undefined, trackId: number) =>
  project?.links?.filter((l) => l.source.trackId === trackId || l.target.trackId === trackId) ?? [];

export const trackOf = (project: Project | null | undefined, trackId: number): Track | undefined =>
  project?.master?.id === trackId ? project.master : project?.tracks[String(trackId)];

/** A link end's node and, for a built-in parameter, its description. */
export function endOf(project: Project | null | undefined, address: ParamAddress): { track: Track; node: GraphNode; param?: ParamDescriptor } | undefined {
  const track = trackOf(project, address.trackId);
  const node = track ? findNode(track, address.nodeId) : undefined;
  if (!track || !node) return undefined;
  const s = useDawg.getState();
  const param = isPluginNode(node) ? undefined : infoOf(track.id, node, s.nodeTypes, s.nodes)?.params.find((p) => p.id === address.param);
  return { track, node, param };
}

export const isPluginNode = (node: GraphNode) => node.type === NODE_TYPES.pluginEffect || node.type === NODE_TYPES.pluginInstrument;

type Domain = 'units' | 'octaves' | 'normalised';

const isFrequency = (p: ParamDescriptor) => p.unit === 'Hz' && p.min > 0 && p.max > p.min;

/** The terms a link works in (L2), as linkDomain() in the engine. */
export function linkDomain(source: ParamDescriptor | undefined, target: ParamDescriptor | undefined): Domain {
  if (!source || !target || source.unit !== target.unit) return 'normalised';
  if (isFrequency(source) && isFrequency(target)) return 'octaves';
  return isFrequency(source) === isFrequency(target) ? 'units' : 'normalised';
}

function toDomain(domain: Domain, p: ParamDescriptor | undefined, value: number) {
  if (domain === 'units') return value;
  if (domain === 'octaves') return Math.log2(Math.max(value, p!.min));
  return p ? toPosition(p, value) : Math.min(1, Math.max(0, value));
}

function fromDomain(domain: Domain, p: ParamDescriptor | undefined, value: number) {
  if (!p) return Math.min(1, Math.max(0, value));
  if (domain === 'normalised') return fromPosition(p, value);
  let result = domain === 'octaves' ? Math.pow(2, value) : value;
  if (p.kind !== 'continuous') result = Math.round(result);
  return Math.min(p.max, Math.max(p.min, result));
}

function centreOf(mapping: LinkMapping, domain: Domain, source: ParamDescriptor | undefined) {
  if (mapping.centre !== undefined) return mapping.centre;
  if (!source || domain === 'normalised') return 0.5;
  return (toDomain(domain, source, source.min) + toDomain(domain, source, source.max)) / 2;
}

function shapeAt(mapping: LinkMapping, t: number) {
  const curve = mapping.curve ?? [];
  return curve.length === 0 ? t : Math.min(1, Math.max(0, evaluateCurve(curve, t * LINK_CURVE_TICKS, t)));
}

/** The target's value for a source value: built-in values in their units, plugin values normalised. */
export function mapLinkValue(mapping: LinkMapping, source: ParamDescriptor | undefined, target: ParamDescriptor | undefined, value: number) {
  if (mapping.kind === 'range') {
    const x = toDomain('normalised', source, value);
    const inMin = mapping.inMin ?? 0;
    const width = (mapping.inMax ?? 1) - inMin;
    const t = Math.abs(width) > 1e-9 ? Math.min(1, Math.max(0, (x - inMin) / width)) : x >= inMin ? 1 : 0;
    const outMin = mapping.outMin ?? 0;
    return fromDomain('normalised', target, outMin + ((mapping.outMax ?? 1) - outMin) * shapeAt(mapping, t));
  }

  const domain = linkDomain(source, target);
  const x = toDomain(domain, source, value);
  const c = centreOf(mapping, domain, source);
  const amount = mapping.amount ?? (mapping.kind === 'scaled' ? 1 : 0);
  const y = mapping.kind === 'inverted' ? 2 * c - x : mapping.kind === 'offset' ? x + amount : mapping.kind === 'scaled' ? c + amount * (x - c) : x;
  return fromDomain(domain, target, y);
}

/** Whether a mapping works in the parameters' units: frequencies in octaves, else as their own numbers. */
export const mappingUnit = (link: ParamLink, project: Project | null | undefined) => {
  const source = endOf(project, link.source)?.param;
  const target = endOf(project, link.target)?.param;
  const domain = link.mapping.kind === 'range' ? 'normalised' : linkDomain(source, target);
  return domain === 'octaves' ? 'oct' : domain === 'units' ? (source?.unit ?? '') : '%';
};

/** What a mapping does, in a few words. */
export function describeMapping(mapping: LinkMapping, unit: string): string {
  const number = (v: number) => (unit === '%' ? `${Math.round(v * 100)} %` : `${+v.toFixed(2)}${unit ? ` ${unit}` : ''}`);
  switch (mapping.kind) {
    case 'identity':
      return 'the same';
    case 'inverted':
      return 'inverted';
    case 'offset':
      return `${(mapping.amount ?? 0) >= 0 ? '+' : '−'}${number(Math.abs(mapping.amount ?? 0))}`;
    case 'scaled':
      return `× ${+(mapping.amount ?? 1).toFixed(2)}`;
    case 'range':
      return `range ${Math.round((mapping.outMin ?? 0) * 100)}–${Math.round((mapping.outMax ?? 1) * 100)} %`;
  }
}

/** A parameter's name for a link list: the node's and the parameter's. */
export function endName(project: Project | null | undefined, address: ParamAddress) {
  const end = endOf(project, address);
  if (!end) return address.param;
  const s = useDawg.getState();
  const nodeName = end.node.plugin?.name ?? infoOf(end.track.id, end.node, s.nodeTypes, s.nodes)?.name ?? end.node.type;
  return `${nodeName} · ${end.param?.name ?? address.param}`;
}

/** The value a driven built-in parameter shows: its source's (as the source shows it) through the mapping, so a knob
    follows its source's automation as the engine does (L6). Undefined when nothing drives it or the source is a plugin. */
export function drivenValue(project: Project | null | undefined, target: ParamAddress, sourceValue: (address: ParamAddress, param: ParamDescriptor) => number): number | undefined {
  const link = linkDriving(project, target);
  if (!link || !link.active) return undefined;
  const source = endOf(project, link.source);
  const end = endOf(project, target);
  if (!source?.param || !end?.param) return undefined;
  const upstream = drivenValue(project, link.source, sourceValue) ?? sourceValue(link.source, source.param);
  return mapLinkValue(link.mapping, source.param, end.param, upstream);
}

export function addLink(source: ParamAddress, target: ParamAddress, mapping: LinkMapping = { kind: 'identity' }) {
  dispatch({ command: 'addLink', args: { source, target, mapping } });
}

export function setLink(id: number, changes: { mapping?: LinkMapping; active?: boolean }) {
  dispatch({ command: 'setLink', args: { id, ...changes } });
}

export function removeLinks(ids: number[]) {
  if (ids.length > 0) dispatch({ command: 'removeLink', args: { ids } });
}

export type LinkPreset = 'mirror' | 'mirrorInverted';

export function linkNodes(source: { trackId: number; nodeId: number }, target: { trackId: number; nodeId: number }, preset: LinkPreset) {
  dispatch({ command: 'linkNodes', args: { source, target, preset } });
}

/** Links grouped by the pair of nodes they join, for the Links editor (L16). */
export function groupByNodes(links: ParamLink[]) {
  const groups = new Map<string, ParamLink[]>();
  for (const link of links) {
    const key = `${link.source.trackId}:${link.source.nodeId}>${link.target.trackId}:${link.target.nodeId}`;
    groups.set(key, [...(groups.get(key) ?? []), link]);
  }
  return [...groups.values()];
}

/** The nodes of a project that a node can be linked to as a whole (L5): built-in ones of its type, not itself. */
export function linkableNodes(project: Project | null | undefined, node: GraphNode): Array<{ track: Track; node: GraphNode }> {
  if (!project || isPluginNode(node) || node.type === NODE_TYPES.faust) return [];
  const found: Array<{ track: Track; node: GraphNode }> = [];
  const tracks = [...project.trackOrder.map((id) => project.tracks[String(id)]), project.master].filter((t): t is Track => !!t);
  const visit = (track: Track, nodes: Record<string, GraphNode>) => {
    for (const other of Object.values(nodes)) {
      if (other.type === node.type && other.id !== node.id) found.push({ track, node: other });
      if (other.component) visit(track, other.component.graph.nodes);
      if (other.voice) visit(track, other.voice.nodes);
    }
  };
  for (const track of tracks) visit(track, track.graph.nodes);
  return found;
}
