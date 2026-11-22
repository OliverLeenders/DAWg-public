import { ArrowLeftRight, Link2, Music } from 'lucide-react';
import { useLayoutEffect } from 'react';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useDawg } from '../store/store';
import { tempoMapOf } from '../time/tempoMap';
import { useNodeDrag } from './analysis';
import { Knob } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';

// The delay's panel (docs/design/sampler-midi-delay-limiter.md, section 1): its echoes on a time line, the left channel
// above the middle and the right below, with the times as notes or as knobs.

/** The longest time the delay holds; the engine clamps to it too. */
const MAX_SECONDS = 8;
const MIN_LEVEL = 0.01;

/** A note value's length in beats (quarter notes), from its name: "1/8" an eighth, "1/8." dotted, "1/8T" triplet. */
export function noteBeats(name: string): number {
  const [top, bottom] = name.replace(/[.T]$/, '').split('/').map(Number);
  const scale = name.endsWith('.') ? 1.5 : name.endsWith('T') ? 2 / 3 : 1;
  return ((4 * top) / bottom) * scale;
}

export interface Echo {
  seconds: number;
  level: number;
  channel: 0 | 1;
}

/** The echoes of one impulse in the mono input, loudest first in time: in ping-pong they cross from side to side. */
export function delayEchoes(left: number, right: number, feedback: number, pingPong: boolean): Echo[] {
  const echoes: Echo[] = [];
  const paths: Array<{ channel: 0 | 1; at: number; level: number }> = pingPong
    ? [{ channel: 0, at: left, level: 1 }]
    : [
        { channel: 0, at: left, level: 1 },
        { channel: 1, at: right, level: 1 },
      ];

  for (const path of paths) {
    let { channel, at, level } = path;
    while (level >= MIN_LEVEL && at <= MAX_SECONDS && feedback > 0 && echoes.length < 200) {
      echoes.push({ seconds: at, level, channel });
      if (pingPong) channel = channel === 0 ? 1 : 0;
      at += channel === 0 ? left : right;
      level *= feedback;
    }
    if (feedback <= 0) echoes.push({ seconds: at, level, channel });
  }
  return echoes.sort((a, b) => a.seconds - b.seconds);
}

export function DelayPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const on = (id: string) => value(id) >= 0.5;
  const set = (id: string, v: number) => {
    edit.begin([id]);
    edit.set({ [id]: v });
    edit.end();
  };

  const bpm = useDawg((s) => (s.project ? tempoMapOf(s.project).bpmAt(s.telemetry.positionBeats * 15360) : 120));
  const synced = on('sync');
  const linked = on('link');
  const notes = param('noteL').choices;
  const seconds = (note: string, time: string) =>
    Math.min(MAX_SECONDS, synced ? (noteBeats(notes[Math.round(value(note))]) * 60) / bpm : value(time) / 1000);
  const left = seconds('noteL', 'timeL');
  const right = linked ? left : seconds('noteR', 'timeR');
  // The echoes at the Wet level; the dry sound is always at full.
  const echoes = delayEchoes(left, right, value('feedback') / 100, on('pingPong')).map((e) => ({ ...e, level: e.level * (value('mix') / 100) }));

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 || canvas.size.height < 16) return;
    const theme = readTheme();
    const { width, height } = canvas.size;
    const middle = height / 2;
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);

    const span = Math.max(0.5, Math.min(MAX_SECONDS, (echoes.at(-1)?.seconds ?? 0.5) * 1.05));
    const toX = (s: number) => (s / span) * width;
    ctx.strokeStyle = theme.border;
    ctx.beginPath();
    ctx.moveTo(0, Math.round(middle) + 0.5);
    ctx.lineTo(width, Math.round(middle) + 0.5);
    ctx.stroke();

    ctx.fillStyle = theme.textDim;
    ctx.font = font(theme, 'xs');
    ctx.fillText('L', 4, 12);
    ctx.fillText('R', 4, height - 5);
    ctx.textAlign = 'right';
    ctx.fillText(span < 1 ? `${Math.round(span * 1000)} ms` : `${span.toFixed(1)} s`, width - 4, 12);
    ctx.textAlign = 'left';

    // The dry sound at the start, then each echo as a stem as tall as it is loud.
    ctx.fillStyle = theme.textDim;
    ctx.fillRect(0, 2, 2, height - 4);
    ctx.fillStyle = theme.accent;
    for (const echo of echoes) {
      const size = echo.level * (middle - 4);
      ctx.fillRect(toX(echo.seconds), echo.channel === 0 ? middle - size : middle, 2, size);
    }
  });

  const toggle = (id: string, label: string, title: string, icon: React.ReactNode) => (
    <button className={on(id) ? 'active' : ''} title={`${label}: ${title}`} aria-label={label} onClick={() => set(id, on(id) ? 0 : 1)}>
      {icon}
    </button>
  );
  const noteSelect = (id: string, label: string) => (
    <label className="field">
      <span>{label}</span>
      <select value={Math.round(value(id))} title={`${label} delay as a note length`} onChange={(e) => set(id, Number(e.target.value))}>
        {notes.map((name, i) => (
          <option key={name} value={i}>
            {name}
          </option>
        ))}
      </select>
    </label>
  );

  return (
    <div className="device-panel delay-panel">
      <div className="panel-main">
        <div className="delay-header">
          <div className="segmented toggles">
            {toggle('sync', 'Sync', 'times follow the tempo', <Music size={13} />)}
            {toggle('link', 'Link', 'the right time follows the left', <Link2 size={13} />)}
            {toggle('pingPong', 'Ping-pong', 'echoes cross from side to side', <ArrowLeftRight size={13} />)}
          </div>
          {synced && noteSelect('noteL', linked ? 'Time' : 'Left')}
          {synced && !linked && noteSelect('noteR', 'Right')}
        </div>
        <canvas ref={canvas.ref} className="panel-canvas" />
      </div>
      <div className="panel-sections">
        {!synced && (
          <Section label="Time">
            <Knob track={track} node={node} param={param('timeL')} />
            {!linked && <Knob track={track} node={node} param={param('timeR')} />}
          </Section>
        )}
        <Section label="Echoes">
          {['feedback', 'lowCut', 'highCut'].map((id) => (
            <Knob key={id} track={track} node={node} param={param(id)} />
          ))}
        </Section>
        <Section label="Output">
          <Knob track={track} node={node} param={param('mix')} />
        </Section>
      </div>
    </div>
  );
}
