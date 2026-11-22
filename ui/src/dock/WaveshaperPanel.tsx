import { useLayoutEffect } from 'react';
import type { Command, CurvePoint } from '../bridge/protocol';
import { dispatch } from '../store/store';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useNodeAnalysis, useNodeDrag, useResponse } from './analysis';
import { Knob } from './Knob';
import { Section } from './Section';
import { outputRange } from './SaturatorPanel';
import { WaveshaperCurveEditor } from './WaveshaperCurveEditor';
import {
  ASYMMETRIC_PRESETS,
  DRAWN,
  drawnDot,
  drawnPoints,
  inputRange,
  isSymmetric,
  mirrored,
  PLOT_PAD,
  plotSize,
  positiveHalf,
  symmetricReach,
  SYMMETRIC_PRESETS,
  type WaveshaperMeters,
} from './waveshaper';
import { drawLevelDot, MIN_DOT_LEVEL, shaperDot } from './panelDraw';
import type { PanelProps } from './panels';
import { Button } from '../ui/controls';

// The waveshaper's panel (docs/design/waveshaper.md, S12): the Shape and the oversampling above a square plot, the
// knobs in their own column beside it. The plot shows the static transfer curve, output over input from -1 to 1
// before Drive; with the Drawn shape it shows the drawn curve over the shaper's input instead, with the range a
// full-scale input reaches marked, and is its editor: in the symmetric mode only the positive quadrant, filling the
// plot. The plot is the largest square its box holds, so nothing overlaps it at any card width or dock height. A dot
// marks the input level on the curve, as on the compressor.

