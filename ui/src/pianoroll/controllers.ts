import { useEffect, useRef, useState } from 'react';
import type { Clip, CurvePoint } from '../bridge/protocol';
import {
  addPoint,
  bendSegmentToPointer,
  deletePoints,
  handlePosition,
  handleTimeFor,
  hitCurve,
  movePoints,
  pointsInRect,
  type CurveMapping,
  straightenSegment,
} from './curveEditing';
import { evaluateCurve, snap, tickToX, xToTick, type View } from './geometry';
import { alpha, font, LIGHT, type Size, type Theme } from '../canvas/theme';
import { dispatch } from '../store/store';

// Clip controller lanes in the piano roll (docs/design/phase10-automation.md, A2 and section 5): sustain, mod wheel
// and any other controller a clip has, edited with the expression curve tools. Times are content positions; the
// lane shows the clip's passes on the timeline. A click selects a point, Shift toggles it, Alt+click empty space
// adds one, double-click deletes one, and dragging empty space box-selects points (Shift adds to the selection).

export const SUSTAIN = 64;
export const MOD_WHEEL = 1;
export const PITCH_BEND = 128;
export const PRESSURE = 129;

const NAMES: Record<number, string> = {
  [SUSTAIN]: 'Sustain',
  [MOD_WHEEL]: 'Mod wheel',
  2: 'Breath',
  7: 'Volume CC',
  10: 'Pan CC',
  11: 'Expression',
  [PITCH_BEND]: 'Pitch bend',
  [PRESSURE]: 'Channel pressure',
};

export const controllerLabel = (controller: number) => NAMES[controller] ?? `CC ${controller}`;

/** The neutral value of a controller lane: centred pitch bend, everything else 0. */
export const controllerNeutral = (controller: number) => (controller === PITCH_BEND ? 0.5 : 0);

/** The sustain pedal is on or off: its points are steps and snap to 0 or 1. */
const isPedal = (controller: number) => controller === SUSTAIN;

/** The clip's passes over the timeline (like the engine's forEachPass). */
export function clipPasses(clip: Clip): Array<{ start: number; end: number; contentAt: number }> {
  const clipEnd = clip.start + clip.length;
  if (clip.loopLength <= 0) return [{ start: clip.start, end: clipEnd, contentAt: clip.offset }];

  const passes = [];
  const loopEnd = clip.loopStart + clip.loopLength;
  let contentFrom = clip.offset < loopEnd ? clip.offset : clip.loopStart;

  for (let passStart = clip.start; passStart < clipEnd; ) {
    const passEnd = Math.min(clipEnd, passStart + (loopEnd - contentFrom));
    passes.push({ start: passStart, end: passEnd, contentAt: contentFrom });
    passStart = passEnd;
    contentFrom = clip.loopStart;
  }

  return passes;
}

const PAD = 6;

/** A selection box in lane pixels. */
interface Band {
  x0: number;
  y0: number;
  x1: number;
  y1: number;
}

