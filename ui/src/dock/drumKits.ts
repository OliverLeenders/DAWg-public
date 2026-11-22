import { NODE_TYPES, type Command, type DrumModel, type DrumPad, type GraphNode, type ParamDescriptor } from '../bridge/protocol';
import { drumParamId, padsOf } from './drumSynth';

// The Drum Synth's presets (docs/design/drum-synth.md, DR13): kits, which are the whole node (its sixteen pads and
// every parameter), and pad sounds, which are one model's settings for one pad. Factory kits are named by their
// character; "808-style" and "909-style" appear only in their descriptions.

/** Each model's own parameters, as in DrumVoices.cpp, for checking presets against. */
export const MODEL_PARAMS: Record<Exclude<DrumModel, 'off'>, string[]> = {
  kick: ['pitch', 'sweep', 'sweepTime', 'hold', 'click', 'clickTone', 'drive'],
  snare: ['pitch', 'sweep', 'tone', 'snappy', 'noiseDecay', 'noiseTone'],
  tom: ['pitch', 'sweep', 'sweepTime', 'overtones', 'stick'],
  hat: ['tone', 'colour', 'noise', 'attack'],
  cymbal: ['tone', 'colour', 'wash', 'sizzle', 'ride', 'swell', 'width'],
  clap: ['tone', 'spread', 'bursts', 'tail'],
  rim: ['pitch', 'tone', 'click'],
};

/** The parameters every pad has. */
export const COMMON_PARAMS = ['level', 'pan', 'tune', 'decay', 'velocity'];

/** A model's settings: its own parameters, and the common ones that make its character (Decay, Tune). */
type Settings = Record<string, number>;

/** A pad sound (DR13): one model's settings, loaded onto a pad. */
export interface DrumPadPreset {
  id: string;
  name: string;
  model: Exclude<DrumModel, 'off'>;
  settings: Settings;
}

/** A factory kit before it becomes a NodePreset: settings by model for every pad that plays it, then by pad. */
interface KitSpec {
  id: string;
  name: string;
  description: string;
  byModel: Partial<Record<Exclude<DrumModel, 'off'>, Settings>>;
  byPad?: Record<number, Settings>;
}

/** A kit as a node preset: its pads, and the parameters it sets (the rest load as their defaults). */
export interface DrumKitPreset {
  id: string;
  name: string;
  type: string;
  category?: string;
  description?: string;
  params: Record<string, number>;
  drumPads: DrumPad[];
  isUser?: boolean;
}

const settingId = (pad: number, model: DrumModel, id: string) => (COMMON_PARAMS.includes(id) ? drumParamId(pad, id) : drumParamId(pad, id, model));

function kit(spec: KitSpec): DrumKitPreset {
  const pads = padsOf({ type: NODE_TYPES.drumSynth, params: {} } as unknown as GraphNode);
  const params: Record<string, number> = {};
  pads.forEach((pad, i) => {
    if (pad.model === 'off') return;
    for (const [id, value] of Object.entries({ ...spec.byModel[pad.model], ...spec.byPad?.[i] })) params[settingId(i, pad.model, id)] = value;
  });
  return { id: spec.id, name: spec.name, type: NODE_TYPES.drumSynth, category: 'Kits', description: spec.description, params, drumPads: pads };
}

// The pads of the default kit, by what they are: the hats' decays differ by pad, and the crash and the ride are one
// model at two settings of Ride, so a kit sets each by its pad.
const CLOSED_HAT = 6;
const PEDAL_HAT = 8;
const OPEN_HAT = 10;
const CRASH = 13;
const RIDE = 15;

