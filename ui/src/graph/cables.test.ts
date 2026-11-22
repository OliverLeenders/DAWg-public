import { describe, expect, it } from 'vitest';
import { acrossFlow, detourAt, facing, movable, moveStretch, removeBend, routeOf, splitAt, stepAt, translate } from './cables';
import type { End } from './grid';

const from: End = { at: { x: 0, y: 40 }, side: 'right' };
const to: End = { at: { x: 300, y: 120 }, side: 'left' };

describe('cable bends', () => {
  it('makes a step where an unbent cable is grabbed', () => {
    expect(stepAt(from, to, { x: 140, y: 77 })).toEqual([{ x: 140, y: 40 }, { x: 140, y: 120 }]);
    const down: [End, End] = [{ at: { x: 40, y: 0 }, side: 'bottom' }, { at: { x: 120, y: 300 }, side: 'top' }];
    expect(stepAt(...down, { x: 77, y: 140 })).toEqual([{ x: 40, y: 140 }, { x: 120, y: 140 }]);
  });

  it('pulls an unbent cable aside across the flow', () => {
    expect(acrossFlow(from, { x: 100, y: 50 }, { x: 103, y: 90 })).toBe(true);
    expect(acrossFlow(from, { x: 100, y: 50 }, { x: 130, y: 52 })).toBe(false);
    const bends = detourAt(from, to, { x: 140, y: 100 }, 40);
    expect(bends).toEqual([{ x: 120, y: 40 }, { x: 120, y: 100 }, { x: 160, y: 100 }, { x: 160, y: 120 }]);
    expect(moveStretch(bends, routeOf(bends, from, to), 2, 180).slice(1, 3)).toEqual([{ x: 120, y: 180 }, { x: 160, y: 180 }]);
  });

  it('moves a stretch sideways, not the ones at the ports', () => {
    const bends = stepAt(from, to, { x: 140, y: 0 });
    const route = routeOf(bends, from, to);
    expect(movable(route)).toEqual([1]);
    expect(moveStretch(bends, route, 1, 200)).toEqual([{ x: 200, y: 40 }, { x: 200, y: 120 }]);
  });

  it('adds a pair of bends where a stretch is double-clicked, and takes them out again', () => {
    const bends = stepAt(from, to, { x: 140, y: 0 });
    const route = routeOf(bends, from, to);
    // On the last stretch, from (140, 120) to the input at (300, 120).
    const split = splitAt(bends, route, 2, { x: 220, y: 131 });
    expect(split).toEqual([{ x: 140, y: 40 }, { x: 140, y: 120 }, { x: 220, y: 120 }, { x: 220, y: 120 }]);
    // Dragging the half before the new pair makes a step there; the one at the input stays put.
    const stepped = moveStretch(split, routeOf(split, from, to), 2, 160);
    expect(stepped).toEqual([{ x: 140, y: 40 }, { x: 140, y: 160 }, { x: 220, y: 160 }, { x: 220, y: 120 }]);
    expect(removeBend(split, 2)).toEqual(bends);
    expect(removeBend(bends, 0)).toEqual([]);
  });

  it('moves bends with their nodes and turns them with the flow', () => {
    expect(translate([{ x: 1, y: 2 }], 10, 20)).toEqual([{ x: 11, y: 22 }]);
    expect(facing({ x: 1, y: 2 }, true)).toEqual({ x: 2, y: 1 });
    expect(facing({ x: 1, y: 2 }, false)).toEqual({ x: 1, y: 2 });
  });
});