/** Draws a controller lane: the curve over every pass of the clip, points in the first pass. */
export function drawControllerLane(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  clip: Clip | undefined,
  controller: number,
  points: CurvePoint[],
  colour: string,
  selected: ReadonlySet<number>,
) {
  ctx.fillStyle = theme.bg;
  ctx.fillRect(0, 0, size.width, size.height);
  const span = Math.max(1, size.height - 2 * PAD);
  const toY = (v: number) => PAD + (1 - v) * span;

  ctx.fillStyle = theme.border;
  for (const v of [0, 0.5, 1]) ctx.fillRect(0, Math.round(toY(v)) + 0.5, size.width, 1);

  if (!clip) {
    ctx.fillStyle = theme.textDim;
    ctx.font = font(theme, 's');
    ctx.fillText('Double-click a clip in the arrangement to edit its controllers.', 12, size.height / 2);
    return;
  }

  const neutral = controllerNeutral(controller);

  for (const [i, pass] of clipPasses(clip).entries()) {
    const x0 = Math.max(0, tickToX(view, pass.start));
    const x1 = Math.min(size.width, tickToX(view, pass.end));
    if (x1 <= x0) continue;

    ctx.fillStyle = alpha(LIGHT, 0.025);
    ctx.fillRect(x0, 0, x1 - x0, size.height);
    ctx.strokeStyle = colour;
    ctx.globalAlpha = i === 0 ? 1 : 0.5;
    ctx.lineWidth = 1.5;

    const xs = new Set<number>();
    for (let x = x0; x <= x1; x += 2) xs.add(x);
    xs.add(x0);
    xs.add(x1);
    if (i === 0) {
      for (let s = 0; s + 1 < points.length; s++) {
        if (points[s][1] === points[s + 1][1]) continue;
        const t = handleTimeFor(points, s);
        const x = tickToX(view, pass.start + t - pass.contentAt);
        if (x >= x0 && x <= x1) xs.add(x);
      }
    }

    ctx.beginPath();
    [...xs]
      .sort((a, b) => a - b)
      .forEach((x, index) => {
        const content = pass.contentAt + (view.startTick + x / view.pxPerTick - pass.start);
        const y = toY(evaluateCurve(points, content, neutral));
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });

    ctx.stroke();
    ctx.globalAlpha = 1;

    if (i === 0) {
      points.forEach((p, index) => {
        const x = tickToX(view, pass.start + p[0] - pass.contentAt);
        if (x < x0 - 4 || x > x1 + 4) return;
        ctx.fillStyle = selected.has(index) ? theme.text : colour;
        ctx.fillRect(x - 3, toY(p[1]) - 3, 6, 6);
      });

      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = 1.5;
      for (let s = 0; s + 1 < points.length; s++) {
        if (points[s][1] === points[s + 1][1]) continue;
        const t = handleTimeFor(points, s);
        const x = tickToX(view, pass.start + t - pass.contentAt);
        if (x < x0 - 4 || x > x1 + 4) continue;
        ctx.beginPath();
        ctx.arc(x, toY(evaluateCurve(points, t, neutral)), 3.5, 0, Math.PI * 2);
        ctx.stroke();
      }
    }
  }
}

