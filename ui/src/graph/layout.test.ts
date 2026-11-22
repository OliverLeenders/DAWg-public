import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type Connection, type GraphNode, type NodeTypeInfo, type TrackGraph } from '../bridge/protocol';
import { curveHits, routeAround, routeHits } from './routing';
import { cableId, chipFacing, chipSpot, layoutGraph, routedPath, signalOrder, type MeasuredNode } from './layout';
import { connectionProblem, feedbackChips } from './validate';
import { focusOf } from './focus';

const node = (id: number, type = 'x') => ({ id, type, params: {}, x: 0, y: 0 }) as unknown as GraphNode;
const cable = (fromNode: number, toNode: number, toPort = 0): Connection => ({ fromNode, fromPort: 0, toNode, toPort });
const graphOf = (nodes: GraphNode[], cables: Connection[]) =>
  ({ nodes: Object.fromEntries(nodes.map((n) => [String(n.id), n])), connections: Object.fromEntries(cables.map((c) => [cableId(c), c])) }) as unknown as TrackGraph;

// Nodes with two inputs and an output, in left-to-right flow.
const measure = (graph: TrackGraph, vertical = false): MeasuredNode[] =>
  Object.values(graph.nodes).map((n) => {
    const chip = n.type === NODE_TYPES.feedback;
    const [width, height] = chip ? [90, 28] : [160, 80];
    return {
      id: n.id,
      width,
      height,
      ports: vertical
        ? [
            { handle: 'in-0', side: 'top' as const, x: 40, y: 0 },
            { handle: 'in-1', side: 'top' as const, x: 90, y: 0 },
            { handle: 'out-0', side: 'bottom' as const, x: 80, y: height },
          ]
        : [
            { handle: 'in-0', side: 'left' as const, x: 0, y: 30 },
            { handle: 'in-1', side: 'left' as const, x: 0, y: 55 },
            { handle: 'out-0', side: 'right' as const, x: width, y: 30 },
          ],
    };
  });

const crossesABox = (graph: TrackGraph, measured: MeasuredNode[], layout: Awaited<ReturnType<typeof layoutGraph>>) => {
  const problems: string[] = [];

  for (const c of Object.values(graph.connections)) {
    const bends = layout.routes.get(cableId(c)) ?? [];
    for (let i = 0; i + 1 < bends.length; i++) {
      const [a, b] = [bends[i], bends[i + 1]];
      for (const m of measured) {
        if (m.id === c.fromNode || m.id === c.toNode) continue;
        const at = layout.positions.get(m.id)!;
        const overlaps =
          Math.max(a.x, b.x) > at.x && Math.min(a.x, b.x) < at.x + m.width && Math.max(a.y, b.y) > at.y && Math.min(a.y, b.y) < at.y + m.height;
        if (overlaps) problems.push(`${cableId(c)} crosses ${m.id}`);
      }
    }
  }

  return problems;
};

describe('graph layout', () => {
  // A chain with a long skip over it, and a modulation-like cable from the start to the end.
  const skipping = graphOf(
    [1, 2, 3, 4, 5, 6].map((id) => node(id)),
    [cable(1, 2), cable(2, 3), cable(3, 4), cable(4, 5), cable(5, 6), cable(1, 6, 1), cable(2, 5, 1)],
  );

  it('keeps long cables out from behind nodes', async () => {
    for (const vertical of [false, true]) {
      const measured = measure(skipping, vertical);
      const layout = await layoutGraph(skipping, measured, vertical);
      expect(layout.positions.size).toBe(6);
      expect(crossesABox(skipping, measured, layout)).toEqual([]);
    }
  });

  it('runs the signal in order', async () => {
    const layout = await layoutGraph(skipping, measure(skipping), false);
    const xs = [1, 2, 3, 4, 5, 6].map((id) => layout.positions.get(id)!.x);
    expect(xs).toEqual([...xs].sort((a, b) => a - b));
  });

  // In -> Mix -> Feedback -> Filter -> Mix, and Filter -> Out.
  const looped = graphOf(
    [node(1), node(2), node(3, NODE_TYPES.feedback), node(4), node(5)],
    [cable(1, 2), cable(2, 3), cable(3, 4), cable(4, 2, 1), cable(4, 5)],
  );

  it('takes a Feedback node with one way in and out as a chip', () => {
    expect([...feedbackChips(looped).keys()]).toEqual([3]);

    // With a tap too, it's a node like any other.
    const tapped = graphOf(Object.values(looped.nodes), [...Object.values(looped.connections), cable(3, 5, 1)]);
    expect(feedbackChips(tapped).size).toBe(0);

    // And with something modulating it.
    const modulated = graphOf([...Object.values(looped.nodes), node(6)], [...Object.values(looped.connections), cable(6, 3, 1)]);
    expect(feedbackChips(modulated).size).toBe(0);
  });

  it('puts a chip on the way back, with the cable through it', async () => {
    const measured = measure(looped);
    const layout = await layoutGraph(looped, measured, false);
    expect(layout.positions.size).toBe(5);
    expect(layout.routes.has('2:0>3:0')).toBe(true);
    expect(layout.routes.has('3:0>4:0')).toBe(true);

    // The loop reads forward: Filter, then Mix, with the chip's cable going back.
    expect(layout.positions.get(4)!.x).toBeLessThan(layout.positions.get(2)!.x);
    expect(crossesABox(looped, measured, layout)).toEqual([]);
  });

  it('lays out several loops without cables crossing nodes', async () => {
    const nodes = [node(1), node(2), node(3, NODE_TYPES.feedback), node(4), node(5, NODE_TYPES.feedback), node(6), node(7, NODE_TYPES.feedback), node(8)];
    const graph = graphOf(nodes, [cable(1, 2), cable(2, 3), cable(3, 4), cable(4, 2, 1), cable(4, 6), cable(6, 5), cable(5, 6, 1), cable(6, 8), cable(8, 7), cable(7, 4, 1)]);
    const measured = measure(graph);
    const layout = await layoutGraph(graph, measured, false);
    expect(layout.positions.size).toBe(8);
    expect(crossesABox(graph, measured, layout)).toEqual([]);
  });

  it('orders nodes by signal, leaving out the ways back', () => {
    expect(signalOrder(looped).indexOf(4)).toBeLessThan(signalOrder(looped).indexOf(2));
  });

  it('prefers a stretch running against the signal for a chip', () => {
    const route = [{ x: 500, y: 0 }, { x: 500, y: 100 }, { x: 0, y: 100 }, { x: 0, y: 0 }];
    expect(chipSpot(route, false).at).toEqual({ x: 250, y: 100 });
    expect(chipSpot(route, false).segment).toBe(1);
  });

  it('rounds corners', () => {
    const path = routedPath({ x: 0, y: 0 }, [{ x: 100, y: 0 }], { x: 100, y: 100 });
    expect(path).toBe('M 0 0 L 92 0 Q 100 0 100 8 L 100 100');
  });
});