export function WaveshaperPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const params = Object.fromEntries(info.params.map((p) => [p.id, value(p.id)]));
  const drawn = Math.round(value('shape')) === DRAWN;
  const curve = useResponse(node.type, params);
  const meters = useNodeAnalysis(track.id, node.id)?.meters as WaveshaperMeters | null | undefined;
  const size = plotSize(canvas.size.width, canvas.size.height);
  const symmetric = isSymmetric(params);
  const shown = drawn && symmetric;
  const band: [number, number] = shown ? [0, symmetricReach(value('drive'), value('bias'))] : inputRange(value('drive'), value('bias'));

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx) return;
    ctx.clearRect(0, 0, canvas.size.width, canvas.size.height);
    if (size < 2 * PLOT_PAD + 8) return;
    const theme = readTheme();
    const s = size;
    const range = drawn ? 1 : Math.max(1, curve ? outputRange(curve.y) : 1);
    // The symmetric drawn curve shows only the positive quadrant, 0..1 on both axes.
    const low = shown ? 0 : -1;
    const toX = (x: number) => PLOT_PAD + ((x - low) / (1 - low)) * (s - 2 * PLOT_PAD);
    const toY = (y: number) => s - PLOT_PAD - ((y / range - low) / (1 - low)) * (s - 2 * PLOT_PAD);
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, s, s);

    // With the drawn curve, the part a full-scale input reaches at the current Drive and Bias.
    if (drawn) {
      ctx.fillStyle = theme.accent;
      ctx.globalAlpha = 0.1;
      ctx.fillRect(toX(band[0]), 0, toX(band[1]) - toX(band[0]), s);
      ctx.globalAlpha = 1;
    }

    ctx.lineWidth = 1;
    ctx.strokeStyle = theme.border;
    for (const v of shown ? [0.25, 0.5, 0.75] : [-0.5, 0, 0.5]) {
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
    ctx.fillText(drawn ? 'u' : 'in', s - 14, s - 6);
    if (range > 1) ctx.fillText(`out ±${range.toFixed(1)}`, 4, 12);

    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, s, s);
    ctx.clip();
    ctx.setLineDash([3, 3]);
    ctx.strokeStyle = theme.textDim;
    ctx.beginPath();
    ctx.moveTo(toX(low), toY(low));
    ctx.lineTo(toX(1), toY(1));
    ctx.stroke();
    ctx.setLineDash([]);
    // The drawn curve is the editor's to draw, so it follows a drag.
    if (curve && !drawn) {
      ctx.beginPath();
      curve.x.forEach((x, i) => (i === 0 ? ctx.moveTo(toX(x), toY(curve.y[i])) : ctx.lineTo(toX(x), toY(curve.y[i]))));
      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = 2;
      ctx.stroke();
    }

    // The signal: the input level on the curve, as on the compressor; on the drawn curve's plot, at the shaper's input.
    const dot = drawn ? drawnDot(meters, symmetric, MIN_DOT_LEVEL) : shaperDot(curve, meters?.input);
    if (dot) drawLevelDot(ctx, theme, toX(dot.x), toY(dot.y));
    ctx.restore();
    ctx.lineWidth = 1;
    ctx.strokeStyle = theme.border;
    ctx.strokeRect(0.5, 0.5, s - 1, s - 1);
  });

  // Switching to asymmetric keeps the sound: the curve becomes its positive half and that half's mirror image, with a
  // control point at (0, 0). Switching to symmetric plays the positive half of whatever is drawn.
  const switchSymmetry = (to: boolean): Command => ({
    command: 'batch',
    args: {
      label: to ? 'Symmetric curve' : 'Asymmetric curve',
      commands: [
        { command: 'setNodeParam', args: { trackId: track.id, nodeId: node.id, param: 'symmetric', value: to ? 1 : 0 } },
        ...(to
          ? []
          : [
              {
                command: 'setModulationShape',
                args: { trackId: track.id, nodeId: node.id, points: mirrored(positiveHalf(drawnPoints(node))) },
              } as Command,
            ]),
      ],
    },
  });

  const shape = param('shape');
  const oversampling = param('oversampling');
  const choose = (id: string, choice: number) => {
    edit.begin([id]);
    edit.set({ [id]: choice });
    edit.end();
  };

  return (
    <div className="device-panel waveshaper-panel">
      <div className="panel-main">
        <div className="waveshaper-header">
          <select
            className="waveshaper-shape"
            title="Shape: the transfer curve"
            value={Math.round(value('shape'))}
            onChange={(e) => choose('shape', Number(e.target.value))}
          >
            {shape.choices.map((label, i) => (
              <option key={label} value={i}>
                {label}
              </option>
            ))}
          </select>
          <div className="segmented" title="Oversampling: less aliasing and brighter highs, more latency and CPU">
            {oversampling.choices.map((label, i) => (
              <button
                key={label}
                className={Math.round(value('oversampling')) === i ? 'active' : ''}
                onClick={() => choose('oversampling', i)}
              >
                {label}
              </button>
            ))}
          </div>
        </div>
        <div className="waveshaper-plot">
          <canvas ref={canvas.ref} className="panel-canvas" />
          {drawn && size > 2 * PLOT_PAD + 8 && <WaveshaperCurveEditor track={track} node={node} size={size} symmetric={symmetric} />}
        </div>
        {drawn && (
          <div className="waveshaper-presets">
            <div className="segmented" title="Symmetric: draw the positive half, mirrored for negative inputs">
              {['Symmetric', 'Asymmetric'].map((label, i) => (
                <button
                  key={label}
                  className={symmetric === (i === 0) ? 'active' : ''}
                  onClick={() => symmetric !== (i === 0) && dispatch(switchSymmetry(i === 0))}
                >
                  {label}
                </button>
              ))}
            </div>
            {Object.entries(symmetric ? SYMMETRIC_PRESETS : ASYMMETRIC_PRESETS).map(([name, preset]) => (
              <Button
                key={name}
                title={`Draw the ${name} curve`}
                onClick={() =>
                  dispatch({
                    command: 'setModulationShape',
                    args: {
                      trackId: track.id,
                      nodeId: node.id,
                      points: (symmetric ? mirrored(preset) : preset).map((p) => [...p] as CurvePoint),
                    },
                  })
                }
              >
                {name}
              </Button>
            ))}
          </div>
        )}
      </div>
      <div className="panel-sections">
        <Section label="Shape">
          {['drive', 'bias', 'tone'].map((id) => (
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