/** Editing one controller lane of a clip, in its first pass. */
export function useControllerEditing(clip: Clip | undefined, controller: number, view: View, gridTicks: number, height: number) {
  const [preview, setPreview] = useState<CurvePoint[] | null>(null);
  const [selected, setSelected] = useState<Set<number>>(new Set());
  const [band, setBand] = useState<Band | null>(null);
  const drag = useRef<
    | { kind: 'points'; points: CurvePoint[]; indices: number[]; grab: number; moved: boolean }
    | { kind: 'bend'; points: CurvePoint[]; segment: number; grab: { x: number; y: number } }
    | { kind: 'band'; band: Band; base: ReadonlySet<number> }
    | null
  >(null);

  // Point indices only mean something in their own lane and clip.
  useEffect(() => {
    setSelected(new Set());
    setBand(null);
  }, [controller, clip?.id]);

  const points = clip?.controllers?.[String(controller)] ?? [];
  const pass = clip ? clipPasses(clip)[0] : undefined;
  const span = Math.max(1, height - 2 * PAD);

  const mapping = (): CurveMapping => ({
    toX: (t) => tickToX(view, (pass?.start ?? 0) + t - (pass?.contentAt ?? 0)),
    toY: (v) => PAD + (1 - v) * span,
    fromX: (x) => xToTick(view, x) - (pass?.start ?? 0) + (pass?.contentAt ?? 0),
    fromY: (y) => 1 - (y - PAD) / span,
    min: 0,
    max: 1,
    maxTime: clip ? clip.offset + clip.length + (clip.loopLength > 0 ? clip.loopLength : 0) : 0,
    neutral: controllerNeutral(controller),
    snapTime: (t) => Math.max(0, snap(t, gridTicks, true)),
    snapValue: isPedal(controller) ? (v) => (v >= 0.5 ? 1 : 0) : undefined,
  });

  const commit = (next: CurvePoint[]) => {
    setPreview(null);
    if (clip) dispatch({ command: 'setClipController', args: { clipId: clip.id, controller, points: next } });
  };

  // New pedal points are steps.
  const shaped = (curve: CurvePoint[]): CurvePoint[] =>
    isPedal(controller) ? curve.map(([t, v, , tension]) => [t, v >= 0.5 ? 1 : 0, 1, tension]) : curve;

  const down = (x: number, y: number, clicks: number, mods: { alt: boolean; shift: boolean; free: boolean }) => {
    if (!clip) return;
    const map = mapping();
    const hit = hitCurve(points, x, y, map);

    if (hit?.kind === 'point' && clicks >= 2) {
      setSelected(new Set());
      commit(deletePoints(points, [hit.index]));
      return;
    }

    if (hit?.kind === 'handle' && clicks >= 2) {
      commit(straightenSegment(points, hit.segment));
      return;
    }

    if (hit?.kind === 'handle') {
      const handle = handlePosition(points, hit.segment, map);
      drag.current = {
        kind: 'bend',
        points,
        segment: hit.segment,
        grab: { x: x - handle.x, y: y - handle.y },
      };
      return;
    }

    if (hit?.kind === 'point') {
      if (mods.shift) {
        const next = new Set(selected);
        if (next.has(hit.index)) next.delete(hit.index);
        else next.add(hit.index);
        setSelected(next);
        return;
      }
      const indices = selected.has(hit.index) ? [...selected] : [hit.index];
      setSelected(new Set(indices));
      drag.current = { kind: 'points', points, indices, grab: hit.index, moved: false };
      return;
    }

    // Alt+click or a double-click on empty space adds a point.
    if (mods.alt || clicks >= 2) {
      const added = addPoint(points, x, y, map, mods.free);
      const next = shaped(added.points);
      setSelected(new Set([added.index]));
      setPreview(next);
      drag.current = { kind: 'points', points: next, indices: [added.index], grab: added.index, moved: true };
      return;
    }

    // Empty space starts a box selection; Shift adds to the current selection.
    const base = mods.shift ? new Set(selected) : new Set<number>();
    if (!mods.shift) setSelected(new Set());
    const box = { x0: x, y0: y, x1: x, y1: y };
    setBand(box);
    drag.current = { kind: 'band', band: box, base };
  };

  const move = (x: number, y: number, free: boolean) => {
    const d = drag.current;
    if (!d) return;
    if (d.kind === 'band') {
      d.band = { ...d.band, x1: x, y1: y };
      setBand(d.band);
      return;
    }
    if (d.kind === 'bend') {
      const bent = bendSegmentToPointer(d.points, d.segment, mapping(), x, y, d.grab);
      setPreview(shaped(bent.points));
      return;
    }
    d.moved = true;
    const moved = movePoints(d.points, d.indices, d.grab, x, y, mapping(), free);
    setPreview(shaped(moved.points));
    setSelected(new Set(moved.indices));
  };

  const up = () => {
    const d = drag.current;
    drag.current = null;
    if (d?.kind === 'band') {
      const inside = pointsInRect(points, mapping(), d.band.x0, d.band.y0, d.band.x1, d.band.y1);
      setSelected(new Set([...d.base, ...inside]));
      setBand(null);
      setPreview(null);
      return;
    }
    if (d && preview && (d.kind === 'bend' || d.moved)) commit(preview);
    else setPreview(null);
  };

  const removeSelected = () => {
    if (selected.size === 0) return false;
    commit(deletePoints(points, [...selected]));
    setSelected(new Set());
    return true;
  };

  return { points: preview ?? points, selected, band, down, move, up, removeSelected };
}
