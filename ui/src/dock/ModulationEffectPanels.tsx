import { Music } from 'lucide-react';
import { glyphFor } from '../components/Glyphs';
import { useLayoutEffect } from 'react';
import { NODE_TYPES } from '../bridge/protocol';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useNodeAnalysis, useNodeDrag } from './analysis';
import { Knob } from './Knob';
import { REFERENCE_MS, sweepCurves, sweepRange } from './modulationEffects';
import { Section } from './Section';
import type { PanelProps } from './panels';

// The Chorus, Flanger and Phaser panels (docs/design/modulation-effects.md, the panels): the sweep over one LFO cycle,
// the left channel's taps in the accent colour and the right's in the second, with the LFO's current phase marked;
// above it Sync, the cycle when synced, and the shape. The controls sit beside it in two rows of sections (one row in
// a short dock), and the card's measured min-content width keeps them off the graph.

interface Layout {
  /** The middle section: its label and knobs. */
  sweep: { label: string; ids: string[] };
  /** A choice shown as a select in the middle section (the Phaser's stages). */
  select?: { id: string; label: string };
  /** A toggle in the header (the Flanger's through-zero). */
  toggle?: { id: string; label: string; title: string };
}

const LAYOUTS: Record<string, Layout> = {
  [NODE_TYPES.chorus]: { sweep: { label: 'Delay', ids: ['delay', 'depth', 'voices'] } },
  [NODE_TYPES.flanger]: {
    sweep: { label: 'Delay', ids: ['delay', 'depth'] },
    toggle: { id: 'throughZero', label: 'Through-zero', title: 'Sweep through zero against a 5 ms reference (adds 5 ms of latency)' },
  },
  [NODE_TYPES.phaser]: { sweep: { label: 'Sweep', ids: ['centre', 'depth'] }, select: { id: 'stages', label: 'Stages' } },
};

