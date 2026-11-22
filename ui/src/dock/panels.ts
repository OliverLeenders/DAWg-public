import type { ComponentType } from 'react';
import { NODE_TYPES, type GraphNode, type NodeTypeInfo, type Track } from '../bridge/protocol';
import { AdsrPanel } from './AdsrPanel';
import { BitCrusherPanel } from './BitCrusherPanel';
import { ClipperPanel } from './ClipperPanel';
import { ComponentPanel } from './ComponentPanel';
import { CompressorPanel } from './CompressorPanel';
import { ConvolutionReverbPanel } from './ConvolutionReverbPanel';
import { DelayPanel } from './DelayPanel';
import { DrumSynthPanel } from './DrumSynthPanel';
import { EqPanel } from './EqPanel';
import { FeedbackPanel } from './FeedbackPanel';
import { FilterPanel } from './FilterPanel';
import { GatePanel } from './GatePanel';
import { LfoPanel } from './LfoPanel';
import { LimiterPanel } from './LimiterPanel';
import { LoopReverbPanel } from './LoopReverbPanel';
import { ArpeggiatorPanel, ChordPanel, NoteRepeatPanel, ScaleLockPanel } from './MidiEffectPanels';
import { OscillatorPanel } from './OscillatorPanel';
import { StepSequencerPanel } from './StepSequencerPanel';
import { ChorusPanel, FlangerPanel, PhaserPanel } from './ModulationEffectPanels';
import { SamplerPanel } from './SamplerPanel';
import { SaturatorPanel } from './SaturatorPanel';
import { UtilityPanel } from './UtilityPanel';
import { WaveshaperPanel } from './WaveshaperPanel';

// The graphical panels of the built-in effects (docs/design/phase11-faust-panels.md, P4), by node type.

export interface PanelProps {
  track: Track;
  node: GraphNode;
  info: NodeTypeInfo;
}

const PANELS: Record<string, ComponentType<PanelProps>> = {
  [NODE_TYPES.eq]: EqPanel,
  [NODE_TYPES.filter]: FilterPanel,
  [NODE_TYPES.saturator]: SaturatorPanel,
  [NODE_TYPES.clipper]: ClipperPanel,
  [NODE_TYPES.waveshaper]: WaveshaperPanel,
  [NODE_TYPES.bitcrusher]: BitCrusherPanel,
  [NODE_TYPES.delay]: DelayPanel,
  [NODE_TYPES.feedback]: FeedbackPanel,
  [NODE_TYPES.limiter]: LimiterPanel,
  [NODE_TYPES.sampler]: SamplerPanel,
  [NODE_TYPES.sampleVoice]: SamplerPanel,
  [NODE_TYPES.drumSynth]: DrumSynthPanel,
  [NODE_TYPES.compressor]: CompressorPanel,
  [NODE_TYPES.gate]: GatePanel,
  [NODE_TYPES.expander]: GatePanel,
  [NODE_TYPES.utility]: UtilityPanel,
  [NODE_TYPES.loopReverb]: LoopReverbPanel,
  [NODE_TYPES.convolutionReverb]: ConvolutionReverbPanel,
  [NODE_TYPES.chorus]: ChorusPanel,
  [NODE_TYPES.flanger]: FlangerPanel,
  [NODE_TYPES.phaser]: PhaserPanel,
  [NODE_TYPES.lfo]: LfoPanel,
  [NODE_TYPES.adsr]: AdsrPanel,
  [NODE_TYPES.component]: ComponentPanel,
  [NODE_TYPES.arpeggiator]: ArpeggiatorPanel,
  [NODE_TYPES.chord]: ChordPanel,
  [NODE_TYPES.scaleLock]: ScaleLockPanel,
  [NODE_TYPES.noteRepeat]: NoteRepeatPanel,
  [NODE_TYPES.stepSequencer]: StepSequencerPanel,
  [NODE_TYPES.oscillator]: OscillatorPanel,
};

export const panelFor = (type: string): ComponentType<PanelProps> | null => PANELS[type] ?? null;
