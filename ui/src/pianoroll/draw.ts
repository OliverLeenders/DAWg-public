import { type Clip, type CurvePoint, type LaneId, type LoopRange, type Marker } from '../bridge/protocol';

import { alpha, font, LIGHT, mix, SHADE, type Size, type Theme } from '../canvas/theme';
import { pointsOf, type PointSelection } from '../store/uiStore';
import { barTicks, beatTicks, type TempoMap } from '../time/tempoMap';
import { handleTimeFor, handleValueFor } from './curveEditing';
import { getDrumPiece, type DrumMap } from '../store/drumMap';
import type { LinkLine } from './links';
import { degreeOf } from '../music/scales';
import { outsideScale, scaleAt, type KeySpan, type ScaleShown } from './fold';
import {
  evaluateCurve,
  isBent,
  isBlackKey,
  LANE_DEFAULTS,
  pitchAtTick,
  pitchName,
  vibratoHandles,
  pitchToY,
  visibleRowPitches,
  tickToX,
  type PlacedNote,
  type View,
  xToTick,
} from './geometry';

// All piano roll drawing. Pure functions of (context, size, view, data): no state, no DOM access besides the
// theme, so a different renderer could replace this module.

// ---------------------------------------------------------------------------------------------------------
// Grid

export function drawGrid(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  map: TempoMap,
  gridTicks: number,
  spans: readonly KeySpan[] = [],
) {
  ctx.fillStyle = theme.bg;
  ctx.fillRect(0, 0, size.width, size.height);

  // Each stretch in one key draws its own rows (docs/design/scale-highlighting.md, SH9); without keys, one stretch.
  const stretches = (spans.length > 0 ? spans : [{ start: -Infinity, end: Infinity, scale: null }])
    .map((span) => ({
      x0: Math.max(0, Math.round(tickToX(view, span.start))),
      x1: Math.min(size.width, Math.round(tickToX(view, span.end))),
      scale: span.scale,
    }))
    .filter((stretch) => stretch.x1 > stretch.x0);

  for (const p of visibleRowPitches(view, size.height)) {
    // Rows on whole pixels, so neighbouring rows never blend at their edge.
    const top = Math.round(pitchToY(view, p));
    const bottom = Math.round(pitchToY(view, p) + view.keyHeight);
    // Every row is lifted off the background. Without a key, the white keys' rows are the lighter ones, as on the
    // keyboard beside them (the owner's call, 2026-11-15). With one, the scale's rows are the lighter ones and the
    // root's are tinted (SH3). The lighter rows take their full height, as the darker ones do, so both look as wide.
    ctx.fillStyle = alpha(LIGHT, 0.022);
    ctx.fillRect(0, top, size.width, bottom - top);
    for (const { x0, x1, scale } of stretches) {
      const width = x1 - x0;
      const degree = scale ? degreeOf(p, scale.root) : ((p % 12) + 12) % 12;
      const lighter = (q: number) => (scale ? ((scale.mask >> degreeOf(q, scale.root)) & 1) !== 0 : !isBlackKey(q));
      if (lighter(p)) {
        ctx.fillStyle = alpha(LIGHT, 0.058);
        ctx.fillRect(x0, top, width, bottom - top);
      }
      if (scale && degree === 0) {
        ctx.save();
        ctx.globalAlpha = 0.14;
        ctx.fillStyle = theme.accent;
        ctx.fillRect(x0, top, width, bottom - top);
        ctx.restore();
      }
      // Where two lighter rows meet (E and F, B and C without a key), a dark line keeps them two rows; under a C (or
      // the root) it is a little stronger, marking the octave. Elsewhere an octave's first row has a light line.
      if (lighter(p) && lighter(p - 1)) {
        ctx.fillStyle = alpha(SHADE, degree === 0 ? 0.45 : 0.3);
        ctx.fillRect(x0, bottom - 1, width, 1);
      } else if (degree === 0) {
        ctx.fillStyle = alpha(LIGHT, 0.07);
        ctx.fillRect(x0, bottom - 1, width, 1);
      }
    }
  }

  // Where the key changes, a line down the roll.
  for (const span of spans) {
    if (!Number.isFinite(span.start)) continue;
    const x = Math.round(tickToX(view, span.start));
    if (x < 0 || x > size.width) continue;
    ctx.save();
    ctx.globalAlpha = 0.5;
    ctx.fillStyle = theme.accent;
    ctx.fillRect(x, 0, 1, size.height);
    ctx.restore();
  }

  drawTimeGrid(ctx, size, view, map, gridTicks);
}

