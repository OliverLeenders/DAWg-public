// Shapes exchanged with the C++ side. Keep in sync with engine/include/dawg/Model.h
// and app/src/MainComponent.cpp (registerHandlers).

/** Ticks per beat; positions and lengths are integer ticks (see engine/include/dawg/Time.h). */
export const TICKS_PER_BEAT = 15360;

/**
 * A curve point: [time (ticks, note-relative), value, shape (0 linear, 1 hold, 2 power, 3 free), tension,
 * handleTime?, handleValue?]. The last two are fractions of the segment for a free segment's tension point, which
 * its Bezier handles point at.
 */
export type CurvePoint = [number, number, number, number, number?, number?];

/** A note found in a pool file's pitch (docs/design/pitch-correction.md, section 3.4); frames are the file's own. */
export interface DetectedPitchNote {
  /** Where the unvoiced lead-in begins; equal to start without one. */
  leadStart: number;
  start: number;
  end: number;
  /** The perceived pitch in MIDI semitones. */
  centre: number;
  /** Vibrato rate in Hz and depth in cents; 0 without vibrato. */
  vibratoRate: number;
  vibratoDepth: number;
  /** The slow movement around the centre: [frame from start, semitones, shape]. */
  shape: Array<[number, number, number]>;
}

/** audio.pitchAnalysis's reply: not ready, failed, or the analysis. */
export interface PitchAnalysisReply {
  ready: boolean;
  path: string;
  error?: string;
  /** The analyser's version and model, for seedPitchNotes. */
  analyser?: string;
  sampleRate?: number;
  /** The first pitch value's time in seconds, and seconds between values. */
  start?: number;
  hop?: number;
  /** Semitones per value; null where unvoiced. */
  pitch?: Array<number | null>;
  notes?: DetectedPitchNote[];
}

export type LaneId = 'pitch' | 'pressure' | 'timbre' | 'vibrato';

/** Vibrato waveform: 0 sine, 1 triangle, 2 square, 3 saw up, 4 saw down. */
export type VibratoShape = 0 | 1 | 2 | 3 | 4;

/** Parametric vibrato of a note (see engine/include/dawg/Vibrato.h). depth 0 = off. */
export interface Vibrato {
  depth: number;
  rateHz: number;
  /** Cycle length in beats when tempo-synced, 0 for free-running (rateHz). */
  syncBeats: number;
  shape: VibratoShape;
  /** Ticks after the note start before the vibrato begins. */
  delay: number;
  /** Ticks to ramp up to full depth after the delay. */
  fadeIn: number;
}

export interface Note {
  id: number;
  start: number;
  length: number;
  pitch: number;
  velocity: number;
  releaseVelocity: number;
  expression: Record<LaneId, CurvePoint[]>;
  vibrato: Vibrato;
  /** The later note that continues this note's voice (docs/design/linked-notes.md). */
  link?: { to: number; glideMs: number };
}

export interface Clip {
  id: number;
  start: number;
  length: number;
  /** Content position at the clip's left edge (docs/design/phase6-arrangement.md, A2). */
  offset: number;
  /** Start of the looped content range (content position). */
  loopStart: number;
  /** Length of the looped range, 0 = no loop (A3). */
  loopLength: number;
  /** Empty: show the track name. */
  name: string;
  /** Empty: use the track colour. */
  colour: string;
  /** Linked clips (shared copies) share a non-zero id and always hold the same notes. */
  linkId: number;
  /** Audio clips: the pool file (Project.audioFiles); 0 for note clips. */
  fileId: number;
  /** Audio clips: gain in dB. */
  gainDb: number;
  /** Audio clips: fade lengths in ticks. */
  fadeIn: number;
  fadeOut: number;
  /** 0 linear, 1 equal power, 2 exponential, 3 S-curve. */
  fadeInShape: number;
  fadeOutShape: number;
  /** Audio clips of musical files in the stretch style: semitones, cents as the fraction
      (docs/design/time-stretching.md, TS2). */
  transpose: number;
  /** The takes lane it sits in, 0 for the top row; its place in the track's stack, the highest playing where clips
      overlap (docs/design/takes-lanes.md, T1 and T2). */
  lane: number;
  order: number;
  /** A sequencer clip's pattern of the track's step sequencer, or -1 for a clip of notes
      (docs/design/step-sequencer.md, SS2). */
  sequencerPattern: number;
  /** Sequencer clips: the step the pattern starts on at the clip's content origin (SS3). */
  sequencerStartStep: number;
  /** MIDI controller curves keyed by controller (0–127 CC, 128 pitch bend, 129 pressure); content ticks, values
      0..1 (phase 10, A2). */
  controllers: Record<string, CurvePoint[]>;
  /** Keyed by note id; sort by start for display. */
  notes: Record<string, Note>;
}

/** What an automation lane controls (docs/design/phase10-automation.md, A1). */
export interface AutomationTarget {
  kind: 'gain' | 'pan' | 'node' | 'plugin' | 'send';
  nodeId: number;
  /** Node parameter id or plugin parameter id ("gain"/"pan" for track targets, the send's id for sends). */
  param: string;
  /** Plugin parameters: display name. */
  name: string;
}

export interface AutomationLane extends AutomationTarget {
  id: number;
  /** Absolute ticks, values 0..1. */
  points: CurvePoint[];
  active: boolean;
  height: number;
}

/** 0 off, 1 read, 2 touch, 3 latch, 4 write. */
export type AutomationMode = 0 | 1 | 2 | 3 | 4;

/** A MIDI learn mapping (docs/design/phase9-export-midi-input.md, M8). */
export interface MidiMapping {
  /** Empty: any device. */
  device: string;
  channel: number;
  controller: number;
  trackId: number;
  /** 0: a track parameter ("gain" or "pan"). */
  nodeId: number;
  param: string;
}

/** A MIDI input device and its settings (midi.devices). */
export interface MidiDevice {
  identifier: string;
  name: string;
  enabled: boolean;
  mpe: boolean;
  memberBendRange: number;
  channelBendRange: number;
}

/** Export options (export.start; docs/design/phase9-export-midi-input.md, X1–X4). */
export interface ExportOptions {
  startBeats: number;
  endBeats: number;
  tail: 'auto' | 'fixed' | 'none';
  tailSeconds: number;
  format: 'wav16' | 'wav24' | 'wav32' | 'flac16' | 'flac24' | 'mp3';
  /** How an MP3 is encoded and tagged (X6); only read for format 'mp3'. */
  mp3: { quality: Mp3Quality; tags: Mp3Tags };
  normalise: boolean;
  normaliseDb: number;
  mix: boolean;
  stems: number[];
  folder: string;
  name: string;
  realtime: boolean;
}

/** MP3 quality (X6): a constant bitrate in kbps, or LAME's variable bitrate V0 (best) or V2. */
export type Mp3Quality = 'cbr320' | 'cbr256' | 'cbr192' | 'cbr128' | 'v0' | 'v2';

/** An MP3's ID3v2 tag (X6); empty fields are left out, and the title falls back to the file name. */
export interface Mp3Tags {
  title: string;
  artist: string;
  album: string;
  year: string;
  track: string;
  genre: string;
  comment: string;
}

/** Export progress and result (event dawg_export). */
export interface ExportState {
  /** 'stretching': rendering stretched audio first (docs/design/time-stretching.md, TS10). */
  state: 'stretching' | 'rendering' | 'writing' | 'done' | 'failed' | 'cancelled';
  pass?: number;
  passes?: number;
  progress?: number;
  error?: string;
  files?: Array<{ name: string; path: string; peakDb: number; lufs: number }>;
}

/** Node types (see engine/include/dawg/NodeRegistry.h). */
export const NODE_TYPES = {
  sequencer: 'dawg.sequencer',
  mpeSynth: 'dawg.mpeSynth',
  gain: 'dawg.gain',
  trackOutput: 'dawg.trackOutput',
  busOutput: 'dawg.busOutput',
  pluginInstrument: 'dawg.pluginInstrument',
  pluginEffect: 'dawg.pluginEffect',
  feedback: 'dawg.feedback',
  audioPlayer: 'dawg.audioPlayer',
  mixInput: 'dawg.mixInput',
  eq: 'dawg.eq',
  filter: 'dawg.filter',
  saturator: 'dawg.saturator',
  clipper: 'dawg.clipper',
  waveshaper: 'dawg.waveshaper',
  bitcrusher: 'dawg.bitcrusher',
  delay: 'dawg.delay',
  limiter: 'dawg.limiter',
  compressor: 'dawg.compressor',
  gate: 'dawg.gate',
  expander: 'dawg.expander',
  utility: 'dawg.utility',
  mix: 'dawg.mix',
  bandSplit: 'dawg.bandSplit',
  stereoSplit: 'dawg.stereoSplit',
  stereoMerge: 'dawg.stereoMerge',
  faust: 'dawg.faust',
  loopReverb: 'dawg.loopReverb',
  convolutionReverb: 'dawg.convolutionReverb',
  chorus: 'dawg.chorus',
  flanger: 'dawg.flanger',
  phaser: 'dawg.phaser',
  lfo: 'dawg.lfo',
  adsr: 'dawg.adsr',
  noteExpression: 'dawg.noteExpression',
  macros: 'dawg.macros',
  oscillator: 'dawg.oscillator',
  sampler: 'dawg.sampler',
  drumSynth: 'dawg.drumSynth',
  sampleVoice: 'dawg.sampleVoice',
  voiceInstrument: 'dawg.voiceInstrument',
  voiceInput: 'dawg.voiceInput',
  voiceOutput: 'dawg.voiceOutput',
  component: 'dawg.component',
  componentInput: 'dawg.componentInput',
  componentOutput: 'dawg.componentOutput',
  arpeggiator: 'dawg.arpeggiator',
  chord: 'dawg.chord',
  scaleLock: 'dawg.scaleLock',
  noteRepeat: 'dawg.noteRepeat',
  stepSequencer: 'dawg.stepSequencer',
} as const;

