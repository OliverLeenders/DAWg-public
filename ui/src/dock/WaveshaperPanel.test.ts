import { describe, expect, it } from 'vitest';
import { NODE_TYPES, TICKS_PER_BEAT, type CurvePoint } from '../bridge/protocol';
import { evaluateCurve } from '../pianoroll/geometry';
import { deviceWidth } from './deviceWidths';
import { FACTORY_PRESETS } from './presets';
import {
  ASYMMETRIC_PRESETS,
  drawnDot,
  drawnPoints,
  inputAt,
  inputRange,
  isSymmetric,
  mirrored,
  pinOrigin,
  plotSize,
  positiveHalf,
  symmetricReach,
  SYMMETRIC_PRESETS,
  timeAt,
} from './waveshaper';

describe('the waveshaper panel', () => {
  it('makes the plot the largest square its box holds', () => {
    expect(plotSize(300, 120)).toBe(120);
    expect(plotSize(90.6, 400)).toBe(90);
    expect(plotSize(0, 50)).toBe(0);
    expect(plotSize(-4, 50)).toBe(0);
  });

  it('marks the range of the shaper input a full-scale signal reaches', () => {
    expect(inputRange(0, 0)).toEqual([-1, 1]);
    const [lo, hi] = inputRange(-6, 0);
    expect(lo).toBeCloseTo(-0.501, 3);
    expect(hi).toBeCloseTo(0.501, 3);
    const [biasedLo, biasedHi] = inputRange(-12, 50);
    expect(biasedLo).toBeCloseTo(0.5 - 0.251, 3);
    expect(biasedHi).toBeCloseTo(0.5 + 0.251, 3);
    // Clipped to the drawn curve's -1..1.
    expect(inputRange(24, -100)).toEqual([-1, 1]);
    expect(inputRange(-24, 100)[1]).toBe(1);
  });

  it('maps the drawn curve times to the shaper input and back', () => {
    expect(inputAt(0)).toBe(-1);
    expect(inputAt(TICKS_PER_BEAT / 2)).toBe(0);
    expect(inputAt(TICKS_PER_BEAT)).toBe(1);
    for (const u of [-1, -0.25, 0.3, 1]) expect(inputAt(timeAt(u))).toBeCloseTo(u, 12);
  });

  it('draws the identity for a node without a drawn curve', () => {
    const points = drawnPoints({ modulationShape: [] } as never);
    for (const u of [-1, -0.5, 0, 0.7, 1]) expect(evaluateCurve(points, timeAt(u), 0)).toBeCloseTo(u, 9);
  });

  it('has drawn curve presets that span the whole input and stay within -1..1', () => {
    for (const [name, preset] of Object.entries(ASYMMETRIC_PRESETS)) {
      expect(preset[0][0], name).toBe(0);
      expect(preset[preset.length - 1][0], name).toBe(TICKS_PER_BEAT);
      for (let i = 1; i < preset.length; i++) expect(preset[i][0], name).toBeGreaterThan(preset[i - 1][0]);
      for (let t = 0; t <= TICKS_PER_BEAT; t += TICKS_PER_BEAT / 64) {
        const v = evaluateCurve(preset, t, 0);
        expect(Math.abs(v), name).toBeLessThanOrEqual(1 + 1e-9);
      }
    }
    for (const [name, preset] of Object.entries(SYMMETRIC_PRESETS)) {
      expect(preset[0].slice(0, 2), name).toEqual([0, 0]);
      expect(preset[preset.length - 1][0], name).toBe(TICKS_PER_BEAT);
    }
    // The fold rises twice as fast as the identity and folds back at +-0.5.
    expect(evaluateCurve(ASYMMETRIC_PRESETS.Fold, timeAt(0.25), 0)).toBeCloseTo(0.5, 9);
    expect(evaluateCurve(ASYMMETRIC_PRESETS.Fold, timeAt(0.75), 0)).toBeCloseTo(0.5, 9);
    expect(evaluateCurve(ASYMMETRIC_PRESETS.Fold, timeAt(-0.25), 0)).toBeCloseTo(-0.5, 9);
    // The steps hold, with a step of 0 around the origin.
    expect(evaluateCurve(ASYMMETRIC_PRESETS.Steps, timeAt(0.1), 0)).toBeCloseTo(0, 9);
    expect(evaluateCurve(ASYMMETRIC_PRESETS.Steps, timeAt(0.3), 0)).toBeCloseTo(1 / 3, 9);
    expect(evaluateCurve(ASYMMETRIC_PRESETS.Steps, timeAt(-0.3), 0)).toBeCloseTo(-1 / 3, 9);
  });

  // A positive half's value at u (0..1), as the symmetric editor draws it.
  const halfAt = (half: CurvePoint[], u: number) => evaluateCurve(half, u * TICKS_PER_BEAT, 0);

  it('mirrors a symmetric curve into the whole curve that plays the same, with a point at the origin', () => {
    const halves: Record<string, CurvePoint[]> = {
      ...SYMMETRIC_PRESETS,
      'hold after a slope': [
        [0, 0, 0, 0],
        [TICKS_PER_BEAT / 2, 0.5, 1, 0],
        [TICKS_PER_BEAT, 1, 0, 0],
      ],
      'bent and held': [
        [0, 0, 3, 0, 0.9, 0.3],
        [TICKS_PER_BEAT / 4, 0.6, 1, 0],
        [TICKS_PER_BEAT / 2, 0.2, 3, 0, 0.2, 0.7],
        [TICKS_PER_BEAT, 0.9, 0, 0],
      ],
    };
    for (const [name, half] of Object.entries(halves)) {
      const full = mirrored(half);
      expect(full.some(([time, value]) => time === TICKS_PER_BEAT / 2 && value === 0), name).toBe(true);
      for (let i = 1; i < full.length; i++) expect(full[i][0], name).toBeGreaterThan(full[i - 1][0]);
      // Off the points (and a tick away from them), the whole curve is the half and its image through the origin.
      for (let k = 1; k < 200; k++) {
        const u = k / 200 + 0.0013;
        if (u >= 1) continue;
        expect(evaluateCurve(full, timeAt(u), 0), `${name} at ${u}`).toBeCloseTo(halfAt(half, u), 3);
        expect(evaluateCurve(full, timeAt(-u), 0), `${name} at -${u}`).toBeCloseTo(-halfAt(half, u), 3);
      }
      // Beyond the ends too.
      expect(evaluateCurve(full, -10, 0), name).toBeCloseTo(-halfAt(half, 1), 9);
      expect(evaluateCurve(full, TICKS_PER_BEAT + 10, 0), name).toBeCloseTo(halfAt(half, 1), 9);
      // And the editor shows the half again.
      const back = positiveHalf(full);
      for (let k = 0; k <= 50; k++) expect(halfAt(back, k / 50 + 0.0013 * Number(k < 50)), name).toBeCloseTo(halfAt(half, k / 50 + 0.0013 * Number(k < 50)), 3);
    }
  });

  it('pins a symmetric curve to the origin when it switches or is edited', () => {
    // An asymmetric curve's positive half starts at (0, 0) whatever the curve does at 0.
    const full: CurvePoint[] = [
      [0, -0.3, 0, 0],
      [TICKS_PER_BEAT / 4, 0.4, 0, 0],
      [(TICKS_PER_BEAT * 3) / 4, 0.8, 0, 0],
      [TICKS_PER_BEAT, 0.5, 0, 0],
    ];
    const half = positiveHalf(full);
    expect(half[0].slice(0, 2)).toEqual([0, 0]);
    expect(half.map(([time]) => time)).toEqual([0, TICKS_PER_BEAT / 2, TICKS_PER_BEAT]);
    expect(pinOrigin([[0, 0.4, 0, 0], [TICKS_PER_BEAT, 1, 0, 0]])[0].slice(0, 2)).toEqual([0, 0]);
    expect(pinOrigin([[100, 0.4, 0, 0]])).toHaveLength(2);
  });

  it('reads the symmetric mode as on unless switched off, and marks the reach of the input', () => {
    expect(isSymmetric({})).toBe(true);
    expect(isSymmetric({ symmetric: 0 })).toBe(false);
    expect(symmetricReach(-6, 0)).toBeCloseTo(0.501, 3);
    expect(symmetricReach(-12, -50)).toBeCloseTo(0.751, 3);
    expect(symmetricReach(6, 0)).toBe(1);
  });

  it('keeps the plot at least 120 px wide at the card minimum', () => {
    // The card's padding, the gap to the knob column, and the knob column: three knobs of 58 px with 4 px gaps.
    const { min } = deviceWidth(NODE_TYPES.waveshaper, 0);
    const knobs = 3 * 58 + 2 * 4;
    expect(min - 24 - 16 - knobs).toBeGreaterThanOrEqual(120);
  });

  it('has factory presets, one with a drawn curve', () => {
    const presets = FACTORY_PRESETS[NODE_TYPES.waveshaper];
    expect(presets.length).toBeGreaterThanOrEqual(8);
    const drawn = presets.filter((p) => p.shape);
    expect(drawn).toHaveLength(2);
    for (const preset of drawn) expect(preset.params.shape).toBe(6);
  });
});

describe('the waveshaper level dot on the drawn curve', () => {
  it('sits at the shaper input on the curve', () => {
    expect(drawnDot({ input: 0.5, shaperInput: 0.7, shaped: 0.3 }, false, 0.001)).toEqual({ x: 0.7, y: 0.3 });
    expect(drawnDot({ input: 0.5, shaperInput: -0.4, shaped: -0.2 }, false, 0.001)).toEqual({ x: -0.4, y: -0.2 });
  });

  it('mirrors a negative input into the symmetric plot, and clamps to it', () => {
    expect(drawnDot({ input: 0.5, shaperInput: -0.4, shaped: -0.2 }, true, 0.001)).toEqual({ x: 0.4, y: 0.2 });
    expect(drawnDot({ input: 1, shaperInput: 2.5, shaped: 0.9 }, true, 0.001)).toEqual({ x: 1, y: 0.9 });
  });

  it('hides for a quiet input', () => {
    expect(drawnDot({ input: 0.0001, shaperInput: 0.1, shaped: 0.1 }, false, 0.001)).toBeNull();
    expect(drawnDot(null, false, 0.001)).toBeNull();
  });
});