/** The bars a view shows, with each bar's start, end and signature (docs/design/tempo-map.md, TM14). */
export function* visibleBars(map: TempoMap, view: View, width: number) {
  const lastTick = xToTick(view, width);
  for (let bar = Math.max(0, map.barAt(xToTick(view, 0)).bar); ; bar++) {
    const start = map.tickOfBar(bar);
    if (start > lastTick) return;
    yield { bar, start, end: map.tickOfBar(bar + 1), signature: map.signatureAt(start) };
  }
}

/**
 * Vertical grid lines: snap grid (faint), beats and bars (strongest), bar by bar from the tempo map, so the grid
 * restarts at every bar line and beats follow the signature's denominator. Shared by the roll and the lanes.
 */
export function drawTimeGrid(ctx: CanvasRenderingContext2D, size: Size, view: View, map: TempoMap, gridTicks: number) {
  const minSpacingPx = 6;
  const line = (tick: number) => ctx.fillRect(Math.round(tickToX(view, tick)) - 0.5, 0, 1, size.height);

  for (const { bar, start, end, signature } of visibleBars(map, view, size.width)) {
    const barPx = barTicks(signature) * view.pxPerTick;
    const lines = (step: number, colour: string) => {
      if (step <= 0 || step >= end - start || step * view.pxPerTick < minSpacingPx) return;
      ctx.fillStyle = colour;
      for (let t = start + step; t < end - 0.5; t += step) line(t);
    };
    lines(gridTicks, alpha(LIGHT, 0.03));
    lines(beatTicks(signature), alpha(LIGHT, 0.07));
    // Zoomed far out, only every few bars gets a line.
    if (bar % Math.max(1, Math.ceil(minSpacingPx / barPx)) === 0) {
      ctx.fillStyle = alpha(LIGHT, 0.16);
      line(start);
    }
  }
}

export function drawClips(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  clips: Clip[],
  colour: string,
  editedClipId: number | null = null,
) {
  // Inside a clip the grid shows as it is; outside every clip it is darkened, not tinted (the owner's call,
  // 2026-11-15), so where notes belong reads at a glance.
  const inside = clips
    .map((clip) => [tickToX(view, clip.start), tickToX(view, clip.start + clip.length)] as const)
    .filter(([x0, x1]) => x1 > 0 && x0 < size.width)
    .sort((a, b) => a[0] - b[0]);
  ctx.fillStyle = alpha(SHADE, 0.32);
  let from = 0;
  for (const [x0, x1] of inside) {
    if (x0 > from) ctx.fillRect(from, 0, Math.round(x0) - from, size.height);
    from = Math.max(from, Math.round(x1));
  }
  if (from < size.width) ctx.fillRect(from, 0, size.width - from, size.height);

  for (const clip of clips) {
    const x0 = tickToX(view, clip.start);
    const x1 = tickToX(view, clip.start + clip.length);
    if (x1 < 0 || x0 > size.width) continue;
    // The edited clip's edges in its colour; the others' fainter.
    ctx.globalAlpha = editedClipId === null || editedClipId === clip.id ? 1 : 0.45;
    ctx.fillStyle = mix(colour, SHADE, 0.45);
    ctx.fillRect(Math.round(x0) - 0.5, 0, 1, size.height);
    ctx.fillRect(Math.round(x1) - 0.5, 0, 1, size.height);
  }
  ctx.globalAlpha = 1;
}

/** Width of a clip's edge handle in the roll's ruler, and how near the pointer must be to take it. */
export const CLIP_HANDLE_PX = 6;

/** The clips' extents along the bottom of the roll's ruler, with handles at their ends: dragging one resizes the clip
    (PianoRoll.tsx). The edited clip is drawn stronger. */
export function drawClipSpans(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  clips: Clip[],
  colour: string,
  editedClipId: number | null,
) {
  const y = size.height - 4;
  for (const clip of clips) {
    const x0 = tickToX(view, clip.start);
    const x1 = tickToX(view, clip.start + clip.length);
    if (x1 < 0 || x0 > size.width) continue;
    ctx.globalAlpha = editedClipId === null || editedClipId === clip.id ? 1 : 0.45;
    ctx.fillStyle = mix(colour, SHADE, 0.25);
    ctx.fillRect(x0, y, x1 - x0, 4);
    ctx.fillStyle = colour;
    ctx.fillRect(Math.round(x0), y - 6, CLIP_HANDLE_PX / 2, 10);
    ctx.fillRect(Math.round(x1) - CLIP_HANDLE_PX / 2, y - 6, CLIP_HANDLE_PX / 2, 10);
  }
  ctx.globalAlpha = 1;
}

