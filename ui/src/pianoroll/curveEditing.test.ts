import { describe, expect, it } from 'vitest';
import type { CurvePoint } from '../bridge/protocol';
import {
  addPoint,
  bendSegmentThrough,
  straightenSegment,
  bendSegmentToPointer,
  boxSelect,
  deletePoints,
  handlePosition,
  handleTimeBounds,
  handleTimeFor,
  hitCurve,
  movePoints,
  pastePoints,
  pointsInRect,
  shiftPoints,
  type CurveMapping,
} from './curveEditing';
import { evaluateCurve, freeControlPoints } from './geometry';

/** A free segment's value at x by the handle rule, found by sampling the Bezier finely rather than by bisection. */
function reference(x: number, hx: number, hy: number) {
  const start = Math.max(hx, hy);
  const end = Math.max(1 - hx, 1 - hy);
  const [c1x, c1y, c2x, c2y] = [hx * start, hy * start, 1 + (hx - 1) * end, 1 + (hy - 1) * end];
  const at = (u: number, a: number, b: number) => 3 * (1 - u) ** 2 * u * a + 3 * (1 - u) * u * u * b + u ** 3;
  let best = 0;
  for (let i = 0; i <= 200000; i++) {
    const u = i / 200000;
    if (Math.abs(at(u, c1x, c2x) - x) < Math.abs(at(best, c1x, c2x) - x)) best = u;
  }
  return at(best, c1y, c2y);
}

// 1 px per tick horizontally, values 0..1 over 100 px (y grows downwards); the note is 1000 ticks long.
const map: CurveMapping = {
  toX: (t) => t,
  toY: (v) => 100 - v * 100,
  fromX: (x) => x,
  fromY: (y) => (100 - y) / 100,
  min: 0,
  max: 1,
  maxTime: 1000,
  neutral: 0,
};

// Pitch-like mapping: 10 px per semitone, semitone and 100-tick grid snapping.
const pitchMap: CurveMapping = {
  ...map,
  min: -48,
  max: 48,
  fromY: (y) => (100 - y) / 10,
  toY: (v) => 100 - v * 10,
  snapValue: Math.round,
  snapTime: (t) => Math.round(t / 100) * 100,
};

const line: CurvePoint[] = [
  [0, 0, 0, 0],
  [100, 1, 0, 0],
];

describe('evaluateCurve', () => {
  it('matches the engine rules (hold outside, linear, hold and power segments)', () => {
    expect(evaluateCurve([], 5, 0.5)).toBe(0.5);
    expect(evaluateCurve(line, -10, 0)).toBe(0);
    expect(evaluateCurve(line, 50, 0)).toBeCloseTo(0.5);
    expect(evaluateCurve(line, 500, 0)).toBe(1);
    expect(evaluateCurve([[0, 0, 1, 0], [100, 1, 0, 0]], 99, 0)).toBe(0);
    expect(evaluateCurve([[0, 0, 2, 1], [100, 1, 0, 0]], 50, 0)).toBeCloseTo(Math.pow(0.5, 8));
  });

  it('evaluates a free segment with its handles pointing at the tension point', () => {
    const free: CurvePoint[] = [[0, 0, 3, 0, 0.25, 0.75], [100, 1, 0, 0]];
    expect(evaluateCurve(free, 0, 0)).toBe(0);
    expect(evaluateCurve(free, 25, 0)).toBeCloseTo(reference(0.25, 0.25, 0.75), 4);
    expect(evaluateCurve(free, 100, 0)).toBe(1);
  });

  it('scales each handle by how far the tension point lies towards the opposite side', () => {
    // From the start node through (0.25, 0.75) the line meets the top at y = 1: 75 % of the way, so the start handle
    // ends at 75 % of the tension point. From the end node, the line meets the left at x = 0: 75 % of the way too.
    const { c1, c2 } = freeControlPoints(0.25, 0.75);
    expect(c1[0]).toBeCloseTo(0.1875);
    expect(c1[1]).toBeCloseTo(0.5625);
    expect(c2[0]).toBeCloseTo(1 - 0.75 * 0.75);
    expect(c2[1]).toBeCloseTo(1 - 0.75 * 0.25);
    // The middle is a straight line; a corner puts both handles on the tension point.
    const middle: CurvePoint[] = [[0, 0, 3, 0, 0.5, 0.5], [100, 1, 0, 0]];
    for (const t of [10, 33, 50, 80]) expect(evaluateCurve(middle, t, 0)).toBeCloseTo(t / 100, 6);
    const corner = freeControlPoints(1, 0);
    expect(corner.c1).toEqual([1, 0]);
    expect(corner.c2).toEqual([1, 0]);
  });
});

