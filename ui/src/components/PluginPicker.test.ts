import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type NodeTypeInfo } from '../bridge/protocol';
import { builtinItems } from './PluginPicker';

const info = (type: string, category: NodeTypeInfo['category'], audioInputs: number) =>
  ({ type, name: type, category, userEditable: true, inputs: Array.from({ length: audioInputs }, () => ({ type: 'audio' })), outputs: [], params: [] }) as unknown as NodeTypeInfo;

const types = Object.fromEntries(
  [
    info(NODE_TYPES.gain, 'utility', 1),
    info(NODE_TYPES.delay, 'effect', 1),
    info(NODE_TYPES.compressor, 'effect', 2),
    info(NODE_TYPES.stereoMerge, 'utility', 2),
    info(NODE_TYPES.lfo, 'utility', 0),
    info(NODE_TYPES.sampler, 'instrument', 0),
    info(NODE_TYPES.arpeggiator, 'midiEffect', 0),
  ].map((t) => [t.type, t]),
);

const offered = (...args: Parameters<typeof builtinItems> extends [unknown, ...infer Rest] ? Rest : never) =>
  builtinItems(types, ...args).map((i) => i.type).sort();

describe('builtinItems', () => {
  it("offers a chain's effects, the Gain among them", () => {
    expect(offered('effect')).toEqual([NODE_TYPES.compressor, NODE_TYPES.delay, NODE_TYPES.gain].sort());
    expect(offered('instrument')).toEqual([NODE_TYPES.sampler]);
    expect(offered('midiEffect')).toEqual([NODE_TYPES.arpeggiator]);
  });

  it('offers a graph everything, instruments only where they go', () => {
    // The Stereo Merge twice: mid/side and left/right.
    expect(offered('any')).toHaveLength(Object.keys(types).length + 1);
    expect(offered('any', false)).not.toContain(NODE_TYPES.sampler);
  });

  it("files each in the add menu's folders", () => {
    const gain = builtinItems(types, 'effect').find((i) => i.type === NODE_TYPES.gain);
    expect(gain?.folders).toEqual(['builtin', 'utility']);
  });
});