/** A node type's parameter (nodes.types; docs/design/phase8-node-graph.md, G2). */
export interface ParamDescriptor {
  id: string;
  name: string;
  min: number;
  max: number;
  default: number;
  kind: 'continuous' | 'integer' | 'choice' | 'toggle';
  /** Shown after the value; empty for none. */
  unit: string;
  /** The value at a control's middle for skewed ranges; 0 for linear. */
  mid: number;
  /** Labels of choice parameters, by value. */
  choices: string[];
  /** How far a modulation depth of 1 moves it, in modulationScale's terms: octaves, knob travel (1) or its units. */
  modulationSpan: number;
  /** How a depth moves it (docs/design/macros-and-modulation-column.md, MC1, MC2): in octaves for a frequency, along
      its knob for another skewed parameter, or linearly. */
  modulationScale?: 'linear' | 'octaves' | 'travel';
  /** Which source polarity is meaningful when this parameter is modulated. */
  modulationPolarity: 'bipolar' | 'positive' | 'negative';
  /** Whether a modulation slot can target it (docs/design/modulation.md, M2). */
  modulatable: boolean;
  /** Whether an automation lane can target it: not one that changes the node's latency (saturator.md, W7). */
  automatable: boolean;
}

export interface PortDescriptor {
  /** 'any' is a component's spare port, which takes the type of the first cable to it (component-nodes.md, N2). */
  type: 'audio' | 'events' | 'modulation' | 'any';
  name: string;
  /** Audio ports: 1 mono, 2 stereo, 0 adaptive (mono when all its sources are; docs/design/mono-ports.md, MP1). */
  channels?: number;
  /** UI only: a port the node has but doesn't show, such as a Band Split's bands past its count. */
  hidden?: boolean;
  /** UI only: the port's colour, such as a band's or the mid's (docs/design/split-nodes.md, section 6). */
  tone?: string;
}

/** The widths a node's ports got in the last compilation, for nodes with a mono port or a folding input (MP8, MP9). */
export interface NodeWidths {
  /** Per input port: 1 or 2, 0 for a port that isn't audio. */
  inputs: number[];
  /** Per output port: 1 or 2, 0 for a port that isn't audio. */
  outputs: number[];
  /** The input ports where stereo is folded to mono. */
  folded: number[];
}

/** The main bus layouts an effect plugin may take (MP4). */
export type PluginChannels = 'auto' | 'monoToStereo' | 'stereo';

/** A port of a component. */
export interface ComponentPort {
  type: 'audio' | 'events' | 'modulation';
  name: string;
  /** Audio ports: 1 mono, 0 adaptive; absent for stereo (docs/design/mono-ports.md, MP11). */
  channels?: number;
}

/** What's in a component (docs/design/component-nodes.md, N1). */
export interface ComponentGraph {
  /** The name the user gave it; empty for none. */
  name: string;
  graph: TrackGraph;
  inputs: ComponentPort[];
  outputs: ComponentPort[];
  /** The inner parameters shown on it (N6). */
  exposed: ExposedParam[];
}

/** A component saved for reuse, as listed: what's in it is read when it's dropped in (N9). */
export interface SavedComponent {
  file: string;
  name: string;
  inputs: ComponentPort[];
  outputs: ComponentPort[];
}

export interface ExposedParam {
  nodeId: number;
  param: string;
  name: string;
}

/** One modulation input slot (docs/design/modulation.md, M2). */
export interface ModulationSlot {
  param: string;
  depth: number;
  active: boolean;
  /** The track's macro knob (0..7) that feeds it without a cable (docs/design/macros-and-modulation-column.md, MC7). */
  macro?: number;
}

/** A node type as the engine's registry describes it (nodes.types). */
export interface NodeTypeInfo {
  type: string;
  name: string;
  category: 'io' | 'instrument' | 'midiEffect' | 'effect' | 'utility';
  /** False for fixed nodes (sequencer, audio player, mix input, track output). */
  userEditable: boolean;
  inputs: PortDescriptor[];
  outputs: PortDescriptor[];
  params: ParamDescriptor[];
}

/** Identifies a hosted plugin (identifier is PluginInfo.id). */
export interface PluginRef {
  identifier: string;
  name: string;
  manufacturer: string;
  format: string;
}

/** How an instrument plugin receives notes: MPE (per-note channels) or plain MIDI on channel 1. */
export interface MidiSettings {
  mpe: boolean;
  bendRange: number;
}

export interface GraphNode {
  id: number;
  type: string;
  params: Record<string, number>;
  x: number;
  y: number;
  bypassed: boolean;
  /** Plugin nodes only. */
  plugin?: PluginRef;
  /** Plugin nodes only. */
  midi?: MidiSettings;
  /** Faust nodes only: the script (docs/design/phase11-faust-panels.md, F2). */
  code?: string;
  /** Modulation inputs by port index (docs/design/modulation.md, M2). */
  modulations?: Record<string, ModulationSlot>;
  /** Macros devices only: the knobs' names, by knob; a missing or empty one shows as "Macro N" (MC5). */
  macroNames?: string[];
  /** LFO nodes only: one cycle of breakpoints, times 0..TICKS_PER_BEAT, values -1..1. */
  modulationShape?: CurvePoint[];
  /** Voice Instruments only: the graph that runs once per note (docs/design/phase12-voice-scope.md, V1). */
  voice?: TrackGraph;
  /** Components only: what's in it. */
  component?: ComponentGraph;
  /** Samplers only: what it plays (docs/design/sampler-midi-delay-limiter.md, section 4). */
  zones?: SampleZone[];
  /** Drum Synths only: the sixteen pads, keys 36-51 (docs/design/drum-synth.md, DR2); absent for the default kit. */
  pads?: DrumPad[];
  /** Convolution Reverbs only: the impulse response's pool file (docs/design/convolution-reverb.md, I10); absent for none. */
  fileId?: number;
  /** Output to track nodes only: the output track it feeds, or 0 (docs/design/multi-output-instruments.md, MO3). */
  target?: number;
  /** Instrument plugins only: all the plugin's output buses on, each from a port of its own (multi-output-instruments.md, MO1). */
  allOutputs?: boolean;
  /** Band Splits only: the number of bands, absent for 3 (docs/design/split-nodes.md, SP1). */
  bands?: number;
  /** Stereo Splits and Merges only: left and right, absent for mid and side (SP4, SP5). */
  mode?: 'leftRight';
  /** Effect plugins only: its main bus layouts, absent for Auto (docs/design/mono-ports.md, MP4). */
  channels?: Exclude<PluginChannels, 'auto'>;
  /** Step sequencers only: rows and patterns (docs/design/step-sequencer.md). */
  steps?: StepSequence;
}

/** A step: [on, velocity, length in steps, chance, ratchet] (SS6). Steps after a row's last lit one are left out. */
export type StepCell = [boolean, number, number, number, number];

export interface StepSequence {
  rows: Array<{ pitch: number; name: string }>;
  /** Each with one row per sequence row. `rate` indexes STEP_RATES; swing 0.5..0.75. */
  patterns: Array<{ rate: number; swing: number; rows: Array<{ length: number; cells: StepCell[] }> }>;
}

/** The step sizes, in the order of a pattern's `rate`. */
export const STEP_RATES = ['1/4', '1/8', '1/16', '1/32', '1/8T', '1/16T', '1/32T'];
/** Steps per beat for each rate. */
export const STEP_RATE_PER_BEAT = [1, 2, 4, 8, 3, 6, 12];

/** One sample of a Sampler and the notes it plays. Positions are frames of the file; an end of 0 is the file's end. */
/** What a Drum Synth pad plays (docs/design/drum-synth.md, DR6). */
export type DrumModel = 'off' | 'kick' | 'snare' | 'tom' | 'hat' | 'cymbal' | 'clap' | 'rim';

/** A Drum Synth pad's structure; its sound is in the node's parameters, `p{n}.{id}` and `p{n}.{model}.{id}` (DR2, DR3). */
export interface DrumPad {
  name: string;
  model: DrumModel;
  /** 1 (Main) to 16; an output nothing is wired to plays in Main. */
  output: number;
  /** 0 for none, 1 to 8. */
  choke: number;
  mono: boolean;
  locked: boolean;
}

