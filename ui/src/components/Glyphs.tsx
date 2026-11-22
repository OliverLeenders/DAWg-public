import { ArrowDown, ArrowUp, ArrowUpDown, Funnel } from 'lucide-react';
import type { ReactNode } from 'react';

// Small pictures for values that are shapes or directions (docs/ui-review-2026-11-10.md, 2.1 and 2.2): a waveform, a
// filter's response, a quantiser's staircase, a clip curve, an arrow. Drawn in the current text colour, 16 × 12 by default, always with the value's name as
// the control's title.

function Glyph({ d, size = 16, fill = false }: { d: string; size?: number; fill?: boolean }) {
  return (
    <svg width={size} height={(size * 3) / 4} viewBox="0 0 16 12" aria-hidden className="glyph">
      <path d={d} fill={fill ? 'currentColor' : 'none'} stroke="currentColor" strokeWidth={1.4} strokeLinejoin="round" strokeLinecap="round" />
    </svg>
  );
}

const SINE = 'M 1 6 C 3 0, 5 0, 8 6 S 13 12, 15 6';
const TRIANGLE = 'M 1 6 L 4.5 1.5 L 11.5 10.5 L 15 6';
// One cycle: up, ramp down, up again.
const SAW = 'M 2 10.5 L 2 1.5 L 14 10.5 L 14 1.5';
const PULSE = 'M 1 10.5 L 1 1.5 L 8 1.5 L 8 10.5 L 15 10.5 L 15 1.5';
const NOISE = 'M 1 6 L 2.5 2 L 4 9 L 5.5 4 L 7 10.5 L 8.5 1.5 L 10 8 L 11.5 3 L 13 9.5 L 15 5';
// Filter responses: level up, frequency across.
const LOW_PASS = 'M 1 3 L 9 3 C 11 3, 12 6, 14 11';
const HIGH_PASS = 'M 2 11 C 4 6, 5 3, 7 3 L 15 3';
const BAND_PASS = 'M 1 11 C 4 11, 6 2, 8 2 S 12 11, 15 11';
const NOTCH = 'M 1 3 L 5 3 C 7 3, 7 11, 8 11 S 9 3, 11 3 L 15 3';
// Quantiser staircases, output over input: a tread through the middle, or a riser.
const MID_TREAD = 'M 1 10.5 H 3.5 V 8.25 H 6.5 V 6 H 9.5 V 3.75 H 12.5 V 1.5 H 15';
const MID_RISE = 'M 1 10.5 H 4.5 V 7.5 H 8 V 4.5 H 11.5 V 1.5 H 15';
// Clip curves: a straight line cut flat, or bent into the limit.
const HARD_CLIP = 'M 1 10.5 L 4 10.5 L 12 1.5 L 15 1.5';
const SOFT_CLIP = 'M 1 10.5 C 7 10.5, 9 1.5, 15 1.5';

/** A waveform's or a filter type's picture, or an arrow, by the name its choice has; null for names without one. */
export function glyphFor(name: string): ReactNode {
  switch (name) {
    case 'Sine':
      return <Glyph d={SINE} />;
    case 'Triangle':
      return <Glyph d={TRIANGLE} />;
    case 'Saw':
      return <Glyph d={SAW} />;
    case 'Pulse':
      return <Glyph d={PULSE} />;
    case 'Noise':
      return <Glyph d={NOISE} />;
    case 'Low-pass':
      return <Glyph d={LOW_PASS} />;
    case 'High-pass':
      return <Glyph d={HIGH_PASS} />;
    case 'Band-pass':
      return <Glyph d={BAND_PASS} />;
    case 'Notch':
      return <Glyph d={NOTCH} />;
    case 'Mid-tread':
      return <Glyph d={MID_TREAD} />;
    case 'Mid-rise':
      return <Glyph d={MID_RISE} />;
    case 'Hard':
      return <Glyph d={HARD_CLIP} />;
    case 'Soft':
      return <Glyph d={SOFT_CLIP} />;
    case 'Up':
      return <ArrowUp size={13} />;
    case 'Down':
      return <ArrowDown size={13} />;
    case 'Nearest':
      return <ArrowUpDown size={13} />;
    case 'Filter':
      return <Funnel size={12} />;
    default:
      return null;
  }
}

/** Whether every one of a choice's values has a picture, so it can be shown as a row of pictures. */
export const allHaveGlyphs = (choices: string[]) => choices.length > 0 && choices.every((c) => glyphFor(c) !== null);

/** The sidechain's key filters as pictures: a high-pass and a low-pass slope. */
export const HighPassGlyph = () => <Glyph d={HIGH_PASS} />;
export const LowPassGlyph = () => <Glyph d={LOW_PASS} />;

/** Polarity inversion: Ø. */
export function PhaseGlyph() {
  return (
    <svg width={14} height={14} viewBox="0 0 14 14" aria-hidden className="glyph">
      <circle cx={7} cy={7} r={4.5} fill="none" stroke="currentColor" strokeWidth={1.4} />
      <line x1={2.5} y1={11.5} x2={11.5} y2={2.5} stroke="currentColor" strokeWidth={1.4} strokeLinecap="round" />
    </svg>
  );
}

/** Mono: one circle where stereo would be two. */
export function MonoGlyph() {
  return (
    <svg width={14} height={14} viewBox="0 0 14 14" aria-hidden className="glyph">
      <circle cx={7} cy={7} r={4.5} fill="none" stroke="currentColor" strokeWidth={1.4} />
    </svg>
  );
}
