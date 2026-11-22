import { useEffect, useMemo, useRef, useState } from 'react';
import type { TempoPoint } from '../bridge/protocol';
import { TextPrompt } from '../components/ConfirmDiscard';
import { drawTimeGrid } from '../pianoroll/draw';
import { alpha, font, LIGHT, readTheme } from '../canvas/theme';
import { tickToX, xToTick, type View } from '../pianoroll/geometry';
import { useCanvas } from '../canvas/useCanvas';
import { dispatch, newGestureId } from '../store/store';
import type { TempoMap } from '../time/tempoMap';

// The tempo lane (docs/design/tempo-map.md, TM12): the tempo as a line over a BPM scale, drawn like an automation
// lane. Click adds a point, drag moves one (Ctrl/Cmd: off the grid), double-click types its BPM, right-click removes
// it; Alt+click or right-click on a segment switches it between a step and a ramp to the next point.

const POINT_PX = 6;
const PAD = 8;
const MIN_BPM = 20;
const MAX_BPM = 999;

/** A BPM range around the points, in steps of 10 and at least 40 wide. */
export function bpmRange(points: TempoPoint[]): [number, number] {
  const bpms = points.map((p) => p.bpm);
  let lo = Math.floor((Math.min(...bpms) - 10) / 10) * 10;
  let hi = Math.ceil((Math.max(...bpms) + 10) / 10) * 10;
  if (hi - lo < 40) {
    const mid = (lo + hi) / 2;
    lo = Math.floor((mid - 20) / 10) * 10;
    hi = lo + 40;
  }
  return [Math.max(MIN_BPM - 10, lo), Math.min(MAX_BPM + 10, hi)];
}

interface Drag {
  index: number;
  range: [number, number];
  gesture: string;
}

