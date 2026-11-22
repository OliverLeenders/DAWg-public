import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode, type NodeTypeInfo } from '../bridge/protocol';
import { CROSSOVER_GAP, frequencyToPosition, orderCrossovers, positionToFrequency, splitInfo } from './splits';

const audio = (name: string, channels = 0) => ({ type: 'audio' as const, name, channels });
const node = (type: string, extra: Partial<GraphNode> = {}): GraphNode => ({ id: 1, type, x: 0, y: 0, params: {}, bypassed: false, ...extra }) as GraphNode;
const param = (id: string, name = id) => ({ id, name, min: 0, max: 1, default: 0, kind: 'continuous' as const, unit: '', choices: [] }) as unknown as NodeTypeInfo['params'][number];

const bandInfo: NodeTypeInfo = {
  type: NODE_TYPES.bandSplit,
  name: 'Band Split',
  category: 'utility',
  userEditable: true,
  inputs: [audio('Audio')],
  outputs: [audio('Low'), audio('Band 2'), audio('Band 3'), audio('High'), audio('Dry')],
  params: [param('crossover1'), param('crossover2'), param('crossover3'), { ...param('solo'), kind: 'choice' }, param('phase')],
};

describe('split nodes', () => {
  it('names a Band Split\'s ports for its count and hides the rest, keeping Dry at its index', () => {
    const two = splitInfo(node(NODE_TYPES.bandSplit, { bands: 2 }), bandInfo);
    expect(two.outputs.map((p) => (p.hidden ? '-' : p.name))).toEqual(['Low', 'High', '-', '-', 'Dry']);
    expect(two.params.map((p) => p.id)).toEqual(['crossover1', 'solo', 'phase']);
    expect(two.params.find((p) => p.id === 'solo')?.choices).toEqual(['Off', 'Low', 'High']);

    const three = splitInfo(node(NODE_TYPES.bandSplit), bandInfo);
    expect(three.outputs.slice(0, 3).map((p) => p.name)).toEqual(['Low', 'Mid', 'High']);
    expect(three.outputs[0].tone).toBe('band-0');
  });

  it('names the stereo nodes\' parts for their mode', () => {
    const merge: NodeTypeInfo = { ...bandInfo, type: NODE_TYPES.stereoMerge, inputs: [audio('Mid', 1), audio('Side', 1), { type: 'modulation', name: 'Mod' }], outputs: [audio('Audio', 2)], params: [param('gain1', 'Mid'), param('gain2', 'Side')] };
    const leftRight = splitInfo(node(NODE_TYPES.stereoMerge, { mode: 'leftRight' }), merge);
    expect(leftRight.inputs.map((p) => p.name)).toEqual(['Left', 'Right', 'Mod']);
    expect(leftRight.params.map((p) => p.name)).toEqual(['Left', 'Right']);
    expect(splitInfo(node(NODE_TYPES.stereoMerge), merge).inputs[1].tone).toBe('side');
  });

  it('keeps crossovers in order, a third of an octave apart, pushing the others along', () => {
    const pushed = orderCrossovers([200, 2500, 6000], 0, 5000);
    expect(pushed[0]).toBe(5000);
    expect(pushed[1]).toBeCloseTo(5000 * CROSSOVER_GAP);
    expect(pushed[2]).toBeCloseTo(5000 * CROSSOVER_GAP * CROSSOVER_GAP);

    const down = orderCrossovers([200, 2500, 6000], 2, 25);
    expect(down[2]).toBeCloseTo(20 * CROSSOVER_GAP * CROSSOVER_GAP);
    expect(down[0]).toBeCloseTo(20);

    expect(orderCrossovers([1000], 0, 50000)).toEqual([20000]);
  });

  it('maps frequencies across the view on a log scale', () => {
    expect(frequencyToPosition(20)).toBeCloseTo(0);
    expect(frequencyToPosition(20000)).toBeCloseTo(1);
    expect(positionToFrequency(frequencyToPosition(1000))).toBeCloseTo(1000);
  });
});
