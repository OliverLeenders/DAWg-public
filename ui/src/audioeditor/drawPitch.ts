import type { AudioFile, CurvePoint, PitchAnalysisReply } from '../bridge/protocol';
import { alpha, font, LIGHT, SHADE, type Size } from '../canvas/theme';
import { drawRuler, drawTimeGrid } from '../pianoroll/draw';
import { evaluateCurve, isBlackKey, pitchName, tickToX, xToTick } from '../pianoroll/geometry';
import { drawWave } from '../timeline/waveform';
import { CLIPS_STRIP, RULER_HEIGHT, type EditorScene } from './draw';
import { noteEdited } from './pitchCurve';
import { pivotsOf } from './pitchShapes';

// The audio editor's Pitch view (docs/design/pitch-correction.md, sections 4 and 7): the notes as blobs at their
// centres, the sung pitch (and the corrected pitch once notes are edited), and each note's shape, drawn over the file's
// semitone rows.

export type PitchScene = Pick<EditorScene, 'view' | 'map' | 'theme' | 'file' | 'clip' | 'axis' | 'playheadTick' | 'gridTicks'> & {
  /** Undefined while the analysis runs. */
  analysis: PitchAnalysisReply | undefined;
  /** The notes as they sound: the file's edits once seeded, else the detected ones. */
  notes: ViewNote[];
  /** The selected notes' ids. */
  selected: ReadonlySet<number>;
  /** The corrected pitch, one value per analysis.pitch value; null when nothing is edited. */
  corrected: Array<number | null> | null;
  /** A box selection being dragged, in pixels. */
  box: [number, number] | null;
  /** Whether a vibrato handle is being dragged: the selected notes show their amounts. */
  adjustingVibrato: boolean;
};

/** How far above a note's bar its tilt handles sit, and their size, in pixels. */
const TILT_HANDLE_GAP = 7;
export const TILT_HANDLE_PX = 6;

/**
 * Where a note's tilt handles are (section 4.5): small triangles above its bar's start and end. Dragging one up or
 * down tilts that side of the note around its pivot.
 */
export function tiltHandles(note: Pick<ViewNote, 'start' | 'end' | 'centre'>, layout: ReturnType<typeof pitchLayout>, frameX: (frame: number) => number) {
  const y = layout.y(note.centre) - layout.blobHeight / 2 - TILT_HANDLE_GAP;
  return { left: { x: frameX(note.start), y }, right: { x: frameX(note.end), y } };
}

/** Half the width and half the height of a note's vibrato handle, in pixels. */
export const VIBRATO_HANDLE_W = 9;
export const VIBRATO_HANDLE_H = 3;

/**
 * Where a note's vibrato handle is (section 4.4): a small bar below the middle of its bar. Dragging it up deepens the
 * note's vibrato, down flattens it.
 */
export function vibratoHandle(note: Pick<ViewNote, 'start' | 'end' | 'centre'>, layout: ReturnType<typeof pitchLayout>, frameX: (frame: number) => number) {
  return { x: (frameX(note.start) + frameX(note.end)) / 2, y: layout.y(note.centre) + layout.blobHeight / 2 + TILT_HANDLE_GAP };
}

/** A note as the view draws it: where it is, its centre and its shape relative to the centre. */
export interface ViewNote {
  /** The file's note id; 0 for a detected note not seeded yet. */
  id: number;
  detectedCentre: number;
  leadStart: number;
  start: number;
  end: number;
  centre: number;
  points: CurvePoint[];
  /** Tilt pivots, frames from its start. */
  pivots: number[];
  /** Whether its pitch was edited. */
  edited: boolean;
  /** Its vibrato's amount: 0 removes it, 1 as sung, 2 doubles it. */
  vibrato: number;
}

/** The file's notes as they sound: its pitch edits once seeded, else what detection found. */
export function viewNotes(file: AudioFile, analysis: PitchAnalysisReply | undefined): ViewNote[] {
  if (file.pitch.notes.length > 0)
    return file.pitch.notes.map((n) => ({
      id: n.id,
      detectedCentre: n.detectedCentre,
      leadStart: n.leadStart,
      start: n.start,
      end: n.end,
      centre: n.targetCentre,
      points: n.shape,
      pivots: n.pivots,
      edited: noteEdited(n),
      vibrato: n.vibrato,
    }));
  return (analysis?.notes ?? []).map((n) => ({
    id: 0,
    detectedCentre: n.centre,
    leadStart: n.leadStart,
    start: n.start,
    end: n.end,
    centre: n.centre,
    points: n.shape.map(([time, value, shape]): CurvePoint => [time, value, shape, 0]),
    pivots: [],
    edited: false,
    vibrato: 1,
  }));
}

/** The semitones the view shows: the notes' sung centres (or the sung pitch) with a margin, at least an octave. Sung,
 * not edited, so the rows stay put while notes are dragged. */
