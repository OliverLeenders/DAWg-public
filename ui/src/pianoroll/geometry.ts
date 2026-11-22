import { audibleNotes, clipEnd, contentOrigin } from '../timeline/clips';
import { TICKS_PER_BEAT, type Clip, type CurvePoint, type LaneId, type Note, type Track, type Vibrato } from '../bridge/protocol';
import { TempoMap } from '../time/tempoMap';

// Coordinate mapping and model helpers for the piano roll. Time is in ticks, pitch in MIDI semitones.

export const MIN_PITCH = 0;
export const MAX_PITCH = 127;

export interface View {
  /** Tick at the left edge of the roll. */
  startTick: number;
  /** Row at the top edge of the roll (fractional when scrolled mid-row): the pitch, unless `rows` folds the roll. */
  topPitch: number;
  /** Horizontal zoom. */
  pxPerTick: number;
  /** Height of one row. */
  keyHeight: number;
  /**
   * A folded roll's rows (docs/design/scale-highlighting.md, SH4): the pitches shown, ascending, row 0 the lowest.
   * Absent, every semitone is a row and a row is its pitch.
   */
  rows?: readonly number[];
}

export const DEFAULT_VIEW: View = {
  startTick: 0,
  topPitch: 84.5,
  pxPerTick: 96 / TICKS_PER_BEAT,
  keyHeight: 14,
};

export const tickToX = (view: View, tick: number) => (tick - view.startTick) * view.pxPerTick;
export const xToTick = (view: View, x: number) => view.startTick + x / view.pxPerTick;

/**
 * The row of a pitch, continuous. In a folded roll a pitch between two shown rows (a bend, a glide) lies between them
 * in proportion, and one beyond the ends goes on a semitone per row, so pitch curves stay continuous.
 */
export function pitchToRow(rows: readonly number[] | undefined, pitch: number) {
  if (!rows || rows.length === 0) return pitch;
  const last = rows.length - 1;
  if (pitch <= rows[0]) return pitch - rows[0];
  if (pitch >= rows[last]) return last + pitch - rows[last];
  let low = 0;
  let high = last;
  while (high - low > 1) {
    const middle = (low + high) >> 1;
    if (rows[middle] <= pitch) low = middle;
    else high = middle;
  }
  return low + (pitch - rows[low]) / (rows[high] - rows[low]);
}

/** The pitch of a row, continuous: the inverse of pitchToRow(). */
export function rowToPitch(rows: readonly number[] | undefined, row: number) {
  if (!rows || rows.length === 0) return row;
  const last = rows.length - 1;
  if (row <= 0) return rows[0] + row;
  if (row >= last) return rows[last] + row - last;
  const low = Math.floor(row);
  return rows[low] + (row - low) * (rows[low + 1] - rows[low]);
}

/** Top edge of a pitch's row. */
export const pitchToY = (view: View, pitch: number) => (view.topPitch - pitchToRow(view.rows, pitch) - 0.5) * view.keyHeight;
/** Pitch at y, continuous: a row's centre maps to its integer pitch. */
export const yToPitch = (view: View, y: number) => rowToPitch(view.rows, view.topPitch - y / view.keyHeight);
/** The row under y: its index, which is its pitch unless the roll is folded. */
export const rowAtY = (view: View, y: number) => Math.round(view.topPitch - y / view.keyHeight);

/** The pitch of the row under y: a shown row's pitch in a folded roll. */
export function pitchAtY(view: View, y: number) {
  const row = rowAtY(view, y);
  const rows = view.rows;
  if (!rows || rows.length === 0) return row;
  return rows[Math.min(rows.length - 1, Math.max(0, row))];
}

/** A pitch moved by a number of rows: semitones, or in a folded roll the shown rows (SH5). */
export function stepRows(view: View, pitch: number, delta: number) {
  const rows = view.rows;
  if (!rows || rows.length === 0) return pitch + delta;
  const from = Math.round(pitchToRow(rows, pitch));
  return rows[Math.min(rows.length - 1, Math.max(0, from + delta))];
}