export const DRUM_KITS: DrumKitPreset[] = [
  kit({
    id: 'drums-boom',
    name: 'Boom',
    description: '808-style: a long, deep kick, soft snare, thin hats',
    byModel: {
      kick: { pitch: 45, sweep: 12, sweepTime: 60, click: 10, drive: 2, decay: 300 },
      snare: { pitch: 180, tone: 40, snappy: 55, noiseTone: 6000 },
      tom: { sweep: 6, sweepTime: 200, overtones: 20, stick: 10, decay: 160 },
      hat: { tone: 8000, colour: 50, noise: 15 },
      clap: { tail: 70 },
    },
    byPad: {
      [CLOSED_HAT]: { decay: 30 },
      [OPEN_HAT]: { decay: 260 },
      [CRASH]: { tone: 5000, wash: 30, sizzle: 35 },
      [RIDE]: { tone: 7500, wash: 20 },
    },
  }),
  kit({
    id: 'drums-punch',
    name: 'Punch',
    description: '909-style: a short kick with a hard click, a bright snare, crisp hats',
    byModel: {
      kick: { pitch: 55, sweep: 30, sweepTime: 20, click: 70, clickTone: 5000, drive: 6, decay: 70 },
      snare: { pitch: 210, tone: 60, snappy: 75, noiseTone: 7000, noiseDecay: 220 },
      tom: { sweep: 5, sweepTime: 90, overtones: 35, stick: 40 },
      hat: { tone: 9000, colour: 65, noise: 40 },
    },
    byPad: { [CRASH]: { tone: 7500, wash: 45, sizzle: 65 }, [RIDE]: { tone: 10000, sizzle: 20 } },
  }),
  kit({
    id: 'drums-electro',
    name: 'Electro',
    description: 'Tight and synthetic: a zappy kick, a pitched snare, quick claps',
    byModel: {
      kick: { pitch: 60, sweep: 36, sweepTime: 12, click: 30, drive: 9, decay: 60 },
      snare: { pitch: 240, sweep: 8, tone: 45, snappy: 50, noiseDecay: 120 },
      tom: { sweep: 10, sweepTime: 80, overtones: 10, stick: 0 },
      hat: { tone: 10000, colour: 80, noise: 10 },
      clap: { spread: 8, bursts: 4, tail: 40, tone: 1500 },
    },
  }),
  kit({
    id: 'drums-acoustic',
    name: 'Acoustic',
    description: 'Closer to a real kit: a dry kick, a snappy snare, ringing toms, a wide crash',
    byModel: {
      kick: { pitch: 58, sweep: 8, sweepTime: 25, click: 50, clickTone: 3000, drive: 0, decay: 50 },
      snare: { pitch: 190, sweep: 2, tone: 55, snappy: 70, noiseTone: 4500, noiseDecay: 260 },
      tom: { sweep: 3, sweepTime: 150, overtones: 70, stick: 50 },
      hat: { tone: 6500, colour: 40, noise: 35 },
    },
    byPad: { [CRASH]: { decay: 130, wash: 40, width: 75 }, [RIDE]: { colour: 40, width: 50 } },
  }),
  kit({
    id: 'drums-lofi',
    name: 'Lo-fi',
    description: 'Dark and driven: a saturated kick, dull hats and cymbals',
    byModel: {
      kick: { pitch: 48, sweep: 18, drive: 12, click: 20, clickTone: 2000 },
      snare: { noiseTone: 3000, tone: 50 },
      hat: { tone: 5000, colour: 30, noise: 20 },
      cymbal: { tone: 4000, colour: 40 },
      rim: { tone: 30 },
    },
    byPad: { [PEDAL_HAT]: { decay: 20 } },
  }),
];

