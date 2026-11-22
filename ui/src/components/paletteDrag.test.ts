import { beforeAll, describe, expect, it } from 'vitest';
import { NODE_TYPES, type NodeTypeInfo, type Project, type SavedComponent, type Track } from '../bridge/protocol';
import { useDawg } from '../store/store';
import { chainProblem, chainRole } from './paletteDrag';

const type = (category: string, audioInputs: number, userEditable = true) =>
  ({ category, userEditable, inputs: Array.from({ length: audioInputs }, () => ({ type: 'audio' })) }) as unknown as NodeTypeInfo;

const types: Record<string, NodeTypeInfo> = Object.fromEntries(
  Object.entries({
    'dawg.sampler': type('instrument', 0),
    'dawg.delay': type('effect', 1),
    'dawg.compressor': type('effect', 2),
    'dawg.mix': type('utility', 3),
    [NODE_TYPES.stereoMerge]: type('utility', 2),
    [NODE_TYPES.gain]: type('utility', 1),
    [NODE_TYPES.lfo]: type('utility', 1),
    'dawg.trackOutput': type('io', 1, false),
  }).map(([id, info]) => [id, { ...info, type: id }]),
);

const saved: SavedComponent[] = [
  { file: 'box', name: 'Box', inputs: [{ type: 'audio' }], outputs: [{ type: 'audio' }] } as unknown as SavedComponent,
  { file: 'synth', name: 'Synth', inputs: [{ type: 'notes' }], outputs: [{ type: 'audio' }] } as unknown as SavedComponent,
];

describe('chainRole', () => {
  const role = (picked: Parameters<typeof chainRole>[0]) => chainRole(picked, types, saved);

  it('puts instruments in the instrument slot and one-input effects in the inserts', () => {
    expect(role({ type: 'dawg.sampler' })).toBe('instrument');
    expect(role({ type: 'dawg.delay' })).toBe('effect');
    // A Gain is an effect like any other; the chain's own Gain is the one before the output.
    expect(role({ type: NODE_TYPES.gain })).toBe('effect');
    // A keyable effect: its second audio input is the key.
    expect(role({ type: 'dawg.compressor' })).toBe('effect');
    expect(role({ type: NODE_TYPES.pluginInstrument })).toBe('instrument');
    expect(role({ type: NODE_TYPES.pluginEffect })).toBe('effect');
    expect(role({ type: NODE_TYPES.faust })).toBe('effect');
  });

  it('leaves what needs wiring to the graph view', () => {
    expect(role({ type: 'dawg.mix' })).toBeNull();
    expect(role({ type: NODE_TYPES.lfo })).toBeNull();
    // Two audio inputs, yet a split: it only goes in a graph.
    expect(role({ type: NODE_TYPES.stereoMerge })).toBeNull();
    expect(role({ type: 'dawg.trackOutput' })).toBeNull();
    // A factory patch that is no component (Plucked String) is wired in the graph.
    expect(role({ type: '', patch: 'plucked-string' })).toBeNull();
    expect(role({ type: NODE_TYPES.component, patch: 'dub-delay' })).toBe('effect');
  });

  it('takes saved components with one audio input and output as effects', () => {
    expect(role({ type: NODE_TYPES.component, saved: 'box' })).toBe('effect');
    expect(role({ type: NODE_TYPES.component, saved: 'synth' })).toBeNull();
  });
});

describe('chainProblem', () => {
  const kit = {
    id: 1,
    kind: 'instrument',
    graph: {
      nodes: {
        '2': { id: 2, type: NODE_TYPES.sequencer },
        '3': { id: 3, type: 'dawg.sampler' },
        '4': { id: 4, type: NODE_TYPES.gain },
        '5': { id: 5, type: NODE_TYPES.trackOutput },
      },
      connections: [
        { fromNode: 2, fromPort: 0, toNode: 3, toPort: 0 },
        { fromNode: 3, fromPort: 0, toNode: 4, toPort: 0 },
        { fromNode: 4, fromPort: 0, toNode: 5, toPort: 0 },
      ],
    },
  } as unknown as Track;
  const project = (outputs: boolean) =>
    ({
      trackOrder: outputs ? [1, 6] : [1],
      tracks: { '1': kit, ...(outputs ? { '6': { id: 6, kind: 'output', source: 1 } } : {}) },
    }) as unknown as Project;
  beforeAll(() => useDawg.setState({ nodeTypes: types, savedComponents: saved }));

  it('sends effects for a header track to its output tracks (MO12)', () => {
    expect(chainProblem(kit, { type: 'dawg.delay' }, project(false))).toBeNull();
    expect(chainProblem(kit, { type: 'dawg.delay' }, project(true))).toMatch(/output tracks/);
    expect(chainProblem(kit, { type: 'dawg.sampler' }, project(true))).toBeNull();
  });
});
