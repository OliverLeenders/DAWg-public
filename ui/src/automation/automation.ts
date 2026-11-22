import { infoOf } from '../graph/nodeInfo';
import type { AutomationLane, AutomationTarget, NodeTypeInfo, Track } from '../bridge/protocol';
import { nodeName } from '../components/chain';
import { formatParam, fromPosition, toPosition } from '../graph/params';
import { findNode } from '../graph/voice';
import { useDawg } from '../store/store';
import { drumParamLabel } from '../dock/drumSynth';
import { NODE_TYPES } from '../bridge/protocol';

// Automation helpers shared by the arrangement lanes, the headers and the inspector
// (docs/design/phase10-automation.md). Lane values are normalised 0..1 across the parameter's range.

export const MODE_LABELS = ['Off', 'R', 'T', 'L', 'W'] as const;
export const MODE_NAMES = ['Off', 'Read', 'Touch', 'Latch', 'Write'] as const;

/** The fader curve of automation and MIDI learn: square law, 0 dB at about 0.71. */
export const gainToPosition = (gainDb: number) => Math.sqrt(Math.pow(10, gainDb / 20) / 2);
export const positionToGain = (position: number) =>
  position <= 0 ? -96 : Math.max(-96, Math.min(12, 20 * Math.log10(2 * position * position)));

export const sameTarget = (a: AutomationTarget, b: AutomationTarget) =>
  a.kind === b.kind && a.nodeId === b.nodeId && a.param === b.param;

/** The track's lanes in display order. */
export const lanesOfTrack = (track: Track): AutomationLane[] =>
  (track.automationOrder ?? []).map((id) => track.automation?.[String(id)]).filter((l): l is AutomationLane => !!l);

export const laneFor = (track: Track, target: AutomationTarget) => lanesOfTrack(track).find((l) => sameTarget(l, target));

/** A track's send that a lane targets. */
const sendOf = (target: AutomationTarget, track: Track) => (target.kind === 'send' ? track.sends?.[target.param] : undefined);

/** Display name of a lane's parameter. */
export function laneLabel(target: AutomationTarget, track: Track, types: Record<string, NodeTypeInfo>) {
  if (target.kind === 'gain') return 'Volume';
  if (target.kind === 'pan') return 'Pan';
  if (target.kind === 'send') {
    const send = sendOf(target, track);
    const name = send ? useDawg.getState().project?.tracks[String(send.target)]?.name : undefined;
    return `Send → ${name ?? 'return'}`;
  }
  const node = findNode(track, target.nodeId);
  const owner = node ? nodeName(node, types) : 'Node';
  if (target.kind === 'plugin') return `${owner} · ${target.name || target.param}`;
  const param = node ? infoOf(track.id, node, types)?.params.find((p) => p.id === target.param) : undefined;
  // A Drum Synth's parameters go by their pad's name (docs/design/drum-synth.md, DR3).
  const name = param && node?.type === NODE_TYPES.drumSynth ? drumParamLabel(node, param) : param?.name;
  return `${owner} · ${name ?? target.param}`;
}

/** The parameter's manual value as a lane position (the neutral of a new lane). */
export function manualPosition(target: AutomationTarget, track: Track, types: Record<string, NodeTypeInfo>) {
  if (target.kind === 'gain') return gainToPosition(track.gainDb);
  if (target.kind === 'pan') return (track.pan + 1) / 2;
  if (target.kind === 'send') return gainToPosition(sendOf(target, track)?.levelDb ?? 0);
  if (target.kind === 'plugin') return 0.5;
  const node = findNode(track, target.nodeId);
  const param = node ? infoOf(track.id, node, types)?.params.find((p) => p.id === target.param) : undefined;
  return node && param ? toPosition(param, node.params[param.id] ?? param.default) : 0.5;
}

/** A lane position as text, in the parameter's own unit. */
export function formatPosition(target: AutomationTarget, track: Track, types: Record<string, NodeTypeInfo>, v: number) {
  if (target.kind === 'gain' || target.kind === 'send') {
    const db = positionToGain(v);
    return `${Math.abs(db) < 0.05 ? '0.0' : db.toFixed(1)} dB`;
  }
  if (target.kind === 'pan') {
    const pan = Math.round((v * 2 - 1) * 100);
    return pan === 0 ? 'C' : `${pan < 0 ? 'L' : 'R'}${Math.abs(pan)}`;
  }
  if (target.kind === 'plugin') return `${Math.round(v * 100)}%`;
  const node = findNode(track, target.nodeId);
  const param = node ? infoOf(track.id, node, types)?.params.find((p) => p.id === target.param) : undefined;
  return param ? formatParam(param, fromPosition(param, v)) : v.toFixed(2);
}

/** The value a lane plays now, if it plays (from telemetry). */
export const playedPosition = (automation: Record<string, number> | undefined, laneId: number | undefined) =>
  laneId === undefined ? undefined : automation?.[String(laneId)];
