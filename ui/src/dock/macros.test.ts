import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode, type NodeTypeInfo, type ParamDescriptor, type Track, type TrackGraph } from '../bridge/protocol';
import { depthBetween, formatDepth, modulatedValue, toPosition } from '../graph/params';
import { freeModulationPort, macroName, macroSignal, macroTargets, mappedDepth, signalRange, slotLinks, swingOf } from './macros';

// Macros and modulation links (docs/design/macros-and-modulation-column.md, MC1 to MC13).

const node = (id: number, type: string, extra: Partial<GraphNode> = {}): GraphNode => ({ id, type, params: {}, x: 0, y: 0, bypassed: false, ...extra });

const graph = (nodes: GraphNode[], links: Array<[number, number, number, number]> = []): TrackGraph => ({
  nodes: Object.fromEntries(nodes.map((n) => [String(n.id), n])),
  connections: Object.fromEntries(
    links.map(([from, fromPort, to, toPort]) => [`${from}:${fromPort}>${to}:${toPort}`, { fromNode: from, fromPort, toNode: to, toPort }]),
  ),
});

const track = (g: TrackGraph): Track =>
  ({ id: 1, name: 'T', colour: '#fff', kind: 'instrument', graph: g, clips: {}, sends: {}, automation: {}, automationOrder: [] }) as unknown as Track;

const param = (fields: Partial<ParamDescriptor>): ParamDescriptor => ({
  id: 'p',
  name: 'P',
  min: 0,
  max: 1,
  default: 0,
  kind: 'continuous',
  unit: '',
  mid: 0,
  choices: [],
  modulationSpan: 1,
  modulationPolarity: 'bipolar',
  modulatable: true,
  automatable: true,
  ...fields,
});

const cutoff = param({ id: 'cutoff', min: 20, max: 20000, mid: 1000, unit: 'Hz', modulationScale: 'octaves', modulationSpan: Math.log2(1000) });
const amplitude = param({ id: 'amplitude', min: 0, max: 200, unit: '%', modulationSpan: 200 });
const attack = param({ id: 'attack', min: 0, max: 10000, mid: 100, unit: 'ms', modulationScale: 'travel', modulationSpan: 1 });

describe('octave modulation (MC1 to MC4)', () => {
  it('moves a frequency by the same octaves anywhere in its range', () => {
    const amount = 1 / Math.log2(1000); // one octave
    expect(modulatedValue(cutoff, 200, amount)).toBeCloseTo(400, 6);
    expect(modulatedValue(cutoff, 8000, amount)).toBeCloseTo(16000, 6);
    expect(modulatedValue(cutoff, 15000, amount)).toBe(20000);
    expect(formatDepth(cutoff, 1.5 / Math.log2(1000))).toBe('+1.5 oct');
    expect(formatDepth(cutoff, -0.1)).toBe('-1 oct');
  });

  it('moves a skewed parameter along its knob and a linear one in its units', () => {
    expect(modulatedValue(attack, 0, 0.5)).toBeCloseTo(100, 3);
    expect(formatDepth(attack, 0.25)).toBe('+25%');
    expect(modulatedValue(amplitude, 100, 0.25)).toBe(150);
    expect(formatDepth(amplitude, 0.25)).toBe('+50%');
  });

  it('works out the depth of a move as the inverse', () => {
    expect(depthBetween(cutoff, 500, 1000) * Math.log2(1000)).toBeCloseTo(1, 6);
    expect(depthBetween(amplitude, 100, 50)).toBeCloseTo(-0.25, 9);
    expect(modulatedValue(attack, 50, depthBetween(attack, 50, 2000))).toBeCloseTo(2000, 3);
  });
});

describe('macros', () => {
  it('sends 0 at the neutral position and ±1 at the far end', () => {
    expect(macroSignal(30, 0)).toBeCloseTo(0.3);
    expect(macroSignal(50, 1)).toBe(0);
    expect(macroSignal(0, 1)).toBe(-1);
    expect(macroSignal(100, 2)).toBe(0);
    expect(macroSignal(0, 2)).toBe(-1);
  });

  it('names knobs, and finds every slot a knob feeds, inside a voice graph too', () => {
    const voice = graph([node(20, NODE_TYPES.oscillator, { modulations: { 3: { param: 'coarse', depth: 0.125, active: true, macro: 1 } } })]);
    const macros = node(2, NODE_TYPES.macros, { macroNames: ['', 'Tone'] });
    const filter = node(3, NODE_TYPES.filter, { modulations: { 1: { param: 'cutoff', depth: 0.2, active: true, macro: 0 } } });
    const t = track(graph([macros, filter, node(4, NODE_TYPES.voiceInstrument, { voice })]));
    expect(macroName(macros, 0)).toBe('Macro 1');
    expect(macroName(macros, 1)).toBe('Tone');
    expect(slotLinks(t)).toHaveLength(2);
    expect(macroTargets(t, 0).map((l) => l.node.id)).toEqual([3]);
    expect(macroTargets(t, 1).map((l) => l.node.id)).toEqual([20]);
    expect(macroTargets(t).length).toBe(2);
  });

  it('maps a move at the depth it was moved, signed by the neutral position (MC9)', () => {
    expect(mappedDepth(amplitude, 100, 150, 0)).toBeCloseTo(0.25);
    expect(mappedDepth(amplitude, 100, 150, 1)).toBeCloseTo(0.25);
    expect(mappedDepth(amplitude, 100, 150, 2)).toBeCloseTo(-0.25);
  });

  it('shows the swing on the knob and where the macro puts it now (MC13)', () => {
    const macros = node(2, NODE_TYPES.macros, { params: { macro1: 50 } });
    const gain = node(3, NODE_TYPES.gain, { modulations: { 1: { param: 'amplitude', depth: 0.25, active: true, macro: 0 } } });
    const t = track(graph([macros, gain]));
    const swing = swingOf(t, gain, amplitude, 100, (v) => toPosition(amplitude, v));
    expect(swing?.low).toBeCloseTo(0.5);
    expect(swing?.high).toBeCloseTo(0.75);
    expect(swing?.now).toBeCloseTo(0.625);
  });
});

describe('sources', () => {
  it('knows the range each source covers', () => {
    expect(signalRange(node(1, NODE_TYPES.lfo, { params: { zero: 2 } }), 0)).toEqual([0, 1]);
    expect(signalRange(node(1, NODE_TYPES.lfo), 0)).toEqual([-1, 1]);
    expect(signalRange(node(1, NODE_TYPES.adsr), 0)).toEqual([0, 1]);
    expect(signalRange(node(1, NODE_TYPES.macros, { params: { neutral3: 2 } }), 2)).toEqual([-1, 0]);
  });

  it('finds a free modulation input for a dropped source: no cable and no slot', () => {
    const info = { inputs: [{ type: 'audio', name: 'In' }, { type: 'modulation', name: 'Mod' }, { type: 'modulation', name: 'Mod' }, { type: 'modulation', name: 'Mod' }] } as NodeTypeInfo;
    const target = node(3, NODE_TYPES.filter, { modulations: { 2: { param: 'cutoff', depth: 0, active: true, macro: 0 } } });
    const g = graph([node(1, NODE_TYPES.lfo), target], [[1, 0, 3, 1]]);
    expect(freeModulationPort(g, target, info)).toBe(3);
  });
});