export function pitchRange(notes: ViewNote[], analysis: PitchAnalysisReply | undefined): [number, number] {
  const values = notes.length ? notes.map((n) => n.detectedCentre) : (analysis?.pitch ?? []).filter((p) => p !== null);
  if (values.length === 0) return [54, 72];
  let lo = Math.floor(Math.min(...values)) - 3;
  let hi = Math.ceil(Math.max(...values)) + 3;
  if (hi - lo < 12) {
    const grow = 12 - (hi - lo);
    lo -= Math.floor(grow / 2);
    hi += Math.ceil(grow / 2);
  }
  return [lo, hi];
}

/** Where pitches sit on the canvas: rows from `lo` to `hi`, a semitone each, below the ruler. */
export function pitchLayout(size: Size, notes: ViewNote[], analysis: PitchAnalysisReply | undefined) {
  const top = RULER_HEIGHT + CLIPS_STRIP;
  const height = size.height - top;
  const [lo, hi] = pitchRange(notes, analysis);
  const rowHeight = height / (hi - lo + 1);
  return {
    top,
    height,
    lo,
    hi,
    rowHeight,
    blobHeight: Math.max(4, rowHeight * 0.8),
    y: (pitch: number) => top + (hi + 0.5 - pitch) * rowHeight,
    pitchAt: (y: number) => hi + 0.5 - (y - top) / rowHeight,
  };
}

/** A note's pitch at a frame of the file: its centre plus its shape there. */
export const notePitchAt = (note: ViewNote, frame: number) => note.centre + evaluateCurve(note.points, frame - note.start, 0);

