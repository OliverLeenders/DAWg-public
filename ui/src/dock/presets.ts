import { NODE_TYPES, type Command, type CurvePoint, type DrumPad, type GraphNode, type NodeTypeInfo, type Track } from '../bridge/protocol';
import * as bridge from '../bridge/bridge';
import { dispatch, useDawg } from '../store/store';
import { DRUM_KITS, kitCommands } from './drumKits';
import { ASYMMETRIC_PRESETS } from './waveshaper';

// Presets for DAWg built-in nodes (reverb, convolution reverb, delay, limiter, eq, filter, compressor, gate, expander, saturator,
// clipper, waveshaper, bit crusher,
// chorus, flanger, phaser, utility, lfo, adsr).
// Provides factory presets, user-defined custom preset persistence, and application dispatching.

export interface NodePreset {
  id: string;
  name: string;
  type: string;
  category?: string;
  params: Record<string, number>;
  /** LFO presets: the one-cycle shape; Waveshaper presets: the drawn curve. Loading the preset replaces it too. */
  shape?: CurvePoint[];
  /** Drum Synth kits: the sixteen pads (docs/design/drum-synth.md, DR13). Loading the kit sets their names, models,
      choke groups and Mono; outputs and locks stay. */
  drumPads?: DrumPad[];
  /** What the preset sounds like, for its tooltip. */
  description?: string;
  isUser?: boolean;
}

const STORAGE_KEY = 'dawg_user_presets';

