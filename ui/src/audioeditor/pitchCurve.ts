import type { PitchNote } from '../bridge/protocol';
import { evaluateCurve } from '../pianoroll/geometry';

// The correction of a file's pitch (docs/design/pitch-correction.md, section 4.3, PC6), for drawing the corrected pitch
// live during drags. The twin of pitchCorrection in engine/src/PitchNotes.cpp; both are tested against the same table.

const RAMP_SECONDS = 0.01;
const VIBRATO_SLOW_SECONDS = 0.15;
const VIBRATO_JITTER_SECONDS = 0.04;

/** A pitch track: semitones (null unvoiced) every `hop` seconds from `start` seconds. */
export interface Track {
  start: number;
  hop: number;
  pitch: ReadonlyArray<number | null>;
}

const averaged = (v: number[], half: number) =>
  v.map((_, i) => {
    const from = Math.max(0, i - half);
    const to = Math.min(v.length, i + half + 1);
    let sum = 0;
    for (let j = from; j < to; j++) sum += v[j];
    return sum / (to - from);
  });

/** Whether a note sounds other than sung (PitchNote::untouched in the engine, negated). */
export const noteEdited = (n: PitchNote) =>
  n.targetCentre !== n.detectedCentre || n.vibrato !== 1 || JSON.stringify(n.shape) !== JSON.stringify(n.detectedShape);

/** One note's correction anywhere inside it, without its edge ramps. */
function noteCorrection(note: PitchNote, sampleRate: number, track: Track | undefined) {
  let vibrato: number[] = [];
  let first = 0;
  if (track && track.pitch.length > 0 && Math.abs(note.vibrato - 1) >= 1e-9) {
    const indexOf = (frame: number) => (frame / sampleRate - track.start) / track.hop;
    first = Math.ceil(indexOf(note.start));
    const last = Math.min(track.pitch.length - 1, Math.floor(indexOf(note.end - 1)));
    if (first >= 0 && last >= first) {
      const detail: number[] = [];
      for (let i = first; i <= last; i++) {
        const frame = Math.round((track.start + i * track.hop) * sampleRate);
        const p = track.pitch[i];
        detail.push(p === null ? 0 : p - note.detectedCentre - evaluateCurve(note.detectedShape, frame - note.start, 0));
      }
      const slow = averaged(detail, Math.round(VIBRATO_SLOW_SECONDS / track.hop / 2));
      vibrato = averaged(
        detail.map((d, i) => d - slow[i]),
        Math.max(1, Math.round(VIBRATO_JITTER_SECONDS / track.hop / 2)),
      );
    }
  }
  const shape = note.shape;
  return (frame: number) => {
    const t = frame - note.start;
    let delta = note.targetCentre - note.detectedCentre + evaluateCurve(shape, t, 0) - evaluateCurve(note.detectedShape, t, 0);
    if (vibrato.length > 0 && track) {
      const index = (frame / sampleRate - track.start) / track.hop - first;
      const i = Math.floor(index);
      if (i >= 0 && i < vibrato.length) {
        const j = Math.min(i + 1, vibrato.length - 1);
        delta += (vibrato[i] + (vibrato[j] - vibrato[i]) * (index - i)) * (note.vibrato - 1);
      }
    }
    return delta;
  };
}

/** The correction in semitones at `count` frames from `start`, `hop` apart (0 outside notes and in lead-ins). */
export function pitchCorrection(notes: readonly PitchNote[], sampleRate: number, start: number, count: number, hop: number, track?: Track): number[] {
  const result = new Array<number>(count).fill(0);
  if (notes.length === 0) return result;
  const corrections = notes.map((n) => noteCorrection(n, sampleRate, track));
  const transition = (i: number) => (notes[i + 1].transitionMs * sampleRate) / 1000;
  const joined = (i: number) => i >= 0 && i + 1 < notes.length && notes[i + 1].start - notes[i].end <= transition(i);
  const width = (i: number) => Math.max(transition(i), notes[i + 1].start - notes[i].end);
  const ramp = RAMP_SECONDS * sampleRate;

  for (let k = 0; k < count; k++) {
    const frame = start + k * hop;
    // The last note starting at or before this frame (or the first, before them all).
    let i = 0;
    while (i + 1 < notes.length && notes[i + 1].start <= frame) i++;

    let eased = false;
    for (const j of [i, i - 1]) {
      if (!joined(j)) continue;
      const middle = (notes[j].end + notes[j + 1].start) / 2;
      const w = width(j);
      if (Math.abs(frame - middle) < w / 2) {
        const x = (frame - (middle - w / 2)) / w;
        const s = 0.5 - 0.5 * Math.cos(Math.PI * x);
        const a = corrections[j](Math.min(frame, notes[j].end - 1));
        const b = corrections[j + 1](Math.max(frame, notes[j + 1].start));
        result[k] = a + (b - a) * s;
        eased = true;
        break;
      }
    }
    if (eased) continue;

    const note = notes[i];
    if (frame < note.start || frame >= note.end) continue;
    const rampIn = joined(i - 1) ? 1 : Math.min(1, Math.max(0, (frame - note.start) / ramp));
    const rampOut = joined(i) ? 1 : Math.min(1, Math.max(0, (note.end - frame) / ramp));
    result[k] = corrections[i](frame) * Math.min(rampIn, rampOut);
  }
  return result;
}

/** The perceived pitch of a stretch of a track, as the engine's measureCentre does without levels: the mean of the
 * voiced values over the middle 70 % of them. Null if none is voiced. */
export function measureCentre(track: Track, sampleRate: number, start: number, end: number): number | null {
  const values: number[] = [];
  track.pitch.forEach((p, i) => {
    const frame = (track.start + i * track.hop) * sampleRate;
    if (p !== null && frame >= start && frame < end) values.push(p);
  });
  if (values.length === 0) return null;
  const cut = Math.floor(values.length * 0.15);
  const middle = values.length - 2 * cut > 0 ? values.slice(cut, values.length - cut) : values;
  return middle.reduce((a, b) => a + b, 0) / middle.length;
}