describe('adding points', () => {
  it('adds points in time order', () => {
    const { points, index } = addPoint(line, 50, 20, map);
    expect(points.map((p) => p[0])).toEqual([0, 50, 100]);
    expect(index).toBe(1);
    expect(points[1][1]).toBeCloseTo(0.8);
  });

  it('anchors the note start when the first point is added', () => {
    const { points, index } = addPoint([], 50, 20, map);
    expect(points).toEqual([
      [0, 0, 0, 0],
      [50, 0.8, 0, 0],
    ]);
    expect(index).toBe(1);
    expect(addPoint([], 0, 20, map).points).toHaveLength(1);
  });

  it('snaps time to the grid and pitch to semitones unless free', () => {
    const snapped = addPoint([], 260, 76, pitchMap).points[1];
    expect(snapped[0]).toBe(300);
    expect(snapped[1]).toBe(2); // 2.4 st -> 2
    const free = addPoint([], 260, 76, pitchMap, true).points[1];
    expect(free[0]).toBe(260);
    expect(free[1]).toBeCloseTo(2.4);
  });

  it('never places points outside the note', () => {
    expect(addPoint([], 5000, 50, map).points[1][0]).toBe(1000);
    expect(addPoint([[100, 0.5, 0, 0]], -40, 50, map).points[0][0]).toBe(0);
  });
});

describe('moving points', () => {
  const three: CurvePoint[] = [
    [0, 0, 0, 0],
    [400, 2, 0, 0],
    [600, 4, 0, 0],
  ];

  it('moves the selected group by the grabbed point, snapped', () => {
    const { points, indices } = movePoints(three, [1, 2], 1, 530, 100 - 3.4 * 10, pitchMap);
    expect(points).toEqual([
      [0, 0, 0, 0],
      [500, 3, 0, 0],
      [700, 5, 0, 0],
    ]);
    expect(indices).toEqual([1, 2]);
  });

  it('keeps the whole group inside the note', () => {
    const { points } = movePoints(three, [1, 2], 2, 5000, 100 - 4 * 10, pitchMap);
    expect(points.map((p) => p[0])).toEqual([0, 800, 1000]);
  });

  it('shifts a group by a delta (arrow keys), kept inside the note and the range', () => {
    const up = shiftPoints(three, [1, 2], 0, 1, pitchMap);
    expect(up.points.map((p) => p[1])).toEqual([0, 3, 5]);
    expect(up.indices).toEqual([1, 2]);
    expect(shiftPoints(three, [1, 2], 0, 100, pitchMap).points.map((p) => p[1])).toEqual([0, 46, 48]);
    expect(shiftPoints(three, [2], 5000, 0, pitchMap).points.map((p) => p[0])).toEqual([0, 400, 1000]);
  });

  it('lets a shifted point replace an unmoved one at the same time', () => {
    const { points, indices } = shiftPoints(three, [1], 200, 0, pitchMap);
    expect(points).toEqual([
      [0, 0, 0, 0],
      [600, 2, 0, 0],
    ]);
    expect(indices).toEqual([1]);
  });
});

describe('box selection', () => {
  const none = { notes: new Set<number>(), points: new Map<number, number[]>() };

  it('selects a flat note the box touches', () => {
    const chosen = boxSelect([{ id: 1, pointCount: 0, inside: [], touched: true }, { id: 2, pointCount: 0, inside: [], touched: false }], none);
    expect([...chosen.notes]).toEqual([1]);
    expect(chosen.points.size).toBe(0);
  });

  it('selects the points of a bent note until all of them are inside', () => {
    const partial = boxSelect([{ id: 1, pointCount: 3, inside: [2, 0], touched: true }], none);
    expect(partial.notes.size).toBe(0);
    expect(partial.points.get(1)).toEqual([0, 2]);

    const whole = boxSelect([{ id: 1, pointCount: 3, inside: [0, 1, 2], touched: true }], none);
    expect([...whole.notes]).toEqual([1]);
    expect(whole.points.has(1)).toBe(false);
  });

  it('ignores a bent note whose points are all outside, even if the box touches it', () => {
    const chosen = boxSelect([{ id: 1, pointCount: 2, inside: [], touched: true }], none);
    expect(chosen.notes.size).toBe(0);
    expect(chosen.points.size).toBe(0);
  });

  it('adds to the base selection, completing a note from its selected points', () => {
    const base = { notes: new Set([5]), points: new Map([[1, [0]]]) };
    const chosen = boxSelect(
      [
        { id: 1, pointCount: 2, inside: [1], touched: true },
        { id: 5, pointCount: 2, inside: [], touched: false },
      ],
      base,
    );
    expect([...chosen.notes].sort()).toEqual([1, 5]);
    expect(chosen.points.size).toBe(0);
  });
});

