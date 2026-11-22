import { useLayoutEffect } from 'react';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useNodeAnalysis, useNodeDrag, useResponse } from './analysis';
import { Knob } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';
import { drawLevelDot, shaperDot } from './panelDraw';
import { Segmented } from '../ui/controls';

// The saturator's panel (docs/design/saturator.md, W9): the static transfer curve, output over input from -1 to 1, so
// the bias shows as the curve's asymmetry; the oversampling as a segmented control; drive, bias, output and mix as
// knobs. Drive keeps small signals at unity, so loud ones come out quieter as it rises (at most 1 / gain): the output
// axis follows the curve's peak, named in the corner, so the shape stays readable at any drive. A dot marks the input
// level on the curve, as on the compressor.

export function SaturatorPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const params = Object.fromEntries(info.params.map((p) => [p.id, value(p.id)]));
  const curve = useResponse(node.type, params);
  const level = (useNodeAnalysis(track.id, node.id)?.meters as { input?: number } | null | undefined)?.input;

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 || canvas.size.height < 16) return;
    const theme = readTheme();
    const s = Math.min(canvas.size.width, canvas.size.height);
    const range = curve ? outputRange(curve.y) : 1;
    const toX = (x: number) => ((x + 1) / 2) * s;
    const toY = (y: number) => s - ((y / range + 1) / 2) * s;
    ctx.fillStyle = theme.bgRaised;
    ctx.fillRect(0, 0, canvas.size.width, canvas.size.height);
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
    ctx.fillText(`out ±${range < 1 ? range.toFixed(2) : range.toFixed(1)}`, 4, 12);

    // Unity for reference, then the curve, both clipped to the plot.
    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, s, s);
    ctx.clip();
    ctx.setLineDash([3, 3]);
    ctx.strokeStyle = theme.textDim;
    ctx.beginPath();
    ctx.moveTo(toX(-1), toY(-1));
    ctx.lineTo(toX(1), toY(1));
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
    const dot = shaperDot(curve, level);
    if (dot) drawLevelDot(ctx, theme, toX(dot.x), toY(dot.y));
    ctx.restore();
  });

  const oversampling = param('oversampling');

  return (
    <div className="device-panel saturator-panel">
      <div className="panel-main">
        <Segmented
          label="Oversampling: less aliasing and brighter highs, more latency and CPU"
          options={oversampling.choices.map((label, i) => ({ value: i, label }))}
          value={Math.round(value('oversampling'))}
          onChange={(i) => {
            edit.begin(['oversampling']);
            edit.set({ oversampling: i });
            edit.end();
          }}
        />
        <canvas ref={canvas.ref} className="panel-canvas" />
      </div>
      <div className="panel-sections">
        <Section label="Shape">
          {['drive', 'bias'].map((id) => (
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

/** The output axis's half-range: the curve's peak, rounded up to a step of 0.05 below 1 and 0.5 above, so it doesn't
    jump with every small change. */
export function outputRange(y: number[]): number {
  const peak = Math.max(...y.map(Math.abs));
  const step = peak < 1 ? 0.05 : 0.5;
  return Math.max(step, Math.ceil(peak / step - 1e-9) * step);
}
