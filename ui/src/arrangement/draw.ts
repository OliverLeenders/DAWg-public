import type { AudioFile, Clip, CurvePoint, Marker } from '../bridge/protocol';
import { patternName, sequencerOf, stepHits } from '../dock/stepGrid';
import { handleTimeFor, handleValueFor } from '../pianoroll/curveEditing';
import { evaluateCurve } from '../pianoroll/geometry';
import { drawTimeGrid } from '../pianoroll/draw';
import { alpha, font, LIGHT, mix, SHADE, type Size, type Theme } from '../canvas/theme';
import { tickToX, xToTick, type View } from '../pianoroll/geometry';
import type { TempoMap } from '../time/tempoMap';
import { audioTime } from '../timeline/audioTime';
import { audibleNotes, clipColour, clipName, contentAt, isAudio, isLooped, loopBoundaries } from '../timeline/clips';
import type { CrossfadeTail } from '../timeline/crossfades';
import { drawWave } from '../timeline/waveform';
import { TimingMap } from '../time/timingMap';
import { audioHandles, clipBox, HANDLE_PX, LOOP_HANDLE_PX, NAME_BAR, type AutomationRow, type ClipBox, type Lane } from './layout';

// Arrangement drawing: lanes, clips with a note miniature, loop boundaries, the playhead and the loop range.

export interface ClipDraw {
  clip: Clip;
  lane: Lane;
  /** Drawn faded (the original of a clip being moved or copied). */
  faded?: boolean;
  /** Show the clip gain next to its handle (while dragging it). */
  showGain?: boolean;
  /** In a takes row: the spans something on top covers, drawn dimmed (docs/design/takes-lanes.md, T5). */
  covered?: Array<[number, number]>;
  /** A value shown in the clip while dragging (the ratio and source tempo of a stretch). */
  readout?: string;
  /** In the track's row: covered clips playing on under this clip's fades (docs/design/audio-features.md, AU8). */
  crossfades?: CrossfadeTail[];
}

/** What audio clips need to draw their waveform. */
export interface AudioContext {
  files: Record<string, AudioFile>;
  /** The tempo map: audio content runs at the tempo where each clip starts (timeline/audioTime.ts). */
  map: TempoMap;
  /** Ids of pool files that can't be found. */
  missing: ReadonlySet<number>;
  /** Clips whose stretched audio is still rendering (docs/design/time-stretching.md, TS9). */
  stretchPending?: ReadonlySet<number>;
}

export function drawLanes(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  lanes: Lane[],
  scrollY: number,
  selectedTrackId: number | null,
  map: TempoMap,
  gridTicks: number,
) {
  ctx.fillStyle = theme.bg;
  ctx.fillRect(0, 0, size.width, size.height);

  for (const lane of lanes) {
    const top = lane.top - scrollY;
    if (top > size.height || top + lane.height < 0) continue;
    if (lane.track.id === selectedTrackId && lane.role === 'track') {
      ctx.fillStyle = alpha(theme.accent, 0.05);
      ctx.fillRect(0, top, size.width, lane.height);
    } else if (lane.index % 2 === 1) {
      ctx.fillStyle = alpha(LIGHT, 0.012);
      ctx.fillRect(0, top, size.width, lane.height);
    }

    // The returns section's heading (R10).
    if (lane.role === 'returnsHeading') {
      ctx.fillStyle = alpha(SHADE, 0.25);
      ctx.fillRect(0, top, size.width, lane.height);
      continue;
    }

    // Groups (docs/design/groups-and-sends.md, R9): a faint wash of the group's colour over its members, a
    // stronger one on its arms and folded lane, and where the members' clips are.
    const group = lane.track.kind === 'group' ? lane.track : lane.enclosing[lane.enclosing.length - 1];
    if (group) {
      ctx.globalAlpha = lane.role !== 'track' || lane.track.kind === 'group' ? 0.1 : 0.035;
      ctx.fillStyle = group.colour;
      ctx.fillRect(0, top, size.width, lane.height);
      ctx.globalAlpha = 1;
    }
    if (lane.summary) drawSummary(ctx, size, view, lane, top);
  }

  // Automation lanes have a darker background (docs/design/phase10-automation.md, section 5), and takes rows a lighter
  // shade of it (docs/design/takes-lanes.md, T5).
  ctx.fillStyle = alpha(SHADE, 0.1);
  for (const lane of lanes)
    for (const row of lane.takes) {
      const top = row.top - scrollY;
      if (top <= size.height && top + row.height >= 0) ctx.fillRect(0, top, size.width, row.height);
    }
  ctx.fillStyle = alpha(SHADE, 0.18);
  for (const lane of lanes)
    for (const row of lane.automation) {
      const top = row.top - scrollY;
      if (top <= size.height && top + row.height >= 0) ctx.fillRect(0, top, size.width, row.height);
    }

  drawTimeGrid(ctx, size, view, map, gridTicks);

  ctx.fillStyle = theme.border;
  for (const lane of lanes) {
    const rows = [lane, ...lane.takes, ...lane.automation];
    for (const row of rows) {
      const bottom = Math.round(row.top + row.height - scrollY) - 0.5;
      if (bottom >= 0 && bottom <= size.height) ctx.fillRect(0, bottom, size.width, 1);
    }
  }
}

