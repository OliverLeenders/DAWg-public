import { NODE_TYPES, type DrumModel, type GraphNode, type Track } from '../bridge/protocol';
import { padName, padsOf } from '../dock/drumSynth';

// Drum mapping and roles for drum-specific humanisation and piano roll display.

export type DrumRole =
  | 'kick'
  | 'snare'
  | 'sidestick'
  | 'clap'
  | 'hihat_closed'
  | 'hihat_pedal'
  | 'hihat_open'
  | 'tom_floor'
  | 'tom_low'
  | 'tom_mid'
  | 'tom_high'
  | 'crash'
  | 'ride'
  | 'ride_bell'
  | 'percussion';

export type DrumLimb = 'right_foot' | 'left_foot' | 'right_hand' | 'left_hand' | 'other';

export interface DrumPiece {
  pitch: number;
  name: string;
  role: DrumRole;
  limb: DrumLimb;
}

export interface DrumMap {
  id: string;
  name: string;
  pieces: Record<number, DrumPiece>;
}

export const GENERAL_MIDI_DRUM_MAP: DrumMap = {
  id: 'gm',
  name: 'General MIDI (Standard)',
  pieces: {
    35: { pitch: 35, name: 'Acoustic Kick', role: 'kick', limb: 'right_foot' },
    36: { pitch: 36, name: 'Bass Drum 1', role: 'kick', limb: 'right_foot' },
    37: { pitch: 37, name: 'Side Stick', role: 'sidestick', limb: 'left_hand' },
    38: { pitch: 38, name: 'Acoustic Snare', role: 'snare', limb: 'left_hand' },
    39: { pitch: 39, name: 'Hand Clap', role: 'clap', limb: 'left_hand' },
    40: { pitch: 40, name: 'Electric Snare', role: 'snare', limb: 'left_hand' },
    41: { pitch: 41, name: 'Low Floor Tom', role: 'tom_floor', limb: 'other' },
    42: { pitch: 42, name: 'Closed Hi-Hat', role: 'hihat_closed', limb: 'right_hand' },
    43: { pitch: 43, name: 'High Floor Tom', role: 'tom_floor', limb: 'other' },
    44: { pitch: 44, name: 'Pedal Hi-Hat', role: 'hihat_pedal', limb: 'left_foot' },
    45: { pitch: 45, name: 'Low Tom', role: 'tom_low', limb: 'other' },
    46: { pitch: 46, name: 'Open Hi-Hat', role: 'hihat_open', limb: 'right_hand' },
    47: { pitch: 47, name: 'Low-Mid Tom', role: 'tom_mid', limb: 'other' },
    48: { pitch: 48, name: 'Hi-Mid Tom', role: 'tom_mid', limb: 'other' },
    49: { pitch: 49, name: 'Crash Cymbal 1', role: 'crash', limb: 'right_hand' },
    50: { pitch: 50, name: 'High Tom', role: 'tom_high', limb: 'other' },
    51: { pitch: 51, name: 'Ride Cymbal 1', role: 'ride', limb: 'right_hand' },
    52: { pitch: 52, name: 'Chinese Cymbal', role: 'crash', limb: 'right_hand' },
    53: { pitch: 53, name: 'Ride Bell', role: 'ride_bell', limb: 'right_hand' },
    54: { pitch: 54, name: 'Tambourine', role: 'percussion', limb: 'right_hand' },
    55: { pitch: 55, name: 'Splash Cymbal', role: 'crash', limb: 'right_hand' },
    56: { pitch: 56, name: 'Cowbell', role: 'percussion', limb: 'right_hand' },
    57: { pitch: 57, name: 'Crash Cymbal 2', role: 'crash', limb: 'left_hand' },
    58: { pitch: 58, name: 'Vibraslap', role: 'percussion', limb: 'other' },
    59: { pitch: 59, name: 'Ride Cymbal 2', role: 'ride', limb: 'right_hand' },
    60: { pitch: 60, name: 'Hi Bongo', role: 'percussion', limb: 'right_hand' },
    61: { pitch: 61, name: 'Low Bongo', role: 'percussion', limb: 'left_hand' },
    62: { pitch: 62, name: 'Mute Hi Conga', role: 'percussion', limb: 'right_hand' },
    63: { pitch: 63, name: 'Open Hi Conga', role: 'percussion', limb: 'right_hand' },
    64: { pitch: 64, name: 'Low Conga', role: 'percussion', limb: 'left_hand' },
    69: { pitch: 69, name: 'Cabasa', role: 'percussion', limb: 'other' },
    70: { pitch: 70, name: 'Maracas', role: 'percussion', limb: 'other' },
    75: { pitch: 75, name: 'Claves', role: 'percussion', limb: 'other' },
    76: { pitch: 76, name: 'Hi Wood Block', role: 'percussion', limb: 'other' },
    77: { pitch: 77, name: 'Low Wood Block', role: 'percussion', limb: 'other' },
    82: { pitch: 82, name: 'Shaker', role: 'percussion', limb: 'other' },
  },
};

export const ROLAND_V_DRUMS_MAP: DrumMap = {
  id: 'roland',
  name: 'Roland V-Drums',
  pieces: {
    ...GENERAL_MIDI_DRUM_MAP.pieces,
    22: { pitch: 22, name: 'Hi-Hat Edge', role: 'hihat_closed', limb: 'right_hand' },
    26: { pitch: 26, name: 'Hi-Hat Open Edge', role: 'hihat_open', limb: 'right_hand' },
    31: { pitch: 31, name: 'Snare Cross Stick', role: 'sidestick', limb: 'left_hand' },
    34: { pitch: 34, name: 'Snare Rimshot', role: 'snare', limb: 'left_hand' },
  },
};

