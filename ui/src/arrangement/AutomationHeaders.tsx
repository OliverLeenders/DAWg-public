import { infoOf } from '../graph/nodeInfo';
import { X } from 'lucide-react';
import { useEffect, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { NODE_TYPES, type AutomationTarget, type Track } from '../bridge/protocol';
import { formatPosition, laneFor, laneLabel, MODE_LABELS, MODE_NAMES, playedPosition } from '../automation/automation';
import { nodeName } from '../components/chain';
import { drumParamLabel } from '../dock/drumSynth';
import { paramApplies } from '../graph/lfo';
import { dispatch, newGestureId, useDawg } from '../store/store';
import type { AutomationRow } from './layout';
import { indentStyle, Spines } from './Spines';
import { IconButton, Power } from '../ui/controls';
import { Popover } from '../ui/overlays';

// Automation in the track headers (docs/design/phase10-automation.md, section 5): the mode button, the lane headers
// under an expanded track, and the picker that adds lanes.

/** Shows a parameter's lane: reveals an existing one, or adds it (which expands the track). */
export function showAutomation(track: Track, target: AutomationTarget) {
  if (laneFor(track, target)) dispatch({ command: 'setTrackAutomation', args: { id: track.id, expanded: true } });
  else dispatch({ command: 'addAutomationLane', args: { trackId: track.id, target } });
}

/** Cycles Off, Read, Touch, Latch, Write. */
export function AutomationModeButton({ track }: { track: Track }) {
  const mode = track.automationMode ?? 1;
  return (
    <button
      className={`switch automation-mode m${mode}`}
      title={`Automation: ${MODE_NAMES[mode]} (click to change)`}
      onClick={(e) => {
        e.stopPropagation();
        dispatch({ command: 'setTrackAutomation', args: { id: track.id, mode: ((mode + 1) % 5) as 0 | 1 | 2 | 3 | 4 } });
      }}
    >
      {MODE_LABELS[mode]}
    </button>
  );
}

/** The parameters a track can automate: its fader and pan, node parameters and plugin parameters. */
export function AutomationPicker({ track, x, y, onClose }: { track: Track; x: number; y: number; onClose: () => void }) {
  const types = useDawg((s) => s.nodeTypes);
  const [plugins, setPlugins] = useState<Record<string, Array<{ id: string; name: string }>>>({});
  const [filter, setFilter] = useState('');
  const nodes = Object.values(track.graph.nodes).filter((n) => types[n.type]?.userEditable);

  useEffect(() => {
    for (const node of nodes)
      if (node.plugin)
        bridge
          .call('plugin.parameters', { trackId: track.id, nodeId: node.id })
          .then((list) => setPlugins((p) => ({ ...p, [String(node.id)]: list })))
          .catch(() => {});
    // Fetch once per opening.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const entries: Array<{ key: string; group: string; label: string; target: AutomationTarget }> = [
    { key: 'gain', group: 'Track', label: 'Volume', target: { kind: 'gain', nodeId: 0, param: 'gain', name: '' } },
    { key: 'pan', group: 'Track', label: 'Pan', target: { kind: 'pan', nodeId: 0, param: 'pan', name: '' } },
  ];

  // Send levels (docs/design/groups-and-sends.md, R8).
  const tracks = useDawg.getState().project?.tracks ?? {};
  for (const send of Object.values(track.sends ?? {}))
    entries.push({
      key: `send:${send.id}`,
      group: 'Sends',
      label: `Send → ${tracks[String(send.target)]?.name ?? 'return'}`,
      target: { kind: 'send', nodeId: 0, param: String(send.id), name: '' },
    });

  for (const node of nodes) {
    const group = nodeName(node, types);
    if (node.plugin)
      for (const p of plugins[String(node.id)] ?? [])
        entries.push({ key: `${node.id}:${p.id}`, group, label: p.name, target: { kind: 'plugin', nodeId: node.id, param: p.id, name: p.name } });
    else
      for (const p of (infoOf(track.id, node, types)?.params ?? []).filter((p) => p.automatable && paramApplies(node, p.id)))
        entries.push({
          key: `${node.id}:${p.id}`,
          group,
          label: node.type === NODE_TYPES.drumSynth ? drumParamLabel(node, p) : p.name,
          target: { kind: 'node', nodeId: node.id, param: p.id, name: '' },
        });

    // A Voice Instrument's voice nodes: a lane moves the parameter in every voice (phase12-voice-scope.md, V11).
    for (const voiceNode of Object.values(node.voice?.nodes ?? {}).filter((n) => types[n.type]?.userEditable))
      for (const p of (infoOf(track.id, voiceNode, types)?.params ?? []).filter((p) => p.automatable))
        entries.push({
          key: `${voiceNode.id}:${p.id}`,
          group: `${group} › ${nodeName(voiceNode, types)}`,
          label: p.name,
          target: { kind: 'node', nodeId: voiceNode.id, param: p.id, name: '' },
        });
  }

  const f = filter.trim().toLowerCase();
  const shown = entries.filter((e) => !f || `${e.group} ${e.label}`.toLowerCase().includes(f));

  return (
    <>
      <Popover onClose={onClose} at={{ x: Math.min(x, window.innerWidth - 320), y: Math.min(y, window.innerHeight - 360) }} className="plugin-picker automation-picker">
        <input
          className="search"
          autoFocus
          placeholder="Automate a parameter"
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
          onKeyDown={(e) => e.key === 'Escape' && onClose()}
        />
        <div className="picker-list">
          {shown.map((e) => {
            const exists = laneFor(track, e.target) !== undefined;
            return (
              <button
                key={e.key}
                className="picker-item"
                onClick={() => {
                  onClose();
                  showAutomation(track, e.target);
                }}
              >
                <span className="plugin-name">{e.label}</span>
                <span className="plugin-vendor">{e.group}</span>
                <span className="plugin-format">{exists ? 'Shown' : ''}</span>
              </button>
            );
          })}
          {shown.length === 0 && <p className="empty">Nothing matches.</p>}
        </div>
      </Popover>
    </>
  );
}

/** One automation lane's header, aligned with its row. */
export function AutomationHeader({ row, top, groups = [] }: { row: AutomationRow; top: number; groups?: Track[] }) {
  const types = useDawg((s) => s.nodeTypes);
  const played = useDawg((s) => playedPosition(s.telemetry.automation, row.lane.id));
  const resize = useRef<{ startY: number; startHeight: number; gesture: string } | null>(null);
  const { track, lane } = row;

  return (
    <div className={`automation-header ${lane.active ? '' : 'inactive'}`} style={{ top, height: row.height, ...indentStyle(groups) }}>
      <Spines groups={groups} />
      <div className="automation-header-main">
        <span className="automation-name" title={laneLabel(lane, track, types)}>
          {laneLabel(lane, track, types)}
        </span>
        {played !== undefined && <span className="automation-value">{formatPosition(lane, track, types, played)}</span>}
      </div>
      <Power
        size="s"
        name="The lane"
        on={lane.active}
        onChange={(active) => dispatch({ command: 'setAutomationLane', args: { trackId: track.id, laneId: lane.id, active } })}
      />
      <IconButton
        size="s"
        title="Remove this lane and its points"
        onClick={() => dispatch({ command: 'removeAutomationLane', args: { trackId: track.id, laneId: lane.id } })}
      >
        <X size={12} />
      </IconButton>
      <div
        className="track-header-resize"
        title="Drag to resize the lane"
        onPointerDown={(e) => {
          e.stopPropagation();
          e.currentTarget.setPointerCapture(e.pointerId);
          resize.current = { startY: e.clientY, startHeight: lane.height, gesture: newGestureId() };
        }}
        onPointerMove={(e) => {
          const r = resize.current;
          if (!r) return;
          const next = Math.round(r.startHeight + e.clientY - r.startY);
          if (next !== lane.height)
            dispatch({ command: 'setAutomationLane', args: { trackId: track.id, laneId: lane.id, height: next } }, r.gesture);
        }}
        onPointerUp={() => (resize.current = null)}
      />
    </div>
  );
}