describe('a node\'s ports', () => {
  const stereo = { type: 'audio', name: 'Audio' };
  const of = (inputs: unknown[]) => ({ inputs, outputs: [stereo] }) as unknown as NodeTypeInfo;
  const graph = graphOf([node(1), node(2)], []);

  it('are what the node itself has, not its type', () => {
    const type = of([stereo]);
    const script = of([stereo, stereo]);
    const infoFor = (n: GraphNode) => (n.id === 2 ? script : type);
    expect(connectionProblem(graph, infoFor, cable(1, 2, 1))).toBeNull();
    expect(connectionProblem(graph, () => type, cable(1, 2, 1))).toBe('No such port');
  });
});

describe('routing around nodes', () => {
  const box = { x: 200, y: -40, width: 160, height: 80 };
  const from = { at: { x: 0, y: 0 }, side: 'right' as const };
  const to = { at: { x: 560, y: 0 }, side: 'left' as const };

  it('sees a curve that crosses a node, and one that clears it', () => {
    expect(curveHits(from, to, [box])).toBe(true);
    expect(curveHits(from, to, [{ ...box, y: 200 }])).toBe(false);
  });

  it('sees a way back that passes through its own nodes', () => {
    const own = { x: 0, y: -40, width: 160, height: 80 };
    expect(curveHits({ at: { x: 160, y: 0 }, side: 'right' }, { at: { x: 0, y: 0 }, side: 'left' }, [own])).toBe(true);
  });

  it('goes around a node, with few turns', () => {
    const bends = routeAround(from, to, [box])!;
    expect(routeHits([from.at, ...bends, to.at], [box])).toBe(false);
    expect(bends.length).toBeLessThanOrEqual(4);
  });

  it('goes around a whole row of nodes', () => {
    const row = [0, 1, 2, 3].map((i) => ({ ...box, x: 100 + i * 180 }));
    const bends = routeAround({ at: { x: 0, y: 0 }, side: 'right' }, { at: { x: 900, y: 0 }, side: 'left' }, row)!;
    expect(routeHits([{ x: 0, y: 0 }, ...bends, { x: 900, y: 0 }], row)).toBe(false);
  });

  it('gives up on an end that has no way out', () => {
    expect(routeAround(from, to, [{ x: 5, y: -40, width: 100, height: 80 }])).toBeNull();
  });

  it('starts clear of its own node when a port sits inside it', () => {
    const own = { x: -160, y: -40, width: 164, height: 80 };
    const bends = routeAround(from, to, [own, box])!;
    expect(bends).not.toBeNull();
    expect(routeHits([{ x: 20, y: 0 }, ...bends.slice(1), to.at], [own, box])).toBe(false);
  });
});

describe('chips', () => {
  it('face the node feeding them', () => {
    expect(chipFacing({ x: 500, y: 0 }, { x: 0, y: 0 }, false)).toBe('right');
    expect(chipFacing({ x: 0, y: 0 }, { x: 500, y: 0 }, false)).toBe('left');
    expect(chipFacing({ x: 0, y: 500 }, { x: 0, y: 0 }, true)).toBe('bottom');
    expect(chipFacing({ x: 0, y: 0 }, { x: 0, y: 500 }, true)).toBe('top');
  });

  it('stay as they were on a loop round one node', () => {
    expect(chipFacing({ x: 0, y: 0 }, { x: 0, y: 0 }, true)).toBe('bottom');
    expect(chipFacing(undefined, undefined, false)).toBe('right');
  });
});

describe('focus', () => {
  const graph = [cable(1, 2), cable(2, 3), cable(4, 5)];

  it('has nothing to say without a focus', () => {
    expect(focusOf(graph, new Set())).toBeNull();
  });

  it('keeps a node, what it touches and its cables', () => {
    const focus = focusOf(graph, new Set([2]))!;
    expect([...focus.nodes].sort()).toEqual([1, 2, 3]);
    expect(focus.cables.has('1:0>2:0')).toBe(true);
    expect(focus.cables.has('4:0>5:0')).toBe(false);
  });
});