export function TempoLane({ view, map, gridTicks }: { view: View; map: TempoMap; gridTicks: number }) {
  const canvas = useCanvas();
  const theme = useMemo(readTheme, []);
  const drag = useRef<Drag | null>(null);
  const [dragging, setDragging] = useState<Drag | null>(null);
  const [typing, setTyping] = useState<number | null>(null);
  const [hover, setHover] = useState<number | null>(null);

  // The scale stays put while a point is dragged, so the point doesn't run away from the pointer.
  const range = dragging?.range ?? bpmRange(map.points);
  const height = canvas.size.height;
  const toY = (bpm: number) => PAD + (1 - (bpm - range[0]) / (range[1] - range[0])) * Math.max(1, height - 2 * PAD);
  const fromY = (y: number) => range[0] + (1 - (y - PAD) / Math.max(1, height - 2 * PAD)) * (range[1] - range[0]);

  useEffect(() => {
    const ctx = canvas.context();
    if (!ctx) return;
    const { width } = canvas.size;
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);
    drawTimeGrid(ctx, canvas.size, view, map, 0);

    // BPM lines at the first of 10, 20, 50 ... that leaves room for a label between them, labelled at the right.
    const pxPerBpm = Math.max(1, height - 2 * PAD) / (range[1] - range[0]);
    const step = [5, 10, 20, 50, 100, 200, 500].find((s) => s * pxPerBpm >= 16) ?? 1000;
    ctx.font = font(theme, 'xs', { mono: true });
    ctx.textBaseline = 'middle';
    for (let bpm = Math.ceil(range[0] / step) * step; bpm <= range[1]; bpm += step) {
      const y = Math.round(toY(bpm)) + 0.5;
      ctx.fillStyle = alpha(LIGHT, 0.05);
      ctx.fillRect(0, y, width, 1);
      ctx.fillStyle = theme.textDim;
      ctx.globalAlpha = 0.7;
      ctx.fillText(String(bpm), width - 26, y < 12 ? y + 6 : y - 6);
      ctx.globalAlpha = 1;
    }

    // The tempo: steps hold until the next point, ramps run straight to it.
    const points = map.points;
    const end = xToTick(view, width);
    ctx.beginPath();
    points.forEach((p, i) => {
      const x = tickToX(view, p.tick);
      const nextTick = points[i + 1]?.tick ?? Math.max(end, p.tick);
      const nextBpm = points[i + 1] && p.ramp ? points[i + 1].bpm : p.bpm;
      if (i === 0) ctx.moveTo(x, toY(p.bpm));
      else ctx.lineTo(x, toY(p.bpm));
      ctx.lineTo(tickToX(view, nextTick), toY(nextBpm));
    });
    ctx.lineTo(tickToX(view, Math.max(end, points[points.length - 1].tick)), toY(points[points.length - 1].bpm));
    ctx.strokeStyle = theme.accent;
    ctx.lineWidth = 1.75;
    ctx.stroke();
    ctx.lineTo(tickToX(view, Math.max(end, points[points.length - 1].tick)), height);
    ctx.lineTo(tickToX(view, 0), height);
    ctx.fillStyle = theme.accent;
    ctx.globalAlpha = 0.08;
    ctx.fill();
    ctx.globalAlpha = 1;

    ctx.font = font(theme, 'xs', { mono: true });
    points.forEach((p, i) => {
      const x = tickToX(view, p.tick);
      if (x < -40 || x > width + 4) return;
      const y = toY(p.bpm);
      const active = hover === i || dragging?.index === i;
      ctx.beginPath();
      ctx.arc(x, y, active ? 4.5 : 3.5, 0, Math.PI * 2);
      ctx.fillStyle = active ? theme.accent : theme.bg;
      ctx.fill();
      ctx.lineWidth = 1.5;
      ctx.strokeStyle = theme.accent;
      ctx.stroke();
      ctx.fillStyle = active ? theme.text : theme.accent;
      ctx.fillText(formatBpm(p.bpm), x + 6, y < 16 ? y + 9 : y - 8);
    });
  });

  const pointAt = (x: number, y: number) =>
    map.points.findIndex((p) => Math.hypot(tickToX(view, p.tick) - x, toY(p.bpm) - y) <= POINT_PX);

  const tickAt = (x: number, free: boolean) => {
    const tick = Math.max(0, xToTick(view, x));
    return free || gridTicks <= 0 ? Math.round(tick) : map.snap(tick, gridTicks, true);
  };
  const bpmAt = (y: number) => Math.round(Math.min(MAX_BPM, Math.max(MIN_BPM, fromY(y))) * 2) / 2;

  const toggleRamp = (x: number) => {
    const index = map.pointIndexAt(Math.max(0, xToTick(view, x)));
    if (index < map.points.length - 1)
      dispatch({ command: 'setTempoPoint', args: { index, ramp: !map.points[index].ramp } });
  };

  return (
    <>
      <canvas
        ref={canvas.ref}
        className="tempo-lane"
        style={{ cursor: hover !== null || dragging ? 'grab' : 'crosshair' }}
        title="Tempo: click to add a point · drag to move (Ctrl: off the grid) · double-click to type the BPM · right-click a point to remove it · Alt+click a segment: step or ramp"
        onPointerDown={(e) => {
          if (e.button !== 0) return;
          const { offsetX: x, offsetY: y } = e.nativeEvent;
          const index = pointAt(x, y);
          if (index >= 0) {
            e.currentTarget.setPointerCapture(e.pointerId);
            const d = { index, range: bpmRange(map.points), gesture: newGestureId() };
            drag.current = d;
            setDragging(d);
          } else if (e.altKey) {
            toggleRamp(x);
          } else {
            dispatch({ command: 'addTempoPoint', args: { tick: tickAt(x, e.ctrlKey || e.metaKey), bpm: bpmAt(y) } });
          }
        }}
        onPointerMove={(e) => {
          const { offsetX: x, offsetY: y } = e.nativeEvent;
          const d = drag.current;
          if (!d) {
            const index = pointAt(x, y);
            setHover(index >= 0 ? index : null);
            return;
          }
          // The first point stays at the start; the others move between their neighbours (the command clamps).
          const args = d.index === 0 ? { index: 0, bpm: bpmAt(y) } : { index: d.index, tick: tickAt(x, e.ctrlKey || e.metaKey), bpm: bpmAt(y) };
          dispatch({ command: 'setTempoPoint', args }, d.gesture);
        }}
        onPointerUp={() => {
          drag.current = null;
          setDragging(null);
        }}
        onPointerLeave={() => setHover(null)}
        onDoubleClick={(e) => {
          const index = pointAt(e.nativeEvent.offsetX, e.nativeEvent.offsetY);
          if (index >= 0) setTyping(index);
        }}
        onContextMenu={(e) => {
          e.preventDefault();
          const index = pointAt(e.nativeEvent.offsetX, e.nativeEvent.offsetY);
          if (index > 0) dispatch({ command: 'removeTempoPoint', args: { index } });
          else if (index < 0) toggleRamp(e.nativeEvent.offsetX);
        }}
      />
      {typing !== null && map.points[typing] && (
        <TextPrompt
          title={typing === 0 ? 'Starting tempo' : 'Tempo change'}
          label="BPM"
          initial={formatBpm(map.points[typing].bpm)}
          confirmLabel="Set"
          onDone={(value) => {
            const bpm = Number(value?.replace(',', '.'));
            if (value !== null && Number.isFinite(bpm))
              dispatch({ command: 'setTempoPoint', args: { index: typing, bpm: Math.min(MAX_BPM, Math.max(MIN_BPM, bpm)) } });
            setTyping(null);
          }}
        />
      )}
    </>
  );
}

export const formatBpm = (bpm: number) => (Number.isInteger(bpm) ? String(bpm) : bpm.toFixed(Math.abs(bpm * 10 - Math.round(bpm * 10)) < 1e-6 ? 1 : 2));
