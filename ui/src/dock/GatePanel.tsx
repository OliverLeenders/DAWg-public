import { Readout, Readouts } from '../components/Readouts';
import { useLayoutEffect, useRef, useState } from 'react';
import { ParamValueText } from '../components/ValueText';
import { NODE_TYPES } from '../bridge/protocol';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { formatParam } from '../graph/params';
import { useNodeAnalysis, useNodeDrag } from './analysis';
import { CompressorHistory, type CompressorMeters } from './CompressorHistory';
import { useCurveLayout } from './dynamicsLayout';
import { CLOSED_DB, expanderGainChangeDb, expanderRatioThrough, gateCloseDb, gateFloorDb } from './dynamicsCurves';
import { Knob } from './Knob';
import { Section } from './Section';
import { SidechainSection } from './SidechainSection';
import type { PanelProps } from './panels';
import { drawHandle } from './panelDraw';

// The Gate's and the Expander's panel (docs/design/gate-expander.md, N14), laid out as the Compressor's: the transfer
// curve with handles, the level history (or a gain-reduction meter when narrow), the timing knobs, the detector and
// the Sidechain column.
//
// Gate: the open path (unity above the close threshold) and the closed path (the range's floor below the open
// threshold), joined by the two thresholds, the hysteresis loop. Handles: T (threshold), H (close threshold, sets
// the hysteresis), F (the floor, at 0 dB in). Expander: its curve with the knee, the range as a dashed line. Handles: T, R (ratio:
// drag the curve below the threshold up or down) and F.

const MIN_DB = CLOSED_DB;
const MAX_DB = 6;
/** How close a press must be to grab a handle. */
const GRAB = 12;

type Handle = 'threshold' | 'close' | 'ratio' | 'range';

interface GateMeters extends CompressorMeters {
  /** The Gate is open or holding. */
  open?: boolean;
}

const round = (value: number) => Math.round(value * 10) / 10;
const clamp = (value: number, low: number, high: number) => Math.min(high, Math.max(low, value));

function drawArrow(ctx: CanvasRenderingContext2D, x: number, from: number, to: number) {
  const dir = Math.sign(to - from) || 1;
  ctx.beginPath();
  ctx.moveTo(x, from);
  ctx.lineTo(x, to);
  ctx.stroke();
  if (Math.abs(to - from) < 10) return;
  const mid = (from + to) / 2;
  ctx.beginPath();
  ctx.moveTo(x - 4, mid - 4 * dir);
  ctx.lineTo(x, mid + 2 * dir);
  ctx.lineTo(x + 4, mid - 4 * dir);
  ctx.stroke();
}

