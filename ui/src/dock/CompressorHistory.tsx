import { useEffect, useRef, useState } from 'react';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useLayout } from '../store/layoutStore';
import { LevelHistory, SLOT_MS, SPANS } from './levelHistory';

// The compressor's level history (docs/design/phase11-faust-panels.md, P4): input as a filled area, output as a line
// and gain reduction hanging from the top, scrolling right to left over the chosen span. The threshold is a dashed
// line that can be dragged up and down.

/** The levels' top, as on the transfer curve. */
const MAX_DB = 6;
/** Room on the right for the live gain-reduction bar and its scale, and at the bottom for the times. */
const GUTTER = 34;
const FOOTER = 14;
/** How close to the threshold line a press grabs it. */
const GRAB = 6;
/** The key level's line (the input meter's colour for an armed track). */

export interface CompressorMeters {
  /** [input, output, reduction, key] per 10 ms slot since the last frame. */
  history?: number[];
  input: number;
  output: number;
  reduction: number;
  /** How late the key from other tracks arrives (docs/design/sidechain.md, K5). */
  keyLateMs?: number;
}

interface Props {
  meters: CompressorMeters | null;
  threshold: number;
  /** The Gate's close threshold, a second dashed line (docs/design/gate-expander.md, N14). */
  closeThreshold?: number;
  /** The levels' bottom, as on the transfer curve. */
  minDb?: number;
  /** The gain reduction's range, from the top. */
  grRange?: number;
  /** Whether the compressor reacts to a key: the key's level is drawn as a line too (sidechain.md, K8). */
  keyed?: boolean;
  /** A threshold drag: begin, then values, then end. */
  onThreshold: { begin: () => void; set: (db: number) => void; end: () => void };
}