// ---------------------------------------------------------------------------------------------------------
// Notes

const SLICE_PX = 3;
const RIBBON_STEP_PX = 2;

export function drawNotes(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  notes: PlacedNote[],
  colour: string,
  selected: ReadonlySet<number>,
  map: TempoMap,
  selectedPoints: PointSelection | null,
  region?: { start: number; end: number },
  drumMap?: DrumMap | null,
) {
  for (const placed of notes) {
    const x0 = tickToX(view, placed.start);
    const x1 = tickToX(view, placed.end);
    if (x1 < 0 || x0 > size.width) continue;

    const bent = isBent(placed.note) || (placed.glide !== undefined && placed.glide.ms > 0 && placed.glide.offset !== 0);
    const y = pitchToY(view, placed.note.pitch);
    if (!bent && (y > size.height || y + view.keyHeight < 0)) continue;

    const isSelected = selected.has(placed.note.id);
    const outsideRegion = region !== undefined && (placed.start < region.start || placed.start >= region.end);
    if (outsideRegion) ctx.globalAlpha *= 0.45;

    if (bent) drawRibbon(ctx, size, view, theme, placed, colour, isSelected, map);
    else drawBlock(ctx, size, view, theme, placed, colour, isSelected, drumMap);

    if (isSelected) drawVibratoHandles(ctx, view, theme, placed, map);

    // Points show on selected notes and on notes that have points selected by a box selection.
    const pitchSelection = pointsOf(selectedPoints, placed.note.id, 'pitch');
    if ((isSelected || pitchSelection.length > 0) && placed.note.expression.pitch.length > 0)
      drawCurveHandles(
        ctx,
        theme,
        placed.note.expression.pitch,
        (t) => tickToX(view, placed.start + t),
        (v) => pitchToY(view, placed.note.pitch + v) + view.keyHeight / 2,
        size.width,
        pitchSelection,
        (segment) => handleTimeFor(placed.note.expression.pitch, segment),
      );
    if (outsideRegion) ctx.globalAlpha /= 0.45;
  }
}

/** Vibrato handles of a selected note: a diamond for depth, a bar for rate. */
function drawVibratoHandles(ctx: CanvasRenderingContext2D, view: View, theme: Theme, placed: PlacedNote, map: TempoMap) {
  const handles = vibratoHandles(view, placed, map);
  if (!handles) return;

  ctx.fillStyle = theme.ok;
  ctx.strokeStyle = theme.onAccent;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(handles.depth.x, handles.depth.y - 5);
  ctx.lineTo(handles.depth.x + 5, handles.depth.y);
  ctx.lineTo(handles.depth.x, handles.depth.y + 5);
  ctx.lineTo(handles.depth.x - 5, handles.depth.y);
  ctx.closePath();
  ctx.fill();
  ctx.stroke();

  ctx.fillRect(handles.rate.x - 2, handles.rate.y - 7, 4, 14);
  ctx.strokeRect(handles.rate.x - 2 + 0.5, handles.rate.y - 7 + 0.5, 3, 13);
}

/** Fill colour of a note slice: track colour, brighter with velocity and (if it has a curve) pressure. */
function noteFill(theme: Theme, colour: string, placed: PlacedNote, tick: number) {
  const { note } = placed;
  if (note.expression.pressure.length === 0) return mix(theme.bg, colour, 0.35 + 0.65 * note.velocity);
  const pressure = evaluateCurve(note.expression.pressure, tick - placed.start, LANE_DEFAULTS.pressure);
  return mix(theme.bg, colour, 0.25 + 0.75 * pressure * (0.4 + 0.6 * note.velocity));
}

function drawLabel(
  ctx: CanvasRenderingContext2D,
  theme: Theme,
  view: View,
  placed: PlacedNote,
  x: number,
  y: number,
  width: number,
  drumMap?: DrumMap | null,
) {
  if (width <= 24 || view.keyHeight < 12) return;
  const piece = getDrumPiece(drumMap, placed.note.pitch);
  const label = piece ? piece.name : pitchName(placed.note.pitch);
  ctx.fillStyle = alpha(SHADE, 0.7);
  ctx.font = font(theme, 'xs');
  ctx.textBaseline = 'middle';
  ctx.fillText(label, x + 4, y + view.keyHeight / 2 + 0.5);
}