export const FACTORY_PRESETS: Record<string, NodePreset[]> = {
  [NODE_TYPES.drumSynth]: DRUM_KITS,
  // Note lengths by their index in the delay's choices: 7 is 1/16, 10 is 1/8, 12 is 1/8., 13 is 1/4.
  [NODE_TYPES.delay]: [
    {
      id: 'delay-slapback',
      name: 'Slapback',
      type: NODE_TYPES.delay,
      category: 'Short',
      params: { sync: 0, link: 1, pingPong: 0, timeL: 110, timeR: 110, feedback: 10, lowCut: 120, highCut: 6000, mix: 25 },
    },
    {
      id: 'delay-dotted-eighth',
      name: 'Dotted eighth',
      type: NODE_TYPES.delay,
      category: 'Rhythmic',
      params: { sync: 1, link: 1, pingPong: 0, noteL: 12, noteR: 12, feedback: 40, lowCut: 150, highCut: 8000, mix: 25 },
    },
    {
      id: 'delay-ping-pong',
      name: 'Ping-pong eighths',
      type: NODE_TYPES.delay,
      category: 'Rhythmic',
      params: { sync: 1, link: 1, pingPong: 1, noteL: 10, noteR: 10, feedback: 50, lowCut: 200, highCut: 9000, mix: 30 },
    },
    {
      id: 'delay-dub',
      name: 'Dub echo',
      type: NODE_TYPES.delay,
      category: 'Creative',
      params: { sync: 1, link: 1, pingPong: 0, noteL: 13, noteR: 13, feedback: 75, lowCut: 300, highCut: 2500, mix: 35 },
    },
  ],

  [NODE_TYPES.limiter]: [
    {
      id: 'limiter-safety',
      name: 'Safety',
      type: NODE_TYPES.limiter,
      category: 'Mix',
      params: { gain: 0, ceiling: -0.3, release: 100 },
    },
    {
      id: 'limiter-loud',
      name: 'Loud master',
      type: NODE_TYPES.limiter,
      category: 'Master',
      params: { gain: 6, ceiling: -1, release: 150 },
    },
    {
      id: 'limiter-punchy',
      name: 'Punchy drums',
      type: NODE_TYPES.limiter,
      category: 'Drums',
      params: { gain: 4, ceiling: -1, release: 30 },
    },
  ],

  [NODE_TYPES.loopReverb]: [
    {
      id: 'reverb-concert-hall',
      name: 'Concert Hall',
      type: NODE_TYPES.loopReverb,
      category: 'Halls',
      params: { feedback: 75, diffusion: 80, size: 85, predelay: 25, earlyTiming: 45, width: 90, damp: 35, thin: 25, modulation: 30, mix: 35, seed: 1 },
    },
    {
      id: 'reverb-vocal-plate',
      name: 'Vocal Plate',
      type: NODE_TYPES.loopReverb,
      category: 'Plates',
      params: { feedback: 55, diffusion: 95, size: 60, predelay: 15, earlyTiming: 30, width: 70, damp: 15, thin: 40, modulation: 45, mix: 25, seed: 42 },
    },
    {
      id: 'reverb-short-ambience',
      name: 'Short Ambience',
      type: NODE_TYPES.loopReverb,
      category: 'Rooms',
      params: { feedback: 25, diffusion: 60, size: 25, predelay: 0, earlyTiming: 20, width: 80, damp: 40, thin: 15, modulation: 10, mix: 20, seed: 100 },
    },
    {
      id: 'reverb-drum-room',
      name: 'Drum Room',
      type: NODE_TYPES.loopReverb,
      category: 'Rooms',
      params: { feedback: 40, diffusion: 70, size: 40, predelay: 8, earlyTiming: 50, width: 65, damp: 50, thin: 35, modulation: 15, mix: 20, seed: 777 },
    },
    {
      id: 'reverb-deep-space',
      name: 'Deep Space',
      type: NODE_TYPES.loopReverb,
      category: 'Creative',
      params: { feedback: 92, diffusion: 85, size: 100, predelay: 45, earlyTiming: 60, width: 100, damp: 20, thin: 10, modulation: 50, mix: 40, seed: 1234 },
    },
    {
      id: 'reverb-spring-bloom',
      name: 'Spring Bloom',
      type: NODE_TYPES.loopReverb,
      category: 'Creative',
      params: { feedback: 65, diffusion: 50, size: 50, predelay: 5, earlyTiming: 80, width: 50, damp: 60, thin: 50, modulation: 60, mix: 30, seed: 999 },
    },
  ],

  [NODE_TYPES.eq]: [
    {
      id: 'eq-vocal-air',
      name: 'Vocal Air & Presence',
      type: NODE_TYPES.eq,
      category: 'Vocal',
      params: {
        hpOn: 1, hpUsed: 1, hpFreq: 85, hpSlope: 1,
        lowOn: 1, lowUsed: 1, lowFreq: 220, lowGain: -2.5, lowQ: 1.0,
        mid3On: 1, mid3Used: 1, mid3Freq: 3200, mid3Gain: 3.0, mid3Q: 1.2,
        highOn: 1, highUsed: 1, highFreq: 11000, highGain: 4.0, highQ: 0.7,
      },
    },
    {
      id: 'eq-bass-tightener',
      name: 'Bass Tightener',
      type: NODE_TYPES.eq,
      category: 'Bass',
      params: {
        hpOn: 1, hpUsed: 1, hpFreq: 32, hpSlope: 2,
        lowOn: 1, lowUsed: 1, lowFreq: 70, lowGain: 3.5, lowQ: 1.1,
        mid1On: 1, mid1Used: 1, mid1Freq: 280, mid1Gain: -4.0, mid1Q: 1.4,
        lpOn: 1, lpUsed: 1, lpFreq: 6000, lpSlope: 1,
      },
    },
    {
      id: 'eq-acoustic-warmth',
      name: 'Acoustic Warmth',
      type: NODE_TYPES.eq,
      category: 'Instruments',
      params: {
        hpOn: 1, hpUsed: 1, hpFreq: 70, hpSlope: 1,
        mid1On: 1, mid1Used: 1, mid1Freq: 380, mid1Gain: -3.0, mid1Q: 1.3,
        mid3On: 1, mid3Used: 1, mid3Freq: 4500, mid3Gain: 2.5, mid3Q: 0.9,
        highOn: 1, highUsed: 1, highFreq: 10000, highGain: 2.0, highQ: 0.7,
      },
    },
    {
      id: 'eq-mid-scoop',
      name: 'Mid Scoop / Smile',
      type: NODE_TYPES.eq,
      category: 'Mix',
      params: {
        lowOn: 1, lowUsed: 1, lowFreq: 100, lowGain: 4.0, lowQ: 0.7,
        mid2On: 1, mid2Used: 1, mid2Freq: 800, mid2Gain: -4.5, mid2Q: 1.2,
        highOn: 1, highUsed: 1, highFreq: 9000, highGain: 4.0, highQ: 0.7,
      },
    },
    {
      id: 'eq-telephone',
      name: 'Telephone / Radio',
      type: NODE_TYPES.eq,
      category: 'Creative',
      params: {
        hpOn: 1, hpUsed: 1, hpFreq: 500, hpSlope: 3,
        mid2On: 1, mid2Used: 1, mid2Freq: 1800, mid2Gain: 6.0, mid2Q: 2.0,
        lpOn: 1, lpUsed: 1, lpFreq: 3000, lpSlope: 3,
      },
    },
    {
      id: 'eq-sub-clean',
      name: 'Sub Clean (80Hz HP)',
      type: NODE_TYPES.eq,
      category: 'Utility',
      params: { hpOn: 1, hpUsed: 1, hpFreq: 80, hpSlope: 2 },
    },
    {
      id: 'eq-reset',
      name: 'Flat / Reset All',
      type: NODE_TYPES.eq,
      category: 'Utility',
      params: { hpUsed: 0, lowUsed: 0, mid1Used: 0, mid2Used: 0, mid3Used: 0, mid4Used: 0, highUsed: 0, lpUsed: 0 },
    },
  ],

  [NODE_TYPES.filter]: [
    {
      id: 'filter-warm-24db',
      name: 'Warm 24dB Lowpass',
      type: NODE_TYPES.filter,
      params: { mode: 0, cutoff: 1200, resonance: 25, drive: 2.0, slope: 1 },
    },
    {
      id: 'filter-acid-squelch',
      name: 'Acid Resonant Squelch',
      type: NODE_TYPES.filter,
      params: { mode: 0, cutoff: 800, resonance: 85, drive: 9.0, slope: 1 },
    },
    {
      id: 'filter-steep-48db',
      name: 'Steep 48dB Lowpass',
      type: NODE_TYPES.filter,
      params: { mode: 0, cutoff: 2500, resonance: 15, drive: 0.0, slope: 3 },
    },
    {
      id: 'filter-dj-highpass',
      name: 'DJ Highpass Sweep',
      type: NODE_TYPES.filter,
      params: { mode: 1, cutoff: 350, resonance: 30, drive: 1.5, slope: 1 },
    },
    {
      id: 'filter-bandpass',
      name: 'Bandpass Vocalizer',
      type: NODE_TYPES.filter,
      params: { mode: 2, cutoff: 1500, resonance: 60, drive: 3.0, slope: 1 },
    },
    {
      id: 'filter-notch',
      name: 'Notch Phasing',
      type: NODE_TYPES.filter,
      params: { mode: 3, cutoff: 1000, resonance: 75, drive: 4.0, slope: 1 },
    },
    {
      id: 'filter-gentle-12db',
      name: 'Gentle 12dB Rolloff',
      type: NODE_TYPES.filter,
      params: { mode: 0, cutoff: 5000, resonance: 5, drive: 0.0, slope: 0 },
    },
  ],

  [NODE_TYPES.compressor]: [
    {
      id: 'init',
      name: 'Init',
      type: NODE_TYPES.compressor,
      category: 'Drums',
      params: {
        threshold: 0,
        ratio: 2.0,
        attack: 25,
        release: 100,
        makeup: 0.0,
        mix: 100,
        detector: 0,
        keyHp: 0,
        keyHpFreq: 100,
        keyLp: 0,
        keyLpFreq: 8000,
      },
    },
    {
      id: 'comp-drum-bus',
      name: 'Drum Bus Glue',
      type: NODE_TYPES.compressor,
      category: 'Drums',
      params: { threshold: -16, ratio: 3.0, attack: 30, release: 100, makeup: 3.0, mix: 100, detector: 0, keyHp: 1, keyHpFreq: 90 },
    },
    {
      id: 'comp-vocal-leveller',
      name: 'Vocal Leveller',
      type: NODE_TYPES.compressor,
      category: 'Vocal',
      params: { threshold: -20, ratio: 4.0, attack: 15, release: 250, makeup: 4.5, mix: 100, detector: 0, keyHp: 0 },
    },
    {
      id: 'comp-punchy-snare',
      name: 'Punchy Snare',
      type: NODE_TYPES.compressor,
      category: 'Drums',
      params: { threshold: -14, ratio: 6.0, attack: 45, release: 60, makeup: 2.0, mix: 100, detector: 0, keyHp: 0 },
    },
    {
      id: 'comp-parallel-smash',
      name: 'Parallel Smash',
      type: NODE_TYPES.compressor,
      category: 'Creative',
      params: { threshold: -28, ratio: 12.0, attack: 0.5, release: 40, makeup: 8.0, mix: 50, detector: 0, keyHp: 0 },
    },
    {
      id: 'comp-gentle-master',
      name: 'Gentle Master Bus',
      type: NODE_TYPES.compressor,
      category: 'Mix',
      params: { threshold: -10, ratio: 1.8, attack: 50, release: 150, makeup: 1.0, mix: 100, detector: 0, keyHp: 1, keyHpFreq: 120 },
    },
  ],

  // Gate and Expander (docs/design/gate-expander.md, N14). Lookahead by index: 0 off, 1 is 0.5 ms, 2 is 1 ms, 3 is 2 ms.
  [NODE_TYPES.gate]: [
    {
      id: 'gate-default',
      name: 'Default',
      type: NODE_TYPES.gate,
      category: 'General',
      params: { threshold: -40, hysteresis: 6, range: -80, attack: 0.5, hold: 20, release: 100, lookahead: 0, detection: 0, keyHp: 0, keyHpFreq: 100, keyLp: 0, keyLpFreq: 8000 },
    },
    {
      id: 'gate-kick',
      name: 'Kick',
      type: NODE_TYPES.gate,
      category: 'Drums',
      params: { threshold: -30, hysteresis: 6, range: -80, attack: 0.05, hold: 30, release: 80, lookahead: 2, detection: 0, keyHp: 0, keyHpFreq: 100, keyLp: 1, keyLpFreq: 150 },
    },
    {
      id: 'gate-snare',
      name: 'Snare',
      type: NODE_TYPES.gate,
      category: 'Drums',
      params: { threshold: -28, hysteresis: 6, range: -80, attack: 0.1, hold: 40, release: 120, lookahead: 2, detection: 0, keyHp: 1, keyHpFreq: 200, keyLp: 0, keyLpFreq: 8000 },
    },
    {
      id: 'gate-toms',
      name: 'Toms',
      type: NODE_TYPES.gate,
      category: 'Drums',
      params: { threshold: -35, hysteresis: 8, range: -80, attack: 0.5, hold: 80, release: 300, lookahead: 2, detection: 0, keyHp: 0, keyHpFreq: 100, keyLp: 1, keyLpFreq: 300 },
    },
    {
      id: 'gate-gentle',
      name: 'Gentle',
      type: NODE_TYPES.gate,
      category: 'General',
      params: { threshold: -45, hysteresis: 4, range: -12, attack: 1, hold: 20, release: 250, lookahead: 0, detection: 0, keyHp: 0, keyHpFreq: 100, keyLp: 0, keyLpFreq: 8000 },
    },
  ],

  [NODE_TYPES.expander]: [
    {
      id: 'expander-default',
      name: 'Default',
      type: NODE_TYPES.expander,
      category: 'General',
      params: { threshold: -40, ratio: 2, range: -40, attack: 1, hold: 10, release: 150, lookahead: 0, detection: 1, keyHp: 0, keyHpFreq: 100, keyLp: 0, keyLpFreq: 8000 },
    },
    {
      id: 'expander-noise',
      name: 'Noise',
      type: NODE_TYPES.expander,
      category: 'Cleanup',
      params: { threshold: -50, ratio: 2, range: -20, attack: 1, hold: 10, release: 200, lookahead: 0, detection: 1, keyHp: 0, keyHpFreq: 100, keyLp: 0, keyLpFreq: 8000 },
    },
    {
      id: 'expander-vocal',
      name: 'Vocal',
      type: NODE_TYPES.expander,
      category: 'Vocal',
      params: { threshold: -45, ratio: 1.5, range: -12, attack: 2, hold: 20, release: 200, lookahead: 0, detection: 1, keyHp: 0, keyHpFreq: 100, keyLp: 0, keyLpFreq: 8000 },
    },
    {
      id: 'expander-tight',
      name: 'Tight',
      type: NODE_TYPES.expander,
      category: 'Drums',
      params: { threshold: -35, ratio: 4, range: -40, attack: 0.5, hold: 10, release: 80, lookahead: 1, detection: 0, keyHp: 0, keyHpFreq: 100, keyLp: 0, keyLpFreq: 8000 },
    },
  ],

  [NODE_TYPES.saturator]: [
    {
      id: 'sat-tape-warmth',
      name: 'Tape Warmth',
      type: NODE_TYPES.saturator,
      params: { drive: 8.0, bias: 5, output: -1.0, mix: 100, oversampling: 1 },
    },
    {
      id: 'sat-tube-drive',
      name: 'Tube Drive & Grit',
      type: NODE_TYPES.saturator,
      params: { drive: 18.0, bias: 20, output: -3.5, mix: 100, oversampling: 1 },
    },
    {
      id: 'sat-crisp-exciter',
      name: 'Crisp Air Exciter',
      type: NODE_TYPES.saturator,
      params: { drive: 12.0, bias: 0, output: -1.5, mix: 40, oversampling: 2 },
    },
    {
      id: 'sat-asymmetric-fuzz',
      name: 'Asymmetric Fuzz',
      type: NODE_TYPES.saturator,
      params: { drive: 28.0, bias: 45, output: -6.0, mix: 100, oversampling: 2 },
    },
    {
      id: 'sat-clean-4x',
      name: 'Clean Saturation 4x',
      type: NODE_TYPES.saturator,
      params: { drive: 6.0, bias: 0, output: 0.0, mix: 100, oversampling: 2 },
    },
  ],

  [NODE_TYPES.clipper]: [
    {
      id: 'clip-transparent-peaks',
      name: 'Transparent Peaks',
      type: NODE_TYPES.clipper,
      params: { drive: 3.0, ceiling: -0.3, knee: 0, output: 0, mix: 100, oversampling: 2 },
    },
    {
      id: 'clip-drum-bus-snap',
      name: 'Drum Bus Snap',
      type: NODE_TYPES.clipper,
      params: { drive: 6.0, ceiling: -1.0, knee: 20, output: 0, mix: 100, oversampling: 1 },
    },
    {
      id: 'clip-soft-warmth',
      name: 'Soft Clip Warmth',
      type: NODE_TYPES.clipper,
      params: { drive: 6.0, ceiling: -3.0, knee: 100, output: 0, mix: 100, oversampling: 1 },
    },
    {
      id: 'clip-master',
      name: 'Master Clipper',
      type: NODE_TYPES.clipper,
      params: { drive: 2.0, ceiling: -0.1, knee: 10, output: 0, mix: 100, oversampling: 2 },
    },
    {
      id: 'clip-per-voice-grit',
      name: 'Per-voice Grit',
      type: NODE_TYPES.clipper,
      params: { drive: 12.0, ceiling: -6.0, knee: 40, output: 0, mix: 100, oversampling: 0 },
    },
  ],

  // Cycles by their index in lfoCycleChoices: 13 is 1/4, 19 is 1/1, 22 is 2/1 (docs/design/modulation-effects.md).
  [NODE_TYPES.chorus]: [
    {
      id: 'chorus-subtle',
      name: 'Subtle',
      type: NODE_TYPES.chorus,
      params: { sync: 0, rate: 0.8, cycle: 19, shape: 0, spread: 90, delay: 15, depth: 15, voices: 2, feedback: 0, mix: 50 },
    },
    {
      id: 'chorus-wide',
      name: 'Wide',
      type: NODE_TYPES.chorus,
      params: { sync: 0, rate: 0.6, cycle: 19, shape: 0, spread: 180, delay: 18, depth: 20, voices: 3, feedback: 0, mix: 50 },
    },
    {
      id: 'chorus-vibrato',
      name: 'Vibrato',
      type: NODE_TYPES.chorus,
      params: { sync: 0, rate: 5, cycle: 19, shape: 0, spread: 0, delay: 6, depth: 40, voices: 1, feedback: 0, mix: 100 },
    },
    {
      id: 'chorus-resonant',
      name: 'Resonant',
      type: NODE_TYPES.chorus,
      params: { sync: 0, rate: 0.5, cycle: 19, shape: 1, spread: 90, delay: 8, depth: 25, voices: 2, feedback: 60, mix: 50 },
    },
  ],

  [NODE_TYPES.flanger]: [
    {
      id: 'flanger-classic',
      name: 'Classic',
      type: NODE_TYPES.flanger,
      params: { sync: 0, rate: 0.2, cycle: 22, shape: 1, spread: 90, delay: 2, depth: 70, feedback: 50, throughZero: 0, mix: 50 },
    },
    {
      id: 'flanger-jet',
      name: 'Jet',
      type: NODE_TYPES.flanger,
      params: { sync: 0, rate: 0.1, cycle: 22, shape: 1, spread: 90, delay: 3, depth: 100, feedback: 85, throughZero: 0, mix: 50 },
    },
    {
      id: 'flanger-hollow',
      name: 'Hollow',
      type: NODE_TYPES.flanger,
      params: { sync: 0, rate: 0.25, cycle: 22, shape: 1, spread: 90, delay: 2, depth: 70, feedback: -70, throughZero: 0, mix: 50 },
    },
    {
      id: 'flanger-tape',
      name: 'Tape',
      type: NODE_TYPES.flanger,
      params: { sync: 0, rate: 0.15, cycle: 22, shape: 0, spread: 90, delay: 3, depth: 100, feedback: 0, throughZero: 1, mix: 50 },
    },
  ],

  [NODE_TYPES.phaser]: [
    {
      id: 'phaser-gentle',
      name: 'Gentle',
      type: NODE_TYPES.phaser,
      params: { sync: 0, rate: 0.4, cycle: 19, shape: 0, spread: 90, stages: 1, centre: 700, depth: 60, feedback: 40, mix: 50 },
    },
    {
      id: 'phaser-deep',
      name: 'Deep',
      type: NODE_TYPES.phaser,
      params: { sync: 0, rate: 0.3, cycle: 19, shape: 0, spread: 90, stages: 3, centre: 600, depth: 70, feedback: 70, mix: 50 },
    },
    {
      id: 'phaser-swirl',
      name: 'Swirl',
      type: NODE_TYPES.phaser,
      params: { sync: 0, rate: 0.15, cycle: 19, shape: 0, spread: 180, stages: 5, centre: 800, depth: 80, feedback: 50, mix: 50 },
    },
    {
      id: 'phaser-synced',
      name: 'Synced',
      type: NODE_TYPES.phaser,
      params: { sync: 1, rate: 0.4, cycle: 13, shape: 1, spread: 90, stages: 1, centre: 1000, depth: 60, feedback: 40, mix: 50 },
    },
  ],

  // Only parameters: the impulse response stays (docs/design/convolution-reverb.md, the panel).
  [NODE_TYPES.convolutionReverb]: [
    {
      id: 'conv-natural',
      name: 'Natural',
      type: NODE_TYPES.convolutionReverb,
      params: { predelay: 0, start: 0, length: 100, decay: 100, reverse: 0, stretch: 100, normalise: 1, lowCut: 20, highCut: 20000, width: 100, gain: 0, mix: 25 },
    },
    {
      id: 'conv-tight-room',
      name: 'Tight Room',
      type: NODE_TYPES.convolutionReverb,
      params: { predelay: 0, start: 0, length: 40, decay: 60, reverse: 0, stretch: 100, normalise: 1, lowCut: 150, highCut: 20000, width: 100, gain: 0, mix: 25 },
    },
    {
      id: 'conv-dark-hall',
      name: 'Dark Hall',
      type: NODE_TYPES.convolutionReverb,
      params: { predelay: 30, start: 0, length: 100, decay: 100, reverse: 0, stretch: 100, normalise: 1, lowCut: 20, highCut: 5000, width: 100, gain: 0, mix: 30 },
    },
    {
      id: 'conv-reverse-swell',
      name: 'Reverse Swell',
      type: NODE_TYPES.convolutionReverb,
      params: { predelay: 0, start: 0, length: 60, decay: 100, reverse: 1, stretch: 100, normalise: 1, lowCut: 20, highCut: 20000, width: 100, gain: 0, mix: 40 },
    },
    {
      id: 'conv-wide-send',
      name: 'Wide Send',
      type: NODE_TYPES.convolutionReverb,
      params: { predelay: 0, start: 0, length: 100, decay: 100, reverse: 0, stretch: 100, normalise: 1, lowCut: 200, highCut: 20000, width: 150, gain: 0, mix: 100 },
    },
  ],

  [NODE_TYPES.waveshaper]: [
    {
      id: 'ws-gentle-warmth',
      name: 'Gentle Warmth',
      type: NODE_TYPES.waveshaper,
      params: { shape: 0, drive: 6, bias: 0, tone: -2, output: 0, mix: 100, oversampling: 1 },
    },
    {
      id: 'ws-tube-drive',
      name: 'Tube Drive',
      type: NODE_TYPES.waveshaper,
      params: { shape: 1, drive: 12, bias: 0, tone: -3, output: -3, mix: 100, oversampling: 1 },
    },
    {
      id: 'ws-west-coast-fold',
      name: 'West Coast Fold',
      type: NODE_TYPES.waveshaper,
      params: { shape: 2, drive: 18, bias: 0, tone: -4, output: 0, mix: 100, oversampling: 2 },
    },
    {
      id: 'ws-asymmetric-fold',
      name: 'Asymmetric Fold',
      type: NODE_TYPES.waveshaper,
      params: { shape: 2, drive: 15, bias: 40, tone: -3, output: 0, mix: 100, oversampling: 2 },
    },
    {
      id: 'ws-sine-folder',
      name: 'Sine Folder',
      type: NODE_TYPES.waveshaper,
      params: { shape: 3, drive: 18, bias: 0, tone: 0, output: 0, mix: 100, oversampling: 2 },
    },
    {
      id: 'ws-octave-up',
      name: 'Octave Up',
      type: NODE_TYPES.waveshaper,
      params: { shape: 5, drive: 6, bias: 0, tone: -3, output: 0, mix: 100, oversampling: 1 },
    },
    {
      id: 'ws-half-wave-buzz',
      name: 'Half-wave Buzz',
      type: NODE_TYPES.waveshaper,
      params: { shape: 4, drive: 12, bias: 20, tone: 0, output: 0, mix: 100, oversampling: 1 },
    },
    {
      id: 'ws-drawn-steps',
      name: 'Drawn Steps',
      type: NODE_TYPES.waveshaper,
      params: { shape: 6, drive: 0, bias: 0, tone: -3, output: 0, mix: 100, oversampling: 2, symmetric: 1 },
      shape: ASYMMETRIC_PRESETS.Steps,
    },
    {
      id: 'ws-drawn-half-wave',
      name: 'Drawn Half-wave',
      type: NODE_TYPES.waveshaper,
      params: { shape: 6, drive: 3, bias: 0, tone: -2, output: 0, mix: 100, oversampling: 1, symmetric: 0 },
      shape: ASYMMETRIC_PRESETS.Half,
    },
  ],

  [NODE_TYPES.bitcrusher]: [
    {
      id: 'bc-default',
      name: 'Default',
      type: NODE_TYPES.bitcrusher,
      category: 'General',
      params: { bits: 8, quantiser: 0, rateMode: 0, rate: 11000, note: 24, jitter: 0, antiAlias: 0, smooth: 0, dither: 0, drive: 0, clip: 0, bias: 0, mix: 100, output: 0 },
    },
    {
      id: 'bc-8-bit',
      name: '8-bit',
      type: NODE_TYPES.bitcrusher,
      category: 'General',
      params: { bits: 8, quantiser: 0, rateMode: 0, rate: 22000, note: 24, jitter: 0, antiAlias: 0, smooth: 0, dither: 0, drive: 0, clip: 0, bias: 0, mix: 100, output: 0 },
    },
    {
      id: 'bc-console',
      name: 'Console',
      type: NODE_TYPES.bitcrusher,
      category: 'Retro',
      params: { bits: 4, quantiser: 1, rateMode: 0, rate: 16000, note: 24, jitter: 0, antiAlias: 0, smooth: 0, dither: 0, drive: 0, clip: 0, bias: 0, mix: 100, output: 0 },
    },
    {
      id: 'bc-telephone',
      name: 'Telephone',
      type: NODE_TYPES.bitcrusher,
      category: 'Retro',
      params: { bits: 12, quantiser: 0, rateMode: 0, rate: 8000, note: 24, jitter: 0, antiAlias: 1, smooth: 1, dither: 0, drive: 0, clip: 0, bias: 0, mix: 100, output: 0 },
    },
    {
      id: 'bc-old-sampler',
      name: 'Old Sampler',
      type: NODE_TYPES.bitcrusher,
      category: 'Retro',
      params: { bits: 12, quantiser: 0, rateMode: 0, rate: 26000, note: 24, jitter: 0, antiAlias: 0, smooth: 1, dither: 0, drive: 0, clip: 0, bias: 0, mix: 100, output: 0 },
    },
    {
      id: 'bc-1-bit-fuzz',
      name: '1-bit Fuzz',
      type: NODE_TYPES.bitcrusher,
      category: 'Destroy',
      params: { bits: 1, quantiser: 1, rateMode: 0, rate: 192000, note: 24, jitter: 0, antiAlias: 0, smooth: 0, dither: 0, drive: 12, clip: 1, bias: 0, mix: 100, output: -6 },
    },
    {
      id: 'bc-broken',
      name: 'Broken',
      type: NODE_TYPES.bitcrusher,
      category: 'Destroy',
      params: { bits: 5, quantiser: 0, rateMode: 0, rate: 6000, note: 24, jitter: 60, antiAlias: 0, smooth: 0, dither: 0, drive: 0, clip: 0, bias: 50, mix: 100, output: 0 },
    },
    {
      id: 'bc-tuned',
      name: 'Tuned',
      type: NODE_TYPES.bitcrusher,
      category: 'Voice',
      params: { bits: 6, quantiser: 0, rateMode: 1, rate: 11000, note: 24, jitter: 0, antiAlias: 0, smooth: 0, dither: 0, drive: 0, clip: 0, bias: 0, mix: 100, output: 0 },
    },
  ],

  [NODE_TYPES.utility]: [
    {
      id: 'util-stereo-widener',
      name: 'Stereo Widener (150%)',
      type: NODE_TYPES.utility,
      params: { gain: 0, pan: 0, width: 150, invertLeft: 0, invertRight: 0, mono: 0 },
    },
    {
      id: 'util-mono-sum',
      name: 'Mono Sum / Bass Mono',
      type: NODE_TYPES.utility,
      params: { gain: 0, pan: 0, width: 0, invertLeft: 0, invertRight: 0, mono: 1 },
    },
    {
      id: 'util-side-only',
      name: 'Side Only (Ambience)',
      type: NODE_TYPES.utility,
      params: { gain: 0, pan: 0, width: 200, invertLeft: 0, invertRight: 0, mono: 0 },
    },
    {
      id: 'util-phase-invert',
      name: 'Phase Invert Right',
      type: NODE_TYPES.utility,
      params: { gain: 0, pan: 0, width: 100, invertLeft: 0, invertRight: 1, mono: 0 },
    },
    {
      id: 'util-boost-3db',
      name: '+3 dB Clean Boost',
      type: NODE_TYPES.utility,
      params: { gain: 3.0, pan: 0, width: 100, invertLeft: 0, invertRight: 0, mono: 0 },
    },
  ],

  [NODE_TYPES.lfo]: [
    {
      id: 'lfo-1-4-sync',
      name: '1/4 Note Sync',
      type: NODE_TYPES.lfo,
      params: { sync: 1, cycle: 1, rateMs: 500, retrigger: 1, phase: 0, zero: 1 },
    },
    {
      id: 'lfo-1-8-wobble',
      name: '1/8 Note Wobble',
      type: NODE_TYPES.lfo,
      params: { sync: 1, cycle: 2, rateMs: 250, retrigger: 1, phase: 0, zero: 1 },
    },
    {
      id: 'lfo-slow-evolution',
      name: 'Slow 4-Bar Evolution',
      type: NODE_TYPES.lfo,
      params: { sync: 1, cycle: 7, rateMs: 8000, retrigger: 0, phase: 0, zero: 1 },
    },
    {
      id: 'lfo-fast-vibrato',
      name: 'Free Fast Vibrato (6 Hz)',
      type: NODE_TYPES.lfo,
      params: { sync: 0, rateMs: 166, cycle: 1, retrigger: 0, phase: 0, zero: 1 },
    },
    {
      id: 'lfo-unipolar-half',
      name: 'Unipolar 1/2 Note',
      type: NODE_TYPES.lfo,
      params: { sync: 1, cycle: 0, rateMs: 1000, retrigger: 1, phase: 0, zero: 0 },
    },
  ],

  [NODE_TYPES.adsr]: [
    {
      id: 'adsr-pluck',
      name: 'Pluck / Staccato',
      type: NODE_TYPES.adsr,
      params: { attackMs: 2, decayMs: 180, sustain: 0, releaseMs: 120, attackHandleTime: 50, attackHandleLevel: 50, decayHandleTime: 35, decayHandleLevel: 65, releaseHandleTime: 50, releaseHandleLevel: 50 },
    },
    {
      id: 'adsr-ambient-pad',
      name: 'Slow Ambient Pad',
      type: NODE_TYPES.adsr,
      params: { attackMs: 1200, decayMs: 800, sustain: 85, releaseMs: 1800, attackHandleTime: 65, attackHandleLevel: 35, decayHandleTime: 50, decayHandleLevel: 50, releaseHandleTime: 70, releaseHandleLevel: 30 },
    },
    {
      id: 'adsr-punchy-bass',
      name: 'Punchy Bass',
      type: NODE_TYPES.adsr,
      params: { attackMs: 1, decayMs: 350, sustain: 40, releaseMs: 200, attackHandleTime: 50, attackHandleLevel: 50, decayHandleTime: 40, decayHandleLevel: 60, releaseHandleTime: 50, releaseHandleLevel: 50 },
    },
    {
      id: 'adsr-gate-organ',
      name: 'Gate Organ',
      type: NODE_TYPES.adsr,
      params: { attackMs: 1, decayMs: 50, sustain: 100, releaseMs: 10, attackHandleTime: 50, attackHandleLevel: 50, decayHandleTime: 50, decayHandleLevel: 50, releaseHandleTime: 50, releaseHandleLevel: 50 },
    },
  ],
};

