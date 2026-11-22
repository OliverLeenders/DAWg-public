import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode, type ParamDescriptor } from '../bridge/protocol';
import { inspectorKnobs } from './InstrumentSection';

const node = (type: string, extra: Partial<GraphNode> = {}) => ({ id: 2, type, x: 0, y: 0, params: {}, ...extra }) as GraphNode;
const param = (id: string, kind: ParamDescriptor['kind'] = 'continuous') => ({ id, name: id, min: 0, max: 1, default: 0, kind }) as ParamDescriptor;

describe('inspectorKnobs', () => {
  it('shows no knob row for the MPE Synth, the Sampler or the Drum Synth', () => {
    expect(inspectorKnobs(node(NODE_TYPES.drumSynth), [param('p1.level'), param('p1.pan')])).toEqual([]);
    expect(inspectorKnobs(node(NODE_TYPES.mpeSynth), [param('gain'), param('attack')])).toEqual([]);
    expect(inspectorKnobs(node(NODE_TYPES.sampler), [param('pitch'), param('level'), param('velocity')])).toEqual([]);
  });

  it('shows none for a plugin instrument', () => {
    const plugin = { identifier: 'x', name: 'X', manufacturer: 'Y', format: 'VST3' };
    expect(inspectorKnobs(node(NODE_TYPES.pluginInstrument, { plugin }), [param('a')])).toEqual([]);
  });

  it("shows another instrument's first knobs, leaving out choices and switches", () => {
    const params = [param('mode', 'choice'), ...Array.from({ length: 10 }, (_, i) => param(`p${i}`)), param('on', 'toggle')];
    const knobs = inspectorKnobs(node(NODE_TYPES.voiceInstrument), params);
    expect(knobs.map((p) => p.id)).toEqual(['p0', 'p1', 'p2', 'p3', 'p4', 'p5', 'p6', 'p7']);
  });
});
