import { describe, expect, it } from 'vitest';
import { NODE_TYPES } from '../bridge/protocol';
import { builtinFolders, FOLDERS, intoFolders, PATCH_FOLDERS, pluginFolders, type FolderKey } from './addFolders';
import { PATCHES } from '../graph/patches';

describe('pluginFolders', () => {
  it("files a VST3 by its subcategories, in each one it names", () => {
    expect(pluginFolders('Fx|EQ', false)).toEqual(['eq']);
    expect(pluginFolders('Fx|Dynamics|Distortion', false)).toEqual(['dynamics', 'distortion']);
    expect(pluginFolders('Fx|Pitch Shift', false)).toEqual(['pitch']);
    expect(pluginFolders('Instrument|Synth', true)).toEqual(['synth']);
    expect(pluginFolders('Instrument|Drum', true)).toEqual(['drums']);
  });

  it("files a CLAP by its features", () => {
    expect(pluginFolders('audio-effect|equalizer|stereo', false)).toEqual(['eq']);
    expect(pluginFolders('instrument|synthesizer|stereo', true)).toEqual(['synth']);
    expect(pluginFolders('audio-effect|compressor|limiter', false)).toEqual(['dynamics']);
  });

  it('puts what its category says nothing of in Other or Other instruments', () => {
    expect(pluginFolders('Fx', false)).toEqual(['other']);
    expect(pluginFolders('Effect', false)).toEqual(['other']);
    expect(pluginFolders('Instrument', true)).toEqual(['instrument']);
    // An effect that calls itself a sampler is still an effect, and an instrument with an EQ still an instrument.
    expect(pluginFolders('Fx|Sampler', false)).toEqual(['other']);
    expect(pluginFolders('Instrument|EQ', true)).toEqual(['instrument']);
  });
});

describe('builtinFolders', () => {
  it("puts DAWg's own nodes in Built in and in what they are", () => {
    expect(builtinFolders({ type: NODE_TYPES.saturator, category: 'effect' })).toEqual(['builtin', 'distortion', 'saturation']);
    expect(builtinFolders({ type: NODE_TYPES.gain, category: 'utility' })).toEqual(['builtin', 'utility']);
    expect(builtinFolders({ type: NODE_TYPES.drumSynth, category: 'instrument' })).toEqual(['builtin', 'drums', 'synth']);
  });

  it('files a type it does not know by its category', () => {
    expect(builtinFolders({ type: 'dawg.new', category: 'instrument' })).toEqual(['builtin', 'instrument']);
    expect(builtinFolders({ type: 'dawg.new', category: 'midiEffect' })).toEqual(['builtin', 'midiEffect']);
    expect(builtinFolders({ type: 'dawg.new', category: 'effect' })).toEqual(['builtin', 'other']);
  });

  it('files every factory patch', () => {
    for (const patch of PATCHES) expect(PATCH_FOLDERS[patch.key], patch.key).toContain('builtin');
  });
});

describe('intoFolders', () => {
  const item = (name: string, ...folders: FolderKey[]) => ({ name, folders });

  it('lists the folders that hold something in menu order, each sorted by name', () => {
    const folders = intoFolders([item('Saturator', 'builtin', 'distortion', 'saturation'), item('Amp', 'distortion'), item('EQ', 'builtin', 'eq')]);
    expect(folders.map((f) => f.key)).toEqual(['builtin', 'eq', 'distortion', 'saturation']);
    expect(folders.find((f) => f.key === 'distortion')?.items.map((i) => i.name)).toEqual(['Amp', 'Saturator']);
  });

  it('starts with Built in', () => {
    expect(FOLDERS[0]).toEqual({ key: 'builtin', name: 'Built in' });
  });
});
