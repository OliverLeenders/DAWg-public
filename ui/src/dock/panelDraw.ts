import { font, type Size, type Theme } from '../canvas/theme';

// Drawing helpers shared by the device panels (docs/design/phase11-faust-panels.md, P3): a log frequency axis from
// 20 Hz to 20 kHz and a linear dB axis.

export const MIN_FREQ = 20;
export const MAX_FREQ = 20000;

export const freqToX = (f: number, width: number) => (Math.log(f / MIN_FREQ) / Math.log(MAX_FREQ / MIN_FREQ)) * width;
export const xToFreq = (x: number, width: number) =>
  MIN_FREQ * Math.pow(MAX_FREQ / MIN_FREQ, Math.min(1, Math.max(0, x / Math.max(1, width))));

export const dbToY = (db: number, height: number, range: number) => height / 2 - (db / range) * (height / 2);
export const yToDb = (y: number, height: number, range: number) => ((height / 2 - y) / (height / 2)) * range;

/** The frequencies that get a name, most important first: where names would overlap, the later ones are left out. */
const NAMED_FREQS = [100, 1000, 10000, 20000, 50, 500, 5000, 200, 2000];

/** Grid lines at decades and octave-ish steps, names at 1-2-5 steps where they fit, and dB lines every `step`. */
export function drawFrequencyGrid(ctx: CanvasRenderingContext2D, size: Size, theme: Theme, range: number, step: number) {
  ctx.fillStyle = theme.bg;
  ctx.fillRect(0, 0, size.width, size.height);
  ctx.lineWidth = 1;
  ctx.font = font(theme, 'xs');
  ctx.textBaseline = 'top';

  for (const f of [30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 20000]) {
    const x = Math.round(freqToX(f, size.width)) + 0.5;
    const major = f === 100 || f === 1000 || f === 10000;
    ctx.strokeStyle = theme.border;
    ctx.globalAlpha = major ? 1 : 0.45;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, size.height);
    ctx.stroke();
  }
  ctx.globalAlpha = 1;

  ctx.fillStyle = theme.textDim;
  const taken: Array<[number, number]> = [];
  for (const f of NAMED_FREQS) {
    const x = Math.round(freqToX(f, size.width)) + 0.5;
    const name = f >= 1000 ? `${f / 1000}k` : String(f);
    const width = ctx.measureText(name).width;
    // 20k sits on the right edge, so its name goes to the left of the line.
    const left = f === MAX_FREQ ? x - 3 - width : x + 3;
    if (taken.some(([a, b]) => left < b + 6 && left + width > a - 6)) continue;
    taken.push([left, left + width]);
    ctx.fillText(name, left, size.height - 14);
  }

  for (let db = -range; db <= range; db += step) {
    const y = Math.round(dbToY(db, size.height, range)) + 0.5;
    ctx.strokeStyle = theme.border;
    ctx.globalAlpha = db === 0 ? 1 : 0.45;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(size.width, y);
    ctx.stroke();
    ctx.globalAlpha = 1;
    if (db !== -range && db !== range) {
      ctx.fillStyle = theme.textDim;
      ctx.fillText(`${db > 0 ? '+' : ''}${db}`, 3, y + 2);
    }
  }
}

const SPECTRUM_TOP = 0; // dB at the top of the panel
const SPECTRUM_FLOOR = -84; // and at the bottom

/** How a spectrum is drawn: its colour, and the opacity of its line and of the top of its fill. */
export interface SpectrumStyle {
  colour?: string;
  line?: number;
  fill?: number;
}

/** The analyser's spectrum (dB, log bands from 20 Hz to 20 kHz, tilted by the engine) as a line, with a fill below
    it that fades out towards the bottom. The bands are joined through their midpoints, so the line has no corners. */
export function drawSpectrum(
  ctx: CanvasRenderingContext2D,
  size: Size,
  theme: Theme,
  spectrum: number[] | null,
  style: SpectrumStyle = {},
) {
  const colour = style.colour ?? theme.textDim;
  if (!spectrum || spectrum.length < 2) return;
  const points = spectrum.map((db, i) => ({
    x: (i / (spectrum.length - 1)) * size.width,
    y:
      size.height *
      ((SPECTRUM_TOP - Math.max(SPECTRUM_FLOOR, Math.min(SPECTRUM_TOP, db))) / (SPECTRUM_TOP - SPECTRUM_FLOOR)),
  }));

  const trace = () => {
    ctx.beginPath();
    ctx.moveTo(points[0].x, points[0].y);
    for (let i = 1; i < points.length - 1; i++)
      ctx.quadraticCurveTo(points[i].x, points[i].y, (points[i].x + points[i + 1].x) / 2, (points[i].y + points[i + 1].y) / 2);
    ctx.lineTo(points[points.length - 1].x, points[points.length - 1].y);
  };

  trace();

  ctx.lineTo(size.width, size.height);
  ctx.lineTo(0, size.height);
  ctx.closePath();
  // The gradient starts at the loudest band, so a quiet spectrum's fill is as visible as a loud one's.
  const fill = ctx.createLinearGradient(0, Math.min(...points.map((p) => p.y)), 0, size.height);
  fill.addColorStop(0, colour);
  fill.addColorStop(1, 'transparent');
  ctx.fillStyle = fill;
  ctx.globalAlpha = style.fill ?? 0.4;
  ctx.fill();

  // The fill's path ends along the bottom: stroke the line on its own.
  trace();
  ctx.strokeStyle = colour;
  ctx.lineWidth = 1.25;
  ctx.globalAlpha = style.line ?? 0.7;
  ctx.stroke();
  ctx.globalAlpha = 1;
}

