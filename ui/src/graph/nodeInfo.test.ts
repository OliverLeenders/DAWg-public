import { beforeEach, describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode, type NodeTypeInfo, type PluginNodeStatus, type Project } from '../bridge/protocol';
import { useDawg } from '../store/store';
import { infoOf } from './nodeInfo';

const plugin = { id: 2, type: NODE_TYPES.pluginInstrument, x: 0, y: 0, params: {}, bypassed: false, modulations: {} } as unknown as GraphNode;
const ports = ['Main', ...Array.from({ length: 15 }, (_, i) => `Out ${i + 2}`)].map((name) => ({ type: 'audio' as const, name }));
const types = { [NODE_TYPES.pluginInstrument]: { type: NODE_TYPES.pluginInstrument, inputs: [], outputs: ports, params: [] } } as unknown as Record<string, NodeTypeInfo>;
const buses = (on: boolean) => [
  { name: 'Main', on: true },
  { name: 'Kick', on },
  { name: 'Snare', on },
];
const statuses = (on: boolean): Record<string, PluginNodeStatus> => ({ '1:2': { status: 'ready', error: '', outputs: buses(on) } });
const shown = (on: boolean) => infoOf(1, plugin, types, statuses(on))?.outputs.map((p) => p.name);

const withConnections = (connections: Record<string, { fromNode: number; fromPort: number; toNode: number; toPort: number }>) =>
  useDawg.setState({ project: { tracks: { '1': { id: 1, graph: { nodes: { '2': plugin }, connections } } } } as unknown as Project });

describe("an instrument plugin's outputs (multi-output-instruments.md, MO1)", () => {
  beforeEach(() => withConnections({}));

  it('shows Main alone while the other buses are off', () => {
    expect(shown(false)).toEqual(['Main']);
  });

  it('shows the buses that are on, named as the plugin names them', () => {
    expect(shown(true)).toEqual(['Main', 'Kick', 'Snare']);
  });

  it('keeps a wired port showing after its bus goes off', () => {
    withConnections({ '9': { fromNode: 2, fromPort: 2, toNode: 5, toPort: 0 } });
    expect(shown(false)).toEqual(['Main', 'Kick', 'Snare']);
  });
});
