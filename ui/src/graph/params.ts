import type { ParamDescriptor } from '../bridge/protocol';

// Parameter controls (docs/design/phase8-node-graph.md, G2): skewed ranges put `mid` at the middle of a control,
// and values are shown with their unit.

/** The base of the exponential curve that puts `mid` at the control's middle (1 for a linear range). The curve,
    min + (max - min) * (r^position - 1) / (r - 1), behaves like a logarithmic scale for frequencies. */
function curveOf(p: ParamDescriptor): number {
  if (p.mid <= p.min || p.mid >= p.max) return 1;
  const r = Math.pow((p.max - p.mid) / (p.mid - p.min), 2);
  return Math.abs(r - 1) < 1e-6 ? 1 : r;
}

/** A value's position on its control, 0..1. */
export function toPosition(p: ParamDescriptor, value: number): number {
  const linear = Math.min(1, Math.max(0, (value - p.min) / (p.max - p.min)));
  const r = curveOf(p);
  return r === 1 ? linear : Math.log(1 + linear * (r - 1)) / Math.log(r);
}

/** The value at a control position (0..1), rounded for integer, choice and toggle parameters. */
/** A parameter's knob curve, for a number field dragged along it (components/dragScale.ts). */
export const paramTaper = (p: ParamDescriptor) => ({ toNorm: (v: number) => toPosition(p, v), fromNorm: (n: number) => fromPosition(p, n) });

export function fromPosition(p: ParamDescriptor, position: number): number {
  const t = Math.min(1, Math.max(0, position));
  const r = curveOf(p);
  const shaped = r === 1 ? t : (Math.pow(r, t) - 1) / (r - 1);
  const value = p.min + (p.max - p.min) * shaped;
  return p.kind === 'continuous' ? value : Math.round(value);
}

/** The value as shown: choice labels, On/Off, or a number with its unit (kHz above 1000 Hz). */
/** A modulation depth's span: how far a depth of 1 moves the parameter (modulation.md, M5), in octaves for a frequency,
    in knob travel (1) for another skewed parameter, else in its units (macros-and-modulation-column.md, MC1, MC2). */
export const depthSpan = (p: ParamDescriptor) => p.modulationSpan;

/** How a depth moves the parameter; 'linear' for descriptors that don't say. */
export const depthScale = (p: ParamDescriptor) => p.modulationScale ?? 'linear';

/** The step for editing a depth in its span's terms: a power of ten near a thousandth of its range, whole for an
    integer linear parameter. */
export function depthStep(p: ParamDescriptor) {
  const step = Math.pow(10, Math.floor(Math.log10(depthSpan(p) / 1000)));
  return p.kind === 'integer' && depthScale(p) === 'linear' ? Math.max(1, step) : step;
}

/** A depth as the amount it moves the parameter, signed: "+1.5 oct" for a frequency, "+25%" of a skewed knob's travel,
    or in the parameter's units, e.g. "+100%" or "-2.40 st". */
export function formatDepth(p: ParamDescriptor, depth: number): string {
  const amount = depth * depthSpan(p);
  const sign = amount > 0 ? '+' : amount < 0 ? '-' : '';
  const scale = depthScale(p);
  if (scale === 'octaves') return `${sign}${Math.abs(amount).toFixed(Math.abs(amount) >= 10 ? 1 : 2).replace(/\.?0+$/, '')} oct`;
  if (scale === 'travel') return `${sign}${(Math.abs(amount) * 100).toFixed(0)}%`;
  return `${sign}${formatParam(p, Math.abs(amount))}`;
}

/** The value modulation sets for a base and a summed amount (source times depth), as the engine works it out
    (modulatedParamValue, MC1 to MC3): clamped to the range. */
export function modulatedValue(p: ParamDescriptor, base: number, amount: number): number {
  const scale = depthScale(p);
  let value: number;
  if (scale === 'octaves') value = Math.max(base, p.min) * Math.pow(2, amount * depthSpan(p));
  else if (scale === 'travel') return fromPosition(p, toPosition(p, base) + amount);
  else value = base + amount * depthSpan(p);
  if (p.kind === 'integer') value = Math.round(value);
  return Math.min(p.max, Math.max(p.min, value));
}

/** The depth that moves a parameter from `from` to `to` at a full-scale source: the inverse of modulatedValue, clamped
    to -1..1 (Map mode, MC9). */
export function depthBetween(p: ParamDescriptor, from: number, to: number): number {
  const span = depthSpan(p);
  if (!(span > 0)) return 0;
  const scale = depthScale(p);
  const amount =
    scale === 'octaves'
      ? Math.log2(Math.max(to, p.min, 1e-9) / Math.max(from, p.min, 1e-9)) / span
      : scale === 'travel'
        ? toPosition(p, to) - toPosition(p, from)
        : (to - from) / span;
  return Math.min(1, Math.max(-1, amount));
}

/** Whether a parameter is a pan from -100 (left) to 100 (right), shown as the track's pan is: C, L30, R30. */
export const isPan = (p: ParamDescriptor) => /(^|\.)pan$/.test(p.id) && p.min === -100 && p.max === 100 && p.kind === 'continuous';

