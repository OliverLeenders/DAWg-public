import { NODE_TYPES, type NodeTypeInfo } from '../bridge/protocol';

// The folders of the menus that add a node (PluginPicker): what each instrument or effect is, so a long list of plugins
// reads like a shop's shelves. One thing may sit in several folders (a clipper is dynamics and distortion). Plugins are
// filed by their own categories: a VST3's subcategories ("Fx|EQ|Dynamics") and a CLAP's features
// ("audio-effect|equalizer"), which the scan joins the same way.

/** The folders, in the order the menu lists them. */
export const FOLDERS = [
  { key: 'builtin', name: 'Built in' },
  { key: 'synth', name: 'Synths' },
  { key: 'sampler', name: 'Samplers' },
  { key: 'drums', name: 'Drums' },
  { key: 'instrument', name: 'Other instruments' },
  { key: 'midiEffect', name: 'MIDI effects' },
  { key: 'eq', name: 'EQ' },
  { key: 'dynamics', name: 'Dynamics' },
  { key: 'filter', name: 'Filter' },
  { key: 'distortion', name: 'Distortion' },
  { key: 'saturation', name: 'Saturation' },
  { key: 'modulation', name: 'Modulation' },
  { key: 'delay', name: 'Delay' },
  { key: 'reverb', name: 'Reverb' },
  { key: 'pitch', name: 'Pitch' },
  { key: 'spatial', name: 'Spatial' },
  { key: 'utility', name: 'Utility' },
  { key: 'routing', name: 'Routing' },
  { key: 'modulators', name: 'Modulators' },
  { key: 'components', name: 'Your components' },
  { key: 'faust', name: 'Faust' },
  { key: 'other', name: 'Other' },
] as const;

export type FolderKey = (typeof FOLDERS)[number]['key'];

/** A plugin category's words, lower case, and the folders each one files a plugin in. Words not here (a VST3's "Fx",
    "Stereo" or "OnlyRT", a CLAP's "audio-effect" or "mono") say nothing about what the plugin does. */
const PLUGIN_WORDS: Record<string, FolderKey[]> = {
  synth: ['synth'],
  synthesizer: ['synth'],
  'instrument synth': ['synth'],
  sampler: ['sampler'],
  drum: ['drums'],
  drums: ['drums'],
  'drum-machine': ['drums'],
  eq: ['eq'],
  equalizer: ['eq'],
  dynamics: ['dynamics'],
  compressor: ['dynamics'],
  limiter: ['dynamics'],
  gate: ['dynamics'],
  expander: ['dynamics'],
  'transient-shaper': ['dynamics'],
  'de-esser': ['dynamics'],
  'channel strip': ['eq', 'dynamics'],
  filter: ['filter'],
  distortion: ['distortion'],
  saturation: ['saturation'],
  saturator: ['saturation'],
  modulation: ['modulation'],
  chorus: ['modulation'],
  flanger: ['modulation'],
  phaser: ['modulation'],
  tremolo: ['modulation'],
  'rotary-speaker': ['modulation'],
  'ring-modulator': ['modulation'],
  'frequency-shifter': ['modulation', 'pitch'],
  delay: ['delay'],
  reverb: ['reverb'],
  'pitch shift': ['pitch'],
  'pitch-shifter': ['pitch'],
  'pitch-correction': ['pitch'],
  spatial: ['spatial'],
  surround: ['spatial'],
  ambisonics: ['spatial'],
  ambisonic: ['spatial'],
  tools: ['utility'],
  utility: ['utility'],
  analyzer: ['utility'],
  restoration: ['utility'],
};

/** A plugin's folders from its category. An instrument the category says nothing more of is another instrument; an
    effect, another effect. */
export function pluginFolders(category: string, isInstrument: boolean): FolderKey[] {
  const folders = new Set<FolderKey>();
  for (const word of category.toLowerCase().split('|')) for (const folder of PLUGIN_WORDS[word.trim()] ?? []) folders.add(folder);
  const instrumentFolders: FolderKey[] = ['synth', 'sampler', 'drums'];
  // An instrument goes only in instrument folders, an effect only in effect folders: a "Sampler" effect is no sampler.
  const kept = [...folders].filter((f) => instrumentFolders.includes(f) === isInstrument);
  return kept.length > 0 ? kept : [isInstrument ? 'instrument' : 'other'];
}

