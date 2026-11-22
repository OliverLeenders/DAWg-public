import { beforeEach, describe, expect, it, vi } from 'vitest';
import { NODE_TYPES, type Command, type DrumPad, type GraphNode, type ParamDescriptor, type Track } from '../bridge/protocol';
import * as store from '../store/store';
import { COMMON_PARAMS, DRUM_KITS, DRUM_PAD_PRESETS, MODEL_PARAMS, kitCommands, padPresetCommands, padPresetsFor } from './drumKits';
import { parseDrumParam, padsOf } from './drumSynth';
import { applyPreset, clearUserPresetsForTesting, getPresetsForNode, getUserPresets, hasPresetsFor, saveUserPreset } from './presets';

// The Drum Synth's kits and pad sounds (docs/design/drum-synth.md, DR13, step 6).

const target = { trackId: 1, nodeId: 9 };
const node = (params: Record<string, number> = {}, pads?: DrumPad[]) => ({ id: 9, type: NODE_TYPES.drumSynth, params, pads }) as unknown as GraphNode;
const param = (id: string, def: number): ParamDescriptor => ({ id, name: id, min: 0, max: 20000, default: def, kind: 'continuous' }) as unknown as ParamDescriptor;

// A slice of the descriptor: pad 1 (a kick) and pad 3 (a snare), with their defaults.
const params = [
  param('p1.level', 0),
  param('p1.decay', 100),
  param('p1.tune', 0),
  ...MODEL_PARAMS.kick.map((id) => param(`p1.kick.${id}`, 10)),
  param('p1.snare.pitch', 185),
  param('p3.decay', 100),
  param('p3.tune', 0),
  ...MODEL_PARAMS.snare.map((id) => param(`p3.snare.${id}`, 20)),
];

const settingsOf = (commands: Command[]) =>
  Object.fromEntries(commands.filter((c) => c.command === 'setNodeParam').map((c) => [(c.args as { param: string }).param, (c.args as { value: number }).value]));

describe('Drum Synth kits and pad sounds', () => {
  beforeEach(() => {
    clearUserPresetsForTesting();
    vi.restoreAllMocks();
  });

  it('name only parameters their pads and models have', () => {
    for (const kit of DRUM_KITS) {
      expect(kit.drumPads).toHaveLength(16);
      for (const id of Object.keys(kit.params)) {
        const parsed = parseDrumParam(id);
        expect(parsed, `${kit.name} ${id}`).not.toBeNull();
        const pad = kit.drumPads[parsed!.pad];
        if (parsed!.model) {
          expect(parsed!.model, `${kit.name} ${id}`).toBe(pad.model);
          expect(MODEL_PARAMS[parsed!.model as keyof typeof MODEL_PARAMS]).toContain(parsed!.id);
        } else {
          expect(COMMON_PARAMS).toContain(parsed!.id);
        }
      }
    }
    for (const sound of DRUM_PAD_PRESETS)
      for (const id of Object.keys(sound.settings)) expect([...MODEL_PARAMS[sound.model], 'decay', 'tune'], `${sound.name} ${id}`).toContain(id);
  });

  it('are named by character, the machines only in descriptions', () => {
    expect(DRUM_KITS.map((k) => k.name)).toEqual(['Boom', 'Punch', 'Electro', 'Acoustic', 'Lo-fi']);
    expect(DRUM_KITS.some((k) => /808|909/.test(k.name))).toBe(false);
    expect(DRUM_KITS[0].description).toMatch(/808/);
    expect(hasPresetsFor(NODE_TYPES.drumSynth)).toBe(true);
    expect(getPresetsForNode(NODE_TYPES.drumSynth).factory).toHaveLength(5);
    expect(padPresetsFor('kick').length).toBeGreaterThan(1);
    expect(padPresetsFor('off')).toHaveLength(0);
    expect(new Set(DRUM_PAD_PRESETS.map((p) => p.id)).size).toBe(DRUM_PAD_PRESETS.length);
  });

  it('load a kit as the whole node: what it leaves out goes back to its default', () => {
    const before = node({ 'p1.kick.pitch': 70, 'p1.kick.drive': 4, 'p3.snare.pitch': 185 });
    const commands = kitCommands(target, before, params, { params: { 'p1.kick.pitch': 45 }, drumPads: padsOf(before) });
    expect(settingsOf(commands)).toEqual({ 'p1.kick.pitch': 45, 'p1.kick.drive': 10, 'p3.snare.pitch': 20 });
    // Only what changes; the same kit twice is nothing.
    const loaded = node({ 'p1.kick.pitch': 45 }, padsOf(before));
    const again = kitCommands(target, { ...loaded, params: { ...settingsOf(commands) } } as GraphNode, params, { params: { 'p1.kick.pitch': 45 }, drumPads: padsOf(before) });
    expect(again).toHaveLength(0);
  });

  it("load a kit's pads but keep each pad's output and lock", () => {
    const pads = padsOf(node()).map((p, i) => (i === 2 ? { ...p, output: 4, locked: true } : p));
    const kitPads = padsOf(node()).map((p, i) => (i === 2 ? { ...p, name: 'Crack', model: 'clap' as const, choke: 2 } : p));
    const commands = kitCommands(target, node({}, pads), [], { params: {}, drumPads: kitPads });
    expect(commands).toEqual([{ command: 'setDrumPad', args: { ...target, pad: 2, name: 'Crack', model: 'clap', chokeGroup: 2, mono: false } }]);
  });

  it("load a pad sound onto one pad: its model's parameters, Decay and Tune, nothing else", () => {
    const snare = DRUM_PAD_PRESETS.find((p) => p.id === 'snare-fat')!;
    const before = node({ 'p3.level': -6, 'p3.tune': 3 });
    const commands = padPresetCommands(target, before, params, 2, snare);
    const set = settingsOf(commands);
    expect(set['p3.snare.pitch']).toBe(170);
    expect(set['p3.decay']).toBe(140);
    expect(set['p3.tune']).toBe(0); // back to the default
    expect(set['p3.level']).toBeUndefined();
    expect(commands.some((c) => c.command === 'setDrumPad')).toBe(false); // already a snare

    // On a pad playing another model, the pad changes model first.
    const kick = DRUM_PAD_PRESETS.find((p) => p.id === 'kick-punch')!;
    const onSnare = padPresetCommands(target, node(), params, 2, kick);
    expect(onSnare[0]).toEqual({ command: 'setDrumPad', args: { ...target, pad: 2, model: 'kick' } });
  });

  it('apply a kit preset as one undo step, and save the pads with a user kit', () => {
    const dispatch = vi.spyOn(store, 'dispatch').mockImplementation(() => {});
    store.useDawg.setState({ nodeTypes: { [NODE_TYPES.drumSynth]: { type: NODE_TYPES.drumSynth, params } } } as never);
    applyPreset({ id: 1 } as Track, node(), DRUM_KITS[1]);
    expect(dispatch).toHaveBeenCalledTimes(1);
    const call = dispatch.mock.calls[0][0];
    expect(call.command).toBe('batch');
    if (call.command !== 'batch') throw new Error('expected a batch');
    expect(call.args.label).toBe('Preset: Punch');
    expect(settingsOf(call.args.commands)['p1.kick.click']).toBe(70);

    const pads = padsOf(node()).map((p, i) => (i === 0 ? { ...p, name: 'Big kick' } : p));
    saveUserPreset(NODE_TYPES.drumSynth, 'Mine', { 'p1.kick.pitch': 52 }, undefined, pads);
    expect(getUserPresets(NODE_TYPES.drumSynth)[0].drumPads?.[0].name).toBe('Big kick');
  });
});
