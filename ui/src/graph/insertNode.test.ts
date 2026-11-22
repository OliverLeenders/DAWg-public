import { describe, expect, it } from 'vitest';
import type { Connection, GraphNode, NodeTypeInfo, TrackGraph } from '../bridge/protocol';
import { detachment, graphAfter, insertionCables, makeRoom, ShakeDetector, type Box } from './insertNode';

const port = (type: 'audio' | 'events' | 'modulation') => ({ type, name: type });

const infos: Record<string, Partial<NodeTypeInfo>> = {
  // A key input after the main one, and modulation inputs before nothing: only the first audio input takes the cable.
  effect: { inputs: [port('modulation'), port('audio'), port('audio')], outputs: [port('audio')], userEditable: true },
  synth: { inputs: [port('events')], outputs: [port('audio')], userEditable: true },
  arp: { inputs: [port('events')], outputs: [port('events')], userEditable: true },
  lfo: { inputs: [], outputs: [port('modulation')], userEditable: true },
  output: { inputs: [port('audio')], outputs: [port('audio')], userEditable: false },
};

const node = (id: number, type: string) => ({ id, type }) as GraphNode;
const cable = (fromNode: number, fromPort: number, toNode: number, toPort: number): Connection => ({
  fromNode,
  fromPort,
  toNode,
  toPort,
});

const graphOf = (nodes: GraphNode[], connections: Connection[]) =>
  ({
    nodes: Object.fromEntries(nodes.map((n) => [String(n.id), n])),
    connections: Object.fromEntries(connections.map((c, i) => [String(i), c])),
  }) as unknown as TrackGraph;

const infoFor = (n: GraphNode) => infos[n.type] as NodeTypeInfo;

describe('inserting a node into a cable', () => {
  const a = node(1, 'effect');
  const b = node(2, 'effect');
  const wire = cable(1, 0, 2, 1);

  it('routes the cable through the first input and output of its kind', () => {
    const c = node(3, 'effect');
    expect(insertionCables(graphOf([a, b, c], [wire]), c, wire, infoFor)).toEqual([cable(1, 0, 3, 1), cable(3, 0, 2, 1)]);
  });

  it('keeps the ports the cable used at both ends', () => {
    const c = node(3, 'effect');
    const toKey = cable(1, 0, 2, 2);
    expect(insertionCables(graphOf([a, b, c], [toKey]), c, toKey, infoFor)?.[1]).toEqual(cable(3, 0, 2, 2));
  });

  it('takes event cables too', () => {
    const seq = node(1, 'arp');
    const synth = node(2, 'synth');
    const arp = node(3, 'arp');
    const notes = cable(1, 0, 2, 0);
    expect(insertionCables(graphOf([seq, synth, arp], [notes]), arp, notes, infoFor)).toEqual([
      cable(1, 0, 3, 0),
      cable(3, 0, 2, 0),
    ]);
  });

  it('refuses a node that is connected', () => {
    const c = node(3, 'effect');
    const other = cable(3, 0, 2, 2);
    expect(insertionCables(graphOf([a, b, c], [wire, other]), c, wire, infoFor)).toBeNull();
    expect(insertionCables(graphOf([a, b], [wire]), a, wire, infoFor)).toBeNull();
  });

  it('refuses a node without ports of the kind, a modulation cable and a fixed node', () => {
    const synth = node(3, 'synth');
    expect(insertionCables(graphOf([a, b, synth], [wire]), synth, wire, infoFor)).toBeNull();

    const lfo = node(4, 'lfo');
    const modulation = cable(4, 0, 2, 0);
    const c = node(3, 'effect');
    expect(insertionCables(graphOf([lfo, b, c], [modulation]), c, modulation, infoFor)).toBeNull();

    const fixed = node(3, 'output');
    expect(insertionCables(graphOf([a, b, fixed], [wire]), fixed, wire, infoFor)).toBeNull();
  });
});