/** A note without pitch bends: a rectangle on its row. */
function drawBlock(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  placed: PlacedNote,
  colour: string,
  isSelected: boolean,
  drumMap?: DrumMap | null,
) {
  const h = view.keyHeight;
  const x0 = tickToX(view, placed.start);
  const width = Math.max(2, tickToX(view, placed.end) - x0);
  const y = pitchToY(view, placed.note.pitch);

  if (placed.note.expression.pressure.length === 0) {
    // No pressure curve: one plain rectangle (slices would leave faint anti-aliasing seams).
    ctx.fillStyle = noteFill(theme, colour, placed, placed.start);
    ctx.fillRect(x0, y + 1, width, h - 2);
  } else {
    // Integer slice edges so neighbouring slices meet without seams.
    for (let x = Math.floor(Math.max(0, x0)); x < Math.min(size.width, x0 + width); x += SLICE_PX) {
      ctx.fillStyle = noteFill(theme, colour, placed, xToTick(view, x + SLICE_PX / 2));
      const left = Math.max(x, x0);
      ctx.fillRect(left, y + 1, Math.min(x + SLICE_PX, x0 + width) - left, h - 2);
    }
  }

  ctx.strokeStyle = isSelected ? theme.text : mix(colour, SHADE, 0.35);
  ctx.lineWidth = isSelected ? 1.5 : 1;
  ctx.strokeRect(x0 + 0.5, y + 1.5, width - 1, h - 3);
  drawLabel(ctx, theme, view, placed, x0, y, width, drumMap);
}

/** A note with pitch bends: a band one row high that follows the sounding pitch. */
function drawRibbon(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  placed: PlacedNote,
  colour: string,
  isSelected: boolean,
  map: TempoMap,
) {
  const half = view.keyHeight / 2 - 1;
  const x0 = tickToX(view, placed.start);
  const x1 = tickToX(view, placed.end);
  const from = Math.max(x0, -SLICE_PX);
  const to = Math.min(x1, size.width + SLICE_PX);
  const centre = (x: number) => pitchToY(view, pitchAtTick(placed, xToTick(view, x), map)) + view.keyHeight / 2;

  // Faint outline at the base pitch while selected, so the note's anchor stays visible.
  if (isSelected) {
    const y = pitchToY(view, placed.note.pitch);
    ctx.setLineDash([3, 3]);
    ctx.strokeStyle = mix(theme.bg, theme.text, 0.45);
    ctx.lineWidth = 1;
    ctx.strokeRect(x0 + 0.5, y + 1.5, Math.max(2, x1 - x0) - 1, view.keyHeight - 3);
    ctx.setLineDash([]);
  }

  // Body: quads between successive samples of the pitch path, shaded by pressure. Include each handle's
  // own x, so the ribbon passes through the handle instead of cutting the corner between samples.
  const xsSet = new Set<number>();
  for (let x = from; x < to; x += RIBBON_STEP_PX) xsSet.add(x);
  xsSet.add(to);
  const pitchPoints = placed.note.expression.pitch;
  for (let i = 0; i + 1 < pitchPoints.length; i++) {
    if (pitchPoints[i][1] === pitchPoints[i + 1][1]) continue;
    const t = handleTimeFor(pitchPoints, i);
    const x = tickToX(view, placed.start + t);
    if (x >= from && x <= to) xsSet.add(x);
  }
  const xs = [...xsSet].sort((a, b) => a - b);
  const ys = xs.map(centre);

  for (let i = 0; i + 1 < xs.length; i++) {
    ctx.fillStyle = noteFill(theme, colour, placed, xToTick(view, (xs[i] + xs[i + 1]) / 2));
    ctx.beginPath();
    ctx.moveTo(xs[i], ys[i] - half);
    ctx.lineTo(xs[i + 1] + 0.5, ys[i + 1] - half);
    ctx.lineTo(xs[i + 1] + 0.5, ys[i + 1] + half);
    ctx.lineTo(xs[i], ys[i] + half);
    ctx.closePath();
    ctx.fill();
  }

  // Outline: along the top edge, then back along the bottom edge.
  ctx.beginPath();
  xs.forEach((x, i) => (i === 0 ? ctx.moveTo(x, ys[i] - half) : ctx.lineTo(x, ys[i] - half)));
  for (let i = xs.length - 1; i >= 0; i--) ctx.lineTo(xs[i], ys[i] + half);
  ctx.closePath();
  ctx.strokeStyle = isSelected ? theme.text : mix(colour, SHADE, 0.35);
  ctx.lineWidth = isSelected ? 1.5 : 1;
  ctx.stroke();

  drawLabel(ctx, theme, view, placed, x0, ys[0] - view.keyHeight / 2, x1 - x0);
}