/** DAWg's own nodes' folders, besides Built in. */
const BUILTIN_FOLDERS: Record<string, FolderKey[]> = {
  [NODE_TYPES.mpeSynth]: ['synth'],
  [NODE_TYPES.voiceInstrument]: ['synth'],
  [NODE_TYPES.oscillator]: ['synth'],
  [NODE_TYPES.sampler]: ['sampler'],
  [NODE_TYPES.sampleVoice]: ['sampler'],
  [NODE_TYPES.drumSynth]: ['drums', 'synth'],
  [NODE_TYPES.eq]: ['eq'],
  [NODE_TYPES.filter]: ['filter'],
  [NODE_TYPES.compressor]: ['dynamics'],
  [NODE_TYPES.limiter]: ['dynamics'],
  [NODE_TYPES.gate]: ['dynamics'],
  [NODE_TYPES.expander]: ['dynamics'],
  [NODE_TYPES.clipper]: ['dynamics', 'distortion', 'saturation'],
  [NODE_TYPES.saturator]: ['distortion', 'saturation'],
  [NODE_TYPES.waveshaper]: ['distortion', 'saturation'],
  [NODE_TYPES.bitcrusher]: ['distortion'],
  [NODE_TYPES.chorus]: ['modulation'],
  [NODE_TYPES.flanger]: ['modulation'],
  [NODE_TYPES.phaser]: ['modulation'],
  [NODE_TYPES.delay]: ['delay'],
  [NODE_TYPES.feedback]: ['delay', 'routing'],
  [NODE_TYPES.loopReverb]: ['reverb'],
  [NODE_TYPES.convolutionReverb]: ['reverb'],
  'private.loopReverbClassic': ['reverb'],
  [NODE_TYPES.gain]: ['utility'],
  [NODE_TYPES.utility]: ['utility', 'spatial'],
  [NODE_TYPES.mix]: ['routing'],
  [NODE_TYPES.bandSplit]: ['routing'],
  [NODE_TYPES.stereoSplit]: ['routing', 'spatial'],
  [NODE_TYPES.stereoMerge]: ['routing', 'spatial'],
  [NODE_TYPES.busOutput]: ['routing'],
  [NODE_TYPES.component]: ['routing'],
  [NODE_TYPES.lfo]: ['modulators'],
  [NODE_TYPES.adsr]: ['modulators'],
  [NODE_TYPES.noteExpression]: ['modulators'],
  [NODE_TYPES.macros]: ['modulators'],
  [NODE_TYPES.faust]: ['faust'],
};

/** A built-in node's folders: Built in, and what it is. A type not filed here goes by its category. */
export function builtinFolders(info: Pick<NodeTypeInfo, 'type' | 'category'>): FolderKey[] {
  const what = BUILTIN_FOLDERS[info.type] ?? (info.category === 'instrument' ? ['instrument'] : info.category === 'midiEffect' ? ['midiEffect'] : ['other']);
  return ['builtin', ...what];
}

/** The factory patches' folders (graph/patches.ts): they are DAWg's own too. */
export const PATCH_FOLDERS: Record<string, FolderKey[]> = {
  'dub-delay': ['builtin', 'delay'],
  resonator: ['builtin', 'filter'],
  flanger: ['builtin', 'modulation'],
  'plucked-string': ['builtin', 'synth'],
  'multiband-compressor': ['builtin', 'dynamics'],
  'mid-side': ['builtin', 'utility', 'spatial'],
};

/** A folder of the menu and what is in it, sorted by name. */
export interface Folder<T> {
  key: FolderKey;
  name: string;
  items: T[];
}

/** The folders that hold something, in menu order; each item goes in every folder it names. */
export function intoFolders<T extends { name: string; folders: readonly FolderKey[] }>(items: T[]): Folder<T>[] {
  return FOLDERS.map(({ key, name }) => ({
    key,
    name,
    items: items.filter((item) => item.folders.includes(key)).sort((a, b) => a.name.localeCompare(b.name)),
  })).filter((folder) => folder.items.length > 0);
}