export interface SampleZone {
  id: number;
  fileId: number;
  lowKey: number;
  highKey: number;
  rootKey: number;
  lowVelocity: number;
  highVelocity: number;
  start: number;
  end: number;
  tune: number;
  gainDb: number;
  pan: number;
  mode: 'oneShot' | 'gate' | 'loop';
  loopStart: number;
  loopEnd: number;
  crossfade: number;
  chokeGroup: number;
  /** 1..8: a note plays a zone from each layer that holds it (docs/design/sampler-mapping.md, TL1). */
  layer: number;
  /** The Sampler output it plays from: 1 is Main, up to 16 (docs/design/multi-output-instruments.md, MO1). An output with
   * nothing wired to it plays in Main. */
  output: number;
  /** Velocity crossfades (docs/design/sampler-mapping.md, SF7): silent at fadeInLowVelocity, full from fadeInHighVelocity, full
   * to fadeOutLowVelocity, silent at fadeOutHighVelocity. Equal ends mean no fade. */
  fadeInLowVelocity: number;
  fadeInHighVelocity: number;
  fadeOutLowVelocity: number;
  fadeOutHighVelocity: number;
  attackMs: number;
  decayMs: number;
  /** 0..1. */
  sustain: number;
  releaseMs: number;
  /** Each sloped segment's tension point, as fractions of its rectangle: 0.5 and 0.5 is a straight line. */
  attackHandleTime: number;
  attackHandleLevel: number;
  decayHandleTime: number;
  decayHandleLevel: number;
  releaseHandleTime: number;
  releaseHandleLevel: number;
}

/** A track's graph, or a Voice Instrument's voice graph. */
export interface TrackGraph {
  nodes: Record<string, GraphNode>;
  connections: Record<string, Connection>;
}

/** A group or control of a Faust script's layout (docs/design/phase11-faust-panels.md, F8). */
export interface FaustLayoutItem {
  kind: 'hgroup' | 'vgroup' | 'tgroup' | 'hslider' | 'vslider' | 'nentry' | 'checkbox' | 'button' | 'hbargraph' | 'vbargraph';
  label: string;
  /** Style metadata, e.g. "knob" or "menu{...}"; empty for none. */
  style: string;
  /** Groups only. */
  items?: FaustLayoutItem[];
  /** Controls only: the parameter id. */
  path?: string;
  /** Bargraphs only: the index into telemetry.faustMeters. */
  meter?: number;
  min?: number;
  max?: number;
  unit?: string;
}

/** A compiled Faust script: its node description, channel counts and control layout (F3). */
export interface FaustProgramInfo extends NodeTypeInfo {
  channels: { inputs: number; outputs: number };
  layout: FaustLayoutItem;
}

/** Runtime state of a plugin or Faust node, keyed "<trackId>:<nodeId>" (event dawg_nodes). */
export interface PluginNodeStatus {
  /** Faust nodes: compiling, ready or failed (their script's latest result). */
  status: 'loading' | 'ready' | 'failed' | 'crashed' | 'compiling';
  error: string;
  /** Plugin nodes only. */
  latency?: number;
  /** Effect plugin nodes only: the plugin has a sidechain input bus the node's key feeds (sidechain.md, K7). */
  hasSidechain?: boolean;
  /** The plugin takes notes by id (CLAP): MIDI settings don't apply (docs/design/clap-hosting.md, C7). */
  nativeNotes?: boolean;
  /** A plugin in a helper process: blocks that came back too late. */
  lateBlocks?: number;
  /** The plugin runs in a helper process. */
  separateProcess?: boolean;
  /** Instrument plugins only: the plugin's output buses, the main one first, up to 16 (multi-output-instruments.md, MO1). */
  outputs?: Array<{ name: string; on: boolean }>;
  /** Plugin nodes only. */
  editorOpen?: boolean;
  /** Faust nodes only: the program the node runs (the last script that compiled), or null. */
  program?: FaustProgramInfo | null;
}

/** A built-in effect's response curve (nodes.response; docs/design/phase11-faust-panels.md, P1): frequency (Hz) to
    gain (dB) for the EQ, with each band's too, and the filter; input to output level (dB) for the compressor. */
export interface ResponseCurve {
  x: number[];
  y: number[];
  bands?: number[][];
  /** The EQ with a band on the mid or the side only: the side's curve (y is then the mid's). */
  side?: number[];
}

/** Analysis of the node the dock shows, at 30 Hz (event dawg_analysis; P2). */
export interface NodeAnalysis {
  /** "<trackId>:<nodeId>". */
  key: string;
  /** 256 log-spaced bands from 20 Hz to 20 kHz, in dB (a full-scale sine at 0 dB at 1 kHz), averaged over time and
      tilted by 3 dB per octave about 1 kHz, so that pink noise reads flat. */
  spectrum: number[];
  /** The same for the side, (L - R) / 2; spectrum is the mid's, (L + R) / 2. */
  sideSpectrum?: number[];
  /** 1: mono, 0: unrelated, -1: out of phase. */
  correlation: number;
  /** Side to mid ratio. */
  width: number;
  /** Recent frames as [l, r, l, r, ...] for the goniometer. */
  scope: number[];
  /** RMS level (dB) of the last 1/30 s, [left, right]. */
  level: [number, number];
  /** The node's own measurements, e.g. the compressor's { reduction, input, output } in dB and its history of
      [input, output, reduction] per 10 ms slot since the last frame. */
  meters: Record<string, number | number[]> | null;
}

/** A Faust script from the examples or the user's library (faust.scripts; F6). */
export interface FaustScript {
  name: string;
  file: string;
  source: 'example' | 'library';
  code: string;
}

export interface Connection {
  fromNode: number;
  fromPort: number;
  toNode: number;
  toPort: number;
  /** Where the cable bends on the canvas, along and across the flow as node positions are; absent for the usual curve
      (docs/design/graph-grid-and-cables.md, G5). */
  bends?: Array<{ x: number; y: number }>;
}

/** A send to a return track (docs/design/groups-and-sends.md, R4). */
export interface Send {
  id: number;
  target: number;
  /** 0: the target return's Mix Input; otherwise a keyable node on the target track, whose key input the send feeds
      (docs/design/sidechain.md, K2). */
  targetNode: number;
  levelDb: number;
  preFader: boolean;
  active: boolean;
}

/** A frozen track's render (docs/design/audio-features.md, AU15). */
export interface TrackFreeze {
  fileId: number;
  start: number;
  key: string;
  /** The renders of its Output to track nodes, which its output tracks play while it is frozen. */
  outputs?: { node: number; fileId: number }[];
  /** Whether what the render depends on changed since. */
  stale?: boolean;
}

export interface Track {
  id: number;
  name: string;
  /** Set while the track is frozen. */
  freeze?: TrackFreeze | null;
  colour: string;
  gainDb: number;
  /** -1 (left) to +1 (right); 0 is centre (docs/design/mixing-basics.md, M1). */
  pan: number;
  /** Timing offset in milliseconds (phase 13): positive delays, negative pulls earlier. */
  offsetMs: number;
  /** Whether the piano roll treats the track as drums: by its name, or as the user chose. */
  drumMode: DrumMode;
  /** Lane height in the arrangement, in pixels. */
  height: number;
  kind: 'instrument' | 'audio' | 'master' | 'group' | 'return' | 'output';
  /** The group the track is in, or 0 (docs/design/groups-and-sends.md, R2). Members follow their group. */
  parent: number;
  /** Output tracks: the instrument or audio track whose output it is, which it follows (multi-output-instruments.md, MO4). */
  source?: number;
  /**
   * Output tracks: the track's Main, which plays what reaches its Track Output. A track with output tracks is only a
   * header and plays nothing itself (multi-output-instruments.md, MO12).
   */
  mainOutput?: boolean;
  /** Groups: whether the arrangement folds the members away. */
  folded: boolean;
  /** Sends to return tracks, keyed by id (ids grow, so that is the order they were added). */
  sends: Record<string, Send>;
  mute: boolean;
  solo: boolean;
  /** Audio tracks: first input channel (0-based), -1 = none. */
  inputChannel: number;
  inputStereo: boolean;
  /** 0 off, 1 auto, 2 on. */
  monitor: number;
  /** Automation lanes keyed by id; automationOrder gives the display order (phase 10). */
  automation: Record<string, AutomationLane>;
  automationOrder: number[];
  automationMode: AutomationMode;
  automationExpanded: boolean;
  /** How many takes lanes the track has, and whether the arrangement shows them (docs/design/takes-lanes.md, T2, T5). */
  lanes: number;
  lanesExpanded: boolean;
  /** Keyed by clip id. */
  clips: Record<string, Clip>;
  graph: TrackGraph;
}

/** Pins a file frame to a content tick (docs/design/time-stretching.md, TS2). */
export interface TimingMarker {
  tick: number;
  frame: number;
}

