// What the Convolution Reverb's panel draws (docs/design/convolution-reverb.md, I8 and I12): which part of the IR is
// kept, how Decay and the fades bend its envelope, and the readouts. The engine's shaping (engine/src/Convolver.cpp,
// shapeImpulse) is the reference; these follow the same formulas.

/** The node's panel meters. */
export interface ConvolutionMeters {
  /** The whole file's envelope in dB, at most 256 points over its length. */
  envelope?: number[];
  /** The file's length in seconds. */
  seconds?: number;
  /** Its estimated decay time to −60 dB, or 0. */
  decay?: number;
  pending?: boolean;
  missing?: boolean;
  truncated?: boolean;
}

/** The shaping controls, as the panel reads them. */
export interface ShapeValues {
  startMs: number;
  lengthPercent: number;
  decayPercent: number;
  reverse: boolean;
}

/** The kept part of the file, in seconds: from Start, Length per cent of what follows. */
export function keptRange(seconds: number, startMs: number, lengthPercent: number): { from: number; to: number } {
  const from = Math.min(Math.max(0, startMs / 1000), seconds);
  const to = from + (seconds - from) * (Math.min(100, Math.max(1, lengthPercent)) / 100);
  return { from, to };
}

/** What Decay adds to the IR's own slope, in dB per second (negative), for a file that decays in `decaySeconds`. */
export function decaySlopeDb(decaySeconds: number, decayPercent: number): number {
  const decay = Math.min(100, Math.max(10, decayPercent)) / 100;
  if (decaySeconds <= 0 || decay >= 1) return 0;
  return (-60 / decaySeconds) * (1 / decay - 1);
}

/** The gain in dB of a raised-cosine fade, `fraction` of the way in from its silent end. */
export function fadeDb(fraction: number): number {
  const g = 0.5 - 0.5 * Math.cos(Math.PI * Math.min(1, Math.max(0, fraction)));
  return 20 * Math.log10(Math.max(g, 1e-6));
}

/**
 * The envelope after shaping, over the kept part, in the file's time (seconds) and dB: each envelope point inside the
 * kept range gets Decay's slope (for a file that decays in `decaySeconds`) and the end fade, measured from the kept
 * part's start; Reverse mirrors the result within the kept range.
 */
export function shapedEnvelope(
  envelope: number[],
  seconds: number,
  decaySeconds: number,
  shape: ShapeValues,
): Array<{ t: number; db: number }> {
  if (envelope.length === 0 || seconds <= 0) return [];
  const { from, to } = keptRange(seconds, shape.startMs, shape.lengthPercent);
  const kept = to - from;
  if (kept <= 0) return [];
  const span = seconds / envelope.length;
  const slope = decaySlopeDb(decaySeconds, shape.decayPercent);
  const fade = shape.lengthPercent < 100 ? Math.min(kept, Math.max(0.005, kept / 10)) : 0;
  const points: Array<{ t: number; db: number }> = [];

  envelope.forEach((db, i) => {
    const t = (i + 0.5) * span;
    if (t < from || t > to) return;
    const age = t - from;
    let level = db + slope * age;
    if (fade > 0 && age > kept - fade) level += fadeDb((kept - age) / fade);
    points.push({ t: shape.reverse ? to - age : t, db: level });
  });

  return shape.reverse ? points.reverse() : points;
}

/** The reverb time the controls give: the file's decay time, shortened by Decay and scaled by Stretch. */
export function reverbTime(decaySeconds: number, decayPercent: number, stretchPercent: number): number {
  if (decaySeconds <= 0) return 0;
  return decaySeconds * (Math.min(100, Math.max(10, decayPercent)) / 100) * (stretchPercent / 100);
}

/** The reverb time's readout, e.g. "RT 2.4 s", or a dash when it isn't known. */
export function reverbTimeReadout(seconds: number): string {
  if (!(seconds > 0)) return 'RT —';
  return seconds < 1 ? `RT ${Math.round(seconds * 1000)} ms` : `RT ${seconds.toFixed(1)} s`;
}

/** What the header names: the file, the generated default, or a missing file. */
export function impulseName(fileId: number | undefined, fileName: string | undefined, missing: boolean): string {
  if (!fileId) return 'Default room';
  if (missing || fileName === undefined) return 'Missing file';
  return fileName;
}

/** A step of 1, 2 or 5 times a power of ten giving about `count` labels over `range`. */
export function tickStep(range: number, count = 5): number {
  if (!(range > 0)) return 1;
  const raw = range / count;
  const power = Math.pow(10, Math.floor(Math.log10(raw)));
  return [1, 2, 5, 10].map((m) => m * power).find((s) => s >= raw) ?? raw;
}