/** Breakpoints (squares) and tension handles (circles that move across their segment) of a selected note's curve. */
function drawCurveHandles(
  ctx: CanvasRenderingContext2D,
  theme: Theme,
  points: CurvePoint[],
  toX: (t: number) => number,
  toY: (v: number) => number,
  width: number,
  selected: number[],
  handleTimeAt?: (segment: number) => number,
) {
  ctx.strokeStyle = theme.accent;
  ctx.lineWidth = 1.5;

  for (let i = 0; i + 1 < points.length; i++) {
    if (points[i][1] === points[i + 1][1]) continue;
    const t = handleTimeAt ? handleTimeAt(i) : handleTimeFor(points, i);
    const x = toX(t);
    if (x < -4 || x > width + 4) continue;
    ctx.beginPath();
    ctx.arc(x, toY(handleValueFor(points, i, t)), 3.5, 0, Math.PI * 2);
    ctx.stroke();
  }

  points.forEach(([t, v], i) => {
    const x = toX(t);
    if (x < -4 || x > width + 4) return;
    if (selected.includes(i)) {
      ctx.fillStyle = theme.accent;
      ctx.fillRect(x - 4, toY(v) - 4, 8, 8);
      ctx.strokeStyle = theme.text;
      ctx.lineWidth = 1;
      ctx.strokeRect(x - 4 + 0.5, toY(v) - 4 + 0.5, 7, 7);
    } else {
      ctx.fillStyle = theme.text;
      ctx.fillRect(x - 3, toY(v) - 3, 6, 6);
    }
  });
}

// ---------------------------------------------------------------------------------------------------------
// Playhead, ruler and keyboard

export function drawPlayhead(ctx: CanvasRenderingContext2D, size: Size, view: View, theme: Theme, tick: number) {
  const x = Math.round(tickToX(view, tick)) - 0.5;
  if (x < 0 || x > size.width) return;
  ctx.fillStyle = theme.playhead;
  ctx.fillRect(x, 0, 1.5, size.height);
}

/** The punch range's colour in the ruler. */

/** The ruler's upper part, where dragging sets the loop range (docs/design/phase6-arrangement.md, section 7). */
export const LOOP_ZONE = 0.42;