describe('taking a node out of the wiring', () => {
  const a = node(1, 'effect');
  const b = node(2, 'effect');
  const c = node(3, 'effect');

  it('closes the gap it leaves', () => {
    const graph = graphOf([a, b, c], [cable(1, 0, 3, 1), cable(3, 0, 2, 1)]);
    const plan = detachment(graph, c, infoFor);
    expect(plan?.remove).toHaveLength(2);
    expect(plan?.add).toEqual([cable(1, 0, 2, 1)]);
    expect(Object.values(graphAfter(graph, plan).connections)).toEqual([cable(1, 0, 2, 1)]);
  });

  it('reverses an insertion', () => {
    const wire = cable(1, 0, 2, 1);
    const [first, second] = insertionCables(graphOf([a, b, c], [wire]), c, wire, infoFor)!;
    expect(detachment(graphOf([a, b, c], [first, second]), c, infoFor)?.add).toEqual([wire]);
  });

  it('only unplugs what has no way through', () => {
    const key = cable(1, 0, 3, 2); // into the second audio input
    expect(detachment(graphOf([a, b, c], [key, cable(3, 0, 2, 1)]), c, infoFor)?.add).toEqual([]);
  });

  it('does not double a cable that is already there, and fans out', () => {
    const existing = cable(1, 0, 2, 1);
    const through = [cable(1, 0, 3, 1), cable(3, 0, 2, 1)];
    expect(detachment(graphOf([a, b, c], [existing, ...through]), c, infoFor)?.add).toEqual([]);

    const d = node(4, 'effect');
    const fan = [cable(1, 0, 3, 1), cable(3, 0, 2, 1), cable(3, 0, 4, 1)];
    expect(detachment(graphOf([a, b, c, d], fan), c, infoFor)?.add).toEqual([cable(1, 0, 2, 1), cable(1, 0, 4, 1)]);
  });

  it('refuses a fixed node, and a node without cables', () => {
    const fixed = node(3, 'output');
    expect(detachment(graphOf([a, fixed], [cable(1, 0, 3, 0)]), fixed, infoFor)).toBeNull();
    expect(detachment(graphOf([a, c], []), c, infoFor)).toBeNull();
  });
});

describe('recognising a shake', () => {
  const run = (points: Array<[number, number]>, step = 40) => {
    const shake = new ShakeDetector();
    return points.some(([x, y], i) => shake.feed(x, y, i * step));
  };
  const zigzag: Array<[number, number]> = [[0, 0], [80, 0], [0, 0], [80, 0], [0, 0], [80, 0], [0, 0]];

  it('sees quick strokes back and forth', () => {
    expect(run(zigzag)).toBe(true);
    expect(run(zigzag.map(([x, y]) => [y, x]))).toBe(true);
  });

  it('ignores a straight drag, and jitter along one', () => {
    expect(run(Array.from({ length: 40 }, (_, i): [number, number] => [i * 10, 0]))).toBe(false);
    expect(run(Array.from({ length: 40 }, (_, i): [number, number] => [i * 10 + (i % 2) * 8, 0]))).toBe(false);
  });

  it('ignores a small wobble, and a few strokes while finding a place', () => {
    expect(run(Array.from({ length: 20 }, (_, i): [number, number] => [(i % 2) * 40, 0]))).toBe(false);
    expect(run(zigzag.slice(0, 5))).toBe(false);
  });

  it('ignores slow strokes', () => {
    expect(run(zigzag, 500)).toBe(false);
  });
});

describe('making room for an inserted node', () => {
  const box = (x: number, y = 0, width = 100, height = 50): Box => ({ x, y, width, height });

  it('moves everything from the far end on, by what clears the node', () => {
    const [from, to, after] = [box(0), box(160), box(400)];
    const room = makeRoom(box(160), from, to, [from, to, after]);
    // The node stays clear of `from` at 140 or more, so it stays at 160, and its right edge plus padding is 300.
    expect(room).toEqual({ x: 160, shift: 140, moved: [to, after] });
  });

  it('takes the node off the near end', () => {
    const [from, to] = [box(0), box(400)];
    const room = makeRoom(box(50), from, to, [from, to]);
    expect(room.x).toBe(140);
    expect(room.shift).toBe(0);
  });

  it('leaves everything alone when there is space', () => {
    const [from, to] = [box(0), box(600)];
    expect(makeRoom(box(250), from, to, [from, to])).toEqual({ x: 250, shift: 0, moved: [] });
  });

  it('needs no room from nodes far away across the signal', () => {
    const [from, to] = [box(0), box(160)];
    expect(makeRoom(box(160, 400), from, to, [from, to]).shift).toBe(0);
  });

  it('moves a parallel branch to the right along with the rest, keeping nodes to the left', () => {
    const [from, to, left, right] = [box(0), box(160), box(0, 200), box(170, 200)];
    const room = makeRoom(box(160), from, to, [from, to, left, right]);
    expect(room.moved).toEqual([to, right]);
  });
});
