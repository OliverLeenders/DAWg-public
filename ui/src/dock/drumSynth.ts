import type { DrumModel, DrumPad, GraphNode, ParamDescriptor } from '../bridge/protocol';
import { padKeys } from './samplerLayout';

// The Drum Synth's pads and parameters for its panel (docs/design/drum-synth.md, DR12), apart from the drawing. A pad's
// sound is in the node's parameters: `p{n}.{id}` for the five every pad has, `p{n}.{model}.{id}` for each model's
// own (DR3 as built: every model's on every pad, the pad's current model's shown).

export const DRUM_PAD_COUNT = 16;
export const FIRST_DRUM_KEY = 36;

/** The pads' keys in reading order, as the Sampler's: the lowest at the bottom left. */
export const DRUM_PAD_KEYS = padKeys();

export const DRUM_MODELS: Array<{ model: DrumModel; name: string }> = [
  { model: 'kick', name: 'Kick' },
  { model: 'snare', name: 'Snare' },
  { model: 'tom', name: 'Tom' },
  { model: 'hat', name: 'Hat' },
  { model: 'cymbal', name: 'Cymbal' },
  { model: 'clap', name: 'Clap' },
  { model: 'rim', name: 'Rim' },
  { model: 'off', name: 'Off' },
];

export const modelName = (model: DrumModel) => DRUM_MODELS.find((m) => m.model === model)?.name ?? model;

/** The default kit, as the engine makes it (DrumSynth.cpp): General MIDI's keys 36-51. */
const DEFAULT_KIT: Array<[string, DrumModel, number, boolean]> = [
  ['Kick', 'kick', 0, true],
  ['Rim', 'rim', 0, false],
  ['Snare', 'snare', 0, false],
  ['Clap', 'clap', 0, false],
  ['Snare 2', 'snare', 0, false],
  ['Low floor tom', 'tom', 0, true],
  ['Closed hat', 'hat', 1, false],
  ['High floor tom', 'tom', 0, true],
  ['Pedal hat', 'hat', 1, false],
  ['Low tom', 'tom', 0, true],
  ['Open hat', 'hat', 1, false],
  ['Low-mid tom', 'tom', 0, true],
  ['High-mid tom', 'tom', 0, true],
  ['Crash', 'cymbal', 0, false],
  ['High tom', 'tom', 0, true],
  ['Ride', 'cymbal', 0, false],
];

/** A node's pads: its own, or the default kit's when it has none. */
export function padsOf(node: GraphNode): DrumPad[] {
  if (node.pads?.length === DRUM_PAD_COUNT) return node.pads;
  return DEFAULT_KIT.map(([name, model, choke, mono]) => ({ name, model, output: 1, choke, mono, locked: false }));
}

/** The pad a key plays, or -1. */
export const padOfKey = (key: number) => (key >= FIRST_DRUM_KEY && key < FIRST_DRUM_KEY + DRUM_PAD_COUNT ? key - FIRST_DRUM_KEY : -1);

/** A pad's name for showing: its own, or its model's. */
export const padName = (pad: DrumPad) => pad.name || modelName(pad.model);

/** A pad parameter's id: a common one without a model, a model's own with it. Pads count from 0. */
export const drumParamId = (pad: number, id: string, model?: DrumModel) => (model ? `p${pad + 1}.${model}.${id}` : `p${pad + 1}.${id}`);

/** What a pad parameter's id says: its pad from 0, its model (absent for a common one) and its id; null for another id. */
export function parseDrumParam(paramId: string): { pad: number; model?: DrumModel; id: string } | null {
  const match = /^p(\d+)\.(?:([a-z]+)\.)?([A-Za-z]+)$/.exec(paramId);
  if (!match) return null;
  const pad = Number(match[1]) - 1;
  if (pad < 0 || pad >= DRUM_PAD_COUNT) return null;
  return { pad, model: match[2] as DrumModel | undefined, id: match[3] };
}

/** Whether a Drum Synth's parameter does anything: its pad plays, and it is common or its pad's model's. Lists of
    parameters (the inspector, automation, modulation targets) show only these. */
export function drumParamApplies(node: GraphNode, paramId: string) {
  const parsed = parseDrumParam(paramId);
  if (!parsed) return true;
  const pad = padsOf(node)[parsed.pad];
  return pad.model !== 'off' && (parsed.model === undefined || parsed.model === pad.model);
}

/** A parameter's name without its "Pad 3 Snare " prefix: "Snappy". */
export function shortParamName(param: ParamDescriptor) {
  const parsed = parseDrumParam(param.id);
  if (!parsed) return param.name;
  const prefix = `Pad ${parsed.pad + 1} ${parsed.model ? `${modelName(parsed.model)} ` : ''}`;
  return param.name.startsWith(prefix) ? param.name.slice(prefix.length) : param.name;
}