/** Whether a pool file follows the tempo, and how (TS2). */
export interface FileTiming {
  mode: 'oneShot' | 'musical';
  /** Musical files: keep the pitch, or change it with the speed (tape). */
  musicalStyle: 'stretch' | 'repitch';
  /** The file's own tempo: the content-to-file rate past the last marker. */
  sourceBpm: number;
  /** Sorted, ticks and frames strictly increasing. */
  markers: TimingMarker[];
}

/** A file in the project's audio pool (docs/design/phase7-audio-tracks.md, T4). */
export interface AudioFile {
  id: number;
  path: string;
  name: string;
  sampleRate: number;
  channels: number;
  frames: number;
  timing: FileTiming;
  /** A suggested source tempo (TS11), 0 for none. */
  suggestedBpm: number;
  /** For a reversed copy (docs/design/audio-features.md, AU1): the file it reverses, else 0. */
  reverseOf: number;
  /** Its pitch edits (docs/design/pitch-correction.md, PC5); no notes until the Pitch view seeds them. */
  pitch: FilePitch;
}

/** A note of a file's pitch edits (PC5): frames are the file's, shapes relative to their centre. */
export interface PitchNote {
  id: number;
  leadStart: number;
  start: number;
  end: number;
  detectedCentre: number;
  targetCentre: number;
  detectedShape: CurvePoint[];
  shape: CurvePoint[];
  pivots: number[];
  /** 0 removes the vibrato, 1 keeps it, 2 doubles it. */
  vibrato: number;
  transitionMs: number;
}

export interface FilePitch {
  analyser: string;
  notes: PitchNote[];
}

/** Waveform peaks of a file at one mip level (audio.peaks). */
export interface PeaksReply {
  ready: boolean;
  path: string;
  samplesPerPoint?: number;
  channels?: number;
  points?: number;
  frames?: number;
  sampleRate?: number;
  levels?: number;
  /** base64 int8 (min, max) per point and channel. */
  data?: string;
}

export interface Marker {
  id: number;
  position: number;
  name: string;
}

export interface LoopRange {
  enabled: boolean;
  start: number;
  end: number;
}

/** A tempo change (docs/design/tempo-map.md, TM1): bpm from tick on, ramping linearly to the next point if ramp. */
export interface TempoPoint {
  tick: number;
  bpm: number;
  ramp: boolean;
}

/** A time-signature change on a 0-based bar (TM2); denominator 2, 4, 8 or 16. */
export interface TimeSignature {
  bar: number;
  numerator: number;
  denominator: number;
}

/** The project's key (docs/design/scale-highlighting.md, SH1). */
export interface ProjectKey {
  enabled: boolean;
  /** The root's pitch class, 0 (C) to 11 (B). */
  root: number;
  /** An index into SCALES (ui/src/music/scales.ts). */
  scale: number;
}

/** A key from a 0-based bar on (SH9). */
export interface KeyChange extends ProjectKey {
  bar: number;
}

/** A parameter anywhere in the project (docs/design/parameter-linking.md, L1). */
export interface ParamAddress {
  trackId: number;
  nodeId: number;
  /** A built-in parameter id, or a plugin's stable parameter id. */
  param: string;
}

/** How a link maps its source (L2): in the parameters' units, octaves for frequencies, or normalised terms. */
export interface LinkMapping {
  kind: 'identity' | 'inverted' | 'offset' | 'scaled' | 'range';
  /** Offset: what is added (octaves for a frequency); scaled: the factor. */
  amount?: number;
  /** Inverted and scaled: the value they turn around; none for the middle of the source's range. */
  centre?: number;
  /** Range: the part of the source's normalised range that is mapped, to the target's normalised outMin..outMax. */
  inMin?: number;
  inMax?: number;
  outMin?: number;
  outMax?: number;
  /** Range: its shape, times 0..960 and values 0..1; none for a straight line. */
  curve?: CurvePoint[];
}

/** A parameter that follows another (L1). */
export interface ParamLink {
  id: number;
  source: ParamAddress;
  target: ParamAddress;
  mapping: LinkMapping;
  active: boolean;
}

export interface Project {
  /** Sorted by tick, the first at tick 0. */
  tempoMap: TempoPoint[];
  /** Sorted by bar, the first on bar 0. */
  signatures: TimeSignature[];
  metronome: boolean;
  /** The length of automatic crossfades in milliseconds, 0 for none (docs/design/audio-features.md, AU7). */
  autoCrossfadeMs: number;
  /** The keys the piano roll shows and Scale Lock can follow, by bar, the first on bar 0
      (docs/design/scale-highlighting.md, SH1, SH9). */
  keys: KeyChange[];
  /** Keyed by marker id. */
  markers: Record<string, Marker>;
  /** Keyed by file id. */
  audioFiles: Record<string, AudioFile>;
  loop: LoopRange;
  /** The punch range (docs/design/audio-features.md, AU10), the same shape as the loop range. */
  punch: LoopRange;
  /** Track ids in display order. */
  trackOrder: number[];
  midiMappings: MidiMapping[];
  /** Parameter links, by creation (docs/design/parameter-linking.md). */
  links: ParamLink[];
  /** Keyed by track id. */
  tracks: Record<string, Track>;
  /** The master track (docs/design/mixing-basics.md, M4); null only while no project is open. */
  master: Track | null;
}

export interface History {
  canUndo: boolean;
  canRedo: boolean;
  undoLabel: string;
  redoLabel: string;
}

export interface Snapshot {
  rev: number;
  project: Project;
  history: History;
}

export type PatchOp =
  | { op: 'add' | 'replace'; path: string; value: unknown }
  | { op: 'remove'; path: string };

export interface Patch {
  rev: number;
  ops: PatchOp[];
  history: History;
}

export interface Telemetry {
  playing: boolean;
  positionBeats: number;
  peakLeft: number;
  peakRight: number;
  sampleRate: number;
  /** The engine's delay compensation in samples (phase 13). */
  latencySamples: number;
  cpu: number;
  /** Heap allocations seen on the audio thread (Debug builds only; should stay 0). */
  allocationViolations: number;
  /** Heap allocations made by plugins on the audio thread (Debug builds only; not DAWg's fault). */
  pluginAllocations: number;
  recording: boolean;
  /** The sound explorer preview's position in seconds; negative when nothing previews. */
  previewSeconds: number;
  /** Where the current take began (beats), negative before the first recorded sample. */
  recordStartBeats: number;
  /** Device input peak since the last poll. */
  inputPeak: number;
  /** Per-track peaks since the last poll, keyed by track id (master included): [left, right, input]. Left and
      right are after fader and pan (docs/design/mixing-basics.md, M2). */
  meters: Record<string, [number, number, number]>;
  /** MIDI messages received since the last poll (activity light). */
  midiActivity: number;
  /** The values automation lanes play, by lane id (normalised; phase 10, A4). */
  automation: Record<string, number>;
  /** Bargraph values of Faust nodes, by "<trackId>:<nodeId>" (phase 11, F8). */
  faustMeters?: Record<string, number[]>;
  /** Each Feedback node's limit light and shortest length, by "track:node" (docs/design/feedback-routing.md, R6). */
  loops?: Record<string, { limiting: boolean; shortestMs: number }>;
  /** Sounding voices of each Voice Instrument, by "<trackId>:<nodeId>" (docs/design/phase12-voice-scope.md, V7). */
  voices?: Record<string, number>;
  /** Output protection (docs/design/output-protection.md, S3): why the device output is muted, or null. */
  outputMute?: 'overload' | 'extreme' | null;
  /** Tracks whose output had NaN or infinity since the last poll; they were silenced and reset (S1). */
  invalidTracks?: number[];
}

export interface AudioDeviceInfo {
  open: boolean;
  type?: string;
  name?: string;
  sampleRate?: number;
  bufferSize?: number;
  outputLatency?: number;
}

export interface PluginInfo {
  id: string;
  name: string;
  manufacturer: string;
  format: string;
  category: string;
  version: string;
  isInstrument: boolean;
  file: string;
}

export interface PluginOptions {
  available: boolean;
  outOfProcess: boolean;
}

export interface PluginScanState {
  scanning: boolean;
  progress: number;
  current: string;
  plugins: PluginInfo[];
  failed: string[];
}

/** The open project (event dawg_projectState). */
export interface ProjectState {
  /** False while the welcome screen shows. */
  open: boolean;
  name: string;
  path: string;
  dirty: boolean;
  untitled: boolean;
  /** Offered after a crash: an autosave newer than the saved project. */
  recovery: { id: string; name: string; path: string; time: number } | null;
}

export interface RecentProject {
  path: string;
  name: string;
  /** The folder holding the project folder. */
  folder: string;
  /** Last opened, ms since the epoch. */
  opened: number;
  missing: boolean;
  tempo: number;
  tracks: number;
  lengthSeconds: number;
}

export interface ProjectTemplate {
  /** "empty", a starter template ("starter:mpe", "starter:drums", "starter:audio") or a template file path. */
  id: string;
  name: string;
}

/** What to do with unsaved changes when an action would discard them. */
export type DiscardMode = 'ask' | 'save' | 'discard';

