import type { AudioFile, Clip, TimingMarker } from '../bridge/protocol';
import { drawRuler, drawTimeGrid, visibleBars } from '../pianoroll/draw';
import { alpha, LIGHT, type Size, type Theme } from '../canvas/theme';
import { barTicks, beatTicks } from '../time/tempoMap';
import type { View } from '../pianoroll/geometry';
import { tickToX, xToTick } from '../pianoroll/geometry';
import type { TempoMap } from '../time/tempoMap';
import { drawWave } from '../timeline/waveform';
import type { EditorAxis } from './axis';
import type { OnsetList } from './onsets';

// Drawing of the audio editor (docs/design/time-stretching.md, TS13): the ruler, the file's waveform through its
// timing map, its transients, the clip's range and the timing markers.

export const RULER_HEIGHT = 24;
/** The strip along the top where the file's other clips show as faint ranges. */
export const CLIPS_STRIP = 8;
export const MARKER_HIT_PX = 5;

export interface EditorScene {
  view: View;
  map: TempoMap;
  theme: Theme;
  file: AudioFile;
  clip: Clip;
  axis: EditorAxis;
  others: Array<[number, number]>;
  onsets: OnsetList | undefined;
  selected: ReadonlySet<number>;
  playheadTick: number;
  /** A box selection being dragged, in pixels. */
  box: [number, number] | null;
  /** The snap grid. */
  gridTicks: number;
}

/** A grid of a 1/16 note or coarser, at least 8 px apart. */
export function gridFor(view: View) {
  let grid = 15360 / 4;
  while (grid * view.pxPerTick < 8 && grid < 15360 * 16) grid *= 2;
  return grid;
}

export const markerX = (scene: Pick<EditorScene, 'view' | 'axis'>, marker: TimingMarker) =>
  tickToX(scene.view, scene.axis.tickOfContent(marker.tick));

export function drawEditor(ctx: CanvasRenderingContext2D, size: Size, scene: EditorScene) {
  const { view, map, theme, file, clip, axis } = scene;
  ctx.fillStyle = theme.bg;
  ctx.fillRect(0, 0, size.width, size.height);

  drawRuler(ctx, { width: size.width, height: RULER_HEIGHT }, view, theme, map);
  const top = RULER_HEIGHT + CLIPS_STRIP;
  const height = size.height - top;

  // Other clips of the file, then the edited clip's range.
  ctx.fillStyle = alpha(LIGHT, 0.12);
  for (const [a, b] of scene.others) ctx.fillRect(tickToX(view, a), RULER_HEIGHT + 2, tickToX(view, b) - tickToX(view, a), CLIPS_STRIP - 4);
  const clipX0 = tickToX(view, clip.start);
  const clipX1 = tickToX(view, clip.start + clip.length);
  ctx.fillStyle = alpha(theme.accent, 0.08);
  ctx.fillRect(clipX0, top, clipX1 - clipX0, height);

  ctx.save();
  ctx.translate(0, top);
  drawTimeGrid(ctx, { width: size.width, height }, view, map, scene.gridTicks);
  ctx.restore();

  // The wave through the axis (and so through the timing map); brighter over the clip's range.
  const mid = top + height / 2;
  const half = height / 2 - 4;
  const wave = (x0: number, x1: number, alpha: number) => {
    ctx.globalAlpha = alpha;
    drawWave(
      ctx,
      {
        source: { fileId: file.id },
        frames: file.frames,
        x0: Math.max(0, x0),
        x1: Math.min(size.width, x1),
        frameAt: (x) => axis.tickToFrame(xToTick(view, x)),
        mid,
        half,
        top,
        bottom: top + height,
      },
      theme.accent,
    );
    ctx.globalAlpha = 1;
  };
  wave(0, Math.floor(clipX0), 0.4);
  wave(Math.floor(clipX0), Math.ceil(clipX1), 0.9);
  wave(Math.ceil(clipX1), size.width, 0.4);

  // Transients: a tick along the bottom as tall as the onset is strong, and a very faint line through the wave, so
  // they don't drown the bar lines. At wide zooms only the stronger ones.
  if (scene.onsets) {
    const pxPerSecond = (view.pxPerTick * map.bpmAt(view.startTick) * 15360) / 60;
    const weakest = pxPerSecond > 400 ? 0 : pxPerSecond > 100 ? 0.15 : 0.35;
    const floor = top + height;
    ctx.fillStyle = theme.transient;
    for (const [frame, strength] of scene.onsets) {
      if (strength < weakest) continue;
      const x = Math.round(tickToX(view, axis.frameToTick(frame)));
      if (x < 0 || x > size.width) continue;
      ctx.globalAlpha = 0.03 + 0.05 * strength;
      ctx.fillRect(x, top, 1, height);
      ctx.globalAlpha = 0.35 + 0.5 * strength;
      const tick = 4 + 10 * strength;
      ctx.fillRect(x, floor - tick, 1, tick);
    }
    ctx.globalAlpha = 1;
  }

  // Bar and beat lines on top of the wave and the transients, where they stay readable.
  for (const { bar, start, end, signature } of visibleBars(map, view, size.width)) {
    const barPx = barTicks(signature) * view.pxPerTick;
    const beat = beatTicks(signature);
    if (beat * view.pxPerTick >= 8) {
      ctx.fillStyle = alpha(LIGHT, 0.1);
      for (let t = start + beat; t < end - 0.5; t += beat) ctx.fillRect(Math.round(tickToX(view, t)), top, 1, height);
    }
    if (bar % Math.max(1, Math.ceil(8 / barPx)) === 0) {
      ctx.fillStyle = alpha(LIGHT, 0.62);
      ctx.fillRect(Math.round(tickToX(view, start)), top, 1, height);
    }
  }

  // Timing markers: handles across the full height.
  file.timing.markers.forEach((marker, index) => {
    const x = Math.round(markerX(scene, marker)) + 0.5;
    if (x < -10 || x > size.width + 10) return;
    const selected = scene.selected.has(index);
    ctx.fillStyle = selected ? theme.ok : theme.warpMarker;
    ctx.fillRect(x - 0.5, top, selected ? 2 : 1, height);
    ctx.beginPath();
    ctx.moveTo(x - 5, top);
    ctx.lineTo(x + 5, top);
    ctx.lineTo(x, top + 7);
    ctx.fill();
  });

  if (scene.box) {
    const [a, b] = scene.box;
    ctx.fillStyle = alpha(theme.accent, 0.12);
    ctx.fillRect(Math.min(a, b), top, Math.abs(b - a), height);
  }

  const playhead = tickToX(view, scene.playheadTick);
  if (playhead >= 0 && playhead <= size.width) {
    ctx.fillStyle = theme.playhead;
    ctx.fillRect(Math.round(playhead), 0, 1, size.height);
  }
}