describe('selection, deletion, paste and bending', () => {
  it('finds points before handles', () => {
    expect(hitCurve(line, 1, 99, map)).toEqual({ kind: 'point', index: 0 });
    expect(hitCurve(line, 50, 50, map)).toEqual({ kind: 'handle', segment: 0 });
    expect(hitCurve(line, 50, 10, map)).toBeNull();
  });

  it('selects points inside a rectangle', () => {
    expect(pointsInRect(line, map, 50, 0, 150, 50)).toEqual([1]);
  });

  it('deletes points', () => {
    expect(deletePoints(line, [0])).toEqual([[100, 1, 0, 0]]);
  });

  it('pastes points at a position, replacing that span and dropping what falls off the note', () => {
    const copied: CurvePoint[] = [
      [0, 0.5, 0, 0],
      [200, 1, 0, 0],
    ];
    const target: CurvePoint[] = [
      [0, 0, 0, 0],
      [350, 0.2, 0, 0],
      [900, 0, 0, 0],
    ];
    expect(pastePoints(target, copied, 300, 1000, 0)).toEqual([
      [0, 0, 0, 0],
      [300, 0.5, 0, 0],
      [500, 1, 0, 0],
      [900, 0, 0, 0],
    ]);
    expect(pastePoints([], copied, 900, 1000, 0)).toEqual([
      [0, 0, 0, 0],
      [900, 0.5, 0, 0],
    ]);
  });

  it('draws the handle of an unbent segment on the curve at its middle', () => {
    expect(handlePosition([[0, 0, 0, 0], [100, 1, 0, 0]], 0, map)).toEqual({ x: 50, y: 50 });
    // A power segment from an older project: still mid-segment, on its eased curve.
    const eased = handlePosition([[0, 0, 2, 1], [100, 1, 0, 0]], 0, map);
    expect(eased.x).toBe(50);
    expect(eased.y).toBeCloseTo(100 - 100 * Math.pow(0.5, 8));
  });

  it('draws a free control point from its value range, not its time', () => {
    const points: CurvePoint[] = [
      [200, 0.25, 3, 0, 0.5, 0.75],
      [400, 0.75, 0, 0],
    ];
    const position = handlePosition(points, 0, map);
    expect(position.x).toBe(300);
    expect(position.y).toBe(37.5);
  });

  it('stores the tension point', () => {
    const through = bendSegmentThrough(line, 0, 50, 0.25);
    expect(through[0][2]).toBe(3);
    expect(through[0][4]).toBeCloseTo(0.5);
    expect(through[0][5]).toBeCloseTo(0.25, 6);
    expect(evaluateCurve(through, 50, 0)).toBeCloseTo(reference(0.5, 0.5, 0.25), 4);
  });

  it('bends a falling segment the same way', () => {
    const falling: CurvePoint[] = [
      [0, 1, 0, 0],
      [100, 0, 0, 0],
    ];
    const through = bendSegmentThrough(falling, 0, 50, 0.25);
    expect(evaluateCurve(through, 50, 0)).toBeCloseTo(reference(0.5, 0.5, 0.25), 4);
  });

  it('stores the handle position when it moves horizontally at a fixed value', () => {
    const left = bendSegmentThrough(line, 0, 25, 0.5);
    const right = bendSegmentThrough(line, 0, 75, 0.5);
    expect(left[0][4]).toBeCloseTo(0.25);
    expect(right[0][4]).toBeCloseTo(0.75);
    expect(evaluateCurve(left, 25, 0)).toBeCloseTo(reference(0.25, 0.25, 0.5), 4);
    expect(evaluateCurve(right, 75, 0)).toBeCloseTo(reference(0.75, 0.75, 0.5), 4);
  });

  it('reaches points near the segment corners', () => {
    const bottomRight = bendSegmentThrough(line, 0, 99, 0.01);
    expect(bottomRight[0][4]).toBeCloseTo(0.99);
    expect(bottomRight[0][5]).toBeCloseTo(0.01, 6);
    expect(evaluateCurve(bottomRight, 99, 0)).toBeCloseTo(reference(0.99, 0.99, 0.01), 4);

    const topLeft = bendSegmentThrough(line, 0, 1, 0.99);
    expect(topLeft[0][4]).toBeCloseTo(0.01);
    expect(topLeft[0][5]).toBeCloseTo(0.99, 6);
    expect(evaluateCurve(topLeft, 1, 0)).toBeCloseTo(reference(0.01, 0.01, 0.99), 4);
  });
});