export interface AppInfo {
  version: string;
  juceVersion: string;
  vst2Enabled: boolean;
  allocationGuard: boolean;
  /** Whether the UI draws the window's title bar (docs/design/window-chrome.md). */
  customTitleBar: boolean;
  /** A Linux desktop that moves windows on Alt+drag, and where to change that (app/src/DesktopCheck.h); else null. */
  altDrag: { desktop: string; setting: string } | null;
}

/** Commands understood by model.apply (see engine/include/dawg/Commands.h). */
type NoteFields = {
  start: number;
  length: number;
  pitch: number;
  velocity?: number;
  releaseVelocity?: number;
  expression?: Partial<Record<LaneId, Array<[number, number] | CurvePoint>>>;
  vibrato?: Partial<Vibrato>;
};

export type Command =
  | { command: 'setTempo'; args: { bpm: number; at?: number } }
  | { command: 'addTempoPoint'; args: { tick: number; bpm: number; ramp?: boolean } }
  | { command: 'setTempoPoint'; args: { index: number; tick?: number; bpm?: number; ramp?: boolean } }
  | { command: 'removeTempoPoint'; args: { index: number } }
  | { command: 'setSignature'; args: { bar: number; numerator: number; denominator: number } }
  | { command: 'moveSignature'; args: { from: number; to: number } }
  | { command: 'removeSignature'; args: { bar: number } }
  | { command: 'setMetronome'; args: { enabled: boolean } }
  | {
      command: 'addTrack';
      args: {
        name?: string;
        kind?: 'instrument' | 'audio' | 'group' | 'return' | 'output';
        parent?: number;
        /** Output tracks: the track whose output it is. */
        source?: number;
        sendFrom?: number;
        /** A device for the new track's chain, as insertNode takes it (a browser item dropped where no track is). */
        device?: { type: string; plugin?: PluginRef; code?: string; component?: ComponentGraph };
      };
    }
  | { command: 'addSend'; args: { trackId: number; target: number; targetNode?: number; levelDb?: number; preFader?: boolean } }
  | { command: 'setSend'; args: { trackId: number; sendId: number; levelDb?: number; preFader?: boolean; active?: boolean } }
  | { command: 'removeSend'; args: { trackId: number; sendId: number } }
  | { command: 'groupTracks'; args: { ids: number[]; name?: string } }
  | { command: 'ungroup'; args: { id: number } }
  | { command: 'moveTracks'; args: { ids: number[]; parent: number; before?: number } }
  | { command: 'setTrackFolded'; args: { id: number; folded: boolean } }
  | { command: 'removeTrack'; args: { id: number } }
  | { command: 'renameTrack'; args: { id: number; name: string } }
  | { command: 'setTrackGain'; args: { id: number; gainDb: number } }
  | { command: 'setTrackPan'; args: { id: number; pan: number } }
  | { command: 'setTrackOffset'; args: { id: number; offsetMs: number } }
  | { command: 'setTrackDrumMode'; args: { id: number; mode: DrumMode } }
  | {
      command: 'importMidi';
      args: {
        tracks: MidiFileContents['tracks'];
        start?: number;
        tempo?: MidiFileContents['tempo'];
        signatures?: MidiFileContents['signatures'];
      };
    }
  | { command: 'duplicateTrack'; args: { id: number } }
  | {
      command: 'addClip';
      args: {
        /** Controller curves by key, from recording or a MIDI file. */
        controllers?: Record<string, unknown>;
        trackId: number;
        start: number;
        length: number;
        notes?: NoteFields[];
        name?: string;
        colour?: string;
        offset?: number;
        loopStart?: number;
        loopLength?: number;
        /** A sequencer clip (docs/design/step-sequencer.md, SS2). */
        sequencer?: { pattern: number; startStep?: number };
      };
    }
  | { command: 'removeClip'; args: { id: number } }
  | { command: 'moveClip'; args: { id: number; start: number; trackId?: number } }
  | { command: 'resizeClip'; args: { id: number; length: number } | { id: number; edge: 'start' | 'end'; to: number } }
  | { command: 'deleteClips'; args: { ids: number[] } }
  | {
      command: 'moveClips';
      args: {
        ids: number[];
        delta: number;
        trackDelta?: number;
        /** A destination per clip id, instead of trackDelta (groups lie between lanes). */
        trackIds?: Record<string, number>;
        copy?: boolean;
        linked?: boolean;
      };
    }
  | { command: 'duplicateClips'; args: { ids: number[]; linked?: boolean } }
  | { command: 'makeUnique'; args: { ids: number[] } }
  | { command: 'setTrackColour'; args: { id: number; colour: string } }
  | { command: 'splitClips'; args: { ids: number[]; at: number } }
  | { command: 'setClipLoop'; args: { id: number; loopLength: number; loopStart?: number } }
  | {
      command: 'setClipProps';
      args: {
        id: number;
        name?: string;
        colour?: string;
        gainDb?: number;
        fadeIn?: number;
        fadeOut?: number;
        fadeInShape?: number;
        fadeOutShape?: number;
      };
    }
  | {
      command: 'setTrackSwitches';
      args: { id: number; mute?: boolean; solo?: boolean; monitor?: number; inputChannel?: number; inputStereo?: boolean };
    }
  | { command: 'relinkAudioFile'; args: { id: number; path: string } }
  | { command: 'glueClips'; args: { ids: number[] } }
  /** Replaces a note clip's notes with its track's MIDI effects' output and bypasses them (docs/design/midi-effects.md, ME17). */
  | { command: 'renderMidiEffects'; args: { clipId: number } }
  /** The step sequencer (docs/design/step-sequencer.md, SS11). */
  | {
      command: 'setStepCell';
      args: { trackId: number; nodeId: number; pattern: number; row: number; step: number; on?: boolean; velocity?: number; length?: number; chance?: number; ratchet?: number };
    }
  | {
      command: 'setStepCells';
      args: { trackId: number; nodeId: number; pattern: number; cells: Array<{ row: number; step: number; on?: boolean; velocity?: number }> };
    }
  | { command: 'setStepRow'; args: { trackId: number; nodeId: number; row: number; pitch?: number; name?: string; pattern?: number; length?: number } }
  | { command: 'addStepRow'; args: { trackId: number; nodeId: number; pitch: number; name?: string; index?: number } }
  | { command: 'removeStepRow'; args: { trackId: number; nodeId: number; row: number } }
  | { command: 'setStepPattern'; args: { trackId: number; nodeId: number; pattern: number; rate?: number; swing?: number } }
  | { command: 'copyStepPattern'; args: { trackId: number; nodeId: number; from: number; to: number } }
  | { command: 'clearStepPattern'; args: { trackId: number; nodeId: number; pattern: number } }
  | { command: 'setClipSequencer'; args: { clipId: number; pattern?: number; startStep?: number } }
  | { command: 'renderStepClip'; args: { clipId: number } }
  | { command: 'setTrackHeight'; args: { id: number; height: number } }
  | { command: 'addMarker'; args: { position: number; name?: string } }
  | { command: 'moveMarker'; args: { id: number; position: number } }
  | { command: 'renameMarker'; args: { id: number; name: string } }
  | { command: 'removeMarker'; args: { id: number } }
  | { command: 'setLoop'; args: { enabled?: boolean; start?: number; end?: number } }
  | { command: 'setPunch'; args: { enabled?: boolean; start?: number; end?: number } }
  | { command: 'sliceToSampler'; args: { clipId: number; slices: number[]; startKey?: number; mode?: 'oneShot' | 'gate' } }
  | { command: 'addNotes'; args: { clipId: number; notes: Array<NoteFields & { link?: { toIndex: number; glideMs: number } }> } }
  | { command: 'updateNotes'; args: { clipId: number; notes: Array<Partial<NoteFields> & { id: number }> } }
  | { command: 'removeNotes'; args: { clipId: number; ids: number[] } }
  | { command: 'setNoteLinks'; args: { clipId: number; links: Array<{ from: number; to?: number; glideMs?: number }> } }
  | { command: 'splitNotes'; args: { clipId: number; ids: number[]; at: number } }
  | { command: 'setCurve'; args: { clipId: number; noteId: number; lane: LaneId; points: CurvePoint[] } }
  | {
      command: 'closeLoop';
      args: { trackId: number; voiceNodeId?: number; fromNode: number; fromPort: number; toNode: number; toPort: number; x: number; y: number };
    }
  | { command: 'setNodeParam'; args: { trackId: number; voiceNodeId?: number; nodeId: number; param: string; value: number } }
  | { command: 'addLink'; args: { source: ParamAddress; target: ParamAddress; mapping?: LinkMapping; active?: boolean } }
  | { command: 'setLink'; args: { id: number; mapping?: LinkMapping; active?: boolean } }
  | { command: 'removeLink'; args: { id?: number; ids?: number[] } }
  | {
      command: 'linkNodes';
      args: { source: { trackId: number; nodeId: number }; target: { trackId: number; nodeId: number }; preset: 'mirror' | 'mirrorInverted' };
    }
  | { command: 'insertNode'; args: { trackId: number; type: string; plugin?: PluginRef; component?: ComponentGraph; index?: number; code?: string } }
  | { command: 'removeNode'; args: { trackId: number; voiceNodeId?: number; nodeId: number } }
  | {
      command: 'addNode';
      args: {
        trackId: number;
        voiceNodeId?: number;
        type: string;
        x?: number;
        y?: number;
        plugin?: PluginRef;
        code?: string;
        /** A Stereo Split or Merge in left/right; a Band Split's band count (docs/design/split-nodes.md). */
        mode?: 'leftRight';
        bands?: number;
        /** Puts the new node in this cable's path, through its `input` and `output` ports. */
        insertInto?: Connection & { input: number; output: number };
      };
    }
  | { command: 'setNodeCode'; args: { trackId: number; voiceNodeId?: number; nodeId: number; code: string } }
  | { command: 'setSampleZone'; args: { trackId: number; nodeId: number; zoneId: number } & Partial<Omit<SampleZone, 'id' | 'fileId'>> }
  | { command: 'removeSampleZone'; args: { trackId: number; nodeId: number; zoneId: number } }
  | { command: 'setNodeFile'; args: { trackId: number; nodeId: number; fileId: number } }
  /** Points an Output to track node at an output track of its track, or none with 0 (multi-output-instruments.md, MO3). */
  | { command: 'setNodeTarget'; args: { trackId: number; nodeId: number; target?: number; newTrack?: string } }
  /** Each group of a Sampler's zones or a Drum Synth's pads to the next free output, with an output track and an Output to track node (MO5). */
  | {
      command: 'splitInstrumentOutputs';
      args: { trackId: number; nodeId: number; outputs: Array<{ name: string; zoneIds?: number[]; pads?: number[] }> };
    }
  /** A Drum Synth pad's fields (docs/design/drum-synth.md, DR2); pad 0..15. */
  | {
      command: 'setDrumPad';
      args: {
        trackId: number;
        nodeId: number;
        pad: number;
        name?: string;
        model?: DrumModel;
        output?: number;
        chokeGroup?: number;
        mono?: boolean;
        locked?: boolean;
      };
    }
  /** Copies a pad onto another, all but its output and lock (DR4). */
  | { command: 'copyDrumPad'; args: { trackId: number; nodeId: number; from: number; to: number } }
  /** Randomises pads within safe ranges, or varies them a little (DR14). */
  | { command: 'randomiseDrumPads'; args: { trackId: number; nodeId: number; mode?: 'randomise' | 'vary'; pads?: number[]; seed?: number } }
  | { command: 'setPluginOutputs'; args: { trackId: number; nodeId: number; all: boolean } }
  | { command: 'setPluginChannels'; args: { trackId: number; nodeId: number; channels: PluginChannels } }
  | { command: 'setBandCount'; args: { trackId: number; voiceNodeId?: number; nodeId: number; bands: number } }
  | { command: 'setStereoMode'; args: { trackId: number; voiceNodeId?: number; nodeId: number; mode: 'midSide' | 'leftRight' } }
  | { command: 'splitPluginOutputs'; args: { trackId: number; nodeId: number; outputs: Array<{ port: number; name: string }> } }
  | {
      command: 'addNodes';
      args: {
        trackId: number;
        /** The Voice Instrument whose voice graph the nodes go into (docs/design/phase12-voice-scope.md, section 6). */
        voiceNodeId?: number;
        nodes: Array<{
          type: string;
          x?: number;
          y?: number;
          plugin?: PluginRef;
          /** A pasted plugin's source node, whose state for a copy the app gives it. */
          source?: { trackId: number; nodeId: number };
          params?: Record<string, number>;
          bypassed?: boolean;
          /** A pasted Faust node's script. */
          code?: string;
          /** A Stereo Split or Merge in left/right; a Band Split's band count (docs/design/split-nodes.md). */
          mode?: 'leftRight';
          bands?: number;
          /** A pasted plugin instrument's note settings. */
          midi?: MidiSettings;
          /** A pasted node's modulation assignments. */
          modulations?: Record<string, ModulationSlot>;
          /** A pasted LFO's shape. */
          modulationShape?: CurvePoint[];
          /** A pasted Voice Instrument's voice graph. */
          voice?: TrackGraph;
          /** What's in a pasted component. */
          component?: ComponentGraph;
          zones?: SampleZone[];
          steps?: StepSequence;
        }>;
        connections?: Array<{ from: number; fromPort: number; to: number; toPort: number }>;
      };
    }
  | { command: 'removeNodes'; args: { trackId: number; voiceNodeId?: number; nodeIds: number[] } }
  | { command: 'connect'; args: { trackId: number; voiceNodeId?: number } & Connection }
  | { command: 'connectOrCloseLoop'; args: { trackId: number; voiceNodeId?: number; x: number; y: number } & Connection }
  | { command: 'renameComponent'; args: { trackId: number; nodeId: number; name: string } }
  | { command: 'exposeParam'; args: { trackId: number; nodeId: number; target: number; param: string; name?: string } }
  | { command: 'renameExposedParam'; args: { trackId: number; nodeId: number; target: number; param: string; name: string } }
  | { command: 'unexposeParam'; args: { trackId: number; nodeId: number; target: number; param: string } }
  | { command: 'groupNodes'; args: { trackId: number; voiceNodeId?: number; nodeIds: number[] } }
  | { command: 'ungroupNode'; args: { trackId: number; voiceNodeId?: number; nodeId: number } }
  | { command: 'addComponentPort'; args: { trackId: number; nodeId: number; side: 'input' | 'output'; type: ComponentPort['type']; name: string } }
  | { command: 'renameComponentPort'; args: { trackId: number; nodeId: number; side: 'input' | 'output'; index: number; name: string } }
  | { command: 'setComponentPortChannels'; args: { trackId: number; nodeId: number; side: 'input' | 'output'; index: number; channels: number } }
  | { command: 'removeComponentPort'; args: { trackId: number; nodeId: number; side: 'input' | 'output'; index: number } }
  | { command: 'disconnect'; args: { trackId: number; voiceNodeId?: number } & Connection }
  | {
      command: 'setModulation';
      args: { trackId: number; voiceNodeId?: number; nodeId: number; port: number; param: string; depth?: number; active?: boolean };
    }
  | { command: 'setModulationShape'; args: { trackId: number; voiceNodeId?: number; nodeId: number; points: CurvePoint[] } }
  | { command: 'setNodePosition'; args: { trackId: number; voiceNodeId?: number; nodeId: number; x: number; y: number } }
  | {
      command: 'setConnectionBends';
      args: { trackId: number; voiceNodeId?: number; fromNode: number; fromPort: number; toNode: number; toPort: number; bends?: Array<{ x: number; y: number }> };
    }
  | { command: 'bringToTop'; args: { id: number; from?: number; to?: number } }
  | { command: 'flattenTakes'; args: { trackId: number } }
  | { command: 'removeLane'; args: { trackId: number; lane: number } }
  | { command: 'setTrackLanesExpanded'; args: { id: number; expanded: boolean } }
  | { command: 'addAutomationLane'; args: { trackId: number; target: AutomationTarget; points?: CurvePoint[] } }
  | { command: 'removeAutomationLane'; args: { trackId: number; laneId: number } }
  | { command: 'setAutomationLane'; args: { trackId: number; laneId: number; active?: boolean; height?: number } }
  | { command: 'setAutomationPoints'; args: { trackId: number; laneId: number; points: CurvePoint[] } }
  | { command: 'setTrackAutomation'; args: { id: number; mode?: AutomationMode; expanded?: boolean } }
  | { command: 'setClipController'; args: { clipId: number; controller: number; points: CurvePoint[] } }
  | { command: 'removeMidiMapping'; args: { trackId: number; nodeId?: number; param: string } }
  | {
      command: 'setMidiMapping';
      args: { trackId: number; nodeId?: number; param: string; device?: string; channel: number; controller: number };
    }
  | { command: 'mapMacro'; args: { trackId: number; nodeId: number; param: string; macro: number; depth?: number } }
  | { command: 'renameMacro'; args: { trackId: number; nodeId: number; index: number; name: string } }
  | { command: 'reorderNode'; args: { trackId: number; nodeId: number; index: number } }
  | { command: 'setNodeBypass'; args: { trackId: number; voiceNodeId?: number; nodeId: number; bypassed: boolean } }
  | { command: 'convertToVoiceGraph'; args: { trackId: number; nodeId: number } }
  | { command: 'setPluginMidi'; args: { trackId: number; nodeId: number; mpe?: boolean; bendRange?: number } }
  // Time stretching (docs/design/time-stretching.md, TS14).
  | {
      command: 'setFileTiming';
      args: { fileId: number; mode?: FileTiming['mode']; musicalStyle?: FileTiming['musicalStyle']; sourceBpm?: number };
    }
  | { command: 'setClipTranspose'; args: { clipId: number; semitones: number } }
  | { command: 'addTimingMarker'; args: { fileId: number; tick: number; frame?: number } }
  | { command: 'moveTimingMarker'; args: { fileId: number; index: number; tick?: number; frame?: number } }
  | { command: 'removeTimingMarker'; args: { fileId: number; index: number } }
  | { command: 'resetTimingMarkers'; args: { fileId: number } }
  | { command: 'copyPoolFile'; args: { clipId: number } }
  /** Pitch edits (docs/design/pitch-correction.md, section 4.8). */
  | {
      command: 'seedPitchNotes';
      args: {
        fileId: number;
        analyser?: string;
        notes: Array<Pick<DetectedPitchNote, 'leadStart' | 'start' | 'end' | 'centre' | 'shape'>>;
        replace?: boolean;
      };
    }
  | {
      command: 'setNotePitch';
      args: { fileId: number; noteIds?: number[]; target?: number; delta?: number; vibrato?: number; transitionMs?: number };
    }
  | { command: 'resetPitchNotes'; args: { fileId: number; noteIds?: number[] } }
  | { command: 'splitPitchNote'; args: { fileId: number; noteId: number; frame: number; centres?: [number, number] } }
  | { command: 'mergePitchNotes'; args: { fileId: number; noteIds: number[]; centre?: number } }
  | { command: 'setNoteShape'; args: { fileId: number; noteId: number; shape: CurvePoint[]; centre?: number; pivots?: number[] } }
  | { command: 'snapNotePitch'; args: { fileId: number; noteIds?: number[]; mode: 'semitone' | 'key'; amount?: number } }
  | {
      command: 'shapeNotes';
      args: { fileId: number; noteIds?: number[]; op: 'flattenOnset' | 'flattenRelease' };
    }
  /** Crossfades on the clips' edges over audio below (docs/design/audio-features.md, AU8). */
  | { command: 'crossfadeClips'; args: { clipIds: number[]; ms?: number; shape?: number } }
  /** The length of automatic crossfades, 0 for none (AU7). */
  | { command: 'setAutoCrossfade'; args: { ms: number } }
  /** The key on a bar (0 by default): edits the change there or adds one from the key in effect; arguments left out keep
      their values (docs/design/scale-highlighting.md, SH1, SH9). */
  | { command: 'setKey'; args: { bar?: number; enabled?: boolean; root?: number; scale?: number } }
  | { command: 'moveKey'; args: { from: number; to: number } }
  | { command: 'removeKey'; args: { bar: number } }
  | { command: 'stretchClip'; args: { clipId: number; length: number } }
  | { command: 'batch'; args: { label?: string; commands: Command[] } };