export function CompressorHistory({ meters, threshold, closeThreshold, minDb = -60, grRange = 24, keyed = false, onThreshold }: Props) {
  const MIN_DB = minDb;
  const GR_RANGE = grRange;
  const canvas = useCanvas();
  const history = useRef(new LevelHistory());
  const span = useLayout((s) => s.compressorSpan);
  const [hover, setHover] = useState(false);
  const dragging = useRef(false);
  const live = useRef({ threshold, closeThreshold, span, hover, reduction: 0, keyed, MIN_DB, GR_RANGE });
  live.current = { threshold, closeThreshold, span, hover: hover || dragging.current, reduction: meters?.reduction ?? 0, keyed, MIN_DB, GR_RANGE };
  const dirty = useRef(true);

  useEffect(() => {
    if (meters?.history?.length) history.current.push(meters.history, performance.now());
    dirty.current = true;
  }, [meters]);

  useEffect(() => {
    dirty.current = true;
  });

  const plot = () => ({ width: Math.max(1, canvas.size.width - GUTTER), height: Math.max(1, canvas.size.height - FOOTER) });
  const levelY = (db: number) => {
    const bottom = live.current.MIN_DB;
    return plot().height * (1 - (Math.min(MAX_DB, Math.max(bottom, db)) - bottom) / (MAX_DB - bottom));
  };
  const levelAt = (y: number) => MAX_DB - (y / plot().height) * (MAX_DB - MIN_DB);

  // Drawn on animation frames, so the graph scrolls on even when no analysis comes in.
  useEffect(() => {
    let frame = 0;
    const draw = () => {
      frame = requestAnimationFrame(draw);
      const padded = history.current.pad(performance.now());
      if (!padded && !dirty.current) return;
      dirty.current = false;
      const ctx = canvas.context();
      const { width: W, height: H } = canvas.size;
      if (!ctx || W < 16 || H < 16) return;
      const { threshold, closeThreshold, span, hover, reduction, keyed, MIN_DB, GR_RANGE } = live.current;
      const theme = readTheme();
      const warn = theme.warn;
      const { width, height } = plot();
      const slots = (span * 1000) / SLOT_MS;
      const step = width / slots;
      const count = Math.min(history.current.length, slots + 1);
      const grY = (db: number) => (Math.min(GR_RANGE, -Math.min(0, db)) / GR_RANGE) * height;
      const xAt = (age: number) => width - age * step;

      ctx.fillStyle = theme.bgRaised;
      ctx.fillRect(0, 0, W, H);
      ctx.fillStyle = theme.bg;
      ctx.fillRect(0, 0, width, height);
      ctx.font = font(theme, 'xs');
      ctx.lineWidth = 1;

      // Level lines every 12 dB, then a line and a time each second.
      ctx.textBaseline = 'middle';
      for (let db = Math.ceil((MIN_DB + 1) / 12) * 12; db <= 0; db += 12) {
        const y = Math.round(levelY(db)) + 0.5;
        ctx.strokeStyle = theme.border;
        ctx.globalAlpha = db === 0 ? 1 : 0.45;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(width, y);
        ctx.stroke();
        ctx.globalAlpha = 1;
        ctx.fillStyle = theme.textDim;
        ctx.fillText(String(db), 4, y - 6);
      }
      ctx.textBaseline = 'top';
      for (let s = 1; s < span; s++) {
        const x = Math.round(xAt((s * 1000) / SLOT_MS)) + 0.5;
        ctx.strokeStyle = theme.border;
        ctx.globalAlpha = 0.45;
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x, height);
        ctx.stroke();
        ctx.globalAlpha = 1;
        ctx.fillStyle = theme.textDim;
        ctx.fillText(`−${s}s`, x - 8, height + 2);
      }

      if (count > 1) {
        // The input, filled from the bottom.
        ctx.beginPath();
        ctx.moveTo(xAt(count - 1), height);
        for (let age = count - 1; age >= 0; age--) ctx.lineTo(xAt(age), levelY(history.current.at(age)[0]));
        ctx.lineTo(width, height);
        ctx.closePath();
        ctx.fillStyle = theme.accent;
        ctx.globalAlpha = 0.22;
        ctx.fill();
        ctx.globalAlpha = 1;

        // The output as a line.
        ctx.beginPath();
        for (let age = count - 1; age >= 0; age--) {
          const y = levelY(history.current.at(age)[1]);
          if (age === count - 1) ctx.moveTo(xAt(age), y);
          else ctx.lineTo(xAt(age), y);
        }
        ctx.strokeStyle = theme.meterLow;
        ctx.lineWidth = 1.5;
        ctx.stroke();

        // The key's level, while the compressor reacts to one.
        if (keyed) {
          ctx.beginPath();
          for (let age = count - 1; age >= 0; age--) {
            const y = levelY(history.current.at(age)[3]);
            if (age === count - 1) ctx.moveTo(xAt(age), y);
            else ctx.lineTo(xAt(age), y);
          }
          ctx.strokeStyle = theme.signalKey;
          ctx.lineWidth = 1.25;
          ctx.stroke();
        }

        // The gain reduction, hanging from the top.
        ctx.beginPath();
        ctx.moveTo(xAt(count - 1), 0);
        for (let age = count - 1; age >= 0; age--) ctx.lineTo(xAt(age), grY(history.current.at(age)[2]));
        ctx.lineTo(width, 0);
        ctx.closePath();
        ctx.fillStyle = warn;
        ctx.globalAlpha = 0.18;
        ctx.fill();
        ctx.globalAlpha = 1;
        ctx.beginPath();
        for (let age = count - 1; age >= 0; age--) {
          const y = grY(history.current.at(age)[2]);
          if (age === count - 1) ctx.moveTo(xAt(age), y);
          else ctx.lineTo(xAt(age), y);
        }
        ctx.strokeStyle = warn;
        ctx.lineWidth = 1.25;
        ctx.stroke();
      }

      // The threshold, with its value.
      const ty = Math.round(levelY(threshold)) + 0.5;
      ctx.setLineDash([4, 3]);
      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = hover ? 2 : 1;
      ctx.beginPath();
      ctx.moveTo(0, ty);
      ctx.lineTo(width, ty);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.lineWidth = 1;
      ctx.fillStyle = theme.accent;
      ctx.textBaseline = ty < 14 ? 'top' : 'bottom';
      const label = `T ${threshold.toFixed(1).replace('-', '−')}`;
      ctx.fillText(label, width - ctx.measureText(label).width - 4, ty < 14 ? ty + 3 : ty - 2);

      // The Gate's close threshold, dimmer.
      if (closeThreshold !== undefined && closeThreshold < threshold) {
        const cy = Math.round(levelY(closeThreshold)) + 0.5;
        ctx.setLineDash([2, 3]);
        ctx.strokeStyle = theme.accent;
        ctx.globalAlpha = 0.6;
        ctx.beginPath();
        ctx.moveTo(0, cy);
        ctx.lineTo(width, cy);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.globalAlpha = 1;
      }

      // The live gain reduction as a bar in the gutter, with the reduction scale.
      const barX = width + 4;
      ctx.fillStyle = theme.bg;
      ctx.fillRect(barX, 0, 6, height);
      ctx.fillStyle = warn;
      ctx.fillRect(barX, 0, 6, grY(reduction));
      ctx.textBaseline = 'middle';
      for (const db of [0, -GR_RANGE / 4, -GR_RANGE / 2, -GR_RANGE]) {
        const y = Math.min(height - 5, Math.max(5, grY(db)));
        ctx.fillStyle = warn;
        ctx.globalAlpha = 0.85;
        ctx.fillText(String(db).replace('-', '−'), barX + 9, y);
        ctx.globalAlpha = 1;
      }
    };
    frame = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(frame);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [canvas.size]);

  const near = (e: React.PointerEvent) => {
    const rect = e.currentTarget.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    return x < plot().width && Math.abs(y - levelY(threshold)) <= GRAB;
  };
  const thresholdAt = (e: React.PointerEvent) => {
    const y = e.clientY - e.currentTarget.getBoundingClientRect().top;
    return Math.round(Math.min(0, Math.max(MIN_DB, levelAt(y))) * 10) / 10;
  };
  const reduction = meters?.reduction ?? 0;

  return (
    <div className="compressor-history">
      <canvas
        ref={canvas.ref}
        className="panel-canvas"
        style={{ cursor: hover || dragging.current ? 'ns-resize' : 'default' }}
        onPointerDown={(e) => {
          if (!near(e)) return;
          e.currentTarget.setPointerCapture(e.pointerId);
          dragging.current = true;
          onThreshold.begin();
        }}
        onPointerMove={(e) => {
          if (dragging.current) onThreshold.set(thresholdAt(e));
          else setHover(near(e));
        }}
        onPointerUp={() => {
          if (dragging.current) onThreshold.end();
          dragging.current = false;
        }}
        onPointerLeave={() => setHover(false)}
      />
      <div className="panel-readout compressor-legend">
        <span className="legend output">Output</span>
        <span className="legend input">Input</span>
        {keyed && <span className="legend key">Sidechain</span>}
        <span className="legend reduction">GR {reduction > -0.05 ? '0.0' : reduction.toFixed(1)} dB</span>
        <div className="segmented" title="Time span">
          {SPANS.map((s) => (
            <button key={s} className={s === span ? 'active' : ''} onClick={() => useLayout.setState({ compressorSpan: s })}>
              {s}s
            </button>
          ))}
        </div>
      </div>
    </div>
  );
}
