import { describe, expect, it } from 'vitest';
import { dotOffset, firstPortOf, fitBends, GRID, snapLayout, snapNode } from './grid';
import type { MeasuredNode } from './layout';

describe('the graph grid', () => {
  it('snaps a node by its first port', () => {
    // A port 9 px right and 47 px down of the node's corner: the node goes where that port lands on a grid point.
    expect(snapNode({ x: 103, y: 71 }, { x: 9, y: 47 })).toEqual({ x: 111, y: 73 });
    expect(snapNode({ x: -31, y: -8 }, { x: 0, y: 30 })).toEqual({ x: -40, y: -10 });
  });

  it('snaps by the first input, or the first output of a node without inputs', () => {
    const ports = [
      { handle: 'out-0', x: 180, y: 40 },
      { handle: 'in-1', x: 0, y: 60 },
      { handle: 'in-0', x: 0, y: 40 },
    ];
    expect(firstPortOf(ports)).toEqual({ x: 0, y: 40 });
    expect(firstPortOf(ports.filter((p) => p.handle.startsWith('out')))).toEqual({ x: 180, y: 40 });
    expect(firstPortOf([])).toBeNull();
  });

  it('puts the dots on the grid points', () => {
    // React Flow draws a dot at k * gap - offset - gap / 2 + size / 2.
    for (const [gap, size] of [[GRID, 1], [GRID * 5, 1.5]]) expect((-dotOffset(gap, size) - gap / 2 + size / 2) % gap).toBe(0);
  });

  it('keeps the stretches out of a port straight when its node moves', () => {
    const bends = [{ x: 200, y: 40 }, { x: 200, y: 120 }];
    // Left to right: the first stretch runs along the port's row, the last along the other port's.
    expect(fitBends(bends, { at: { x: 100, y: 60 }, side: 'right' }, { at: { x: 300, y: 140 }, side: 'left' })).toEqual([
      { x: 200, y: 60 },
      { x: 200, y: 140 },
    ]);
    // Top to bottom: along the ports' columns.
    expect(fitBends([{ x: 40, y: 200 }, { x: 120, y: 200 }], { at: { x: 60, y: 100 }, side: 'bottom' }, { at: { x: 140, y: 300 }, side: 'top' })).toEqual([
      { x: 60, y: 200 },
      { x: 140, y: 200 },
    ]);
    expect(fitBends([], { at: { x: 0, y: 0 }, side: 'right' }, { at: { x: 9, y: 9 }, side: 'left' })).toEqual([]);
  });

  const node = (id: number, vertical: boolean): MeasuredNode => ({
    id,
    width: 150,
    height: 70,
    ports: vertical
      ? [{ handle: 'in-0', side: 'top', x: 75, y: 0 }, { handle: 'out-0', side: 'bottom', x: 75, y: 70 }]
      : [{ handle: 'in-0', side: 'left', x: 0, y: 43 }, { handle: 'out-0', side: 'right', x: 150, y: 43 }],
  });

  for (const vertical of [false, true]) {
    it(`snaps Layout's result, ${vertical ? 'top to bottom' : 'left to right'}`, () => {
      const measured = [node(1, vertical), node(2, vertical)];
      const cable = { fromNode: 1, fromPort: 0, toNode: 2, toPort: 0 };
      const result = {
        positions: new Map([[1, { x: 83, y: 81 }], [2, { x: 307, y: 155 }]]),
        routes: new Map([['1:0>2:0', vertical ? [{ x: 158, y: 190 }, { x: 382, y: 190 }] : [{ x: 270, y: 124 }, { x: 270, y: 198 }]]]),
      };
      const snapped = snapLayout(result, measured, [cable], true);
      const port = (id: number, handle: string) => {
        const at = snapped.positions.get(id)!;
        const p = measured[id - 1].ports.find((q) => q.handle === handle)!;
        return { x: at.x + p.x, y: at.y + p.y };
      };
      // Each input on a grid point.
      for (const id of [1, 2]) {
        const p = port(id, 'in-0');
        expect([p.x % GRID, p.y % GRID]).toEqual([0, 0]);
      }
      // The route still leaves and enters straight.
      const [first, last] = [snapped.routes.get('1:0>2:0')![0], snapped.routes.get('1:0>2:0')!.at(-1)!];
      if (vertical) {
        expect(first.x).toBe(port(1, 'out-0').x);
        expect(last.x).toBe(port(2, 'in-0').x);
      } else {
        expect(first.y).toBe(port(1, 'out-0').y);
        expect(last.y).toBe(port(2, 'in-0').y);
      }
      expect(snapLayout(result, measured, [cable], false)).toBe(result);
    });
  }
});