/** Request/response types for every bridge method. */
export interface Methods {
  'app.info': [void, AppInfo];
  'midi.parse': [{ path: string } | { name: string; data: string }, MidiFileContents];
  'midiFiles.list': [{ path?: string }, { path: string; entries: Array<{ name: string; path: string; dir: boolean }> }];
  'midiFiles.search': [{ query: string }, Array<{ name: string; path: string; dir: boolean }>];
  'midi.importDialog': [void, null];
  'midi.exportDialog': [void, null];
  'settings.get': [{ key: string }, string | null];
  'settings.set': [{ key: string; value: string }, null];
  /** Fetches DAWg's releases from GitHub (docs/design/update-checks.md); the answer comes as dawg_updates. False while
      a fetch is running. */
  'updates.fetch': [void, boolean];
  /** The main window's page is alive (docs/design/ui-recovery.md, UR3): sent every few seconds; a page that stops is
      reloaded. */
  'ui.alive': [void, null];
  /** Rebuilds the main window's web view (UR3); the project stays as it is. */
  'app.reloadUi': [void, null];
  /** Shows the log file in the file manager and returns its path (UR1). */
  'app.logFile': [void, string];
  /** The unsaved-changes prompt for quitting is on screen, and when it has been answered (UR6). */
  'project.confirmShown': [void, null];
  'project.confirmDone': [void, null];
  /** Opens a page of DAWg's GitHub repository in the browser (a release's page). */
  'app.openUrl': [{ url: string }, boolean];
  'window.control': [{ window: WindowName; action: WindowAction }, { maximised: boolean }];
  /** The narrowest a window may be made for what its page shows; 0 for its own minimum. */
  'window.minWidth': [{ window: WindowName; width: number }, null];
  'model.get': [void, Snapshot];
  'model.apply': [Command & { gesture?: string }, { rev: number }];
  'model.undo': [void, { rev: number; applied: boolean }];
  'model.redo': [void, { rev: number; applied: boolean }];
  'transport.play': [void, null];
  'transport.stop': [void, null];
  'audio.unmute': [void, null];
  'transport.toggle': [void, null];
  'transport.seek': [{ beats: number }, null];
  'audio.get': [void, AudioDeviceInfo];
  'audio.openSettings': [void, null];
  'audition.noteOn': [{ trackId: number; pitch: number; velocity: number }, { id: number }];
  'audition.noteOff': [{ trackId: number; id: number }, null];
  'plugins.get': [void, PluginScanState];
  'plugins.scan': [void, null];
  /** Running plugins in helper processes (docs/design/out-of-process-plugins.md, O9). */
  'plugins.options': [void, PluginOptions];
  'plugins.setOutOfProcess': [{ enabled: boolean }, null];
  'plugin.setRunInside': [{ identifier: string; inside: boolean }, null];
  'plugins.status': [void, Record<string, PluginNodeStatus>];
  'nodes.widths': [void, Record<string, NodeWidths>];
  'nodes.types': [void, NodeTypeInfo[]];
  'components.list': [void, SavedComponent[]];
  'components.load': [{ file: string }, ComponentGraph];
  'components.save': [{ trackId: number; nodeId: number }, string];
  'components.delete': [{ file: string }, null];
  'faust.scripts': [void, FaustScript[]];
  'faust.saveScript': [{ name: string; code: string }, string];
  'faust.revealLibrary': [void, null];
  'faust.completions': [void, Array<{ name: string; signature: string }>];
  'nodes.response': [{ type: string; params: Record<string, number> }, ResponseCurve | null];
  'nodes.watch': [
    | { window?: WindowName; trackId: number; nodeId: number }
    | { window?: WindowName; nodes: Array<{ trackId: number; nodeId: number }> }
    | null,
    null,
  ];
  'midi.devices': [void, MidiDevice[]];
  'automation.touch': [{ trackId: number; target: AutomationTarget; touching: boolean }, null];
  'plugin.parameters': [{ trackId: number; nodeId: number }, Array<{ id: string; name: string }>];
  /** Previews a step sequencer's pattern while stopped, or stops it (pattern null). */
  'stepSequencer.preview': [{ trackId: number; nodeId: number; pattern: number | null }, null];
  /** The pattern a held key plays while stopped: the one the panel shows. */
  'stepSequencer.shown': [{ trackId: number; nodeId: number; pattern: number }, null];
  'midi.configure': [{ identifier: string; enabled?: boolean; mpe?: boolean; memberBendRange?: number; channelBendRange?: number }, MidiDevice[]];
  'midi.learn': [{ trackId?: number; nodeId?: number; param?: string }, null];
  'track.select': [{ trackId: number }, null];
  'export.defaults': [void, { folder: string; name: string; songEndBeats: number; sampleRate: number; mp3SampleRate: number }];
  'export.chooseFolder': [{ folder: string }, null];
  'export.start': [ExportOptions, null];
  'export.cancel': [void, null];
  'plugin.openEditor': [{ trackId: number; nodeId: number }, null];
  'plugin.closeEditor': [{ trackId: number; nodeId: number }, null];
  /** Empties a Feedback node's delay line, in every voice (docs/design/feedback-routing.md, R7). */
  'node.clear': [{ trackId: number; nodeId: number }, null];
  'plugin.reload': [{ trackId: number; nodeId: number }, null];
  'project.state': [void, ProjectState];
  'project.recent': [void, RecentProject[]];
  'project.templates': [void, ProjectTemplate[]];
  'project.new': [{ template: string; mode: DiscardMode }, { needsDecision: boolean }];
  'project.open': [{ path?: string; mode: DiscardMode }, { needsDecision: boolean }];
  'project.close': [{ mode: DiscardMode }, { needsDecision: boolean }];
  'project.quit': [{ mode: DiscardMode }, { needsDecision: boolean }];
  'project.save': [{ saveAs: boolean }, null];
  'project.saveAsTemplate': [{ name: string }, null];
  'project.recover': [{ accept: boolean }, null];
  'project.removeRecent': [{ path: string }, null];
  'audio.importDialog': [{ trackId?: number; start?: number }, null];
  'audio.import': [{ paths: string[]; trackId?: number; start?: number }, null];
  'sampler.import': [{ trackId: number; nodeId: number; paths: string[]; key?: number; replaceZone?: number }, null];
  'sampler.importDialog': [{ trackId: number; nodeId: number; key?: number; replaceZone?: number }, null];
  /** The pitch of part of a pool file in MIDI semitones, or 0 for none (docs/design/sampler-mapping.md, SM4). */
  'sampler.detectPitch': [{ fileId: number; start: number; end: number }, number];
  /** A Convolution Reverb's impulse response, copied into the pool (docs/design/convolution-reverb.md, I10). */
  'convolution.import': [{ trackId: number; nodeId: number; paths: string[] }, null];
  'convolution.importDialog': [{ trackId: number; nodeId: number }, null];
  'audio.upload': [{ id: string; name: string; data: string; last: boolean; trackId?: number; start?: number }, null];
  'audio.peaks': [{ fileId?: number; path?: string; level: number }, PeaksReply];
  'audio.missing': [void, Array<{ id: number; name: string; path: string }>];
  /** Raw frames (the mono mid) as base64 little-endian floats, for the wave when zoomed in past the peaks. */
  'audio.samples': [{ fileId?: number; path?: string; start: number; count: number }, { start: number; count: number; data: string } | null];
  /** A pool file's onsets as [frame, strength 0..1] (docs/design/time-stretching.md, TS11); dawg_onsets when ready. */
  'audio.onsets': [{ fileId: number }, { ready: boolean; path: string; onsets?: Array<[number, number]> }];
  /** A pool file's pitch analysis (docs/design/pitch-correction.md, section 3.6); dawg_pitchAnalysis when ready. */
  'audio.pitchAnalysis': [{ fileId: number }, PitchAnalysisReply];
  /** Plays one note of a pool file through the preview player, corrected where its render is ready. */
  'audio.auditionNote': [{ fileId: number; noteId: number }, null];
  /** Renders a track before its fader and plays the render in place of its clips and graph (AU15). */
  'audio.freezeTrack': [{ trackId: number }, null];
  'audio.unfreezeTrack': [{ trackId: number }, null];
  /** Renders a whole track through its chain and puts the audio in place (AU16). */
  'audio.bounceTrack': [{ trackId: number }, null];
  /** Drums (onsets) or a monophonic melody in a clip, as notes on a new instrument track (AU4). */
  'audio.toMidi': [{ clipId: number; kind: 'drums' | 'pitch'; sensitivity?: number; split?: boolean; curves?: boolean }, null];
  /** Where a clip would be sliced (AU3); not ready while its onsets are being found (dawg_onsets when done). */
  'audio.slicePoints': [
    { clipId: number; mode: 'transients' | 'grid' | 'markers'; sensitivity?: number; division?: number },
    { ready: boolean; slices?: number[]; range?: [number, number]; sampleRate?: number },
  ];
  /** What audio clips of one track play, without its chain, as one clip (AU16). */
  'audio.consolidateClips': [{ clipIds: number[] }, null];
  /** Tracks after their faders over the loop range, or the clips' span, onto a new audio track (AU17). */
  'audio.renderSelection': [{ trackIds: number[]; clipIds: number[] }, null];
  /** Plays clips backwards through reversed copies of their files, written first (audio-features.md, AU1). */
  'audio.reverseClips': [{ clipIds: number[] }, null];
  /** Sets clips' gain so their sample peak reaches a target (AU2); `together` keeps their balance. */
  'audio.normaliseClips': [{ clipIds: number[]; targetDb: number; together: boolean }, null];
  /** The audio editor window (TS13). */
  'audioEditor.state': [void, { open: boolean }];
  'audioEditor.show': [{ open: boolean; clip?: number | null }, null];
  /** An instrument's own window (docs/design/macros-and-modulation-column.md, MC14). */
  'instrumentEditor.show': [{ open: boolean; trackId: number; nodeId: number }, null];
  'audio.locate': [{ fileId: number }, null];
  /** The stretch render cache (docs/design/time-stretching.md, TS8), in bytes. */
  'audio.stretchCache': [void, { size: number; limit: number }];
  'audio.setStretchCacheLimit': [{ bytes: number }, null];
  'audio.clearStretchCache': [void, null];
  /** Holds back stretch renders during a drag that changes them; they start once it ends (TS9). */
  'stretch.hold': [{ held: boolean }, null];
  'audio.inputs': [void, string[]];
  'transport.record': [{ countIn: boolean }, null];
  /** The pre-roll before recording, in bars (audio-features.md, AU12); without `bars`, reads it. */
  'record.preRoll': [{ bars?: number }, number];
  'files.roots': [void, Array<{ name: string; path: string; project: boolean }>];
  'files.addRoot': [void, null];
  'files.removeRoot': [{ path: string }, Array<{ name: string; path: string; project: boolean }>];
  'files.list': [{ path: string }, { path: string; entries: Array<{ name: string; path: string; dir: boolean }> }];
  'files.search': [{ path: string; query: string }, { path: string; entries: Array<{ name: string; path: string; dir: boolean }> }];
  'preview.play': [{ path: string }, null];
  'preview.stop': [void, null];
  'preview.setVolume': [{ gain: number }, null];
  'track.arm': [{ trackId: number; armed: boolean }, null];
  /** The mixer window (docs/design/mixer.md, X1): whether it is open, and opening or closing it. */
  'mixer.state': [void, { open: boolean }];
  'mixer.show': [{ open: boolean }, null];
  /** The MIDI editor's own window: whether it is open, and opening it on a track and clip. */
  'editor.state': [void, { open: boolean }];
  'editor.show': [{ open: boolean; track?: number | null; clip?: number | null }, null];
  /** Relays a window's selection to the other windows (X11), as dawg_selection. */
  'ui.select': [Selection, null];
}

