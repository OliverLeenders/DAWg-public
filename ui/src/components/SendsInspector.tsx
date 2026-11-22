import { Plus, X } from 'lucide-react';
import { useState } from 'react';
import { type AutomationTarget, type Command, type Send, type Track } from '../bridge/protocol';
import { laneFor, playedPosition, positionToGain } from '../automation/automation';
import { touchProps } from '../automation/touch';
import { dispatch, useDawg } from '../store/store';
import { DragNumber } from './DragNumber';
import { nodeName } from './chain';
import { hasKeyDetector, keyableNodes } from '../dock/KeySelector';
import { gainTaper } from '../dock/TrackFader';
import { Button, IconButton, Power } from '../ui/controls';
import { Menu } from '../ui/overlays';

// A track's sends in the inspector (docs/design/groups-and-sends.md, R11): on/off, the return (or the node a key send
// keys; sidechain.md, K8), the level and pre or post fader for each, and a menu to add one to a return, a new return
// or a keyable node on another track.

export const sendTarget = (send: Send): AutomationTarget => ({ kind: 'send', nodeId: 0, param: String(send.id), name: '' });

/** A send's name: its return's, or for a key send the node it keys (docs/design/sidechain.md, K8). */
export function useSendName(send: Send) {
  return useDawg((s) => {
    const target = s.project?.tracks[String(send.target)];
    if (!target) return 'Return';
    const node = send.targetNode ? target.graph.nodes[String(send.targetNode)] : undefined;
    return node ? `${target.name} · ${nodeName(node, s.nodeTypes)} sidechain` : target.name;
  });
}

/** A send's level: the send's, or the automated value while automation plays (as track faders do). */
export function useSendLevel(track: Track, send: Send) {
  const played = useDawg((s) =>
    track.automationMode === 0 ? undefined : playedPosition(s.telemetry.automation, laneFor(track, sendTarget(send))?.id),
  );
  return played !== undefined ? positionToGain(played) : send.levelDb;
}

function SendRow({ track, send }: { track: Track; send: Send }) {
  const name = useSendName(send);
  const level = useSendLevel(track, send);
  const set = (args: { levelDb?: number; preFader?: boolean; active?: boolean }, gesture?: string) =>
    dispatch({ command: 'setSend', args: { trackId: track.id, sendId: send.id, ...args } }, gesture);

  return (
    <div className={`slot send-slot ${send.active ? '' : 'bypassed'}`}>
      <Power size="s" name={`The send to ${name}`} on={send.active} onChange={(active) => set({ active })} />
      <span className={`slot-name ${send.targetNode ? 'key-send' : ''}`} title={send.targetNode ? `${name} (feeds a sidechain input)` : name}>
        {name}
      </span>
      <DragNumber
        className="send-level"
        value={level}
        min={-96}
        max={12}
        quantum={0.1}
        taper={gainTaper}
        resetValue={0}
        title="Send level: drag, double-click for 0 dB"
        format={(v) => (v <= -96 ? '−∞' : `${v > 0 ? '+' : ''}${v.toFixed(1)} dB`)}
        onChange={(levelDb, gesture) => set({ levelDb }, gesture)}
        {...touchProps(track.id, sendTarget(send))}
      />
      <Button
        className={`send-mode ${send.preFader ? 'pre' : ''}`}
        title={send.preFader ? 'Pre-fader: before the track fader (click for post-fader)' : 'Post-fader: after the track fader and pan (click for pre-fader)'}
        onClick={() => set({ preFader: !send.preFader })}
      >
        {send.preFader ? 'Pre' : 'Post'}
      </Button>
      <IconButton
        size="s"
        title="Remove the send"
        onClick={() => dispatch({ command: 'removeSend', args: { trackId: track.id, sendId: send.id } })}
      >
        <X size={12} />
      </IconButton>
    </div>
  );
}

/** The Add send menu: a return, a keyable node on another track, or a new return. */
export function AddSendMenu({ track, x, y, onClose }: { track: Track; x: number; y: number; onClose: () => void }) {
  const project = useDawg((s) => s.project);
  const statuses = useDawg((s) => s.nodes);
  const types = useDawg((s) => s.nodeTypes);
  const sends = Object.values(track.sends ?? {});
  const returns = (project?.trackOrder ?? [])
    .map((id) => project?.tracks[String(id)])
    .filter((t): t is Track => t?.kind === 'return' && t.id !== track.id && !sends.some((s) => s.target === t.id && !s.targetNode));
  // Keyable nodes on other tracks this track doesn't key yet (sidechain.md, K8).
  const keys = project
    ? keyableNodes(project, track, statuses).filter(({ track: t, node }) => !sends.some((s) => s.target === t.id && s.targetNode === node.id))
    : [];

  return (
    <>
      <Menu onClose={onClose} at={{ x: Math.min(x, window.innerWidth - 220), y: Math.min(y, window.innerHeight - 200) }}>
        {returns.map((r) => (
          <button
            key={r.id}
            className="menu-item"
            onClick={() => {
              onClose();
              dispatch({ command: 'addSend', args: { trackId: track.id, target: r.id } });
            }}
          >
            {r.name}
          </button>
        ))}
        {returns.length > 0 && <div className="menu-separator" />}
        {keys.map(({ track: target, node }) => (
          <button
            key={`${target.id}:${node.id}`}
            className="menu-item"
            onClick={() => {
              onClose();
              const commands: Command[] = [{ command: 'addSend', args: { trackId: track.id, target: target.id, targetNode: node.id } }];
              if (hasKeyDetector(node.type))
                commands.push({ command: 'setNodeParam', args: { trackId: target.id, nodeId: node.id, param: 'detector', value: 1 } });
              dispatch({ command: 'batch', args: { label: 'Add sidechain send', commands } });
            }}
          >
            Sidechain → {target.name} · {nodeName(node, types)}
          </button>
        ))}
        {keys.length > 0 && <div className="menu-separator" />}
        <button
          className="menu-item"
          onClick={() => {
            onClose();
            dispatch({ command: 'addTrack', args: { kind: 'return', sendFrom: track.id } });
          }}
        >
          New return track
        </button>
      </Menu>
    </>
  );
}

export function SendsInspector({ track }: { track: Track }) {
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);
  const sends = Object.values(track.sends ?? {}).sort((a, b) => a.id - b.id);

  return (
    <div className="inspector" onClick={(e) => e.stopPropagation()}>
      <div className="inspector-label">Sends</div>
      {sends.map((send) => (
        <SendRow key={send.id} track={track} send={send} />
      ))}
      <button
        className="slot empty-slot"
        onClick={(e) => {
          const rect = e.currentTarget.getBoundingClientRect();
          setMenu({ x: rect.left, y: rect.bottom + 4 });
        }}
      >
        <Plus size={12} /> Add send
      </button>
      {menu && <AddSendMenu track={track} x={menu.x} y={menu.y} onClose={() => setMenu(null)} />}
    </div>
  );
}