describe('free tension handles', () => {
  it('keeps the handle inside its segment and defaults to the midpoint', () => {
    expect(handleTimeBounds(line, 0)).toEqual({ low: 0, high: 100 });
    expect(handleTimeFor(line, 0)).toBe(50);
  });

  it('keeps the handle at the same fraction when a node moves', () => {
    const bent = bendSegmentThrough(line, 0, 25, 0.5);
    expect(handleTimeFor(bent, 0)).toBe(25);

    const moved: CurvePoint[] = [bent[0], [200, 1, 0, 0]];
    expect(handleTimeFor(moved, 0)).toBe(50);
  });

  it('hit-tests a bent segment at its stored control point', () => {
    const bent = bendSegmentThrough(line, 0, 25, 0.75);
    const handle = handlePosition(bent, 0, map);
    expect(handle.x).toBe(25);
    expect(handle.y).toBeCloseTo(25);
    expect(hitCurve(bent, handle.x, handle.y, map)).toEqual({ kind: 'handle', segment: 0 });
  });

  it('bends through the pointer while preserving the grab offset', () => {
    const centred = bendSegmentToPointer(line, 0, map, 25, 25, { x: 0, y: 0 });
    expect(centred.time).toBe(25);
    expect(evaluateCurve(centred.points, 25, 0)).toBeCloseTo(reference(0.25, 0.25, 0.75), 4);

    const offset = bendSegmentToPointer(line, 0, map, 55, 55, { x: 5, y: 5 });
    expect(offset.time).toBe(50);
    expect(evaluateCurve(offset.points, 50, 0)).toBeCloseTo(0.5, 6);
  });

  it('reaches the top at the middle while keeping the start smooth', () => {
    const centred = bendSegmentToPointer(line, 0, map, 50, 0, { x: 0, y: 0 });
    expect(centred.points[0][2]).toBe(3);
    expect(centred.points[0][4]).toBeCloseTo(0.5);
    expect(centred.points[0][5]).toBeCloseTo(1);
    expect(evaluateCurve(centred.points, 50, 0)).toBeCloseTo(reference(0.5, 0.5, 1), 4);
    // A smooth S-curve: it has only risen a little 10% of the way along, rather than snapping up at the node.
    expect(evaluateCurve(centred.points, 10, 0)).toBeLessThan(0.5);
  });

  it('allows a control point near a corner without forcing the curve through it', () => {
    const nearLeft = bendSegmentToPointer(line, 0, map, 1, 0, { x: 0, y: 0 });
    expect(nearLeft.points[0][4]).toBeCloseTo(0.01, 6);
    expect(evaluateCurve(nearLeft.points, 1, 0)).toBeCloseTo(reference(0.01, 0.01, 1), 4);
    expect(evaluateCurve(nearLeft.points, 10, 0)).toBeLessThan(0.9);
  });
});

describe('the anchor at the note start', () => {
  const anchored: CurveMapping = { ...map, anchorStart: true };
  const curve: CurvePoint[] = [
    [0, 0.2, 0, 0],
    [500, 0.5, 0, 0],
  ];

  it('moves only vertically', () => {
    const moved = movePoints(curve, [0], 0, 300, 50, anchored);
    expect(moved.points[0]).toEqual([0, 0.5, 0, 0]);
  });

  it('keeps the other points after it, up to the note end', () => {
    expect(movePoints(curve, [1], 1, -5000, 50, anchored).points.map((p) => p[0])).toEqual([0, 1]);
    expect(movePoints(curve, [1], 1, 5000, 50, anchored).points.map((p) => p[0])).toEqual([0, 1000]);
  });

  it('cannot be deleted, while a point at the note end can', () => {
    const withEnd: CurvePoint[] = [...curve, [1000, 0.8, 0, 0]];
    expect(deletePoints(withEnd, [0, 1, 2], true)).toEqual([curve[0]]);
    expect(deletePoints(withEnd, [0, 1, 2])).toEqual([]);
  });
});

describe('straightenSegment', () => {
  it('puts the tension point back in the middle, a straight line', () => {
    const bent: CurvePoint[] = [[0, 0, 3, 0, 0.1, 0.9], [100, 1, 0, 0]];
    const straight = straightenSegment(bent, 0);
    expect(straight[0]).toEqual([0, 0, 3, 0, 0.5, 0.5]);
    for (const t of [10, 40, 75]) expect(evaluateCurve(straight, t, 0)).toBeCloseTo(t / 100, 6);
    expect(straightenSegment(bent, 1)).toBe(bent);
  });
});