/** Which UI window this is: the main window, the mixer window, or the MIDI editor window (docs/design/mixer.md,
    X11). */
export type WindowName = 'main' | 'mixer' | 'roll' | 'audio' | 'instrument';

/** What a MIDI file holds, read by the engine (docs/design/sampler-midi-delay-limiter.md, section 3). */
export interface MidiFileContents {
  name: string;
  tracks: Array<{
    name: string;
    drums: boolean;
    length: number;
    notes: Array<{ start: number; length: number; pitch: number; velocity?: number }>;
    controllers: Record<string, unknown>;
  }>;
  tempo: Array<{ tick: number; bpm: number }>;
  signatures: Array<{ bar: number; numerator: number; denominator: number }>;
}
export type WindowAction = 'state' | 'drag' | 'minimise' | 'maximise' | 'close';
export type DrumMode = 'auto' | 'drums' | 'melodic';

/** A window's selection, relayed to the other windows (X6, X11). */
export interface Selection {
  source: WindowName;
  tracks: number[];
  primary: number | null;
  node: { trackId: number; nodeId: number } | null;
  /** The clip open in the MIDI editor, so a detached editor follows the arrangement. */
  clip: number | null;
}

export interface Events {
  dawg_patch: Patch;
  dawg_telemetry: Telemetry;
  dawg_audio: AudioDeviceInfo;
  dawg_plugins: PluginScanState;
  dawg_nodes: Record<string, PluginNodeStatus>;
  /** The ports' widths after a compilation, keyed "<trackId>:<nodeId>" (docs/design/mono-ports.md, MP8). */
  dawg_widths: Record<string, NodeWidths>;
  dawg_analysis: NodeAnalysis;
  dawg_error: string;
  /** How something went, nothing failed (an SFZ patch's import, docs/design/sampler-mapping.md, SF6). */
  dawg_hint: string;
  dawg_projectState: ProjectState;
  /** The native side needs a decision about unsaved changes (e.g. the window was closed). */
  dawg_confirm: { action: 'quit' };
  /** Peaks for a file (absolute path) finished computing. */
  dawg_peaks: { path: string };
  /** Armed track ids (session state). */
  dawg_armed: number[];
  /** MIDI devices changed their own settings (an MPE configuration message). */
  dawg_midi: MidiDevice[];
  /** MIDI learn finished. */
  dawg_learned: { ok: boolean; error: string };
  dawg_export: ExportState;
  /** Track renders: freeze, bounce, render selection (audio-features.md, AU14). */
  dawg_render: { state: ExportState['state']; label?: string; progress?: number; error?: string };
  /** Stretch renders (TS9): the clips still waiting for theirs, and those whose renders just finished. */
  dawg_stretch: { pending: number[]; ready: number[] };
  dawg_midiFile: MidiFileContents;
  dawg_exportFolder: string;
  /** GitHub's answer to updates.fetch: the HTTP status and body, or why there was none. */
  dawg_updates: { status: number; body: string; error: string };
  /** Explorer roots changed (a folder was added). */
  dawg_roots: Array<{ name: string; path: string; project: boolean }>;
  /** Durations in seconds of listed audio files, by absolute path. */
  dawg_durations: Record<string, number>;
  /** The mixer window opened or closed. */
  dawg_mixer: { open: boolean };
  /** The MIDI editor window opened or closed. */
  dawg_editor: { open: boolean };
  /** The audio editor window opened or closed, and the clip it should show (TS13). */
  dawg_audioEditor: { open: boolean };
  /** The instrument window was asked to show another instrument (MC14). */
  dawg_instrumentEditor: { trackId: number; nodeId: number };
  dawg_audioEditorClip: { clip: number };
  /** A file's onsets are ready (TS11). */
  dawg_onsets: { path: string };
  /** A file's pitch analysis is ready (docs/design/pitch-correction.md, section 3.6). */
  dawg_pitchAnalysis: { path: string };
  /** Another window's selection changed. */
  dawg_selection: Selection;
}