/** The pitches of the rows a view shows, lowest first, within MIDI's range. */
export function visibleRowPitches(view: View, height: number): number[] {
  const topRow = Math.ceil(view.topPitch);
  const bottomRow = Math.floor(view.topPitch - height / view.keyHeight - 1);
  const rows = view.rows;
  const pitches: number[] = [];
  if (rows) {
    for (let row = Math.max(0, bottomRow); row <= Math.min(rows.length - 1, topRow); row++) pitches.push(rows[row]);
  } else {
    for (let pitch = Math.max(MIN_PITCH, bottomRow); pitch <= Math.min(MAX_PITCH, topRow); pitch++) pitches.push(pitch);
  }
  return pitches;
}

/** The highest row a view can scroll to: the top pitch, or a folded roll's top row. */
export const topRowOf = (view: View) => (view.rows ? view.rows.length - 1 : MAX_PITCH);
/** The lowest row a view can scroll to. */
export const bottomRowOf = (view: View) => (view.rows ? 0 : MIN_PITCH);

/**
 * A view moved onto other rows (folding or unfolding), keeping the pitch at its top edge, so the notes in sight stay
 * in sight.
 */
export function withRows(view: View, rows: readonly number[] | undefined): View {
  if (view.rows === rows) return view;
  const topPitch = pitchToRow(rows, rowToPitch(view.rows, view.topPitch));
  return { ...view, rows, topPitch };
}

export const snapTicks = (beats: number) => Math.round(beats * TICKS_PER_BEAT);

/** Snaps a tick to the grid (floor by default, nearest when `round`). */
export function snap(tick: number, gridTicks: number, round = false) {
  if (gridTicks <= 0) return Math.round(tick);
  return (round ? Math.round(tick / gridTicks) : Math.floor(tick / gridTicks)) * gridTicks;
}

const BLACK_KEYS = new Set([1, 3, 6, 8, 10]);
export const isBlackKey = (pitch: number) => BLACK_KEYS.has(((Math.round(pitch) % 12) + 12) % 12);

const NOTE_NAMES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
export const pitchName = (pitch: number) =>
  `${NOTE_NAMES[((Math.round(pitch) % 12) + 12) % 12]}${Math.floor(Math.round(pitch) / 12) - 1}`;