/** A parameter's name with its pad's name: "Crack Snappy" in lanes and menus. */
export function drumParamLabel(node: GraphNode, param: ParamDescriptor) {
  const parsed = parseDrumParam(param.id);
  if (!parsed) return param.name;
  return `${padName(padsOf(node)[parsed.pad])} ${shortParamName(param)}`;
}

/** A pad's parameters, common and its model's own, in the descriptor's order. */
export function padParams(params: ParamDescriptor[], pad: number, model: DrumModel) {
  const common: ParamDescriptor[] = [];
  const own: ParamDescriptor[] = [];
  for (const param of params) {
    const parsed = parseDrumParam(param.id);
    if (!parsed || parsed.pad !== pad) continue;
    if (parsed.model === undefined) common.push(param);
    else if (parsed.model === model) own.push(param);
  }
  return { common, own };
}

/** How many outputs the graph shows: Main, and up to the highest a pad plays from or a cable leaves (MO1). */
export function drumOutputsShown(node: GraphNode, wiredPorts: number[]) {
  return Math.max(1, ...padsOf(node).map((p) => p.output), ...wiredPorts.map((port) => port + 1));
}

/** How many of Out 2 to Out 16 no pad plays from and no cable leaves: how many pads can still get tracks of their own. */
export function freeOutputs(pads: DrumPad[], wiredPorts: number[]) {
  const used = new Set([...pads.map((p) => p.output).filter((o) => o > 1), ...wiredPorts.filter((p) => p > 0).map((p) => p + 1)]);
  return DRUM_PAD_COUNT - 1 - used.size;
}

/** The pads that were hit since the last counts, from the node status's `hits` (DR10). */
export function padsHit(before: number[] | undefined, after: number[] | undefined) {
  const hit = new Set<number>();
  if (!after) return hit;
  after.forEach((count, pad) => {
    if (before && count > (before[pad] ?? 0)) hit.add(pad);
  });
  return hit;
}

// The models' decays at Decay 100 %, in seconds, as in DrumVoices.cpp; a cymbal's goes from a crash's 2.5 s to a
// ride's 4 s with Ride.
const BASE_DECAY: Record<DrumModel, number> = { off: 0, kick: 0.45, snare: 0.15, tom: 0.4, hat: 0.15, cymbal: 2.5, clap: 0.25, rim: 0.06 };

/** A pad's envelopes for the panel's picture (DR12): its level, falling to -60 dB at its decay after any hold and attack,
    and its pitch sweep in semitones, at `count` points from 0 to `seconds`. */
export function padEnvelopes(model: DrumModel, values: (id: string) => number, seconds: number, count = 96) {
  const base = model === 'cymbal' ? 2.5 + (1.5 * values('ride')) / 100 : BASE_DECAY[model];
  const decay = (base * values('decay')) / 100;
  const hold = model === 'kick' ? values('hold') / 1000 : 0;
  const attack = model === 'hat' ? values('attack') / 1000 : 0; // a cymbal's modes ring at once; only its wash swells
  const sweep = model === 'kick' || model === 'tom' || model === 'snare' ? values('sweep') : 0;
  const sweepTime = model === 'kick' || model === 'tom' ? values('sweepTime') / 1000 : 0.015;
  const level: Array<{ t: number; v: number }> = [];
  const pitch: Array<{ t: number; v: number }> = [];
  // A cymbal's wash under its modes, as DrumVoices.cpp shapes it: a rise over 10 ms plus half the Swell, then a fast
  // fall (0.12 of the decay) for 60 % of it and the whole decay for the rest, at the Wash's height.
  const wash: Array<{ t: number; v: number }> = [];
  const washRise = 0.01 + values('swell') / 2000;
  const washHeight = values('wash') / 100;
  for (let i = 0; i < count; i++) {
    const t = (seconds * i) / (count - 1);
    let v: number;
    if (t < attack) v = t / attack;
    else if (t < attack + hold) v = 1;
    else v = decay > 0 ? Math.pow(10, (-3 * (t - attack - hold)) / decay) : 0;
    level.push({ t, v });
    pitch.push({ t, v: sweep * Math.exp(-t / Math.max(1e-3, sweepTime)) });
    if (model === 'cymbal' && decay > 0) {
      const fall = 0.6 * Math.pow(10, (-3 * t) / (0.12 * decay)) + 0.4 * Math.pow(10, (-3 * t) / decay);
      wash.push({ t, v: washHeight * Math.min(1, t / washRise) * fall });
    }
  }
  return { level, pitch, wash, decay: attack + hold + decay, sweep };
}
