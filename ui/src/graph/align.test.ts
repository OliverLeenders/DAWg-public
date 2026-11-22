import { describe, expect, it } from 'vitest';
import { align, type AlignNode } from './align';

const node = (id: number, x: number, y: number, height = 60): AlignNode => ({
  id,
  x,
  y,
  width: 140,
  height,
  input: { x: 0, y: 43 },
  output: { x: 140, y: 43 },
});

describe('align and distribute', () => {
  it('lines up the first inputs with the node furthest upstream', () => {
    const moved = align([node(1, 100, 57), node(2, 300, 90), node(3, 500, 12)], 'inputs', false, false);
    expect(moved.get(2)).toEqual({ x: 300, y: 57 });
    expect(moved.get(3)).toEqual({ x: 500, y: 57 });
    expect(moved.has(1)).toBe(false);
  });

  it('ends on the grid with snapping on', () => {
    const moved = align([node(1, 100, 57), node(2, 300, 90)], 'inputs', false, true);
    // The lead's port at 100 is on a grid line; 57 + 43 = 100 too, so only x needs nothing.
    expect(moved.get(2)).toEqual({ x: 300, y: 57 });
    expect(moved.get(1)).toBeUndefined();
  });

  it('aligns along the flow, top to bottom', () => {
    const moved = align([node(1, 40, 100), node(2, 300, 160)], 'flow', true, false);
    expect(moved.get(2)).toEqual({ x: 300, y: 100 });
  });

  it('distributes across the flow with even gaps, keeping the outermost', () => {
    const moved = align([node(1, 0, 0), node(2, 0, 70, 40), node(3, 0, 300)], 'distribute', false, false);
    // 360 tall in all, 160 of nodes: two gaps of 100.
    expect(moved.get(2)).toEqual({ x: 0, y: 160 });
    expect(moved.has(1) || moved.has(3)).toBe(false);
    expect(align([node(1, 0, 0), node(2, 0, 90)], 'distribute', false, false).size).toBe(0);
  });
});
