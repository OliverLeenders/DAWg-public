import { NODE_TYPES, type GraphNode } from '../bridge/protocol';
import { drumParamApplies } from '../dock/drumSynth';

// The parameters that apply right now. The LFO's (docs/design/modulation.md, M4): a free-running LFO has a rate in
// ms, a tempo-synced one a cycle as a note length. The oscillator's depend on its waveform, unison and phase mode.
// The inspector and the dock show the same ones.

/** Whether an LFO is tempo-synced. */
export const lfoSynced = (node: GraphNode) => (node.params.sync ?? 0) >= 0.5;

/** Whether a node's parameter applies in its current mode (always, for anything but an LFO, an oscillator or a Drum
    Synth, whose pads show their own model's). */
export function paramApplies(node: GraphNode, paramId: string) {
  if (node.type === NODE_TYPES.oscillator) return oscillatorParamApplies(node, paramId);
  if (node.type === NODE_TYPES.drumSynth) return drumParamApplies(node, paramId);
  if (node.type !== NODE_TYPES.lfo) return true;
  if (paramId === 'rateMs') return !lfoSynced(node);
  if (paramId === 'cycle') return lfoSynced(node);
  return true;
}

/**
 * Whether a parameter that is shown does anything now (docs/ui-review-2026-11-10.md, 1.8): controls that don't are
 * dimmed rather than hidden, because the mode beside them brings them back. A fixed velocity is used only when
 * velocity is "Fixed", the Chord's strum order only when it strums.
 */
export function paramActive(node: GraphNode, paramId: string) {
  if ((node.type === NODE_TYPES.arpeggiator || node.type === NODE_TYPES.noteRepeat) && paramId === 'fixedVelocity') {
    return Math.round(node.params.velocity ?? 0) === 1;
  }
  if (node.type === NODE_TYPES.chord && paramId === 'strumOrder') return (node.params.strum ?? 0) > 0;
  return paramApplies(node, paramId);
}

// The oscillator's pulse width applies to the pulse wave, detune and spread to unison, the phase and random phase
// unless it runs free (docs/design/phase12-voice-scope.md, V14).
const PULSE_WAVEFORM = 3;

function oscillatorParamApplies(node: GraphNode, paramId: string) {
  if (paramId === 'pulseWidth') return Math.round(node.params.waveform ?? 2) === PULSE_WAVEFORM;
  if (paramId === 'detune' || paramId === 'spread') return Math.round(node.params.unison ?? 1) > 1;
  if (paramId === 'phase' || paramId === 'randomPhase') return (node.params.free ?? 0) < 0.5;
  return true;
}