export function drawRuler(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  map: TempoMap,
  loop?: LoopRange,
  markers: Marker[] = [],
  showSignatures = false,
  /** The punch range (docs/design/audio-features.md, AU10): a red bar along the loop zone's bottom when on. */
  punch?: LoopRange,
) {
  ctx.fillStyle = theme.bgRaised;
  ctx.fillRect(0, 0, size.width, size.height);

  if (punch?.enabled) {
    const x0 = tickToX(view, punch.start);
    const x1 = tickToX(view, punch.end);
    const y = size.height * LOOP_ZONE - 4;
    ctx.fillStyle = theme.punch;
    ctx.fillRect(x0, y, x1 - x0, 3);
    ctx.fillRect(x0, 1, 1.5, y);
    ctx.fillRect(x1 - 1.5, 1, 1.5, y);
  }

  if (loop) {
    const x0 = tickToX(view, loop.start);
    const x1 = tickToX(view, loop.end);
    ctx.fillStyle = loop.enabled ? theme.accent : theme.border;
    ctx.globalAlpha = loop.enabled ? 0.55 : 0.8;
    ctx.fillRect(x0, 1, x1 - x0, size.height * LOOP_ZONE - 2);
    ctx.globalAlpha = 1;

    if (loop.enabled) {
      ctx.fillStyle = theme.accent;
      for (const x of [x0, x1]) {
        ctx.beginPath();
        ctx.moveTo(x, 1);
        ctx.lineTo(x + (x === x0 ? 6 : -6), 1);
        ctx.lineTo(x + (x === x0 ? 6 : -6), size.height * LOOP_ZONE - 2);
        ctx.lineTo(x, size.height * LOOP_ZONE - 2);
        ctx.closePath();
        ctx.fill(        );
      }
    }
  }

  for (const marker of markers) {
    const x = Math.round(tickToX(view, marker.position)) - 0.5;
    if (x < -2 || x > size.width) continue;
    ctx.fillStyle = theme.marker;
    ctx.fillRect(x, size.height * LOOP_ZONE, 1.5, size.height * (1 - LOOP_ZONE));
  }

  ctx.font = font(theme, 'xs', { mono: true });
  ctx.textBaseline = 'middle';
  const changes = new Set(map.signatures.map((sig) => sig.bar));

  for (const { bar, start, signature } of visibleBars(map, view, size.width)) {
    const barPx = barTicks(signature) * view.pxPerTick;
    const labelEvery = Math.max(1, Math.ceil(40 / barPx));
    const tickEvery = Math.max(1, Math.ceil(6 / barPx));
    const x = Math.round(tickToX(view, start)) - 0.5;
    const isChange = showSignatures && changes.has(bar);

    if (bar % tickEvery === 0 || isChange) {
      ctx.fillStyle = theme.border;
      ctx.fillRect(x, size.height * 0.4, 1, size.height * 0.6);
    }

    if (bar % labelEvery === 0 || isChange) {
      ctx.fillStyle = theme.textDim;
      const label = String(bar + 1);
      ctx.fillText(label, x + 4, size.height * 0.45);
      // In the piano roll, where there is no signature strip, a change shows beside its bar number.
      if (isChange) {
        ctx.fillStyle = theme.accent;
        ctx.fillText(`${signature.numerator}/${signature.denominator}`, x + 8 + ctx.measureText(label).width, size.height * 0.45);
      }
    }

    const beat = beatTicks(signature);
    for (let b = 1; b < signature.numerator && beat * view.pxPerTick > 12; b++) {
      const bx = Math.round(tickToX(view, start + b * beat)) - 0.5;
      ctx.fillStyle = theme.border;
      ctx.fillRect(bx, size.height * 0.75, 1, size.height * 0.25);
    }
  }

  ctx.fillStyle = theme.border;
  ctx.fillRect(0, size.height - 1, size.width, 1);
}

export function formatLoopPosition(tick: number, map: TempoMap) {
  const position = map.barAt(tick);
  const seconds = map.secondsAt(tick);
  const minutes = Math.floor(seconds / 60);
  const remainder = (seconds % 60).toFixed(1).padStart(4, '0');
  return `${position.bar + 1}.${position.beat + 1} (${minutes}:${remainder})`;
}

/**
 * The ruler's playhead mark: a small triangle over its top edge. It is drawn on the playhead overlay, so moving
 * the playhead only repaints that thin layer instead of the ruler's bars and labels.
 */
export function drawRulerPlayhead(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  tick: number,
) {
  const px = tickToX(view, tick);
  if (px < 0 || px > size.width) return;
  ctx.fillStyle = theme.playhead;
  ctx.beginPath();
  ctx.moveTo(px - 5, 0);
  ctx.lineTo(px + 5, 0);
  ctx.lineTo(px, 7);
  ctx.fill();
}

export function drawKeyboard(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  pressed: number | null,
  drumMap?: DrumMap | null,
  scale: ScaleShown | null = null,
) {
  ctx.fillStyle = theme.bgRaised;
  ctx.fillRect(0, 0, size.width, size.height);

  ctx.font = font(theme, 'xs');
  ctx.textBaseline = 'middle';
  // A label needs a row as tall as the type plus a pixel above and below.
  const labelled = view.keyHeight >= theme.textXs + 1;
  // With a key, the scale's keys get a mark at the right edge and the root a bar (SH3), so the labels move left.
  const markWidth = scale ? 5 : 0;

  for (const p of visibleRowPitches(view, size.height)) {
    const y = pitchToY(view, p);
    const black = isBlackKey(p);

    ctx.fillStyle = p === pressed ? theme.accent : black ? theme.keyBlack : theme.keyWhite;
    ctx.fillRect(0, y + 0.5, black ? size.width * 0.62 : size.width - markWidth, view.keyHeight - 1);

    const degree = scale ? degreeOf(p, scale.root) : ((p % 12) + 12) % 12;
    if (scale && ((scale.mask >> degree) & 1) !== 0) {
      ctx.fillStyle = degree === 0 ? theme.accent : alpha(LIGHT, 0.4);
      const height = degree === 0 ? view.keyHeight - 1 : Math.min(4, view.keyHeight - 2);
      ctx.fillRect(size.width - markWidth, y + (view.keyHeight - height) / 2, markWidth - 1, height);
    }

    const piece = getDrumPiece(drumMap, p);
    if (piece && labelled) {
      ctx.fillStyle = p === pressed ? theme.onAccent : black ? theme.textDim : theme.keyLabel;
      ctx.fillText(piece.name, 4, y + view.keyHeight / 2);
    } else if (degree === 0 && labelled) {
      ctx.fillStyle = p === pressed ? theme.onAccent : black ? theme.textDim : theme.keyLabel;
      ctx.fillText(pitchName(p), size.width - 26 - markWidth, y + view.keyHeight / 2);
    }
  }

  ctx.fillStyle = theme.border;
  ctx.fillRect(size.width - 1, 0, 1, size.height);
}

