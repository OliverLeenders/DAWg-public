import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode } from '../bridge/protocol';
import { builtinItems } from '../components/PluginPicker';
import { paramApplies } from './lfo';

const oscillator = (params: Record<string, number>): GraphNode => ({
  id: 1,
  type: NODE_TYPES.oscillator,
  params,
  x: 0,
  y: 0,
  bypassed: false,
});

describe('parameters that apply', () => {
  it("shows the oscillator's pulse width, unison spread and phase only when they do something", () => {
    expect(paramApplies(oscillator({}), 'pulseWidth')).toBe(false);
    expect(paramApplies(oscillator({ waveform: 3 }), 'pulseWidth')).toBe(true);
    expect(paramApplies(oscillator({}), 'detune')).toBe(false);
    expect(paramApplies(oscillator({ unison: 3 }), 'spread')).toBe(true);
    expect(paramApplies(oscillator({ free: 1 }), 'phase')).toBe(false);
    expect(paramApplies(oscillator({ free: 1 }), 'randomPhase')).toBe(false);
    expect(paramApplies(oscillator({}), 'randomPhase')).toBe(true);
    expect(paramApplies(oscillator({}), 'pitch')).toBe(true);
  });
});

describe('the node picker', () => {
  it('offers Note Expression and the oscillator only in the graph view', () => {
    const info = (type: string, category: 'utility', inputs: string[]) => ({
      type,
      name: type,
      category,
      userEditable: true,
      inputs: inputs.map((t) => ({ type: t, name: t })),
      outputs: [],
      params: [],
    });
    const types = {
      [NODE_TYPES.noteExpression]: info(NODE_TYPES.noteExpression, 'utility', ['events']),
      [NODE_TYPES.oscillator]: info(NODE_TYPES.oscillator, 'utility', ['modulation']),
    } as unknown as Parameters<typeof builtinItems>[0];

    expect(builtinItems(types, 'effect')).toEqual([]);
    expect(builtinItems(types, 'any').map((item) => item.type).sort()).toEqual(
      [NODE_TYPES.noteExpression, NODE_TYPES.oscillator].sort(),
    );
  });
});