export function drawPitchView(ctx: CanvasRenderingContext2D, size: Size, scene: PitchScene) {
  const { view, map, theme, file, clip, axis, analysis, notes } = scene;
  ctx.fillStyle = theme.bg;
  ctx.fillRect(0, 0, size.width, size.height);
  drawRuler(ctx, { width: size.width, height: RULER_HEIGHT }, view, theme, map);

  const { top, height, lo, hi, rowHeight, blobHeight, y } = pitchLayout(size, notes, analysis);
  const frameX = (frame: number) => tickToX(view, axis.frameToTick(frame));

  // Semitone rows: the black keys' darker, a line and a label at each C.
  for (let p = lo; p <= hi; p++) {
    const rowTop = Math.round(y(p) - rowHeight / 2);
    const rowBottom = Math.round(y(p) + rowHeight / 2);
    ctx.fillStyle = isBlackKey(p) ? alpha(SHADE, 0.18) : alpha(LIGHT, 0.03);
    ctx.fillRect(0, rowTop, size.width, rowBottom - rowTop);
    if (((p % 12) + 12) % 12 === 0) {
      ctx.fillStyle = alpha(LIGHT, 0.14);
      ctx.fillRect(0, rowBottom - 1, size.width, 1);
    }
  }

  const clipX0 = tickToX(view, clip.start);
  const clipX1 = tickToX(view, clip.start + clip.length);
  ctx.fillStyle = alpha(theme.accent, 0.06);
  ctx.fillRect(clipX0, top, clipX1 - clipX0, height);

  ctx.save();
  ctx.translate(0, top);
  drawTimeGrid(ctx, { width: size.width, height }, view, map, scene.gridTicks);
  ctx.restore();

  // The wave, faint behind the notes, to place them against what was sung.
  ctx.globalAlpha = 0.14;
  drawWave(
    ctx,
    {
      source: { fileId: file.id },
      frames: file.frames,
      x0: 0,
      x1: size.width,
      frameAt: (x) => axis.tickToFrame(xToTick(view, x)),
      mid: top + height / 2,
      half: height / 2 - 4,
      top,
      bottom: top + height,
    },
    theme.accent,
  );
  ctx.globalAlpha = 1;

  if (!analysis || analysis.error || !analysis.pitch) {
    ctx.fillStyle = analysis?.error ? theme.danger : theme.textDim;
    ctx.font = font(theme, 's');
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(analysis?.error ?? 'Finding the pitch…', size.width / 2, top + height / 2);
    ctx.textAlign = 'left';
  } else {
    // The blobs: a rounded bar a semitone high at each note's centre; its lead-in (a consonant, a breath) fainter.
    const radius = Math.min(4, blobHeight / 2);
    ctx.font = font(theme, 'xs');
    ctx.textBaseline = 'bottom';
    for (const note of notes) {
      const x0 = frameX(note.start);
      const x1 = frameX(note.end);
      if (x1 < 0 || frameX(note.leadStart) > size.width) continue;
      const blobTop = y(note.centre) - blobHeight / 2;
      if (note.leadStart < note.start) {
        ctx.fillStyle = alpha(theme.accent, 0.12);
        ctx.fillRect(frameX(note.leadStart), blobTop + blobHeight * 0.3, x0 - frameX(note.leadStart), blobHeight * 0.4);
      }
      ctx.beginPath();
      ctx.roundRect(x0, blobTop, Math.max(1, x1 - x0), blobHeight, radius);
      const selected = scene.selected.has(note.id);
      ctx.fillStyle = alpha(note.edited ? theme.warn : theme.accent, selected ? 0.55 : 0.3);
      ctx.fill();
      ctx.strokeStyle = selected ? theme.text : alpha(note.edited ? theme.warn : theme.accent, 0.8);
      ctx.lineWidth = selected ? 1.5 : 1;
      ctx.stroke();
      if (x1 - x0 > 44) {
        const cents = Math.round((note.centre - Math.round(note.centre)) * 100);
        ctx.fillStyle = theme.textDim;
        ctx.fillText(`${pitchName(note.centre)} ${cents > 0 ? '+' : ''}${cents}`, x0 + 2, blobTop - 2);
      }
    }

    // The sung pitch, broken where unvoiced; once notes are edited, fainter, with the corrected pitch over it.
    const { sampleRate = file.sampleRate, start = 0, hop = 0.02 } = analysis;
    const line = (values: ReadonlyArray<number | null>) => {
      ctx.beginPath();
      let drawing = false;
      values.forEach((pitch, i) => {
        if (pitch === null) {
          drawing = false;
          return;
        }
        const x = frameX((start + i * hop) * sampleRate);
        if (drawing) ctx.lineTo(x, y(pitch));
        else ctx.moveTo(x, y(pitch));
        drawing = true;
      });
      ctx.stroke();
    };
    ctx.lineWidth = 1;
    ctx.strokeStyle = alpha(LIGHT, scene.corrected ? 0.2 : 0.45);
    line(analysis.pitch);
    if (scene.corrected) {
      ctx.strokeStyle = alpha(theme.warn, 0.9);
      line(scene.corrected);
    }

    // Each note's fitted shape, sampled every 2 px, with its nodes.
    ctx.strokeStyle = theme.ok;
    ctx.fillStyle = theme.ok;
    ctx.lineWidth = 1.5;
    for (const note of notes) {
      const x0 = Math.max(0, frameX(note.start));
      const x1 = Math.min(size.width, frameX(note.end));
      if (x1 <= x0 || note.points.length === 0) continue;
      ctx.beginPath();
      for (let x = x0; x <= x1 + 0.01; x += 2) {
        const at = Math.min(x, x1);
        const py = y(notePitchAt(note, axis.tickToFrame(xToTick(view, at))));
        if (x === x0) ctx.moveTo(at, py);
        else ctx.lineTo(at, py);
      }
      ctx.stroke();
      const radius = 3;
      for (const [time, value] of note.points) {
        const nx = frameX(note.start + time);
        if (nx < -3 || nx > size.width + 3) continue;
        ctx.beginPath();
        ctx.arc(nx, y(note.centre + value), radius, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // A selected note's tilt handles above its bar, and its pivots as diamonds in the same row between them (a note
    // without pivots tilts around its middle).
    const layout = pitchLayout(size, notes, analysis);
    ctx.fillStyle = theme.text;
    ctx.font = font(theme, 'xs');
    ctx.textBaseline = 'middle';
    for (const note of notes) {
      if (!scene.selected.has(note.id)) continue;
      // The vibrato handle below the bar, with its amount once changed (or while dragged).
      const bar = vibratoHandle(note, layout, frameX);
      ctx.beginPath();
      ctx.roundRect(bar.x - VIBRATO_HANDLE_W, bar.y - VIBRATO_HANDLE_H, VIBRATO_HANDLE_W * 2, VIBRATO_HANDLE_H * 2, VIBRATO_HANDLE_H);
      ctx.fill();
      if (scene.adjustingVibrato || note.vibrato !== 1)
        ctx.fillText(`Vibrato ${Math.round(note.vibrato * 100)} %`, bar.x + VIBRATO_HANDLE_W + 4, bar.y);
      const handles = tiltHandles(note, layout, frameX);
      for (const [handle, inward] of [
        [handles.left, 1],
        [handles.right, -1],
      ] as const) {
        ctx.beginPath();
        ctx.moveTo(handle.x, handle.y - TILT_HANDLE_PX);
        ctx.lineTo(handle.x, handle.y + TILT_HANDLE_PX);
        ctx.lineTo(handle.x + inward * TILT_HANDLE_PX, handle.y);
        ctx.fill();
      }
    }
    {
      for (const note of notes) {
        if (!scene.selected.has(note.id)) continue;
        const py = tiltHandles(note, layout, frameX).left.y;
        for (const pivot of pivotsOf(note.pivots, note.end - note.start)) {
          const px = frameX(note.start + pivot);
          if (px < -6 || px > size.width + 6) continue;
          ctx.beginPath();
          ctx.moveTo(px, py - 5);
          ctx.lineTo(px + 5, py);
          ctx.lineTo(px, py + 5);
          ctx.lineTo(px - 5, py);
          ctx.fill();
        }
      }
    }
  }

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