export function GatePanel({ track, node, info }: PanelProps) {
  const isGate = node.type === NODE_TYPES.gate;
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const meters = (useNodeAnalysis(track.id, node.id)?.meters ?? null) as GateMeters | null;
  const drag = useRef<Handle | null>(null);
  const [active, setActive] = useState<Handle | null>(null);
  const panel = useRef<HTMLDivElement>(null);
  const fit = useCurveLayout(panel);

  const threshold = value('threshold');
  const range = value('range');
  const close = isGate ? gateCloseDb(threshold, value('hysteresis')) : threshold;
  const floor = gateFloorDb(range);
  const ratio = isGate ? 1 : value('ratio');
  const outputAt = (db: number) => db + expanderGainChangeDb(db, threshold, ratio, range);

  // A square plot: input level on x, output level on y, both MIN_DB..MAX_DB.
  const side = () => Math.min(canvas.size.width, canvas.size.height);
  const toX = (db: number) => ((db - MIN_DB) / (MAX_DB - MIN_DB)) * side();
  const toY = (db: number) => side() - ((Math.max(MIN_DB, db) - MIN_DB) / (MAX_DB - MIN_DB)) * side();
  const fromY = (y: number) => MIN_DB + ((side() - y) / Math.max(1, side())) * (MAX_DB - MIN_DB);
  const fromX = (x: number) => MIN_DB + (x / Math.max(1, side())) * (MAX_DB - MIN_DB);
  // Where the F and R handles sit along x: F at 0 dB in, where the floor's output is the range itself, so the whole
  // range is in reach.
  const floorX = () => 0;
  const ratioX = () => Math.max(MIN_DB + 10, threshold - 15);
  const handles = (): Partial<Record<Handle, { x: number; y: number }>> => {
    const f = floorX();
    const floorY = Math.min(side() - 6, toY(f + (isGate ? floor : range)));
    if (isGate)
      return {
        threshold: { x: toX(threshold), y: toY(threshold) },
        close: { x: toX(close), y: toY(close) },
        range: { x: toX(f), y: floorY },
      };
    return {
      threshold: { x: toX(threshold), y: toY(outputAt(threshold)) },
      ratio: { x: toX(ratioX()), y: toY(outputAt(ratioX())) },
      range: { x: toX(f), y: floorY },
    };
  };

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
    for (let db = -72; db <= 0; db += 12) {
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

    // Unity.
    ctx.setLineDash([3, 3]);
    ctx.strokeStyle = theme.textDim;
    ctx.beginPath();
    ctx.moveTo(0, s);
    ctx.lineTo(s, 0);
    ctx.stroke();
    ctx.setLineDash([]);

    ctx.strokeStyle = theme.accent;
    if (isGate) {
      // The closed path along the floor up to the open threshold, the open path from the close threshold up, and the
      // two thresholds joining them: up at T, down at H.
      const floorY = (db: number) => (floor === -Infinity ? s - 1 : toY(db + floor));
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(0, floorY(MIN_DB));
      ctx.lineTo(toX(threshold), floorY(threshold));
      ctx.moveTo(toX(close), toY(close));
      ctx.lineTo(s, toY(MAX_DB));
      ctx.stroke();
      // The floor on above the threshold, dashed: where the gain would be if the gate were closed.
      ctx.lineWidth = 1;
      ctx.setLineDash([2, 3]);
      ctx.globalAlpha = 0.6;
      ctx.beginPath();
      ctx.moveTo(toX(threshold), floorY(threshold));
      ctx.lineTo(s, floorY(MAX_DB));
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;
      ctx.lineWidth = 1.5;
      ctx.globalAlpha = 0.7;
      drawArrow(ctx, toX(threshold), floorY(threshold), toY(threshold));
      if (close < threshold - 0.05) drawArrow(ctx, toX(close), toY(close), floorY(close));
      ctx.globalAlpha = 1;
    } else {
      // The range, then the curve.
      ctx.setLineDash([2, 3]);
      ctx.globalAlpha = 0.6;
      ctx.beginPath();
      ctx.moveTo(toX(MIN_DB - range), toY(MIN_DB));
      ctx.lineTo(s, toY(MAX_DB + range));
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;
      ctx.lineWidth = 2;
      ctx.beginPath();
      for (let db = MIN_DB; db <= MAX_DB; db += 0.5) {
        if (db === MIN_DB) ctx.moveTo(toX(db), toY(outputAt(db)));
        else ctx.lineTo(toX(db), toY(outputAt(db)));
      }
      ctx.stroke();
    }
    ctx.lineWidth = 1;

    // The signal: input and output level.
    if (meters && meters.input > MIN_DB && meters.output > MIN_DB) {
      ctx.beginPath();
      ctx.arc(toX(Math.min(MAX_DB, meters.input)), toY(Math.min(MAX_DB, meters.output)), 4, 0, Math.PI * 2);
      ctx.fillStyle = theme.signalAudio;
      ctx.fill();
    }

    const h = handles();
    const labels: Record<Handle, string> = { threshold: 'T', close: 'H', ratio: 'R', range: 'F' };
    // The threshold last, on top of H when there's no hysteresis.
    for (const which of ['range', 'ratio', 'close', 'threshold'] as Handle[]) {
      const at = h[which];
      if (at) drawHandle(ctx, theme, at.x, at.y, theme.accent, active === which, labels[which]);
    }
  });

  const local = (e: React.PointerEvent) => {
    const rect = e.currentTarget.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  };
  const reduction = Math.max(-48, Math.min(0, meters?.reduction ?? 0));
  const toggle = (id: string) => {
    edit.begin([id]);
    edit.set({ [id]: value(id) > 0.5 ? 0 : 1 });
    edit.end();
  };
  const choose = (id: string, choice: number) => {
    edit.begin([id]);
    edit.set({ [id]: choice });
    edit.end();
  };
  const end = () => {
    if (drag.current) edit.end();
    drag.current = null;
    setActive(null);
  };
  const open = isGate && !!meters?.open;
  const rangeText = isGate && range <= CLOSED_DB ? '−∞ dB' : formatParam(param('range'), range);

  return (
    <div ref={panel} className={`device-panel compressor-panel dynamics-panel fit-${fit.mode}`}>
      <div className="panel-main" style={{ width: fit.curve }}>
        <canvas
          ref={canvas.ref}
          className="panel-canvas"
          style={{ height: fit.curve }}
          onPointerDown={(e) => {
            const { x, y } = local(e);
            const h = handles();
            // The threshold first: on top of H when there's no hysteresis (drag left of it to pull H out).
            const order: Handle[] = ['threshold', 'close', 'ratio', 'range'];
            const which =
              order.find((id) => {
                const at = h[id];
                if (!at || Math.hypot(at.x - x, at.y - y) >= GRAB) return false;
                return !(id === 'threshold' && h.close && Math.abs(h.close.x - at.x) < 2 && x < at.x - 3);
              }) ?? null;
            setActive(which);
            if (!which) return;
            e.currentTarget.setPointerCapture(e.pointerId);
            drag.current = which;
            edit.begin([which === 'close' ? 'hysteresis' : which]);
          }}
          onPointerMove={(e) => {
            const which = drag.current;
            if (!which) return;
            const { x, y } = local(e);
            if (which === 'threshold') edit.drag({ threshold: round(clamp(fromX(x), MIN_DB, 0)) });
            else if (which === 'close') edit.drag({ hysteresis: round(clamp(threshold - fromX(x), 0, 24)) });
            else if (which === 'ratio')
              edit.drag({ ratio: round(clamp(expanderRatioThrough(ratioX(), fromY(y), threshold), 1, 20)) });
            else {
              const out = y >= side() - 7 ? -Infinity : fromY(y);
              edit.drag({ range: round(clamp(out - floorX(), MIN_DB, 0)) });
            }
          }}
          onPointerUp={end}
          onPointerCancel={end}
        />
        <div className="gate-readouts">
          {isGate && <span className={`gate-light ${open ? 'open' : ''}`} title={open ? 'Open' : 'Closed'} />}
          <Readouts>
            <Readout label="Threshold">
              <ParamValueText trackId={track.id} nodeId={node.id} param={param('threshold')} value={threshold} />
            </Readout>
            {isGate ? (
              <Readout label="Hysteresis">
                <ParamValueText trackId={track.id} nodeId={node.id} param={param('hysteresis')} value={value('hysteresis')} />
              </Readout>
            ) : (
              <Readout label="Ratio">
                <ParamValueText trackId={track.id} nodeId={node.id} param={param('ratio')} value={ratio} />
              </Readout>
            )}
            <Readout label="Range">
              <ParamValueText trackId={track.id} nodeId={node.id} param={param('range')} value={range} text={rangeText} />
            </Readout>
          </Readouts>
        </div>
      </div>
      <CompressorHistory
        meters={meters}
        threshold={threshold}
        closeThreshold={isGate ? close : undefined}
        minDb={MIN_DB}
        grRange={48}
        keyed={value('detector') > 0.5}
        onThreshold={{
          begin: () => edit.begin(['threshold']),
          set: (db) => edit.drag({ threshold: db }),
          end: () => edit.end(),
        }}
      />
      <div className="gr-meter" title={`Gain reduction ${reduction.toFixed(1)} dB`}>
        <div className="gr-meter-track">
          <div className="gr-meter-fill" style={{ height: `${(-reduction / 48) * 100}%` }} />
        </div>
        <span>GR</span>
      </div>
      <div className="panel-sections dynamics-sections">
        <Section label="Timing">
          {['attack', 'hold', 'release'].map((id) => (
            <Knob key={id} track={track} node={node} param={param(id)} />
          ))}
        </Section>
        <Section label="Detector" column>
          <div className="segmented" title="Peak reacts to the first transient; RMS follows loudness more smoothly">
            {param('detection').choices.map((label, i) => (
              <button key={label} className={Math.round(value('detection')) === i ? 'active' : ''} onClick={() => choose('detection', i)}>
                {label}
              </button>
            ))}
          </div>
          <label
            className="field dynamics-lookahead"
            title="Lookahead: delays the audio so the gain moves before a transient arrives. Adds that much latency."
          >
            <span>Lookahead</span>
            <select value={Math.round(value('lookahead'))} onChange={(e) => choose('lookahead', Number(e.target.value))}>
              {param('lookahead').choices.map((label, i) => (
                <option key={label} value={i}>
                  {label}
                </option>
              ))}
            </select>
          </label>
        </Section>
      </div>
      <SidechainSection track={track} node={node} param={param} value={value} toggle={toggle} keyLateMs={meters?.keyLateMs ?? 0} />
    </div>
  );
}