/**
 * A mark on each note outside the project's scale (docs/design/scale-highlighting.md, SH3): a small corner at its
 * start, so stray notes are easy to find.
 */
export function drawOutsideMarks(ctx: CanvasRenderingContext2D, size: Size, view: View, theme: Theme, notes: PlacedNote[], spans: readonly KeySpan[]) {
  if (!spans.some((span) => span.scale)) return;
  ctx.fillStyle = theme.marker;
  const corner = Math.min(7, view.keyHeight - 2);
  for (const placed of notes) {
    // Against the key where the note starts (SH9).
    if (!outsideScale(placed.note.pitch, scaleAt(spans, placed.start))) continue;
    const x = Math.round(tickToX(view, placed.start));
    const y = Math.round(pitchToY(view, placed.note.pitch));
    if (x > size.width || x + corner < 0 || y > size.height || y + view.keyHeight < 0) continue;
    ctx.beginPath();
    ctx.moveTo(x + 1, y + 1);
    ctx.lineTo(x + 1 + corner, y + 1);
    ctx.lineTo(x + 1, y + 1 + corner);
    ctx.closePath();
    ctx.fill();
  }
}

// ---------------------------------------------------------------------------------------------------------
// Lanes

/** Vertical padding of the lane drawing area (values map into the height minus twice this). */
export const LANE_PAD = 6;

/** Default (and widest) value range of each lane. */
export const LANE_RANGES: Record<LaneId | 'velocity', [number, number]> = {
  velocity: [0, 1],
  pressure: [0, 1],
  timbre: [0, 1],
  vibrato: [0, 1],
  pitch: [-12, 12],
};

export function drawLane(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  lane: LaneId | 'velocity',
  range: [number, number],
  notes: PlacedNote[],
  colour: string,
  selected: ReadonlySet<number>,
  selectedPoints: PointSelection | null,
  map: TempoMap,
  gridTicks: number,
) {
  ctx.fillStyle = theme.bg;
  ctx.fillRect(0, 0, size.width, size.height);
  drawTimeGrid(ctx, size, view, map, gridTicks);

  const [min, max] = range;
  const pad = LANE_PAD;
  const valueToY = (v: number) => pad + (1 - (v - min) / (max - min)) * (size.height - 2 * pad);

  drawLaneGrid(ctx, size, lane, min, max, valueToY, theme);

  const showAll = selected.size === 0;

  for (const placed of notes) {
    const { note } = placed;
    const x0 = tickToX(view, placed.start);
    const x1 = tickToX(view, placed.end);
    if (x1 < 0 || x0 > size.width) continue;

    const isSelected = selected.has(note.id);
    const dim = !showAll && !isSelected;

    if (lane === 'velocity') {
      ctx.fillStyle = dim ? mix(theme.bg, colour, 0.3) : colour;
      const y = valueToY(note.velocity);
      ctx.fillRect(Math.round(x0), y, 2, size.height - pad - y);
      ctx.beginPath();
      ctx.arc(Math.round(x0) + 1, y, 3, 0, Math.PI * 2);
      ctx.fill();
      continue;
    }

    // Curves are drawn for every note (dimmed when not selected); points only for selected notes and notes with
    // selected points.
    const points = note.expression[lane];
    const startX = Math.max(0, x0);
    const endX = Math.min(size.width, x1);
    const xs = new Set<number>();
    for (let x = startX; x <= endX; x += 2) xs.add(x);
    xs.add(startX);
    xs.add(endX);
    for (let i = 0; i + 1 < points.length; i++) {
      if (points[i][1] === points[i + 1][1]) continue;
      const t = handleTimeFor(points, i);
      const x = tickToX(view, placed.start + t);
      if (x >= startX && x <= endX) xs.add(x);
    }
    ctx.beginPath();
    [...xs]
      .sort((a, b) => a - b)
      .forEach((x, index) => {
        const value = evaluateCurve(points, xToTick(view, x) - placed.start, LANE_DEFAULTS[lane]);
        if (index === 0) ctx.moveTo(x, valueToY(value));
        else ctx.lineTo(x, valueToY(value));
      });
    ctx.strokeStyle = dim ? mix(theme.bg, colour, 0.35) : colour;
    ctx.lineWidth = isSelected ? 2 : 1.5;
    ctx.stroke();

    const laneSelection = pointsOf(selectedPoints, note.id, lane);
    if (isSelected || laneSelection.length > 0)
      drawCurveHandles(
        ctx,
        theme,
        points,
        (t) => tickToX(view, placed.start + t),
        valueToY,
        size.width,
        laneSelection,
        (segment) => handleTimeFor(points, segment),
      );
  }
}

