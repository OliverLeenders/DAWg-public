import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode, type Track } from '../bridge/protocol';
import { allowedInVoiceGraph, findNode, graphOf, ownerChain, ownerOf, runsPerVoice } from './voice';

const node = (id: number, type: string, extra: Partial<GraphNode> = {}): GraphNode =>
  ({ id, type, x: 0, y: 0, params: {}, bypassed: false, modulations: {}, ...extra }) as GraphNode;

const voice = {
  nodes: { '10': node(10, NODE_TYPES.voiceInput), '11': node(11, NODE_TYPES.oscillator), '12': node(12, NODE_TYPES.voiceOutput) },
  connections: {},
};
const track = {
  id: 1,
  graph: {
    nodes: { '1': node(1, NODE_TYPES.sequencer), '2': node(2, NODE_TYPES.voiceInstrument, { voice }) },
    connections: {},
  },
} as unknown as Track;

describe('voice scope', () => {
  it('finds nodes in the track graph and in voice graphs', () => {
    expect(findNode(track, 1)?.type).toBe(NODE_TYPES.sequencer);
    expect(findNode(track, 11)?.type).toBe(NODE_TYPES.oscillator);
    expect(findNode(track, 99)).toBeUndefined();
  });

  it('knows which Voice Instrument owns a node', () => {
    expect(ownerOf(track, 11)?.id).toBe(2);
    expect(ownerOf(track, 1)).toBeUndefined();
  });

  it('shows the scope’s graph', () => {
    expect(graphOf(track, undefined)).toBe(track.graph);
    expect(graphOf(track, 2)).toBe(voice);
    expect(graphOf(track, 1)).toBeUndefined();
  });

  it('allows only nodes that can run per voice', () => {
    expect(allowedInVoiceGraph(NODE_TYPES.oscillator)).toBe(true);
    expect(allowedInVoiceGraph(NODE_TYPES.faust)).toBe(true);
    expect(allowedInVoiceGraph(NODE_TYPES.pluginEffect)).toBe(false);
    expect(allowedInVoiceGraph(NODE_TYPES.mpeSynth)).toBe(false);
    expect(allowedInVoiceGraph(NODE_TYPES.voiceInstrument)).toBe(false);
  });
});

describe('nested scopes', () => {
  const graphWith = (...nodes: GraphNode[]) => ({ nodes: Object.fromEntries(nodes.map((n) => [String(n.id), n])), connections: {} });
  const inner = node(30, NODE_TYPES.component, { component: { graph: graphWith(node(31, NODE_TYPES.gain)), inputs: [], outputs: [], name: '', exposed: [] } });
  const voiced = node(20, NODE_TYPES.voiceInstrument, { voice: graphWith(inner) });
  const outer = node(10, NODE_TYPES.component, { component: { graph: graphWith(voiced), inputs: [], outputs: [], name: '', exposed: [] } });
  const nested = { id: 1, graph: graphWith(outer) } as unknown as Track;

  it('finds nodes at any depth, with what holds them', () => {
    expect(findNode(nested, 31)?.type).toBe(NODE_TYPES.gain);
    expect(ownerChain(nested, 31)?.map((n) => n.id)).toEqual([10, 20, 30]);
    expect(ownerOf(nested, 31)?.id).toBe(30);
    expect(ownerOf(nested, 10)).toBeUndefined();
    expect(ownerChain(nested, 99)).toBeUndefined();
  });

  it('shows what any of them holds', () => {
    expect(graphOf(nested, 30)?.nodes['31']).toBeDefined();
    expect(graphOf(nested, 10)?.nodes['20']).toBeDefined();
    expect(graphOf(nested, 31)).toBeUndefined();
  });

  it('knows what runs per voice, whatever is between', () => {
    expect(runsPerVoice(nested, 10)).toBe(false);
    expect(runsPerVoice(nested, 20)).toBe(true);
    expect(runsPerVoice(nested, 30)).toBe(true);
    expect(runsPerVoice(nested, undefined)).toBe(false);
  });
});