export function formatParam(p: ParamDescriptor, value: number): string {
  if (isPan(p)) {
    const amount = Math.round(Math.abs(value));
    return amount === 0 ? 'C' : `${value < 0 ? 'L' : 'R'}${amount}`;
  }
  if (p.kind === 'choice') return p.choices[Math.round(value)] ?? String(value);
  if (p.kind === 'toggle') return value >= 0.5 ? 'On' : 'Off';
  if (p.kind === 'integer') return `${Math.round(value)}${p.unit ? ` ${p.unit}` : ''}`;
  if (p.unit === 'Hz' && value >= 1000) return `${(value / 1000).toFixed(value >= 10000 ? 1 : 2)} kHz`;
  // One symbol per unit and no more digits than can be heard or set (docs/ui-review-2026-11-10.md, 3.14): degrees as
  // whole °, percentages in whole steps above 10 %, milliseconds to 0.1 ms.
  const symbol = p.unit === 'deg' ? '°' : p.unit;
  const size = Math.abs(value);
  const digits =
    symbol === '°' ? 0 : symbol === '%' || symbol === 'ms' ? (size >= 10 ? (symbol === 'ms' && size < 100 ? 1 : 0) : 1) : size >= 100 ? 0 : size >= 10 ? 1 : 2;
  const unit = symbol === '' ? '' : symbol.startsWith(':') || symbol === '%' || symbol === '°' ? symbol : ` ${symbol}`;
  return `${value.toFixed(digits)}${unit}`;
}

/** A typed value for a continuous or integer parameter, clamped to its range, or null if it isn't a number. A unit
    may follow the number and is ignored, except "k" (and "kHz") for thousands: "1.5k", "1500 Hz" and "1.5 kHz" are
    all 1500. A comma works as the decimal separator too. A frequency also takes a note name (takesNoteNames). */
export function parseParam(p: ParamDescriptor, text: string): number | null {
  if (isPan(p)) {
    const side = /^\s*(?:(c)|([lr])\s*([\d.,]+))\s*%?\s*$/i.exec(text);
    if (side?.[1]) return 0;
    if (side) {
      const amount = Number(side[3].replace(',', '.'));
      if (Number.isFinite(amount)) return Math.min(100, Math.max(-100, side[2].toLowerCase() === 'l' ? -amount : amount));
    }
  }
  if (takesNoteNames(p)) {
    const hz = parseNote(text);
    if (hz !== null) return Math.min(p.max, Math.max(p.min, p.kind === 'integer' ? Math.round(hz) : hz));
  }
  return parseNumber(text, p.min, p.max, p.kind === 'integer');
}

/** Whether a parameter is an audible frequency, whose field takes a note name (docs/design/design-system.md, DS17):
    in Hz, with a range reaching 1 kHz (a cutoff, a band, a crossover), not an LFO's or tremolo's rate. */
export const takesNoteNames = (p: ParamDescriptor) => p.unit === 'Hz' && p.max >= 1000 && (p.kind === 'continuous' || p.kind === 'integer');

const LETTERS: Record<string, number> = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };
const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
/** A4, the tuning reference: DAWg has no project tuning yet (DS17). */
const A4_HZ = 440;

/** A note name as a frequency: "A4", "C#3", "C♯3", "Eb4", "E♭4" (C4 is MIDI 60, A4 440 Hz, equal temperament), with an
    optional offset in cents, "A4+20", "A4 -15c", "A4+20ct". Null if it isn't one. */
export function parseNote(text: string): number | null {
  const m = /^\s*([a-g])([#♯b♭]?)\s*(-?\d{1,2})\s*(?:([+\-−])\s*(\d+(?:[.,]\d+)?)\s*(?:c|ct|cents?)?)?\s*$/i.exec(text);
  if (!m) return null;
  const accidental = m[2] === '#' || m[2] === '♯' ? 1 : m[2] === 'b' || m[2] === '♭' ? -1 : 0;
  const midi = (Number(m[3]) + 1) * 12 + LETTERS[m[1].toLowerCase()] + accidental;
  const cents = m[4] ? (m[4] === '+' ? 1 : -1) * Number(m[5].replace(',', '.')) : 0;
  return A4_HZ * Math.pow(2, (midi - 69 + cents / 100) / 12);
}

/** A frequency's nearest note and how far off it is, as "A4 +0 ct" (the hint beside a frequency field, DS17). */
export function noteOf(hz: number): string {
  if (!(hz > 0)) return '';
  const exact = 69 + 12 * Math.log2(hz / A4_HZ);
  const midi = Math.round(exact);
  const cents = Math.round((exact - midi) * 100);
  return `${NAMES[((midi % 12) + 12) % 12]}${Math.floor(midi / 12) - 1} ${cents < 0 ? '−' : '+'}${Math.abs(cents)} ct`;
}

/** The note hint for a parameter's value, or undefined where the parameter takes no note names. */
export const noteHint = (p: ParamDescriptor, value: number) => (takesNoteNames(p) ? noteOf(value) : undefined);

/** A typed number clamped to a range, read as parseParam() reads it; the minus sign readouts show (−) works, and
    "-inf" or "−∞" is the range's bottom (a gain at its floor shows "−∞ dB"). Null if it isn't a number. */
export function parseNumber(text: string, min: number, max: number, whole = false): number | null {
  const cleaned = text.replace(/\u2212/g, '-');
  if (/^\s*-\s*(inf|\u221e)/i.test(cleaned)) return min;
  const match = /^\s*([+-]?(?:\d+(?:[.,]\d*)?|[.,]\d+))\s*(k)?/i.exec(cleaned);
  if (!match) return null;
  let value = Number(match[1].replace(',', '.')) * (match[2] ? 1000 : 1);
  if (!Number.isFinite(value)) return null;
  if (whole) value = Math.round(value);
  return Math.min(max, Math.max(min, value));
}