/** What a stacked take doesn't play, in its takes row: dimmed, so the parts that play stand out (T5). */
export function drawCovered(ctx: CanvasRenderingContext2D, size: Size, view: View, theme: Theme, box: ClipBox, spans: Array<[number, number]>) {
  ctx.fillStyle = theme.bg;
  ctx.globalAlpha = 0.62;
  for (const [from, to] of spans) {
    const x0 = Math.max(box.x0, tickToX(view, from));
    const x1 = Math.min(box.x1, tickToX(view, to));
    if (x1 <= 0 || x0 >= size.width || x1 <= x0) continue;
    ctx.fillRect(x0, box.y0, x1 - x0, box.y1 - box.y0);
  }
  ctx.globalAlpha = 1;
}

/** A take being picked by a drag across it: the part that will go on top. */
export function drawCompRange(ctx: CanvasRenderingContext2D, theme: Theme, box: ClipBox, x0: number, x1: number) {
  const [left, right] = [Math.max(box.x0, Math.min(x0, x1)), Math.min(box.x1, Math.max(x0, x1))];
  if (right <= left) return;
  ctx.strokeStyle = theme.accent;
  ctx.lineWidth = 2;
  ctx.strokeRect(left + 1, box.y0 + 1, right - left - 2, box.y1 - box.y0 - 2);
}

/** Where a group's members have clips: a thin strip on its top arm, faint blocks on its folded lane. */
function drawSummary(ctx: CanvasRenderingContext2D, size: Size, view: View, lane: Lane, top: number) {
  const thin = lane.role === 'groupTop';
  const [y, h] = thin ? [top + lane.height / 2 - 2, 4] : [top + 3, lane.height - 6];
  ctx.fillStyle = lane.track.colour;
  ctx.globalAlpha = thin ? 0.7 : 0.35;
  for (const span of lane.summary ?? []) {
    const x0 = tickToX(view, span.start);
    const x1 = tickToX(view, span.end);
    if (x1 < 0 || x0 > size.width) continue;
    ctx.fillRect(x0, y, Math.max(1, x1 - x0), h);
  }
  ctx.globalAlpha = 1;
}

/** Vertical padding inside an automation lane, so 0 and 1 stay visible. */
export const AUTOMATION_PAD = 6;

/** The y of a lane value (0..1) in canvas coordinates. */
export const automationY = (row: AutomationRow, value: number, scrollY: number) =>
  row.top - scrollY + AUTOMATION_PAD + (1 - value) * Math.max(1, row.height - 2 * AUTOMATION_PAD);

/**
 * Draws an automation lane: the curve (flat at `neutral` while it has no points), its points, and the selected
 * ones highlighted. The value playing now moves with the playhead, so it is drawn on the playhead overlay instead
 * (drawPlayedValues).
 */