function ModulationEffectPanel({ track, node, info }: PanelProps) {
  const layout = LAYOUTS[node.type];
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const analysis = useNodeAnalysis(track.id, node.id);
  const phase = (analysis?.meters as { phase?: number } | null | undefined)?.phase;
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const on = (id: string) => value(id) >= 0.5;
  const set = (id: string, v: number) => {
    edit.begin([id]);
    edit.set({ [id]: v });
    edit.end();
  };
  const params = Object.fromEntries(info.params.map((p) => [p.id, value(p.id)]));
  const phaser = node.type === NODE_TYPES.phaser;
  const throughZero = node.type === NODE_TYPES.flanger && on('throughZero');

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 || canvas.size.height < 16) return;
    const theme = readTheme();
    const { width, height } = canvas.size;
    const curves = sweepCurves(node.type, params);
    const [low, high] = sweepRange(node.type, curves);
    const pad = 4;
    const toY = phaser
      ? (v: number) => height - pad - (Math.log(v / low) / Math.log(high / low)) * (height - 2 * pad)
      : (v: number) => height - pad - ((v - low) / (high - low)) * (height - 2 * pad);
    const toX = (i: number, points: number) => (i / points) * width;

    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);
    ctx.strokeStyle = theme.border;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (const quarter of [0.25, 0.5, 0.75]) {
      ctx.moveTo(Math.round(quarter * width) + 0.5, 0);
      ctx.lineTo(Math.round(quarter * width) + 0.5, height);
    }
    ctx.stroke();

    if (throughZero) {
      ctx.setLineDash([3, 3]);
      ctx.strokeStyle = theme.textDim;
      ctx.beginPath();
      ctx.moveTo(0, Math.round(toY(REFERENCE_MS)) + 0.5);
      ctx.lineTo(width, Math.round(toY(REFERENCE_MS)) + 0.5);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    // The right channel first, so the left one stays on top where they meet.
    for (const curve of [...curves].reverse()) {
      const points = curve.values.length - 1;
      ctx.beginPath();
      curve.values.forEach((v, i) => (i === 0 ? ctx.moveTo(toX(i, points), toY(v)) : ctx.lineTo(toX(i, points), toY(v))));
      ctx.strokeStyle = curve.channel === 0 ? theme.accent : theme.signalAudio;
      ctx.lineWidth = 2;
      ctx.stroke();
    }

    if (phase !== undefined) {
      ctx.strokeStyle = theme.text;
      ctx.globalAlpha = 0.6;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(Math.round(phase * width) + 0.5, 0);
      ctx.lineTo(Math.round(phase * width) + 0.5, height);
      ctx.stroke();
      ctx.globalAlpha = 1;
    }

    ctx.fillStyle = theme.textDim;
    ctx.font = font(theme, 'xs');
    const label = (v: number) => (phaser ? (v >= 1000 ? `${(v / 1000).toFixed(1)} kHz` : `${Math.round(v)} Hz`) : `${v.toFixed(v < 10 ? 1 : 0)} ms`);
    ctx.fillText(label(high), 4, 12);
    ctx.fillText(label(low), 4, height - 6);
    ctx.textAlign = 'right';
    ctx.fillStyle = theme.accent;
    ctx.fillText('L', width - 16, 12);
    ctx.fillStyle = theme.signalAudio;
    ctx.fillText('R', width - 4, 12);
    ctx.textAlign = 'left';
  });

  const cycles = param('cycle').choices;
  const shapes = param('shape').choices;
  const knobs = (ids: string[]) => ids.map((id) => <Knob key={id} track={track} node={node} param={param(id)} />);

  return (
    <div className="device-panel modfx-panel">
      <div className="panel-main">
        <div className="modfx-header">
          <div className="segmented toggles">
            <button
              className={on('sync') ? 'active' : ''}
              title="Sync: the LFO's cycle follows the tempo"
              aria-label="Sync"
              onClick={() => set('sync', on('sync') ? 0 : 1)}
            >
              <Music size={13} />
            </button>
            {layout.toggle && (
              <button className={on(layout.toggle.id) ? 'active' : ''} title={layout.toggle.title} onClick={() => set(layout.toggle!.id, on(layout.toggle!.id) ? 0 : 1)}>
                {layout.toggle.label}
              </button>
            )}
          </div>
          {on('sync') && (
            <label className="field">
              <span>Cycle</span>
              <select value={Math.round(value('cycle'))} title="The LFO's cycle as a note length" onChange={(e) => set('cycle', Number(e.target.value))}>
                {cycles.map((name, i) => (
                  <option key={name} value={i}>
                    {name}
                  </option>
                ))}
              </select>
            </label>
          )}
          <div className="segmented" title="The LFO's shape">
            {shapes.map((name, i) => (
              <button key={name} className={Math.round(value('shape')) === i ? 'active' : ''} title={name} aria-label={name} onClick={() => set('shape', i)}>
                {glyphFor(name) ?? name}
              </button>
            ))}
          </div>
        </div>
        <canvas ref={canvas.ref} className="panel-canvas" />
      </div>
      <div className="modfx-sections">
        <Section label="LFO">{knobs(on('sync') ? ['spread'] : ['rate', 'spread'])}</Section>
        <Section label="Output">{knobs(['feedback', 'mix'])}</Section>
        <Section label={layout.sweep.label}>
          {knobs(layout.sweep.ids)}
          {layout.select && (
            <label className="field modfx-select">
              <span>{layout.select.label}</span>
              <select value={Math.round(value(layout.select.id))} onChange={(e) => set(layout.select!.id, Number(e.target.value))}>
                {param(layout.select.id).choices.map((name, i) => (
                  <option key={name} value={i}>
                    {name}
                  </option>
                ))}
              </select>
            </label>
          )}
        </Section>
      </div>
    </div>
  );
}

export const ChorusPanel = ModulationEffectPanel;
export const FlangerPanel = ModulationEffectPanel;
export const PhaserPanel = ModulationEffectPanel;
