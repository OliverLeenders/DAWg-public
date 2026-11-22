// What canvases draw with (docs/design/design-system.md, DS13): the design tokens of tokens.css, read once from the
// document, so a canvas has no colour, face or size of its own. A colour is a token, or a mix() or alpha() of tokens.

export interface Size {
  width: number;
  height: number;
}

export interface Theme {
  // Surfaces.
  bg: string;
  bgRaised: string;
  panel: string;
  lcd: string;
  // Rules.
  border: string;
  borderStrong: string;
  // Ink.
  text: string;
  textDim: string;
  textFaint: string;
  // The accent, and words on a filled colour.
  accent: string;
  accentInk: string;
  onAccent: string;
  // Status.
  ok: string;
  warn: string;
  danger: string;
  // Signals and tones.
  signalAudio: string;
  signalEvents: string;
  mod: string;
  signalKey: string;
  tones: string[];
  toneLeft: string;
  toneRight: string;
  toneMid: string;
  toneSide: string;
  // Meters.
  meterLow: string;
  meterMid: string;
  meterClip: string;
  // Timelines.
  playhead: string;
  marker: string;
  punch: string;
  warpMarker: string;
  transient: string;
  keyWhite: string;
  keyBlack: string;
  keyLabel: string;
  // The EQ's bands: the two cuts, and the six bands from low to high.
  eqCut: string;
  eqBands: string[];
  // Type.
  sans: string;
  mono: string;
  textXs: number;
  textS: number;
  textM: number;
}

/** Overlays that only lighten or darken what is under them, as alpha() bases. */
export const LIGHT = '#ffffff';
export const SHADE = '#000000';

let cached: Theme | null = null;

/** The tokens, read from the document the first time. They don't change while the UI runs. */
export function readTheme(): Theme {
  if (cached) return cached;
  const css = getComputedStyle(document.documentElement);
  const v = (name: string) => css.getPropertyValue(name).trim();
  const px = (name: string) => parseFloat(v(name)) || 11;
  const theme: Theme = {
    bg: v('--bg'),
    bgRaised: v('--bg-raised'),
    panel: v('--bg-panel'),
    lcd: v('--lcd'),
    border: v('--border'),
    borderStrong: v('--border-strong'),
    text: v('--text'),
    textDim: v('--text-dim'),
    textFaint: v('--text-faint'),
    accent: v('--accent'),
    accentInk: v('--accent-ink'),
    onAccent: v('--on-accent'),
    ok: v('--ok'),
    warn: v('--warn'),
    danger: v('--danger'),
    signalAudio: v('--signal-audio'),
    signalEvents: v('--signal-events'),
    mod: v('--mod'),
    signalKey: v('--signal-key'),
    tones: [v('--tone-1'), v('--tone-2'), v('--tone-3'), v('--tone-4')],
    toneLeft: v('--tone-left'),
    toneRight: v('--tone-right'),
    toneMid: v('--tone-mid'),
    toneSide: v('--tone-side'),
    meterLow: v('--meter-low'),
    meterMid: v('--meter-mid'),
    meterClip: v('--meter-clip'),
    playhead: v('--playhead'),
    marker: v('--marker'),
    punch: v('--punch'),
    warpMarker: v('--warp-marker'),
    transient: v('--transient'),
    keyWhite: v('--key-white'),
    keyBlack: v('--key-black'),
    keyLabel: v('--key-label'),
    eqCut: v('--eq-cut'),
    eqBands: [1, 2, 3, 4, 5, 6].map((i) => v(`--eq-band-${i}`)),
    sans: v('--font'),
    mono: v('--mono'),
    textXs: px('--text-xs'),
    textS: px('--text-s'),
    textM: px('--text-m'),
  };
  // Before the stylesheet has loaded nothing is set; don't keep that.
  if (theme.bg) cached = theme;
  return theme;
}

/** A canvas font from the type scale: `xs` (11 px) for labels and scales, `s` (12 px) for values, `m` for names.
    A number is a size of its own, kept for a glyph inside a handle (a band's number), which has to fit the handle. */
export function font(theme: Theme, size: 'xs' | 's' | 'm' | number, options: { mono?: boolean; bold?: boolean } = {}) {
  const px = typeof size === 'number' ? size : size === 'xs' ? theme.textXs : size === 's' ? theme.textS : theme.textM;
  return `${options.bold ? '600 ' : ''}${px}px ${options.mono ? theme.mono : theme.sans}`;
}

/** Parses "#rrggbb" into [r, g, b]. */
function rgb(hex: string): [number, number, number] {
  const n = parseInt(hex.replace('#', ''), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

/** Mixes two "#rrggbb" colours; t = 0 gives a, t = 1 gives b. */
export function mix(a: string, b: string, t: number) {
  const [r1, g1, b1] = rgb(a);
  const [r2, g2, b2] = rgb(b);
  const c = (x: number, y: number) => Math.round(x + (y - x) * t);
  return `rgb(${c(r1, r2)}, ${c(g1, g2)}, ${c(b1, b2)})`;
}

/** A "#rrggbb" colour at an opacity. */
export function alpha(colour: string, opacity: number) {
  const [r, g, b] = rgb(colour);
  return `rgba(${r}, ${g}, ${b}, ${opacity})`;
}