/** A typed pitch: a name such as "C#3", "C♯3" or "Eb4" (C4 is 60), or a MIDI number; null if it is neither. */
export function parsePitch(text: string): number | null {
  const typed = text.trim();
  const named = /^([A-Ga-g])([#b♯♭]?)(-?\d+)$/.exec(typed);
  if (named) {
    const base = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 }[named[1].toUpperCase() as 'C'];
    const pitch = (Number(named[3]) + 1) * 12 + base + (named[2] === '#' || named[2] === '♯' ? 1 : named[2] === 'b' || named[2] === '♭' ? -1 : 0);
    return pitch >= 0 && pitch <= 127 ? pitch : null;
  }
  const number = Number(typed);
  return typed !== '' && Number.isInteger(number) && number >= 0 && number <= 127 ? number : null;
}

/** A note placed on the track timeline. */
export interface PlacedNote {
  note: Note;
  clip: Clip;
  /** Absolute start and end on the timeline (end cut at the clip end). */
  start: number;
  end: number;
  /** A linked note's glide, for drawing: the semitones it starts away from its own pitch, fading out over the time. */
  glide?: { offset: number; ms: number };
}

/**
 * The notes the roll edits, in timeline order. With an edited clip (docs/design/phase6-arrangement.md,
 * section 5): all its notes at their first-pass positions, including ones hidden by the clip edges. Without
 * one: the audible first-pass notes of every clip of the track.
 */
export function placeNotes(track: Track, editedClipId: number | null = null): PlacedNote[] {
  const placed: PlacedNote[] = [];
  const edited = editedClipId !== null ? track.clips[String(editedClipId)] : undefined;

  for (const clip of edited ? [edited] : Object.values(track.clips)) {
    const end = clipEnd(clip);

    for (const note of Object.values(clip.notes)) {
      const hidden = note.start < clip.offset || note.start >= clip.offset + clip.length;
      if (hidden && !edited) continue;
      const start = contentOrigin(clip) + note.start;
      placed.push({ note, clip, start, end: Math.max(start + 1, Math.min(start + note.length, end)) });
    }
  }

  return placed.sort((a, b) => a.start - b.start || a.note.id - b.note.id);
}

/** Context notes that can't be edited: other clips of the track and the repeats of a looped edited clip. */
export function ghostNotes(track: Track, editedClipId: number | null): PlacedNote[] {
  if (editedClipId === null || !track.clips[String(editedClipId)]) return [];
  return Object.values(track.clips)
    .flatMap((clip) => audibleNotes(clip).filter((a) => clip.id !== editedClipId || a.repetition > 0))
    .map(({ note, clip, start, end }) => ({ note, clip, start, end }));
}

/**
 * The note before or after the selection in timeline order: by start, then lowest pitch first. Going forward starts from
 * the selection's last note and going back from its first. Without a selection it is the first or the last note; past
 * either end there is none.
 */
export function adjacentNote(notes: PlacedNote[], selected: ReadonlySet<number>, direction: 1 | -1): PlacedNote | null {
  const ordered = [...notes].sort((a, b) => a.start - b.start || a.note.pitch - b.note.pitch || a.note.id - b.note.id);
  const positions = ordered.flatMap((n, i) => (selected.has(n.note.id) ? [i] : []));
  if (positions.length === 0) return ordered.at(direction > 0 ? 0 : -1) ?? null;
  const from = direction > 0 ? positions[positions.length - 1] : positions[0];
  return ordered[from + direction] ?? null;
}

/**
 * The note above or below the selection, for moving through a chord or across parts. It starts from the selection's
 * highest note going up and its lowest going down, and takes the note nearest in time (one sounding with it counts as no
 * distance), then nearest in pitch, then the earliest. Without a selection it is the first note going up, the last going
 * down; with none in that direction there is none.
 */
export function noteInPitchDirection(notes: PlacedNote[], selected: ReadonlySet<number>, direction: 1 | -1): PlacedNote | null {
  const chosen = notes.filter((n) => selected.has(n.note.id));
  if (chosen.length === 0) return adjacentNote(notes, selected, direction);
  const from = chosen.reduce((best, n) => ((n.note.pitch - best.note.pitch) * direction > 0 ? n : best));
  const timeTo = (n: PlacedNote) => Math.max(0, n.start - from.end, from.start - n.end);
  const candidates = notes.filter((n) => (n.note.pitch - from.note.pitch) * direction > 0 && !selected.has(n.note.id));
  return (
    [...candidates].sort(
      (a, b) =>
        timeTo(a) - timeTo(b) ||
        Math.abs(a.note.pitch - from.note.pitch) - Math.abs(b.note.pitch - from.note.pitch) ||
        a.start - b.start,
    )[0] ?? null
  );
}

export const sortedClips = (track: Track) => Object.values(track.clips).sort((a, b) => a.start - b.start);

export const LANE_DEFAULTS: Record<LaneId, number> = { pitch: 0, pressure: 0, timbre: 0.5, vibrato: 0 };

/** Mirrors maxTension in engine/include/dawg/Curve.h. */
export const MAX_TENSION = 4;

/** CurveShape::smooth in engine/include/dawg/Curve.h. */
export const SMOOTH_SHAPE = 4;

/** CurveShape::rounded in engine/include/dawg/Curve.h: a Catmull-Rom run, for pitch shapes. */
export const ROUNDED_SHAPE = 5;

/**
 * Same rules as engine/src/Curve.cpp: hold first/last values outside, linear / hold / power / Bezier / smooth
 * segments.
 */
export function evaluateCurve(points: CurvePoint[], time: number, defaultValue: number) {
  if (points.length === 0) return defaultValue;
  if (time <= points[0][0]) return points[0][1];
  const last = points[points.length - 1];
  if (time >= last[0]) return last[1];

  let i = 1;
  while (points[i][0] <= time) i++;

  const [t0, v0, shape, tension, handleTime, handleValue] = points[i - 1];
  const [t1, v1] = points[i];
  const x = (time - t0) / (t1 - t0);

  if (shape === 1) return v0;
  if (shape === 2) return v0 + (v1 - v0) * Math.pow(x, Math.pow(2, Math.max(-MAX_TENSION, Math.min(MAX_TENSION, tension)) * 3));
  if (shape === 3) return v0 + (v1 - v0) * freeSegment(x, handleTime ?? 0.5, handleValue ?? 0.5);
  if (shape === SMOOTH_SHAPE || shape === ROUNDED_SHAPE) {
    const span = t1 - t0;
    const m0 = smoothSlope(points, i - 1) * span;
    const m1 = smoothSlope(points, i) * span;
    const x2 = x * x;
    const x3 = x2 * x;
    return (2 * x3 - 3 * x2 + 1) * v0 + (x3 - 2 * x2 + x) * m0 + (-2 * x3 + 3 * x2) * v1 + (x3 - x2) * m1;
  }
  return v0 + (v1 - v0) * x;
}

/**
 * The slope of smooth segments at a node, in value per tick: a run of consecutive smooth segments is one monotone
 * cubic through its nodes (Fritsch-Carlson); a run of rounded ones a Catmull-Rom spline. Same math as smoothSlope() in
 * engine/src/Curve.cpp.
 */
export function smoothSlope(points: CurvePoint[], node: number) {
  const smoothAt = (index: number) =>
    index >= 0 && index + 1 < points.length && (points[index][2] === SMOOTH_SHAPE || points[index][2] === ROUNDED_SHAPE);
  const gap = (index: number) => points[index + 1][0] - points[index][0];
  const secant = (index: number) => (points[index + 1][1] - points[index][1]) / gap(index);
  const edge = (h0: number, h1: number, d0: number, d1: number) => {
    const slope = ((2 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    if (slope * d0 <= 0) return 0;
    if (d0 * d1 < 0 && Math.abs(slope) > 3 * Math.abs(d0)) return 3 * d0;
    return slope;
  };

  const left = smoothAt(node - 1);
  const right = smoothAt(node);
  // A rounded curve's first and last nodes are level: the curve holds their values beyond them (no corner there).
  if ((node === 0 && right && points[0][2] === ROUNDED_SHAPE) || (node === points.length - 1 && left && points[node - 1][2] === ROUNDED_SHAPE))
    return 0;

  if (left && right) {
    const d0 = secant(node - 1);
    const d1 = secant(node);
    // A rounded run's inner node: the slope through its neighbours (Catmull-Rom, for uneven spacing).
    if (points[node][2] === ROUNDED_SHAPE && points[node - 1][2] === ROUNDED_SHAPE)
      return (points[node + 1][1] - points[node - 1][1]) / (points[node + 1][0] - points[node - 1][0]);
    if (d0 * d1 <= 0) return 0;
    const h0 = gap(node - 1);
    const h1 = gap(node);
    const w0 = 2 * h1 + h0;
    const w1 = h1 + 2 * h0;
    return (w0 + w1) / (w0 / d0 + w1 / d1);
  }
  if (right) return smoothAt(node + 1) ? edge(gap(node), gap(node + 1), secant(node), secant(node + 1)) : secant(node);
  if (left)
    return smoothAt(node - 2) ? edge(gap(node - 1), gap(node - 2), secant(node - 1), secant(node - 2)) : secant(node - 1);
  return 0;
}

/**
 * The control points of a free segment from its tension point (hx, hy), in the segment's unit rectangle: both handles
 * point at it, each as long as the distance to it times how far it lies along the line from the handle's node to the
 * rectangle's opposite side. Same math as freeSegment() in engine/src/Curve.cpp.
 */
export function freeControlPoints(hx: number, hy: number) {
  hx = Math.max(0, Math.min(1, hx));
  hy = Math.max(0, Math.min(1, hy));
  const start = Math.max(hx, hy);
  const end = Math.max(1 - hx, 1 - hy);
  return { c1: [hx * start, hy * start] as const, c2: [1 + (hx - 1) * end, 1 + (hy - 1) * end] as const };
}

/**
 * A cubic Bezier from (0, 0) to (1, 1) with the handles freeControlPoints() gives. Same math as
 * engine/src/Curve.cpp; x is inverted before y is evaluated.
 */
export function freeSegment(x: number, hx: number, hy: number) {
  const { c1, c2 } = freeControlPoints(hx, hy);
  const bezier = (u: number, a: number, b: number) => {
    const v = 1 - u;
    return 3 * v * v * u * a + 3 * v * u * u * b + u * u * u;
  };

  let low = 0;
  let high = 1;
  for (let i = 0; i < 32; i++) {
    const u = (low + high) / 2;
    if (bezier(u, c1[0], c2[0]) < x) low = u;
    else high = u;
  }

  return bezier((low + high) / 2, c1[1], c2[1]);
}

/** Default vibrato settings: one semitone at 5.5 Hz. Audible only where the intensity lane is above 0. */
export const NO_VIBRATO: Vibrato = { depth: 1, rateHz: 5.5, syncBeats: 0, shape: 0, delay: 0, fadeIn: 0 };

/** One vibrato cycle for a phase in [0, 1): same waveforms as engine/src/Vibrato.cpp. */
function waveform(shape: number, phase: number) {
  switch (shape) {
    case 1:
      return phase < 0.25 ? 4 * phase : phase < 0.75 ? 2 - 4 * phase : 4 * phase - 4;
    case 2:
      return phase < 0.5 ? 1 : -1;
    case 3:
      return phase < 0.5 ? 2 * phase : 2 * phase - 2;
    case 4:
      return phase < 0.5 ? -2 * phase : 2 - 2 * phase;
    default:
      return Math.sin(2 * Math.PI * phase);
  }
}

/** Vibrato offset in semitones; same rules as engine/src/Vibrato.cpp. */
export function vibratoOffset(v: Vibrato, ticksFromStart: number, secondsPerTick: number, depthScale: number) {
  const elapsed = ticksFromStart - v.delay;
  if (v.depth <= 0 || depthScale <= 0 || elapsed < 0) return 0;
  const fade = v.fadeIn > 0 ? Math.min(1, elapsed / v.fadeIn) : 1;
  const cycles = v.syncBeats > 0 ? elapsed / (v.syncBeats * TICKS_PER_BEAT) : elapsed * secondsPerTick * v.rateHz;
  return v.depth * fade * depthScale * waveform(v.shape, cycles - Math.floor(cycles));
}

export const secondsPerTick = (tempo: number) => 60 / (tempo * TICKS_PER_BEAT);

const CONSTANT_TEMPO = new TempoMap();

/** True if the note's vibrato is audible anywhere (its intensity lane rises above 0). */
export const hasVibrato = (note: Note) => (note.expression.vibrato ?? []).some((p) => p[1] > 0);

/** True if the note's pitch moves away from its base pitch anywhere (curve or vibrato). */
export const isBent = (note: Note) => hasVibrato(note) || note.expression.pitch.some((p) => p[1] !== 0);

/** The note's sounding pitch at an absolute tick (base pitch + pitch curve + vibrato). */
export function pitchAtTick(placed: PlacedNote, tick: number, map: TempoMap = CONSTANT_TEMPO) {
  const { note } = placed;
  const t = tick - placed.start;
  const curve = evaluateCurve(note.expression.pitch, t, LANE_DEFAULTS.pitch);
  const vibrato = note.vibrato ?? NO_VIBRATO;
  // As engine/src/Sequence.cpp: the distance from the note before fades out in a straight line, in real time.
  const { glide } = placed;
  const gliding = glide && glide.ms > 0 ? glide.offset * Math.max(0, 1 - ((map.secondsAt(tick) - map.secondsAt(placed.start)) * 1000) / glide.ms) : 0;
  if (!hasVibrato(note)) return note.pitch + curve + gliding;
  const scale = evaluateCurve(note.expression.vibrato ?? [], t, LANE_DEFAULTS.vibrato);
  // As engine/src/Sequence.cpp: a free-running vibrato runs in real time since it began, through the tempo map.
  const began = placed.start + vibrato.delay;
  const spt = tick > began ? (map.secondsAt(tick) - map.secondsAt(began)) / (tick - began) : secondsPerTick(map.bpmAt(tick));
  return note.pitch + curve + gliding + vibratoOffset(vibrato, t, spt, scale);
}

/** Length in ticks of a vibrato's first cycle, which begins at a tick. */
export function cycleTicks(v: Vibrato, begin: number, map: TempoMap = CONSTANT_TEMPO) {
  return v.syncBeats > 0 ? v.syncBeats * TICKS_PER_BEAT : map.tickAt(map.secondsAt(begin) + 1 / v.rateHz) - begin;
}

/**
 * Screen positions of a note's vibrato handles: depth at the first peak (base pitch + depth), rate at the end
 * of the first cycle (on the base pitch).
 */
export function vibratoHandles(view: View, placed: PlacedNote, map: TempoMap = CONSTANT_TEMPO) {
  const v = placed.note.vibrato ?? NO_VIBRATO;
  if (!hasVibrato(placed.note)) return null;
  const begin = placed.start + v.delay;
  const cycle = cycleTicks(v, begin, map);
  const centre = (pitch: number) => pitchToY(view, pitch) + view.keyHeight / 2;
  return {
    depth: { x: tickToX(view, begin + cycle / 4), y: centre(placed.note.pitch + v.depth) },
    rate: { x: tickToX(view, begin + cycle), y: centre(placed.note.pitch) },
    begin,
  };
}