export const DRUM_PAD_PRESETS: DrumPadPreset[] = [
  { id: 'kick-boom', name: 'Long boom', model: 'kick', settings: { pitch: 45, sweep: 12, sweepTime: 60, hold: 40, click: 5, drive: 2, decay: 300 } },
  { id: 'kick-punch', name: 'Punchy', model: 'kick', settings: { pitch: 55, sweep: 30, sweepTime: 20, hold: 0, click: 70, clickTone: 5000, drive: 6, decay: 70 } },
  { id: 'kick-thud', name: 'Thud', model: 'kick', settings: { pitch: 60, sweep: 8, sweepTime: 25, hold: 0, click: 50, clickTone: 3000, drive: 0, decay: 50 } },
  { id: 'kick-dirty', name: 'Distorted', model: 'kick', settings: { pitch: 48, sweep: 18, sweepTime: 40, hold: 10, click: 20, clickTone: 2000, drive: 15 } },
  { id: 'snare-tight', name: 'Tight', model: 'snare', settings: { pitch: 230, sweep: 4, tone: 55, snappy: 65, noiseDecay: 120, noiseTone: 7000, decay: 70 } },
  { id: 'snare-fat', name: 'Fat', model: 'snare', settings: { pitch: 170, sweep: 3, tone: 40, snappy: 60, noiseDecay: 300, noiseTone: 4500, decay: 140 } },
  { id: 'snare-noisy', name: 'Noisy', model: 'snare', settings: { pitch: 200, sweep: 2, tone: 80, snappy: 90, noiseDecay: 250, noiseTone: 6000 } },
  { id: 'tom-floor', name: 'Floor', model: 'tom', settings: { pitch: 80, sweep: 4, sweepTime: 200, overtones: 50, stick: 30, decay: 150 } },
  { id: 'tom-rack', name: 'Rack', model: 'tom', settings: { pitch: 150, sweep: 3, sweepTime: 120, overtones: 60, stick: 45 } },
  { id: 'tom-electronic', name: 'Electronic', model: 'tom', settings: { pitch: 120, sweep: 12, sweepTime: 80, overtones: 0, stick: 0 } },
  { id: 'hat-closed', name: 'Closed, tight', model: 'hat', settings: { tone: 8000, colour: 55, noise: 25, attack: 0, decay: 30 } },
  { id: 'hat-open', name: 'Open, washy', model: 'hat', settings: { tone: 7000, colour: 50, noise: 40, attack: 0, decay: 300 } },
  { id: 'hat-bright', name: 'Bright', model: 'hat', settings: { tone: 10000, colour: 80, noise: 15 } },
  { id: 'hat-dark', name: 'Dark', model: 'hat', settings: { tone: 5000, colour: 25, noise: 30 } },
  { id: 'cymbal-crash', name: 'Crash', model: 'cymbal', settings: { tone: 6000, colour: 60, wash: 35, sizzle: 50, ride: 0, swell: 60, width: 60, decay: 100 } },
  { id: 'cymbal-dark', name: 'Dark crash', model: 'cymbal', settings: { tone: 3500, colour: 35, wash: 45, sizzle: 30, ride: 0, swell: 80, width: 75, decay: 140 } },
  { id: 'cymbal-splash', name: 'Splash', model: 'cymbal', settings: { tone: 8000, colour: 70, wash: 30, sizzle: 60, ride: 0, swell: 30, width: 50, decay: 40 } },
  { id: 'cymbal-ride', name: 'Ride', model: 'cymbal', settings: { tone: 9000, colour: 50, wash: 25, sizzle: 12, ride: 100, swell: 0, width: 40, decay: 100 } },
  { id: 'cymbal-dry-ride', name: 'Dry ride', model: 'cymbal', settings: { tone: 10000, colour: 45, wash: 10, sizzle: 5, ride: 100, swell: 0, width: 30, decay: 70 } },
  { id: 'clap-room', name: 'Big room', model: 'clap', settings: { tone: 1100, spread: 14, bursts: 4, tail: 85 } },
  { id: 'clap-tight', name: 'Tight', model: 'clap', settings: { tone: 1500, spread: 7, bursts: 3, tail: 30 } },
  { id: 'rim-stick', name: 'Side stick', model: 'rim', settings: { pitch: 900, tone: 50, click: 60 } },
  { id: 'rim-wood', name: 'Woody', model: 'rim', settings: { pitch: 600, tone: 25, click: 30 } },
];

/** The pad sounds for a model. */
export const padPresetsFor = (model: DrumModel) => DRUM_PAD_PRESETS.filter((p) => p.model === model);

type Target = { trackId: number; nodeId: number };

/** The commands that load a kit, as one step: every parameter to the kit's value or else its default (only those that
    change), and each pad's name, model, choke group and Mono. Outputs and locks stay, so a split kit stays split. */
export function kitCommands(target: Target, node: GraphNode, params: ParamDescriptor[], kit: { params: Record<string, number>; drumPads?: DrumPad[] }): Command[] {
  const commands: Command[] = [];
  for (const param of params) {
    const value = kit.params[param.id] ?? param.default;
    if ((node.params[param.id] ?? param.default) !== value) commands.push({ command: 'setNodeParam', args: { ...target, param: param.id, value } });
  }
  const now = padsOf(node);
  (kit.drumPads ?? []).forEach((pad, i) => {
    const was = now[i];
    if (!was || (was.name === pad.name && was.model === pad.model && was.choke === pad.choke && was.mono === pad.mono)) return;
    commands.push({ command: 'setDrumPad', args: { ...target, pad: i, name: pad.name, model: pad.model, chokeGroup: pad.choke, mono: pad.mono } });
  });
  return commands;
}

/** The commands that load a pad sound onto a pad, as one step: the pad plays its model, and that model's parameters
    (and Decay and Tune) take the sound's values or else their defaults. Level, Pan and Velocity stay. */
export function padPresetCommands(target: Target, node: GraphNode, params: ParamDescriptor[], pad: number, preset: DrumPadPreset): Command[] {
  const commands: Command[] = [];
  if (padsOf(node)[pad]?.model !== preset.model) commands.push({ command: 'setDrumPad', args: { ...target, pad, model: preset.model } });
  const ids = [...MODEL_PARAMS[preset.model].map((id) => drumParamId(pad, id, preset.model)), drumParamId(pad, 'decay'), drumParamId(pad, 'tune')];
  for (const id of ids) {
    const param = params.find((p) => p.id === id);
    if (!param) continue;
    const short = id.slice(id.lastIndexOf('.') + 1);
    const value = preset.settings[short] ?? param.default;
    if ((node.params[id] ?? param.default) !== value) commands.push({ command: 'setNodeParam', args: { ...target, param: id, value } });
  }
  return commands;
}
