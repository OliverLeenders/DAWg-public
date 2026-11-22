import { Plus } from 'lucide-react';
import { useRef, useState } from 'react';
import type { Send, Track } from '../bridge/protocol';
import { touch } from '../automation/touch';
import { AddSendMenu, sendTarget, useSendLevel, useSendName } from '../components/SendsInspector';
import { dbToFader, faderToDb } from '../dock/TrackFader';
import { dispatch, newGestureId } from '../store/store';
import { Menu } from '../ui/overlays';

// A strip's sends (docs/design/mixer.md, X3): a small knob per send for its level (drag, double-click for 0 dB) and
// its return's name, which switches it on and off; pre or post fader and removing it are in its context menu, and
// "+" adds one. Sidechain sends are purple and named by the node they key. Narrow strips show the knobs only (X5).

const PIXELS = 160; // the knob's full travel per 160 px of drag, as every knob's
const FINE = 10; // Shift: ten times finer, as everywhere (components/dragScale.ts)
const SWEEP = 270;

const formatLevel = (db: number) => (db <= -96 ? '−∞ dB' : `${db > 0 ? '+' : ''}${db.toFixed(1)} dB`);

export function MixerSends({ track, narrow }: { track: Track; narrow: boolean }) {
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);
  const sends = Object.values(track.sends ?? {}).sort((a, b) => a.id - b.id);

  return (
    <div className="strip-section strip-sends">
      {sends.map((send) => (
        <SendRow key={send.id} track={track} send={send} narrow={narrow} />
      ))}
      <button
        className="insert-add"
        title="Add a send"
        onClick={(e) => {
          const rect = e.currentTarget.getBoundingClientRect();
          setMenu({ x: rect.left, y: rect.bottom + 4 });
        }}
      >
        <Plus size={11} />
        {!narrow && ' Send'}
      </button>
      {menu && <AddSendMenu track={track} x={menu.x} y={menu.y} onClose={() => setMenu(null)} />}
    </div>
  );
}

function SendRow({ track, send, narrow }: { track: Track; send: Send; narrow: boolean }) {
  const name = useSendName(send);
  const level = useSendLevel(track, send);
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);
  const set = (args: { levelDb?: number; preFader?: boolean; active?: boolean }, gesture?: string) =>
    dispatch({ command: 'setSend', args: { trackId: track.id, sendId: send.id, ...args } }, gesture);
  const description = `${name}: ${formatLevel(level)}, ${send.preFader ? 'pre' : 'post'}-fader${send.active ? '' : ', off'}`;

  return (
    <div
      className={`send-row ${send.active ? '' : 'off'} ${send.targetNode ? 'key-send' : ''}`}
      title={narrow ? description : undefined}
      onContextMenu={(e) => {
        e.preventDefault();
        setMenu({ x: e.clientX, y: e.clientY });
      }}
    >
      <SendKnob track={track} send={send} level={level} title={description} onChange={(levelDb, gesture) => set({ levelDb }, gesture)} />
      {!narrow && (
        <button className="send-name" title={`${description} (click to switch ${send.active ? 'off' : 'on'})`} onClick={() => set({ active: !send.active })}>
          {name}
        </button>
      )}
      {!narrow && send.preFader && <span className="send-pre">Pre</span>}
      {menu && (
        <>
          <Menu
            onClose={() => setMenu(null)}
            at={{ x: Math.min(menu.x, window.innerWidth - 200), y: Math.min(menu.y, window.innerHeight - 160) }}
            onClick={() => setMenu(null)}
          >
            <div className="menu-heading">{name}</div>
            <button className="menu-item" onClick={() => set({ active: !send.active })}>
              {send.active ? 'Switch off' : 'Switch on'}
            </button>
            <button className="menu-item" onClick={() => set({ preFader: !send.preFader })}>
              {send.preFader ? 'Post-fader' : 'Pre-fader'}
            </button>
            <button className="menu-item danger" onClick={() => dispatch({ command: 'removeSend', args: { trackId: track.id, sendId: send.id } })}>
              Remove
            </button>
          </Menu>
        </>
      )}
    </div>
  );
}

/** A send level knob on the fader's curve: drag up or down (Shift: fine), double-click for 0 dB. */
function SendKnob({
  track,
  send,
  level,
  title,
  onChange,
}: {
  track: Track;
  send: Send;
  level: number;
  title: string;
  onChange: (db: number, gesture?: string) => void;
}) {
  const drag = useRef<{ y: number; position: number; fine: boolean; last?: number; gesture: string } | null>(null);
  const position = dbToFader(level);
  const angle = -SWEEP / 2 + position * SWEEP;
  const rad = (angle * Math.PI) / 180;
  const round = (db: number) => Math.round(Math.max(-96, Math.min(12, db)) * 10) / 10;

  return (
    <svg
      className="send-knob"
      width={18}
      height={18}
      viewBox="0 0 18 18"
      aria-label={title}
      onPointerDown={(e) => {
        if (e.button !== 0) return;
        e.currentTarget.setPointerCapture(e.pointerId);
        drag.current = { y: e.clientY, position, fine: e.shiftKey, gesture: newGestureId() };
        touch(track.id, sendTarget(send), true);
      }}
      onPointerMove={(e) => {
        const d = drag.current;
        if (!d) return;
        // Pressing or letting go of Shift carries on from where the knob is.
        if (e.shiftKey !== d.fine) Object.assign(d, { y: e.clientY, position: d.last ?? d.position, fine: e.shiftKey });
        const next = Math.min(1, Math.max(0, d.position + (d.y - e.clientY) / (d.fine ? PIXELS * FINE : PIXELS)));
        d.last = next;
        onChange(round(faderToDb(next)), d.gesture);
      }}
      onPointerUp={() => {
        if (!drag.current) return;
        drag.current = null;
        touch(track.id, sendTarget(send), false);
      }}
      onDoubleClick={() => onChange(0)}
      onWheel={(e) => onChange(round((level <= -96 ? -60 : level) - Math.sign(e.deltaY) * 0.5))}
    >
      <title>{title}</title>
      <circle cx={9} cy={9} r={7.5} className="send-knob-body" />
      <line x1={9} y1={9} x2={9 + 6 * Math.sin(rad)} y2={9 - 6 * Math.cos(rad)} className="send-knob-pointer" />
    </svg>
  );
}