// The user's presets, kept here and saved in the app's settings (the web view's storage belongs to one profile).
let saved: string | null = null;
let loading: Promise<void> | null = null;

function savePresets(value: string) {
  saved = value;
  if (typeof window !== 'undefined' && bridge.isConnected()) bridge.call('settings.set', { key: STORAGE_KEY, value }).catch(() => {});
}

/** Loads the saved presets once and tells the selectors. Presets from before they moved to the settings come along. */
export function loadUserPresets(): Promise<void> {
  loading ??= bridge
    .call('settings.get', { key: STORAGE_KEY })
    .then((value) => {
      const older = window.localStorage.getItem(STORAGE_KEY);
      if (value === null && older !== null) savePresets(older);
      else saved = value;
      window.dispatchEvent(new CustomEvent('dawg_presets_changed'));
    })
    .catch(() => {});
  return loading;
}

export function clearUserPresetsForTesting(): void {
  saved = null;
  loading = null;
}

/** Checks whether presets are supported for a node type. */
export function hasPresetsFor(type: string): boolean {
  return type in FACTORY_PRESETS;
}

/** Reads the user's presets. */
export function getUserPresets(type?: string): NodePreset[] {
  try {
    const raw = saved;
    if (!raw) return [];
    const parsed = JSON.parse(raw) as NodePreset[];
    if (!Array.isArray(parsed)) return [];
    return type ? parsed.filter((p) => p.type === type) : parsed;
  } catch {
    return [];
  }
}

