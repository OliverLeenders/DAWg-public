import { useLayoutEffect } from 'react';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useNodeAnalysis, useNodeDrag, useResponse } from './analysis';
import { Knob } from './Knob';
import { Section } from './Section';
import { outputRange } from './SaturatorPanel';
import type { PanelProps } from './panels';
import { drawLevelDot, shaperDot } from './panelDraw';

// The clipper's panel (docs/design/clipper.md, C10): the static transfer curve, output over input from -1 to 1 (before
// Drive, so Drive moves the knee inwards), with the ceiling as dashed lines; beside it a meter of how far the input
// goes above the ceiling, and a dot for the input level on the curve, as on the compressor. Oversampling is a
// segmented control above, as on the saturator.

/** The meter's range: how far above the ceiling it shows, in dB. */
export const METER_RANGE_DB = 12;
const METER_WIDTH = 10;
const METER_GAP = 6;

export function ClipperPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const analysis = useNodeAnalysis(track.id, node.id);
  const meters = analysis?.meters as { over?: number; input?: number } | null | undefined;
  const over = meters?.over ?? 0;
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const params = Object.fromEntries(info.params.map((p) => [p.id, value(p.id)]));
  const curve = useResponse(node.type, params);
  const ceiling = ceilingLevel(value('ceiling'), value('output'));

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 + METER_WIDTH + METER_GAP || canvas.size.height < 16) return;
    const theme = readTheme();
    const s = Math.min(canvas.size.width - METER_WIDTH - METER_GAP, canvas.size.height);
    const range = Math.max(1, curve ? outputRange(curve.y) : 1);
    const toX = (x: number) => ((x + 1) / 2) * s;
    const toY = (y: number) => s - ((y / range + 1) / 2) * s;
    // The canvas fills the panel's column; the plot and the meter sit in its top left, each with its own border.
    ctx.clearRect(0, 0, canvas.size.width, canvas.size.height);
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, s, s);

    ctx.lineWidth = 1;
    ctx.strokeStyle = theme.border;
    for (const v of [-0.5, 0, 0.5]) {
      ctx.globalAlpha = v === 0 ? 1 : 0.45;
      ctx.beginPath();
      ctx.moveTo(Math.round(toX(v)) + 0.5, 0);
      ctx.lineTo(Math.round(toX(v)) + 0.5, s);
      ctx.moveTo(0, Math.round(toY(v * range)) + 0.5);
      ctx.lineTo(s, Math.round(toY(v * range)) + 0.5);
      ctx.stroke();
    }
    ctx.globalAlpha = 1;
    ctx.fillStyle = theme.textDim;
    ctx.font = font(theme, 'xs');
    ctx.fillText('in', s - 14, s - 6);
    if (range > 1) ctx.fillText(`out ±${range.toFixed(1)}`, 4, 12);

    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, s, s);
    ctx.clip();
    // Unity for reference, the ceiling, then the curve.
    ctx.setLineDash([3, 3]);
    ctx.strokeStyle = theme.textDim;
    ctx.beginPath();
    ctx.moveTo(toX(-1), toY(-1));
    ctx.lineTo(toX(1), toY(1));
    ctx.stroke();
    ctx.strokeStyle = over > 0 ? theme.accent : theme.textDim;
    ctx.beginPath();
    for (const level of [ceiling, -ceiling]) {
      ctx.moveTo(0, Math.round(toY(level)) + 0.5);
      ctx.lineTo(s, Math.round(toY(level)) + 0.5);
    }
    ctx.stroke();
    ctx.setLineDash([]);
    if (curve) {
      ctx.beginPath();
      curve.x.forEach((x, i) => (i === 0 ? ctx.moveTo(toX(x), toY(curve.y[i])) : ctx.lineTo(toX(x), toY(curve.y[i]))));
      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = 2;
      ctx.stroke();
    }

    // The signal: the input level on the curve, as on the compressor.
    const dot = shaperDot(curve, meters?.input);
    if (dot) drawLevelDot(ctx, theme, toX(dot.x), toY(dot.y));
    ctx.restore();
    ctx.lineWidth = 1;
    ctx.strokeStyle = theme.border;
    ctx.strokeRect(0.5, 0.5, s - 1, s - 1);

    // The meter: how far above the ceiling, filling from the bottom.
    const meterX = s + METER_GAP;
    ctx.fillStyle = theme.bg;
    ctx.fillRect(meterX, 0, METER_WIDTH, s);
    const filled = meterFraction(over) * s;
    ctx.fillStyle = theme.accent;
    ctx.fillRect(meterX, s - filled, METER_WIDTH, filled);
    ctx.strokeStyle = theme.border;
    ctx.beginPath();
    for (const db of [3, 6, 9]) {
      const y = Math.round(s - meterFraction(db) * s) + 0.5;
      ctx.moveTo(meterX, y);
      ctx.lineTo(meterX + METER_WIDTH, y);
    }
    ctx.stroke();
    ctx.strokeRect(meterX + 0.5, 0.5, METER_WIDTH - 1, s - 1);
  });

  const oversampling = param('oversampling');

  return (
    <div className="device-panel clipper-panel">
      <div className="panel-main">
        <div className="segmented" title="Oversampling: less aliasing and brighter highs, more latency and CPU">
          {oversampling.choices.map((label, i) => (
            <button
              key={label}
              className={Math.round(value('oversampling')) === i ? 'active' : ''}
              onClick={() => {
                edit.begin(['oversampling']);
                edit.set({ oversampling: i });
                edit.end();
              }}
            >
              {label}
            </button>
          ))}
        </div>
        <canvas ref={canvas.ref} className="panel-canvas" />
        <div className="panel-readout" title="How far the driven input goes above the ceiling">
          <span>{overReadout(over)}</span>
        </div>
      </div>
      <div className="panel-sections">
        <Section label="Clip">
          {['drive', 'ceiling', 'knee'].map((id) => (
            <Knob key={id} track={track} node={node} param={param(id)} />
          ))}
        </Section>
        <Section label="Output">
          {['output', 'mix'].map((id) => (
            <Knob key={id} track={track} node={node} param={param(id)} />
          ))}
        </Section>
      </div>
    </div>
  );
}

/** The output peak as a linear level: the ceiling scaled by Output (C5). */
export function ceilingLevel(ceilingDb: number, outputDb: number): number {
  return Math.pow(10, (ceilingDb + outputDb) / 20);
}

/** How full the meter is, 0 to 1, for a clip amount in dB. */
export function meterFraction(overDb: number): number {
  return Math.min(1, Math.max(0, overDb / METER_RANGE_DB));
}

/** The readout under the meter: the clip amount, or a dash when nothing clips. */
export function overReadout(overDb: number): string {
  return overDb < 0.05 ? 'Clip —' : `Clip ${overDb.toFixed(1)} dB`;
}
