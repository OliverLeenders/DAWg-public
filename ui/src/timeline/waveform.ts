import { getPeaks, levelFor, rangeAt, type PeakSource } from './peaks';
import { sampleReader } from './samples';

// Draws a file's wave over a range of pixels (the arrangement's audio clips, the audio editor, the Sampler and the
// sound browser's preview). Zoomed out,
// it is the envelope of the peaks, filled as one shape; zoomed in past the finest peaks (256 frames a point), it
// comes from the raw frames: their envelope per pixel, then the wave itself as a line, with a dot on each frame once
// they are far enough apart.

export interface WaveSpec {
  /** A pool file, or a file on disk. */
  source: PeakSource;
  /** The file's length in frames. */
  frames: number;
  /** The pixels to draw, [x0, x1). */
  x0: number;
  x1: number;
  /** The file frame at the left edge of pixel x; increasing, except where a looped clip wraps. */
  frameAt: (x: number) => number;
  /** The centre line and the height of full scale. */
  mid: number;
  half: number;
  /** Vertical limits. */
  top: number;
  bottom: number;
  /** Gain at pixel x (clip gain and fades); 1 if absent. */
  gainAt?: (x: number) => number;
}

/** Below this many frames per pixel the raw frames are drawn instead of peaks. */
export const RAW_FRAMES_PER_PIXEL = 256;
/** Below this, the wave is a line. */
const LINE_FRAMES_PER_PIXEL = 1.5;
/** Below this (frames 8 px or more apart), each frame gets a dot. */
const DOT_FRAMES_PER_PIXEL = 0.125;

export function drawWave(ctx: CanvasRenderingContext2D, spec: WaveSpec, colour: string) {
  const { x0, x1, frameAt, frames } = spec;
  if (x1 <= x0) return;
  const centre = Math.floor((x0 + x1) / 2);
  const framesPerPixel = Math.max(1e-6, Math.abs(frameAt(centre + 1) - frameAt(centre)));
  ctx.fillStyle = colour;
  ctx.strokeStyle = colour;

  if (framesPerPixel < RAW_FRAMES_PER_PIXEL) {
    let first = Infinity;
    let last = -Infinity;
    for (const x of [x0, x1, centre]) {
      first = Math.min(first, frameAt(x));
      last = Math.max(last, frameAt(x));
    }
    const read = sampleReader(spec.source, Math.floor(first) - 1, Math.min(frames - 1, Math.ceil(last) + 1));
    if (read) {
      if (framesPerPixel < LINE_FRAMES_PER_PIXEL) drawLine(ctx, spec, read, framesPerPixel);
      else drawEnvelope(ctx, spec, (frame, next) => rawRange(read, frame, next, frames));
      return;
    }
    // Until the frames arrive, the finest peaks stand in.
  }

  const peaks = getPeaks(spec.source, levelFor(framesPerPixel));
  if (!peaks) {
    ctx.fillRect(x0, spec.mid, x1 - x0, 1); // on the way
    return;
  }
  drawEnvelope(ctx, spec, (frame, next) => {
    const [lo, hi] = rangeAt(peaks, frame, Math.max(1, next - frame));
    return [lo / 127, hi / 127];
  });
}

/** The lowest and highest raw value over the frames a pixel covers. */
function rawRange(read: (frame: number) => number, frame: number, next: number, frames: number): [number, number] {
  const from = Math.max(0, Math.floor(frame));
  const to = Math.min(frames - 1, Math.max(from, Math.ceil(next)));
  let lo = Infinity;
  let hi = -Infinity;
  for (let f = from; f <= to; f++) {
    const v = read(f);
    lo = Math.min(lo, v);
    hi = Math.max(hi, v);
  }
  return [lo, hi];
}

/** Fills the envelope of [lo, hi] per pixel as one shape per run of pixels inside the file. */
function drawEnvelope(ctx: CanvasRenderingContext2D, spec: WaveSpec, rangeOf: (frame: number, next: number) => [number, number]) {
  const { x0, x1, frameAt, frames, mid, half, top, bottom } = spec;
  const xs: number[] = [];
  const ups: number[] = [];
  const downs: number[] = [];

  const flush = () => {
    if (xs.length > 0) {
      ctx.beginPath();
      ctx.moveTo(xs[0], ups[0]);
      for (let i = 1; i < xs.length; i++) ctx.lineTo(xs[i], ups[i]);
      ctx.lineTo(xs[xs.length - 1] + 1, ups[ups.length - 1]);
      ctx.lineTo(xs[xs.length - 1] + 1, downs[downs.length - 1]);
      for (let i = xs.length - 1; i >= 0; i--) ctx.lineTo(xs[i], downs[i]);
      ctx.closePath();
      ctx.fill();
    }
    xs.length = 0;
    ups.length = 0;
    downs.length = 0;
  };

  for (let x = Math.floor(x0); x < x1; x++) {
    const frame = frameAt(x);
    let next = frameAt(x + 1);
    if (next <= frame) next = frame + 1; // a loop wraps here
    if (next <= 0 || frame >= frames) {
      flush();
      continue;
    }
    const [lo, hi] = rangeOf(Math.max(0, frame), next);
    const g = (spec.gainAt?.(x) ?? 1) * half;
    let up = Math.max(top, mid - hi * g);
    let down = Math.min(bottom, mid - lo * g);
    if (down - up < 1) {
      const centreY = (up + down) / 2;
      up = centreY - 0.5;
      down = centreY + 0.5;
    }
    xs.push(x);
    ups.push(up);
    downs.push(down);
  }
  flush();
}

/** The wave as a line through the frames (linear between them), with dots on the frames when zoomed in far. */
function drawLine(ctx: CanvasRenderingContext2D, spec: WaveSpec, read: (frame: number) => number, framesPerPixel: number) {
  const { x0, x1, frameAt, frames, mid, half, top, bottom } = spec;
  const at = (frame: number) => {
    const f = Math.floor(frame);
    const t = frame - f;
    return read(f) * (1 - t) + read(f + 1) * t;
  };
  const yOf = (value: number, x: number) => Math.min(bottom, Math.max(top, mid - value * (spec.gainAt?.(x) ?? 1) * half));

  ctx.lineWidth = 1.25;
  ctx.lineJoin = 'round';
  ctx.beginPath();
  let drawing = false;
  for (let x = Math.floor(x0); x <= x1; x++) {
    const frame = frameAt(x);
    if (frame < 0 || frame > frames - 1) {
      drawing = false;
      continue;
    }
    const y = yOf(at(frame), x);
    if (drawing) ctx.lineTo(x, y);
    else ctx.moveTo(x, y);
    drawing = true;
  }
  ctx.stroke();

  if (framesPerPixel >= DOT_FRAMES_PER_PIXEL) return;
  // A dot on each frame: where it falls between the pixels around it.
  for (let x = Math.floor(x0); x < x1; x++) {
    const a = frameAt(x);
    const b = frameAt(x + 1);
    if (b <= a) continue;
    for (let f = Math.ceil(a); f < b; f++) {
      if (f < 0 || f >= frames) continue;
      const fx = x + (f - a) / (b - a);
      ctx.fillRect(fx - 1.5, yOf(read(f), x) - 1.5, 3, 3);
    }
  }
}