/** Saves a new custom preset created from the current node's settings. */
export function saveUserPreset(
  type: string,
  name: string,
  params: Record<string, number>,
  shape?: CurvePoint[],
  drumPads?: DrumPad[],
): NodePreset {
  const trimmed = name.trim();
  if (!trimmed) throw new Error('Preset name cannot be empty');

  const newPreset: NodePreset = {
    id: `user-${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
    name: trimmed,
    type,
    params: { ...params },
    ...(shape === undefined ? {} : { shape: shape.map((point) => [...point] as CurvePoint) }),
    ...(drumPads === undefined ? {} : { drumPads: drumPads.map((pad) => ({ ...pad })) }),
    isUser: true,
  };

  const existing = getUserPresets();
  const updated = [newPreset, ...existing];
  savePresets(JSON.stringify(updated));

  try {
    if (typeof window !== 'undefined') {
      window.dispatchEvent(new CustomEvent('dawg_presets_changed', { detail: { type } }));
    }
  } catch {
    // ignore
  }

  return newPreset;
}

/** Deletes a custom user preset by id. */
export function deleteUserPreset(type: string, id: string): boolean {
  const existing = getUserPresets();
  const filtered = existing.filter((p) => p.id !== id);
  if (filtered.length === existing.length) return false;

  savePresets(JSON.stringify(filtered));

  try {
    if (typeof window !== 'undefined') {
      window.dispatchEvent(new CustomEvent('dawg_presets_changed', { detail: { type } }));
    }
  } catch {
    // ignore
  }

  return true;
}

/** Gets all presets for a node type: custom user presets first, then factory presets. */
export function getPresetsForNode(type: string): { user: NodePreset[]; factory: NodePreset[] } {
  const user = getUserPresets(type);
  const factory = FACTORY_PRESETS[type] ?? [];
  return { user, factory };
}

/** Applies a preset to a graph node by batch-dispatching setNodeParam commands. */
export function applyPreset(track: Track, node: GraphNode, preset: NodePreset) {
  // A Drum Synth kit is the whole node: what it doesn't set goes back to its default, and its pads come along (DR13).
  const commands: Command[] =
    node.type === NODE_TYPES.drumSynth
      ? kitCommands({ trackId: track.id, nodeId: node.id }, node, useDawg.getState().nodeTypes[node.type]?.params ?? [], preset)
      : Object.entries(preset.params).map(([param, value]) => ({
          command: 'setNodeParam',
          args: { trackId: track.id, nodeId: node.id, param, value },
        }));

  // A saved LFO preset carries its shape, and a Waveshaper preset its drawn curve; loading it replaces the node's
  // current one in the same undo step.
  if (Array.isArray(preset.shape) && (node.type === NODE_TYPES.lfo || node.type === NODE_TYPES.waveshaper)) {
    commands.push({
      command: 'setModulationShape',
      args: {
        trackId: track.id,
        nodeId: node.id,
        points: preset.shape.map((point) => [...point] as CurvePoint),
      },
    });
  }

  if (commands.length === 0) return;

  dispatch({
    command: 'batch',
    args: {
      label: `Preset: ${preset.name}`,
      commands,
    },
  });
}

/** Checks if the node's current parameters exactly or closely match a preset. */
export function detectMatchingPreset(
  type: string,
  nodeParams: Record<string, number>,
  info?: NodeTypeInfo,
): { preset: NodePreset; isExact: boolean } | null {
  const { user, factory } = getPresetsForNode(type);
  const all = [...user, ...factory];

  for (const preset of all) {
    const entries = Object.entries(preset.params);
    if (entries.length === 0) continue;

    let isExact = true;
    for (const [key, target] of entries) {
      const current = nodeParams[key] ?? info?.params.find((p) => p.id === key)?.default ?? 0;
      if (Math.abs(current - target) > 0.05) {
        isExact = false;
        break;
      }
    }

    if (isExact) {
      return { preset, isExact: true };
    }
  }

  return null;
}
