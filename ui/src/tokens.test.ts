import { describe, expect, it } from 'vitest';
import css from './tokens.css?raw';

// The contrast of the token pairs the UI relies on (docs/design/design-system.md, DS15): text at least 4.5:1 on the
// surfaces it sits on, a control's boundary at least 3:1. A token change that breaks a pair fails here.

/** The tokens with a colour of their own (#rrggbb), and the ones that name another token (var(--x)), resolved. */
function readTokens(source: string): Map<string, string> {
  const raw = new Map<string, string>();
  const code = source.replace(/\/\*[\s\S]*?\*\//g, '');
  for (const [, name, value] of code.matchAll(/(--[a-z0-9-]+):\s*([^;]+);/g)) raw.set(name, value.trim());
  const resolve = (value: string, depth = 0): string => {
    const ref = /^var\((--[a-z0-9-]+)\)$/.exec(value);
    return ref && depth < 8 ? resolve(raw.get(ref[1]) ?? '', depth + 1) : value;
  };
  const tokens = new Map<string, string>();
  for (const [name, value] of raw) {
    const resolved = resolve(value);
    if (/^#[0-9a-f]{6}$/i.test(resolved)) tokens.set(name, resolved);
  }
  return tokens;
}

/** WCAG relative luminance of "#rrggbb". */
function luminance(hex: string) {
  const n = parseInt(hex.slice(1), 16);
  const channel = (c: number) => {
    const v = c / 255;
    return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * channel((n >> 16) & 255) + 0.7152 * channel((n >> 8) & 255) + 0.0722 * channel(n & 255);
}

function contrast(a: string, b: string) {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

const tokens = readTokens(css);
const colour = (name: string) => {
  const value = tokens.get(name);
  if (!value) throw new Error(`${name} isn't a colour token`);
  return value;
};

const SURFACES = ['--bg', '--bg-raised', '--bg-panel'];
const TEXT = 4.5;
const BOUNDARY = 3;

/** [foreground, background, the least ratio]. */
const PAIRS: Array<[string, string, number]> = [
  ...['--text', '--text-dim', '--accent', '--accent-ink', '--ok', '--warn', '--danger', '--mod-ink', '--signal-key'].flatMap(
    (ink) => SURFACES.map((surface): [string, string, number] => [ink, surface, TEXT]),
  ),
  ...SURFACES.map((surface): [string, string, number] => ['--text-faint', surface, TEXT]),
  // Words on a filled colour: switches that are on, the play button, chips, badges.
  ...['--accent', '--ok', '--warn', '--danger', '--mute', '--solo', '--arm', '--monitor', '--automation-touch', '--mod', '--signal-key'].map(
    (fill): [string, string, number] => ['--on-accent', fill, TEXT],
  ),
  ['--danger-ink', '--danger-surface', TEXT],
  // A control's edge (DS15): fields, selects, switches, segments.
  ['--control-border', '--bg-raised', BOUNDARY],
  ['--control-border', '--bg-panel', BOUNDARY],
];

describe('design tokens', () => {
  it.each(PAIRS)('%s on %s is at least %s:1', (fg, bg, least) => {
    expect(contrast(colour(fg), colour(bg))).toBeGreaterThanOrEqual(least);
  });
});