/** A response curve given as frequencies and dB. The samples are joined with a smooth path (Catmull-Rom tangents as
    Bézier controls), each control held within its segment so that a sharp peak cannot overshoot the response. */
export function drawCurve(
  ctx: CanvasRenderingContext2D,
  size: Size,
  xs: number[],
  ys: number[],
  range: number,
  colour: string,
  width = 2,
  fill = false,
) {
  if (xs.length === 0) return;

  const points = xs.map((f, i) => ({
    x: freqToX(f, size.width),
    y: dbToY(Math.max(-range * 2, Math.min(range * 2, ys[i])), size.height, range),
  }));
  const between = (value: number, a: number, b: number) => Math.min(Math.max(value, Math.min(a, b)), Math.max(a, b));

  ctx.beginPath();
  ctx.moveTo(points[0].x, points[0].y);
  for (let i = 0; i < points.length - 1; i++) {
    const p0 = points[Math.max(0, i - 1)];
    const p1 = points[i];
    const p2 = points[i + 1];
    const p3 = points[Math.min(points.length - 1, i + 2)];
    const c1x = between(p1.x + (p2.x - p0.x) / 6, p1.x, p2.x);
    const c1y = between(p1.y + (p2.y - p0.y) / 6, p1.y, p2.y);
    const c2x = between(p2.x - (p3.x - p1.x) / 6, p1.x, p2.x);
    const c2y = between(p2.y - (p3.y - p1.y) / 6, p1.y, p2.y);
    ctx.bezierCurveTo(c1x, c1y, c2x, c2y, p2.x, p2.y);
  }
  ctx.strokeStyle = colour;
  ctx.lineWidth = width;
  ctx.stroke();
  if (fill) {
    ctx.lineTo(size.width, dbToY(0, size.height, range));
    ctx.lineTo(0, dbToY(0, size.height, range));
    ctx.closePath();
    ctx.fillStyle = colour;
    ctx.globalAlpha = 0.08;
    ctx.fill();
    ctx.globalAlpha = 1;
  }
}

/** A round handle, filled when active. */
export function drawHandle(ctx: CanvasRenderingContext2D, theme: Theme, x: number, y: number, colour: string, active: boolean, label?: string) {
  ctx.beginPath();
  // Two-letter labels (HP, LP) get a slightly larger handle and a smaller font.
  const wide = (label?.length ?? 0) > 1;
  ctx.arc(x, y, (active ? 7 : 6) + (wide ? 1.5 : 0), 0, Math.PI * 2);
  ctx.fillStyle = active ? colour : theme.onAccent;
  ctx.fill();
  ctx.strokeStyle = colour;
  ctx.lineWidth = 2;
  ctx.stroke();
  if (label) {
    ctx.fillStyle = active ? theme.onAccent : colour;
    // A glyph inside the handle: sized to fit it, the one place below the type scale (DS13).
    ctx.font = font(theme, wide ? 7 : 9, { bold: true });
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(label, x, y + 0.5);
    ctx.textAlign = 'left';
  }
}

/** The level dot's radius: the signal's place on a transfer curve (the compressor's and the shapers'). */
export const LEVEL_DOT_RADIUS = 4;

/** The quietest input a shaper's level dot shows, linear (-60 dB, the bottom of the compressor's plot). */
export const MIN_DOT_LEVEL = 0.001;

/** The signal's place on a transfer curve, as a dot in the audio signal's colour. */
export function drawLevelDot(ctx: CanvasRenderingContext2D, theme: Theme, x: number, y: number) {
  ctx.beginPath();
  ctx.arc(x, y, LEVEL_DOT_RADIUS, 0, Math.PI * 2);
  ctx.fillStyle = theme.signalAudio;
  ctx.fill();
}

/** A sampled curve's value at x, interpolated between its samples and held beyond its ends; x ascending. */
export function curveAt(curve: { x: number[]; y: number[] }, x: number): number {
  const { x: xs, y: ys } = curve;
  if (xs.length === 0) return x;
  if (x <= xs[0]) return ys[0];
  if (x >= xs[xs.length - 1]) return ys[ys.length - 1];
  let lo = 0;
  let hi = xs.length - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (xs[mid] <= x) lo = mid;
    else hi = mid;
  }
  const t = (x - xs[lo]) / (xs[hi] - xs[lo] || 1);
  return ys[lo] + t * (ys[hi] - ys[lo]);
}

/** Where a shaper's level dot sits, in the curve's units: the input level on the positive side, clamped to the plot's
    -1..1 input, on the curve; null when the input is too quiet to show or there is no curve yet. */
export function shaperDot(curve: { x: number[]; y: number[] } | null, level: number | undefined): { x: number; y: number } | null {
  if (!curve || level === undefined || !(level >= MIN_DOT_LEVEL)) return null;
  const x = Math.min(1, level);
  return { x, y: curveAt(curve, x) };
}
