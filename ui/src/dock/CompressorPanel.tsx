import { Readout, Readouts } from '../components/Readouts';
import { useLayoutEffect, useRef, useState } from 'react';
import { ParamValueText } from '../components/ValueText';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useNodeAnalysis, useNodeDrag, useResponse } from './analysis';
import { CompressorHistory, type CompressorMeters } from './CompressorHistory';
import { Knob } from './Knob';
import { Section } from './Section';
import { SidechainSection } from './SidechainSection';
import type { PanelProps } from './panels';
import { useCurveLayout } from './dynamicsLayout';
import { drawHandle, drawLevelDot } from './panelDraw';

// The compressor's panel (docs/design/phase11-faust-panels.md, P4): the transfer curve with a threshold handle (drag
// along the curve) and a ratio handle (drag its end up or down), the input/output level as a dot on the curve, a
// level history beside it (a gain-reduction meter instead when the dock is narrow), and attack, release, makeup and
// mix as knobs.

const MIN_DB = -60;
const MAX_DB = 6;

export function CompressorPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const params = Object.fromEntries(info.params.map((p) => [p.id, value(p.id)]));
  const curve = useResponse(node.type, params);
  const meters = (useNodeAnalysis(track.id, node.id)?.meters ?? null) as CompressorMeters | null;
  const drag = useRef<'threshold' | 'ratio' | null>(null);
  const [active, setActive] = useState<'threshold' | 'ratio' | null>(null);
  const panel = useRef<HTMLDivElement>(null);
  const fit = useCurveLayout(panel);

  // A square plot: input level on x, output level on y, both MIN_DB..MAX_DB.
  const side = () => Math.min(canvas.size.width, canvas.size.height);
  const toX = (db: number) => ((db - MIN_DB) / (MAX_DB - MIN_DB)) * side();
  const toY = (db: number) => side() - ((db - MIN_DB) / (MAX_DB - MIN_DB)) * side();
  const fromX = (x: number) => MIN_DB + (x / Math.max(1, side())) * (MAX_DB - MIN_DB);
  const outputAt = (inputDb: number) => {
    if (!curve) return inputDb;
    const i = Math.round((inputDb - curve.x[0]) / (curve.x[1] - curve.x[0]));
    return curve.y[Math.min(curve.y.length - 1, Math.max(0, i))];
  };
  const handles = () => ({
    threshold: { x: toX(value('threshold')), y: toY(outputAt(value('threshold'))) },
    ratio: { x: toX(0), y: toY(outputAt(0)) },
  });

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 || canvas.size.height < 16) return;
    const theme = readTheme();
    const s = side();
    ctx.fillStyle = theme.bgRaised;
    ctx.fillRect(0, 0, canvas.size.width, canvas.size.height);
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, s, s);
    ctx.lineWidth = 1;
    ctx.font = font(theme, 'xs');
    for (let db = -54; db <= 0; db += 6) {
      ctx.strokeStyle = theme.border;
      ctx.globalAlpha = db === 0 ? 1 : 0.45;
      ctx.beginPath();
      ctx.moveTo(Math.round(toX(db)) + 0.5, 0);
      ctx.lineTo(Math.round(toX(db)) + 0.5, s);
      ctx.moveTo(0, Math.round(toY(db)) + 0.5);
      ctx.lineTo(s, Math.round(toY(db)) + 0.5);
      ctx.stroke();
      ctx.globalAlpha = 1;
    }
    ctx.fillStyle = theme.textDim;
    ctx.fillText('in', s - 14, s - 14);
    ctx.fillText('out', 4, 12);

    // Unity, then the curve.
    ctx.setLineDash([3, 3]);
    ctx.strokeStyle = theme.textDim;
    ctx.beginPath();
    ctx.moveTo(0, s);
    ctx.lineTo(s, 0);
    ctx.stroke();
    ctx.setLineDash([]);
    if (curve) {
      ctx.beginPath();
      curve.x.forEach((x, i) => (i === 0 ? ctx.moveTo(toX(x), toY(curve.y[i])) : ctx.lineTo(toX(x), toY(curve.y[i]))));
      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = 2;
      ctx.stroke();
    }

    // The signal: input level on the curve.
    if (meters && meters.input > MIN_DB) {
      const x = toX(Math.min(MAX_DB, meters.input));
      const y = toY(Math.min(MAX_DB, meters.output));
      drawLevelDot(ctx, theme, x, y);
    }

    const h = handles();
    drawHandle(ctx, theme, h.threshold.x, h.threshold.y, theme.accent, active === 'threshold', 'T');
    drawHandle(ctx, theme, h.ratio.x, h.ratio.y, theme.accent, active === 'ratio', 'R');
  });

  const local = (e: React.PointerEvent) => {
    const rect = e.currentTarget.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  };
  const reduction = Math.max(-24, Math.min(0, meters?.reduction ?? 0));
  const keyLate = meters?.keyLateMs ?? 0;
  const toggle = (id: string) => {
    edit.begin([id]);
    edit.set({ [id]: value(id) > 0.5 ? 0 : 1 });
    edit.end();
  };

  return (
    <div ref={panel} className={`device-panel compressor-panel fit-${fit.mode}`}>
      <div className="panel-main" style={{ width: fit.curve }}>
        <canvas
          ref={canvas.ref}
          className="panel-canvas"
          style={{ height: fit.curve }}
          onPointerDown={(e) => {
            const { x, y } = local(e);
            const h = handles();
            const which =
              Math.hypot(h.ratio.x - x, h.ratio.y - y) < 12 ? 'ratio' : Math.hypot(h.threshold.x - x, h.threshold.y - y) < 12 ? 'threshold' : null;
            setActive(which);
            if (!which) return;
            e.currentTarget.setPointerCapture(e.pointerId);
            drag.current = which;
            edit.begin([which]);
          }}
          onPointerMove={(e) => {
            const which = drag.current;
            if (!which) return;
            const { x, y } = local(e);
            if (which === 'threshold') {
              const threshold = Math.min(0, Math.max(-60, fromX(x)));
              edit.drag({ threshold: Math.round(threshold * 10) / 10 });
            } else {
              // The output at 0 dB in: threshold + (0 - threshold) / ratio (makeup aside).
              const out = MIN_DB + ((side() - y) / Math.max(1, side())) * (MAX_DB - MIN_DB) - value('makeup');
              const threshold = value('threshold');
              const ratio = out <= threshold ? 20 : -threshold / Math.max(0.001, out - threshold);
              edit.drag({ ratio: Math.round(Math.min(20, Math.max(1, ratio)) * 10) / 10 });
            }
          }}
          onPointerUp={() => {
            if (drag.current) edit.end();
            drag.current = null;
            setActive(null);
          }}
          onPointerCancel={() => {
            if (drag.current) edit.end();
            drag.current = null;
            setActive(null);
          }}
        />
        <Readouts>
          <Readout label="Threshold">
            <ParamValueText trackId={track.id} nodeId={node.id} param={param('threshold')} value={value('threshold')} />
          </Readout>
          <Readout label="Ratio">
            <ParamValueText trackId={track.id} nodeId={node.id} param={param('ratio')} value={value('ratio')} />
          </Readout>
        </Readouts>
      </div>
      <CompressorHistory
        meters={meters}
        threshold={value('threshold')}
        keyed={value('detector') > 0.5}
        onThreshold={{
          begin: () => edit.begin(['threshold']),
          set: (threshold) => edit.drag({ threshold }),
          end: () => edit.end(),
        }}
      />
      <div className="gr-meter" title={`Gain reduction ${reduction.toFixed(1)} dB`}>
        <div className="gr-meter-track">
          <div className="gr-meter-fill" style={{ height: `${(-reduction / 24) * 100}%` }} />
        </div>
        <span>GR</span>
      </div>
      <div className="panel-sections">
        <Section label="Timing">
          {['attack', 'release'].map((id) => (
            <Knob key={id} track={track} node={node} param={param(id)} />
          ))}
        </Section>
        <Section label="Output">
          {['makeup', 'mix'].map((id) => (
            <Knob key={id} track={track} node={node} param={param(id)} />
          ))}
        </Section>
      </div>
      <SidechainSection track={track} node={node} param={param} value={value} toggle={toggle} keyLateMs={keyLate} />
    </div>
  );
}