/** Horizontal guide lines: whole semitones (octaves stronger) for pitch, quarters for the others. */
function drawLaneGrid(
  ctx: CanvasRenderingContext2D,
  size: Size,
  lane: LaneId | 'velocity',
  min: number,
  max: number,
  valueToY: (v: number) => number,
  theme: Theme,
) {
  const step = lane === 'pitch' ? 1 : 0.25;
  if (Math.abs(valueToY(0) - valueToY(step)) < 4) return; // too dense to be useful

  ctx.font = font(theme, 'xs', { mono: true });
  ctx.textBaseline = 'middle';

  for (let v = Math.ceil(min / step) * step; v <= max + 1e-9; v += step) {
    const y = Math.round(valueToY(v)) - 0.5;
    const strong = lane === 'pitch' ? v % 12 === 0 : v === 0 || v === 1;
    ctx.fillStyle = strong ? alpha(LIGHT, 0.12) : alpha(LIGHT, 0.04);
    ctx.fillRect(0, y, size.width, 1);

    if (lane === 'pitch' && Math.abs(valueToY(0) - valueToY(1)) >= 10) {
      ctx.fillStyle = theme.textDim;
      ctx.fillText(v > 0 ? `+${v}` : String(v), 4, y);
    }
  }
}

/** Box-selection rectangle. */
/** The links between notes: a thin line from where the earlier note hands over to the later note's start. */
export function drawLinks(ctx: CanvasRenderingContext2D, view: View, colour: string, lines: LinkLine[]) {
  ctx.save();
  ctx.strokeStyle = colour;
  ctx.lineWidth = 2;
  for (const line of lines) {
    ctx.beginPath();
    ctx.moveTo(tickToX(view, line.fromTick), pitchToY(view, line.fromPitch) + view.keyHeight / 2);
    ctx.lineTo(tickToX(view, line.toTick), pitchToY(view, line.toPitch) + view.keyHeight / 2);
    ctx.stroke();
  }
  ctx.restore();
}

/** The line being drawn from a note to the note it will link with. */
export function drawLinkPreview(ctx: CanvasRenderingContext2D, colour: string, drag: { x0: number; y0: number; x1: number; y1: number }) {
  ctx.save();
  ctx.strokeStyle = colour;
  ctx.lineWidth = 2;
  ctx.setLineDash([4, 3]);
  ctx.beginPath();
  ctx.moveTo(drag.x0, drag.y0);
  ctx.lineTo(drag.x1, drag.y1);
  ctx.stroke();
  ctx.restore();
}

export function drawBand(ctx: CanvasRenderingContext2D, theme: Theme, band: { x0: number; y0: number; x1: number; y1: number }) {
  const x = Math.min(band.x0, band.x1);
  const y = Math.min(band.y0, band.y1);
  const w = Math.abs(band.x1 - band.x0);
  const h = Math.abs(band.y1 - band.y0);
  ctx.fillStyle = alpha(theme.accent, 0.12);
  ctx.fillRect(x, y, w, h);
  ctx.strokeStyle = theme.accent;
  ctx.lineWidth = 1;
  ctx.strokeRect(x + 0.5, y + 0.5, w, h);
}