export function drawAutomation(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  row: AutomationRow,
  points: CurvePoint[],
  scrollY: number,
  colour: string,
  options: { neutral: number; selected: ReadonlySet<number> },
) {
  const top = row.top - scrollY;
  if (top > size.height || top + row.height < 0) return;

  ctx.save();
  ctx.beginPath();
  ctx.rect(0, top, size.width, row.height);
  ctx.clip();

  const active = row.lane.active;
  ctx.strokeStyle = active ? colour : theme.textDim;
  ctx.globalAlpha = active ? 1 : 0.5;
  ctx.lineWidth = 1.5;
  ctx.beginPath();

  if (points.length === 0) {
    const y = automationY(row, options.neutral, scrollY);
    ctx.moveTo(0, y);
    ctx.lineTo(size.width, y);
  } else {
    // Sample the curve per few pixels, so shapes (hold, power) draw as they play. Include each control point's
    // x coordinate so a bent segment is not visually undersampled.
    const step = 3;
    const xs = new Set<number>();
    for (let x = 0; x <= size.width + step; x += step) xs.add(x);
    for (let i = 0; i + 1 < points.length; i++) {
      if (points[i][1] === points[i + 1][1]) continue;
      const t = handleTimeFor(points, i);
      const x = tickToX(view, t);
      if (x >= 0 && x <= size.width + step) xs.add(x);
    }
    [...xs]
      .sort((a, b) => a - b)
      .forEach((x, index) => {
        const tick = view.startTick + x / view.pxPerTick;
        const y = automationY(row, evaluateCurve(points, tick, options.neutral), scrollY);
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
  }

  ctx.stroke();

  // A light fill under the curve.
  ctx.lineTo(size.width, top + row.height);
  ctx.lineTo(0, top + row.height);
  ctx.closePath();
  ctx.fillStyle = colour;
  ctx.globalAlpha = active ? 0.08 : 0.04;
  ctx.fill();
  ctx.globalAlpha = 1;

  points.forEach((p, i) => {
    const x = tickToX(view, p[0]);
    if (x < -6 || x > size.width + 6) return;
    const y = automationY(row, p[1], scrollY);
    ctx.fillStyle = options.selected.has(i) ? theme.text : colour;
    ctx.fillRect(x - 3, y - 3, 6, 6);
  });

  ctx.strokeStyle = theme.accent;
  ctx.lineWidth = 1.5;
  for (let i = 0; i + 1 < points.length; i++) {
    if (points[i][1] === points[i + 1][1]) continue;
    const t = handleTimeFor(points, i);
    const x = tickToX(view, t);
    if (x < -6 || x > size.width + 6) continue;
    ctx.beginPath();
    ctx.arc(x, automationY(row, handleValueFor(points, i, t), scrollY), 3.5, 0, Math.PI * 2);
    ctx.stroke();
  }

  ctx.restore();
}

/**
 * The value each active automation lane plays at the playhead, drawn on the playhead overlay so the lanes beneath
 * it do not redraw at the telemetry rate.
 */
export function drawPlayedValues(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  rows: AutomationRow[],
  values: Record<string, number> | undefined,
  scrollY: number,
  playheadTick: number,
) {
  if (!values) return;
  const x = tickToX(view, playheadTick);
  if (x < -6 || x > size.width + 6) return;
  for (const row of rows) {
    if (!row.lane.active) continue;
    const value = values[String(row.lane.id)];
    if (value === undefined) continue;
    const top = row.top - scrollY;
    if (top > size.height || top + row.height < 0) continue;
    const y = automationY(row, value, scrollY);
    ctx.fillStyle = theme.text;
    ctx.beginPath();
    ctx.arc(x, y, 3.5, 0, Math.PI * 2);
    ctx.fill();
  }
}

function roundRect(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number) {
  ctx.beginPath();
  ctx.roundRect(x, y, w, h, Math.min(r, w / 2, h / 2));
}

export function drawClip(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  { clip, lane, faded, readout, crossfades }: ClipDraw,
  scrollY: number,
  selected: boolean,
  /** Linked with a selected clip: drawn with a dashed outline. */
  linkedToSelection = false,
  audio?: AudioContext,
  showGain = false,
) {
  const box = clipBox(view, lane, clip, scrollY);
  if (box.x1 < 0 || box.x0 > size.width || box.y1 < 0 || box.y0 > size.height) return;

  const colour = clipColour(clip, lane.track);
  const width = Math.max(2, box.x1 - box.x0);
  const height = box.y1 - box.y0;

  ctx.save();
  if (faded) ctx.globalAlpha = 0.35;

  // Body and name bar.
  roundRect(ctx, box.x0, box.y0, width, height, 4);
  ctx.fillStyle = mix(colour, theme.bg, 0.72);
  ctx.fill();
  ctx.save();
  ctx.clip();
  ctx.fillStyle = mix(colour, theme.bg, 0.2);
  ctx.fillRect(box.x0, box.y0, width, Math.min(NAME_BAR, height));

  if (width > 24 && height >= NAME_BAR) {
    let textX = box.x0 + 5;

    if (clip.linkId !== 0) {
      drawLinkIcon(ctx, theme, textX, box.y0 + NAME_BAR / 2);
      textX += 14;
    }

    ctx.font = font(theme, 'xs');
    ctx.textBaseline = 'middle';
    ctx.fillStyle = theme.onAccent;
    // A sequencer clip names its pattern and, when it isn't step 1, its start step (docs/design/step-sequencer.md).
    const label =
      clip.sequencerPattern >= 0
        ? `${patternName(clip.sequencerPattern)}${clip.sequencerStartStep > 0 ? ` from ${clip.sequencerStartStep + 1}` : ''} · ${clipName(clip, lane.track)}`
        : clipName(clip, lane.track);
    ctx.fillText(label, textX, box.y0 + NAME_BAR / 2 + 0.5, box.x1 - textX - 5 - LOOP_HANDLE_PX);
  }

  if (isAudio(clip)) {
    if (audio) drawWaveform(ctx, theme, size, view, clip, box, colour, audio, crossfades);
  }

  // Note miniature: pitch range fitted to the body.
  const notes = isAudio(clip) ? [] : audibleNotes(clip);
  const bodyTop = box.y0 + NAME_BAR + 2;
  const bodyHeight = box.y1 - bodyTop - 2;

  // A sequencer clip: what its pattern plays, a row of the sequencer per line (steps by chance drawn fainter).
  const steps = clip.sequencerPattern >= 0 ? sequencerOf(lane.track)?.steps : undefined;
  if (steps && bodyHeight > 4 && steps.rows.length > 0) {
    const rowHeight = Math.max(1, Math.min(5, bodyHeight / steps.rows.length));
    const top = bodyTop + (bodyHeight - rowHeight * steps.rows.length) / 2;
    const fill = mix(colour, LIGHT, 0.25);
    for (const hit of stepHits(steps, clip, Math.max(clip.start, xToTick(view, 0)), xToTick(view, size.width))) {
      const x0 = tickToX(view, hit.start);
      const x1 = tickToX(view, hit.end);
      ctx.globalAlpha = (faded ? 0.35 : 1) * (hit.chancy ? 0.45 : 1);
      ctx.fillStyle = fill;
      ctx.fillRect(x0, top + hit.row * rowHeight, Math.max(1, x1 - x0 - 1), Math.max(1, rowHeight - 1));
    }
    ctx.globalAlpha = faded ? 0.35 : 1;
  }

  if (notes.length > 0 && bodyHeight > 4) {
    const low = Math.min(...notes.map((n) => n.note.pitch)) - 1;
    const high = Math.max(...notes.map((n) => n.note.pitch)) + 1;
    const rowHeight = Math.max(1, Math.min(6, bodyHeight / (high - low + 1)));
    ctx.fillStyle = mix(colour, LIGHT, 0.25);

    for (const n of notes) {
      const x0 = tickToX(view, n.start);
      const x1 = tickToX(view, n.end);
      if (x1 < 0 || x0 > size.width) continue;
      const y = bodyTop + ((high - n.note.pitch) / (high - low)) * (bodyHeight - rowHeight);
      ctx.fillRect(x0, y, Math.max(1, x1 - x0 - 1), rowHeight);
    }
  }

  // Loop boundaries: a notch and a dashed line where the content starts over.
  if (isLooped(clip)) {
    ctx.strokeStyle = mix(colour, SHADE, 0.35);
    ctx.setLineDash([3, 3]);
    for (const t of loopBoundaries(clip)) {
      const x = Math.round(tickToX(view, t)) + 0.5;
      ctx.beginPath();
      ctx.moveTo(x, box.y0 + NAME_BAR);
      ctx.lineTo(x, box.y1);
      ctx.stroke();
    }
    ctx.setLineDash([]);
  }
  ctx.restore();

  // Fade and gain handles of audio clips.
  if (isAudio(clip) && width > 4 * HANDLE_PX && height >= NAME_BAR + HANDLE_PX + 4) {
    const h = audioHandles(view, box, clip, audio?.map);
    ctx.fillStyle = theme.text;
    ctx.strokeStyle = theme.onAccent;
    for (const x of [h.fadeIn, h.fadeOut, h.gain]) {
      ctx.fillRect(x, h.y, HANDLE_PX, HANDLE_PX);
      ctx.strokeRect(x + 0.5, h.y + 0.5, HANDLE_PX - 1, HANDLE_PX - 1);
    }
    if (showGain) {
      ctx.font = font(theme, 'xs', { mono: true });
      ctx.fillStyle = theme.text;
      ctx.fillText(`${clip.gainDb > 0 ? '+' : ''}${clip.gainDb.toFixed(1)} dB`, h.gain + HANDLE_PX + 4, h.y + HANDLE_PX / 2);
    }
  }

  if (readout) {
    ctx.font = font(theme, 'xs', { mono: true });
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = theme.text;
    ctx.fillText(readout, box.x1 - 6, box.y0 + NAME_BAR + 12);
    ctx.textAlign = 'left';
  }

  // Loop handle in the top-right corner.
  if (width > 3 * LOOP_HANDLE_PX && height >= NAME_BAR) {
    ctx.fillStyle = isLooped(clip) ? theme.onAccent : alpha(theme.onAccent, 0.45);
    ctx.beginPath();
    ctx.moveTo(box.x1 - LOOP_HANDLE_PX, box.y0);
    ctx.lineTo(box.x1, box.y0);
    ctx.lineTo(box.x1, box.y0 + LOOP_HANDLE_PX);
    ctx.fill();
  }

  roundRect(ctx, box.x0 + 0.5, box.y0 + 0.5, width - 1, height - 1, 4);
  ctx.strokeStyle = selected || linkedToSelection ? theme.text : mix(colour, theme.bg, 0.35);
  ctx.lineWidth = selected ? 1.5 : 1;
  if (linkedToSelection && !selected) ctx.setLineDash([4, 3]);
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.restore();
}

/** Two interlocked rings: the mark of a linked clip. */
function drawLinkIcon(ctx: CanvasRenderingContext2D, theme: Theme, x: number, y: number) {
  ctx.save();
  ctx.strokeStyle = theme.onAccent;
  ctx.lineWidth = 1.4;
  for (const dx of [0, 5]) {
    ctx.beginPath();
    ctx.roundRect(x + dx, y - 3, 6, 6, 3);
    ctx.stroke();
  }
  ctx.restore();
}

export function drawLoopRange(ctx: CanvasRenderingContext2D, size: Size, view: View, theme: Theme, start: number, end: number) {
  ctx.fillStyle = alpha(theme.accent, 0.06);
  ctx.fillRect(tickToX(view, start), 0, tickToX(view, end) - tickToX(view, start), size.height);
}

export function drawMarkerRow(
  ctx: CanvasRenderingContext2D,
  size: Size,
  view: View,
  theme: Theme,
  markers: Marker[],
  selectedId: number | null,
) {
  ctx.fillStyle = theme.bgRaised;
  ctx.fillRect(0, 0, size.width, size.height);
  ctx.font = font(theme, 'xs');
  ctx.textBaseline = 'middle';

  for (const marker of markers) {
    const x = Math.round(tickToX(view, marker.position)) - 0.5;
    if (x < -200 || x > size.width) continue;
    ctx.fillStyle = theme.marker;
    ctx.fillRect(x, 0, 1.5, size.height);
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x + 7, 0);
    ctx.lineTo(x, 7);
    ctx.fill();
    const selected = marker.id === selectedId;
    if (selected) {
      // A selected marker: its name on a chip, so it reads as selected and not just hovered.
      ctx.globalAlpha = 0.28;
      ctx.fillRect(x + 1.5, 1, ctx.measureText(marker.name).width + 8, size.height - 3);
      ctx.globalAlpha = 1;
    }
    ctx.fillStyle = selected ? theme.text : theme.marker;
    ctx.fillText(marker.name, x + 5, size.height / 2 + 1);
  }

  ctx.fillStyle = theme.border;
  ctx.fillRect(0, size.height - 1, size.width, 1);
}

export function drawBox(ctx: CanvasRenderingContext2D, theme: Theme, x0: number, y0: number, x1: number, y1: number) {
  ctx.fillStyle = alpha(theme.accent, 0.12);
  ctx.strokeStyle = theme.accent;
  ctx.lineWidth = 1;
  ctx.fillRect(Math.min(x0, x1), Math.min(y0, y1), Math.abs(x1 - x0), Math.abs(y1 - y0));
  ctx.strokeRect(Math.min(x0, x1) + 0.5, Math.min(y0, y1) + 0.5, Math.abs(x1 - x0), Math.abs(y1 - y0));
}

/** Fade gain (same curves as fadeGain() in engine/src/AudioPlayer.cpp). */
function fadeGain(shape: number, x: number) {
  const t = Math.min(1, Math.max(0, x));
  if (shape === 1) return Math.sin((t * Math.PI) / 2);
  if (shape === 2) return t * t;
  if (shape === 3) return 0.5 - 0.5 * Math.cos(t * Math.PI);
  return t;
}

/** A mirrored waveform scaled by clip gain and fades; fades are also drawn as curves. */
function drawWaveform(
  ctx: CanvasRenderingContext2D,
  theme: Theme,
  size: Size,
  view: View,
  clip: Clip,
  box: ClipBox,
  colour: string,
  audio: AudioContext,
  crossfades?: CrossfadeTail[],
) {
  const top = box.y0 + NAME_BAR + 1;
  const bottom = box.y1 - 1;
  const mid = (top + bottom) / 2;
  const half = (bottom - top) / 2;
  const file = audio.files[String(clip.fileId)];

  if (!file || audio.missing.has(clip.fileId)) {
    ctx.fillStyle = theme.warn;
    ctx.font = font(theme, 'xs');
    ctx.fillText('Missing audio', box.x0 + 6, mid);
    return;
  }

  // In the clip's content ticks, which run at a constant rate in real time (docs/design/tempo-map.md, TM8); for
  // musical files they are musical ticks. Either maps to frames through the file's timing map (in content seconds for
  // one-shot files), so stretched audio looks stretched (docs/design/time-stretching.md, TS12).
  const time = audioTime(clip, audio.map);
  const musical = file.timing?.mode === 'musical';
  const fileTiming = new TimingMap(file.timing, file.sampleRate);
  const frameOf = (content: number) => fileTiming.frameAt(musical ? content : content * time.secondsPerTick);
  const gain = Math.pow(10, clip.gainDb / 20);
  const fadeAt = (local: number) => {
    let g = gain;
    const fromStart = local;
    const toEnd = time.localLength - local;
    if (clip.fadeIn > 0 && fromStart < clip.fadeIn) g *= fadeGain(clip.fadeInShape, fromStart / clip.fadeIn);
    if (clip.fadeOut > 0 && toEnd < clip.fadeOut) g *= fadeGain(clip.fadeOutShape, toEnd / clip.fadeOut);
    return g;
  };
  const localAtX = (x: number) => time.localAt(view.startTick + x / view.pxPerTick);

  drawWave(
    ctx,
    {
      source: { fileId: file.id },
      frames: file.frames,
      x0: Math.max(0, Math.floor(box.x0)),
      x1: Math.min(size.width, Math.ceil(box.x1)),
      frameAt: (x) => frameOf(contentAt(clip, clip.start + localAtX(x))),
      mid,
      half,
      top,
      bottom,
      gainAt: (x) => fadeAt(localAtX(x)),
    },
    mix(colour, LIGHT, 0.15),
  );

  // Fade curves.
  ctx.strokeStyle = alpha(LIGHT, 0.55);
  ctx.lineWidth = 1;
  const curve = (from: number, to: number, shape: number, rising: boolean) => {
    ctx.beginPath();
    const steps = 24;
    for (let i = 0; i <= steps; i++) {
      const t = i / steps;
      const x = tickToX(view, time.tickAt(from + (to - from) * t));
      const g = fadeGain(shape, rising ? t : 1 - t);
      const y = bottom - (bottom - top) * g;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
  };
  if (clip.fadeIn > 0) curve(0, clip.fadeIn, clip.fadeInShape, true);
  if (clip.fadeOut > 0) curve(time.localLength - clip.fadeOut, time.localLength, clip.fadeOutShape, false);

  // Crossfades: the covered clip's mirrored fade over this one's, dashed in its colour, making an X.
  for (const tail of crossfades ?? []) {
    ctx.save();
    ctx.setLineDash([3, 3]);
    ctx.strokeStyle = mix(tail.covered.colour || colour, LIGHT, 0.35);
    if (tail.edge === 'in') curve(0, clip.fadeIn, tail.shape, false);
    else curve(time.localLength - clip.fadeOut, time.localLength, tail.shape, true);
    ctx.restore();
  }

  if (musical) drawMusicalMarks(ctx, theme, view, clip, box, file, fileTiming, audio);
}

/** Musical clips (docs/design/time-stretching.md, TS12, TS13): the file's timing markers as small read-only ticks
    along the top, a badge for the mode (a note, or "R" for repitch; tinted where a segment is stretched beyond x0.5
    to x2), and a thin bar while renders are pending. */
function drawMusicalMarks(
  ctx: CanvasRenderingContext2D,
  theme: Theme,
  view: View,
  clip: Clip,
  box: ClipBox,
  file: AudioFile,
  timing: TimingMap,
  audio: AudioContext,
) {
  const top = box.y0 + NAME_BAR;
  ctx.fillStyle = alpha(theme.warpMarker, 0.9);
  for (const marker of file.timing.markers) {
    // Where the marker plays on each pass of the clip.
    for (const tick of ticksOfContent(clip, marker.tick)) {
      const x = Math.round(tickToX(view, tick));
      if (x > box.x0 && x < box.x1) ctx.fillRect(x, top, 1, 4);
    }
  }

  // The badge, after the name bar's text space.
  if (box.x1 - box.x0 > 40) {
    const repitch = file.timing.musicalStyle === 'repitch';
    const extreme = stretchedFar(clip, file, timing, audio.map);
    const x = box.x1 - LOOP_HANDLE_PX - 14;
    ctx.font = font(theme, 'xs', { bold: true });
    ctx.textBaseline = 'middle';
    ctx.fillStyle = extreme ? theme.warn : theme.onAccent;
    ctx.fillText(repitch ? 'R' : '♪', x, box.y0 + NAME_BAR / 2 + 0.5);
  }

  if (audio.stretchPending?.has(clip.id)) {
    ctx.fillStyle = alpha(theme.accent, 0.85);
    ctx.fillRect(box.x0 + 1, box.y1 - 3, box.x1 - box.x0 - 2, 2);
  }
}

/** The timeline ticks at which a content tick plays, over the clip's passes. */
function ticksOfContent(clip: Clip, content: number) {
  const result: number[] = [];
  if (clip.loopLength <= 0) {
    if (content >= clip.offset && content < clip.offset + clip.length) result.push(clip.start + content - clip.offset);
    return result;
  }
  const loopEnd = clip.loopStart + clip.loopLength;
  let from = clip.offset < loopEnd ? clip.offset : clip.loopStart;
  let passStart = 0;
  while (passStart < clip.length && result.length < 1000) {
    const passEnd = Math.min(clip.length, passStart + loopEnd - from);
    if (content >= from && content < from + passEnd - passStart) result.push(clip.start + passStart + content - from);
    passStart = passEnd;
    from = clip.loopStart;
  }
  return result;
}

/** Whether any marker segment the clip plays is stretched beyond x0.5 to x2, where artefacts grow (TS12). */
function stretchedFar(clip: Clip, file: AudioFile, timing: TimingMap, map: TempoMap) {
  const points = timing.points;
  const fileEnd = timing.tickAt(file.frames);
  const origin = clip.start - clip.offset;
  for (let i = 0; i < points.length; i++) {
    const a = points[i].tick;
    const b = i + 1 < points.length ? points[i + 1].tick : fileEnd;
    if (b <= a || b <= clip.offset || a >= clip.offset + clip.length) continue;
    const seconds = map.secondsAt(origin + b) - map.secondsAt(origin + a);
    const fileSeconds = (timing.frameAt(b) - timing.frameAt(a)) / file.sampleRate;
    const ratio = seconds / fileSeconds;
    if (ratio < 0.5 || ratio > 2) return true;
  }
  return false;
}