export const ABLETON_DRUM_RACK_MAP: DrumMap = {
  id: 'ableton',
  name: 'Drum Rack (C1 Kick)',
  pieces: {
    36: { pitch: 36, name: 'Kick', role: 'kick', limb: 'right_foot' },
    37: { pitch: 37, name: 'Rim', role: 'sidestick', limb: 'left_hand' },
    38: { pitch: 38, name: 'Snare', role: 'snare', limb: 'left_hand' },
    39: { pitch: 39, name: 'Clap', role: 'clap', limb: 'left_hand' },
    40: { pitch: 40, name: 'Snare 2', role: 'snare', limb: 'left_hand' },
    41: { pitch: 41, name: 'Tom Low', role: 'tom_floor', limb: 'other' },
    42: { pitch: 42, name: 'Closed Hat', role: 'hihat_closed', limb: 'right_hand' },
    43: { pitch: 43, name: 'Tom Mid', role: 'tom_mid', limb: 'other' },
    44: { pitch: 44, name: 'Pedal Hat', role: 'hihat_pedal', limb: 'left_foot' },
    45: { pitch: 45, name: 'Tom High', role: 'tom_high', limb: 'other' },
    46: { pitch: 46, name: 'Open Hat', role: 'hihat_open', limb: 'right_hand' },
    47: { pitch: 47, name: 'Perc 1', role: 'percussion', limb: 'other' },
    48: { pitch: 48, name: 'Perc 2', role: 'percussion', limb: 'other' },
    49: { pitch: 49, name: 'Crash', role: 'crash', limb: 'right_hand' },
    50: { pitch: 50, name: 'Ride', role: 'ride', limb: 'right_hand' },
    51: { pitch: 51, name: 'Ride Bell', role: 'ride_bell', limb: 'right_hand' },
  },
};

export const BUILTIN_DRUM_MAPS: DrumMap[] = [
  GENERAL_MIDI_DRUM_MAP,
  ROLAND_V_DRUMS_MAP,
  ABLETON_DRUM_RACK_MAP,
];

export const DEFAULT_DRUM_MAP = GENERAL_MIDI_DRUM_MAP;

export function getDrumPiece(map: DrumMap | null | undefined, pitch: number): DrumPiece | undefined {
  if (!map) return undefined;
  return map.pieces[pitch];
}

const DRUM_TRACK_REGEX = /\b(drum|drums|percussion|perc|kit|beat|beats|808|909|kick|snare|hihat)\b/i;

/** A track's Drum Synth, if it plays one (docs/design/drum-synth.md, DR10). */
export const drumSynthOf = (track: Track): GraphNode | undefined =>
  Object.values(track.graph.nodes).find((node) => node.type === NODE_TYPES.drumSynth);

/** A track's own choice between drums and melodic, if it made one, or drums for a track playing a Drum Synth (the rest
    go by their names). */
export const explicitDrumMode = (track: Track): boolean | undefined =>
  track.drumMode === 'auto' ? (drumSynthOf(track) ? true : undefined) : track.drumMode === 'drums';

// The role a pad's model plays where its key's General MIDI role is another kind of drum.
const MODEL_ROLES: Record<DrumModel, DrumRole> = {
  off: 'percussion',
  kick: 'kick',
  snare: 'snare',
  tom: 'tom_mid',
  hat: 'hihat_closed',
  cymbal: 'crash',
  clap: 'clap',
  rim: 'sidestick',
};
const ROLE_MODELS: Partial<Record<DrumRole, DrumModel>> = {
  kick: 'kick',
  snare: 'snare',
  sidestick: 'rim',
  clap: 'clap',
  hihat_closed: 'hat',
  hihat_pedal: 'hat',
  hihat_open: 'hat',
  tom_floor: 'tom',
  tom_low: 'tom',
  tom_mid: 'tom',
  tom_high: 'tom',
  crash: 'cymbal',
  ride: 'cymbal',
  ride_bell: 'cymbal',
};

/** The drum map of a track playing a Drum Synth: its pads, by their names, each with its key's General MIDI role and
    limb where that is the same kind of drum, else its model's. Undefined for a track without one. */
export function drumSynthMap(track: Track): DrumMap | undefined {
  const node = drumSynthOf(track);
  if (!node) return undefined;
  const pieces: Record<number, DrumPiece> = {};
  padsOf(node).forEach((pad, i) => {
    if (pad.model === 'off') return;
    const pitch = 36 + i;
    const gm = GENERAL_MIDI_DRUM_MAP.pieces[pitch];
    const same = gm && ROLE_MODELS[gm.role] === pad.model;
    pieces[pitch] = { pitch, name: padName(pad), role: same ? gm.role : MODEL_ROLES[pad.model], limb: same ? gm.limb : 'other' };
  });
  return { id: `drumSynth:${node.id}`, name: 'Drum Synth pads', pieces };
}

export function isDrumTrack(trackName?: string, explicitMode?: boolean): boolean {
  if (explicitMode !== undefined) return explicitMode;
  if (!trackName) return false;
  return DRUM_TRACK_REGEX.test(trackName);
}
