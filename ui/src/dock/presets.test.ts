import { beforeEach, describe, expect, it, vi } from 'vitest';
import { NODE_TYPES, TICKS_PER_BEAT, type CurvePoint, type GraphNode, type Track } from '../bridge/protocol';
import * as store from '../store/store';
import {
  applyPreset,
  clearUserPresetsForTesting,
  deleteUserPreset,
  detectMatchingPreset,
  getPresetsForNode,
  getUserPresets,
  hasPresetsFor,
  saveUserPreset,
  type NodePreset,
} from './presets';

describe('plugin presets', () => {
  beforeEach(() => {
    clearUserPresetsForTesting();
    vi.restoreAllMocks();
  });

  it('identifies supported builtin nodes', () => {
    expect(hasPresetsFor(NODE_TYPES.loopReverb)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.eq)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.filter)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.compressor)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.gate)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.expander)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.saturator)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.clipper)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.bitcrusher)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.chorus)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.flanger)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.phaser)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.utility)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.lfo)).toBe(true);
    expect(hasPresetsFor(NODE_TYPES.adsr)).toBe(true);
    expect(hasPresetsFor('dawg.unknownNode')).toBe(false);
  });

  it('provides factory presets with valid parameters for reverb, eq, and filter', () => {
    const reverbPresets = getPresetsForNode(NODE_TYPES.loopReverb);
    expect(reverbPresets.factory.length).toBeGreaterThan(0);
    const hall = reverbPresets.factory.find((p) => p.name === 'Concert Hall');
    expect(hall).toBeDefined();
    expect(hall?.params.feedback).toBe(75);
    expect(hall?.params.size).toBe(85);

    const eqPresets = getPresetsForNode(NODE_TYPES.eq);
    expect(eqPresets.factory.length).toBeGreaterThan(0);
    const vocalAir = eqPresets.factory.find((p) => p.name.includes('Vocal Air'));
    expect(vocalAir).toBeDefined();
    expect(vocalAir?.params.hpFreq).toBe(85);

    const filterPresets = getPresetsForNode(NODE_TYPES.filter);
    expect(filterPresets.factory.length).toBeGreaterThan(0);
    const squelch = filterPresets.factory.find((p) => p.name.includes('Acid'));
    expect(squelch).toBeDefined();
    expect(squelch?.params.resonance).toBe(85);
  });

  it('saves and retrieves custom user presets', () => {
    const preset = saveUserPreset(NODE_TYPES.filter, 'My Custom Bass Filter', {
      cutoff: 440,
      resonance: 50,
      drive: 6,
      slope: 1,
      mode: 0,
    });

    expect(preset.name).toBe('My Custom Bass Filter');
    expect(preset.isUser).toBe(true);
    expect(preset.params.cutoff).toBe(440);

    const list = getUserPresets(NODE_TYPES.filter);
    expect(list.some((p) => p.id === preset.id && p.name === 'My Custom Bass Filter')).toBe(true);

    const forNode = getPresetsForNode(NODE_TYPES.filter);
    expect(forNode.user.some((p) => p.id === preset.id)).toBe(true);
  });

  it('deletes custom user presets', () => {
    const preset = saveUserPreset(NODE_TYPES.saturator, 'Temp Preset', { drive: 15 });
    expect(getUserPresets(NODE_TYPES.saturator).length).toBe(1);

    const removed = deleteUserPreset(NODE_TYPES.saturator, preset.id);
    expect(removed).toBe(true);
    expect(getUserPresets(NODE_TYPES.saturator).length).toBe(0);
  });

  it('detects matching presets from node parameter state', () => {
    const match = detectMatchingPreset(NODE_TYPES.filter, {
      mode: 0,
      cutoff: 1200,
      resonance: 25,
      drive: 2.0,
      slope: 1,
    });

    expect(match).not.toBeNull();
    expect(match?.preset.name).toBe('Warm 24dB Lowpass');
    expect(match?.isExact).toBe(true);

    const noMatch = detectMatchingPreset(NODE_TYPES.filter, {
      cutoff: 9999,
    });
    expect(noMatch).toBeNull();
  });

  it('applies preset by batching setNodeParam commands', () => {
    const dispatchSpy = vi.spyOn(store, 'dispatch').mockImplementation(() => {});

    const mockTrack = { id: 1 } as Track;
    const mockNode = { id: 10, type: NODE_TYPES.filter, params: {} } as GraphNode;
    const preset: NodePreset = {
      id: 'test-preset',
      name: 'Test Filter',
      type: NODE_TYPES.filter,
      params: { cutoff: 500, resonance: 40 },
    };

    applyPreset(mockTrack, mockNode, preset);

    expect(dispatchSpy).toHaveBeenCalledTimes(1);
    const callArgs = dispatchSpy.mock.calls[0][0];
    expect(callArgs.command).toBe('batch');
    if (callArgs.command !== 'batch') throw new Error('expected a batch command');
    expect(callArgs.args.label).toBe('Preset: Test Filter');
    expect(callArgs.args.commands).toEqual([
      { command: 'setNodeParam', args: { trackId: 1, nodeId: 10, param: 'cutoff', value: 500 } },
      { command: 'setNodeParam', args: { trackId: 1, nodeId: 10, param: 'resonance', value: 40 } },
    ]);
  });

  it('saves an LFO shape with a preset and replaces the shape when loading it', () => {
    const shape: CurvePoint[] = [
      [0, -1, 0, 0],
      [TICKS_PER_BEAT / 2, 1, 0, 0],
      [TICKS_PER_BEAT, -1, 0, 0],
    ];

    const preset = saveUserPreset(NODE_TYPES.lfo, 'Custom Ramp', { sync: 0, rateMs: 500 }, shape);

    expect(preset.shape).toEqual(shape);
    expect(getUserPresets(NODE_TYPES.lfo)[0].shape).toEqual(shape);

    // The preset owns its copy: later edits to the node's shape must not rewrite a saved preset.
    shape[0][1] = 1;
    expect(preset.shape?.[0][1]).toBe(-1);

    const dispatchSpy = vi.spyOn(store, 'dispatch').mockImplementation(() => {});
    const mockTrack = { id: 1 } as Track;
    const mockNode = { id: 10, type: NODE_TYPES.lfo, params: {} } as GraphNode;

    applyPreset(mockTrack, mockNode, preset);

    const callArgs = dispatchSpy.mock.calls[0][0];
    if (callArgs.command !== 'batch') throw new Error('expected a batch command');
    expect(callArgs.args.commands).toContainEqual({
      command: 'setModulationShape',
      args: { trackId: 1, nodeId: 10, points: preset.shape },
    });
  });

  it('leaves the LFO shape alone when a preset carries no shape', () => {
    const dispatchSpy = vi.spyOn(store, 'dispatch').mockImplementation(() => {});
    const mockTrack = { id: 1 } as Track;
    const mockNode = { id: 10, type: NODE_TYPES.lfo, params: {} } as GraphNode;
    const preset: NodePreset = {
      id: 'rate-only',
      name: 'Rate Only',
      type: NODE_TYPES.lfo,
      params: { rateMs: 500 },
    };

    applyPreset(mockTrack, mockNode, preset);

    const callArgs = dispatchSpy.mock.calls[0][0];
    if (callArgs.command !== 'batch') throw new Error('expected a batch command');
    expect(callArgs.args.commands).toEqual([
      { command: 'setNodeParam', args: { trackId: 1, nodeId: 10, param: 'rateMs', value: 500 } },
    ]);
  });
});
