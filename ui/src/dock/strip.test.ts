import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode, type NodeTypeInfo, type PortDescriptor, type Track } from '../bridge/protocol';
import { stripDevices, stripLinks } from './strip';

const node = (id: number, type: string): GraphNode => ({ id, type, params: {}, x: 0, y: 0, bypassed: false });

function track(nodes: GraphNode[], links: Array<[number, number, number?]>): Track {
  return {
    id: 1,
    name: 'T',
    colour: '#fff',
    gainDb: 0,
    pan: 0,
    offsetMs: 0,
    drumMode: 'auto',
    automation: {},
    automationOrder: [],
    automationMode: 1,
    automationExpanded: false,
  lanes: 1,
  lanesExpanded: false,
    height: 64,
    kind: 'instrument',
    parent: 0,
    folded: false,
    sends: {},
    mute: false,
    solo: false,
    inputChannel: -1,
    inputStereo: false,
    monitor: 1,
    clips: {},
    graph: {
      nodes: Object.fromEntries(nodes.map((n) => [String(n.id), n])),
      connections: Object.fromEntries(
        links.map(([from, to, toPort = 0]) => [`${from}:0>${to}:${toPort}`, { fromNode: from, fromPort: 0, toNode: to, toPort }]),
      ),
    },
  };
}

const port = (type: PortDescriptor['type']): PortDescriptor => ({ type, name: type });
const info = (type: string, inputs: PortDescriptor[], outputs: PortDescriptor[], userEditable = true): NodeTypeInfo => ({
  type,
  name: type,
  category: 'effect',
  userEditable,
  inputs,
  outputs,
  params: [],
});

const types: Record<string, NodeTypeInfo> = {
  [NODE_TYPES.sequencer]: info(NODE_TYPES.sequencer, [], [port('events')], false),
  [NODE_TYPES.gain]: info(NODE_TYPES.gain, [port('audio'), port('modulation')], [port('audio')], false),
  [NODE_TYPES.trackOutput]: info(NODE_TYPES.trackOutput, [port('audio')], [], false),
  [NODE_TYPES.mpeSynth]: info(NODE_TYPES.mpeSynth, [port('events')], [port('audio')]),
  [NODE_TYPES.eq]: info(NODE_TYPES.eq, [port('audio'), port('modulation')], [port('audio')]),
  [NODE_TYPES.delay]: info(NODE_TYPES.delay, [port('audio'), port('modulation')], [port('audio')]),
  [NODE_TYPES.lfo]: info(NODE_TYPES.lfo, [], [port('modulation')]),
  [NODE_TYPES.feedback]: info(NODE_TYPES.feedback, [port('audio')], [port('audio')]),
};

// Sequencer 1, synth 2, gain 3, output 4.
const base = [node(1, NODE_TYPES.sequencer), node(2, NODE_TYPES.mpeSynth), node(3, NODE_TYPES.gain), node(4, NODE_TYPES.trackOutput)];

describe('the device strip', () => {
  it('shows a simple chain in order, without its gain, and its modulation after it', () => {
    const t = track([...base, node(5, NODE_TYPES.eq), node(6, NODE_TYPES.delay), node(7, NODE_TYPES.lfo)], [
      [1, 2],
      [2, 6],
      [6, 5],
      [5, 3],
      [3, 4],
      [7, 5, 1],
    ]);
    const strip = stripDevices(t, types);
    expect(strip.simple).toBe(true);
    expect(strip.devices.map((n) => n.id)).toEqual([2, 6, 5]);
    expect(strip.modulation.map((n) => n.id)).toEqual([7]);
  });

  it('shows the gain of a simple chain while something modulates it', () => {
    const t = track([...base, node(7, NODE_TYPES.lfo)], [
      [1, 2],
      [2, 3],
      [3, 4],
      [7, 3, 1],
    ]);
    expect(stripDevices(t, types).devices.map((n) => n.id)).toEqual([2, 3]);
  });

  it('shows every device of a custom graph in signal order, its gain too', () => {
    // The synth splits into an EQ and a delay that meet at the gain.
    const t = track([...base, node(5, NODE_TYPES.eq), node(6, NODE_TYPES.delay)], [
      [1, 2],
      [2, 5],
      [2, 6],
      [5, 3],
      [6, 3],
      [3, 4],
    ]);
    const strip = stripDevices(t, types);
    expect(strip.simple).toBe(false);
    expect(strip.devices.map((n) => n.id)).toEqual([2, 5, 6, 3]);
  });

  it('follows a branch to its end before the next one starts', () => {
    // The synth feeds 5 then 8, and 6, which meet at the gain.
    const t = track([...base, node(5, NODE_TYPES.eq), node(6, NODE_TYPES.delay), node(8, NODE_TYPES.eq)], [
      [1, 2],
      [2, 5],
      [5, 8],
      [2, 6],
      [8, 3],
      [6, 3],
      [3, 4],
    ]);
    expect(stripDevices(t, types).devices.map((n) => n.id)).toEqual([2, 5, 8, 6, 3]);
  });

  it("puts a loop's Feedback node after what feeds it, and marks its way back", () => {
    const t = track([...base, node(5, NODE_TYPES.eq), node(6, NODE_TYPES.delay), node(7, NODE_TYPES.feedback)], [
      [1, 2],
      [2, 5],
      [5, 6],
      [6, 3],
      [6, 7],
      [7, 5],
      [3, 4],
    ]);
    const { devices } = stripDevices(t, types);
    expect(devices.map((n) => n.id)).toEqual([2, 5, 6, 7, 3]);
    const links = stripLinks(t, devices, types);
    expect(links.filter((l) => l.back).map((l) => [l.from, l.to])).toEqual([[7, 5]]);
    expect(links).toHaveLength(5);
    // The delay's cable to the gain runs around the Feedback node between them, and the way back runs above too.
    expect(links.filter((l) => !l.adjacent).map((l) => [l.from, l.to])).toEqual([
      [6, 3],
      [7, 5],
    ]);
  });

  it('leaves modulation out of the route', () => {
    const t = track([...base, node(7, NODE_TYPES.lfo)], [
      [1, 2],
      [2, 3],
      [3, 4],
      [7, 3, 1],
    ]);
    const strip = stripDevices(t, types);
    expect(stripLinks(t, [...strip.devices, ...strip.modulation], types).map((l) => [l.from, l.to])).toEqual([[2, 3]]);
  });
});
