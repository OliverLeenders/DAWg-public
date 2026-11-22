import { NODE_TYPES, type GraphNode } from '../bridge/protocol';

// How wide a device's card is in the device strip, by node type (docs/design/device-strip.md, D5 and section 4). The minimum
// is a floor, where its panel can still be used; the card also never gets narrower than its panel needs at the dock's
// current height, measured live (panelMinWidth.ts). The default is where it reads well.

export interface DeviceWidth {
  initial: number;
  min: number;
}

const WIDTHS: Record<string, DeviceWidth> = {
  [NODE_TYPES.eq]: { initial: 420, min: 240 },
  [NODE_TYPES.filter]: { initial: 360, min: 280 },
  [NODE_TYPES.compressor]: { initial: 320, min: 280 },
  [NODE_TYPES.gate]: { initial: 640, min: 560 },
  [NODE_TYPES.expander]: { initial: 640, min: 560 },
  [NODE_TYPES.saturator]: { initial: 300, min: 280 },
  [NODE_TYPES.clipper]: { initial: 440, min: 380 },
  [NODE_TYPES.waveshaper]: { initial: 460, min: 380 },
  [NODE_TYPES.bitcrusher]: { initial: 800, min: 640 },
  [NODE_TYPES.delay]: { initial: 300, min: 300 },
  [NODE_TYPES.limiter]: { initial: 260, min: 200 },
  [NODE_TYPES.utility]: { initial: 260, min: 200 },
  [NODE_TYPES.feedback]: { initial: 340, min: 320 },
  [NODE_TYPES.adsr]: { initial: 320, min: 200 },
  [NODE_TYPES.lfo]: { initial: 420, min: 320 },
  [NODE_TYPES.loopReverb]: { initial: 660, min: 640 },
  [NODE_TYPES.convolutionReverb]: { initial: 760, min: 640 },
  [NODE_TYPES.chorus]: { initial: 560, min: 440 },
  [NODE_TYPES.flanger]: { initial: 560, min: 440 },
  [NODE_TYPES.phaser]: { initial: 560, min: 440 },
  [NODE_TYPES.stepSequencer]: { initial: 920, min: 700 },
  [NODE_TYPES.arpeggiator]: { initial: 940, min: 900 },
  [NODE_TYPES.oscillator]: { initial: 560, min: 420 },
  [NODE_TYPES.sampler]: { initial: 880, min: 784 },
  [NODE_TYPES.sampleVoice]: { initial: 880, min: 824 },
  [NODE_TYPES.drumSynth]: { initial: 860, min: 700 },
  [NODE_TYPES.component]: { initial: 320, min: 200 },
};

/** The dock's modulation column (docs/design/macros-and-modulation-column.md, MC12): four macro knobs to a row, and the
    LFO's and ADSR's panels laid out for this width rather than the strip's (their minimums above are the strip's). */
export const MOD_COLUMN_WIDTH = 248;

/** A plugin's card, and any other that only names a device and opens it. */
const NAMED: DeviceWidth = { initial: 150, min: 150 };

const KNOB_WIDTH = 74;
const KNOB_ROWS = 2;
const PADDING = 24;

/** A panel of plain knobs, as many columns as its two rows need. */
function knobsWidth(params: number): DeviceWidth {
  const columns = Math.max(1, Math.ceil(params / KNOB_ROWS));
  const width = PADDING + columns * KNOB_WIDTH;
  return { initial: width, min: width };
}

/** What a component's header needs: its name field and the Open and Save buttons. */
const COMPONENT_HEAD = 340;

/**
 * The key a card's width is remembered under. Built-in devices share one width per type; a component, a Faust script
 * and a plugin each have their own, since one of them says nothing about another's size (a three-knob Resonator
 * shouldn't open as wide as a big component widened earlier).
 */
export function widthKey(node: GraphNode): string {
  if (node.plugin) return `plugin:${node.plugin.identifier}`;
  if (node.type === NODE_TYPES.component) return `${node.type}:${node.component?.name ?? ''}`;
  if (node.type === NODE_TYPES.faust) return `${node.type}:${node.code?.match(/declare\s+name\s+"([^"]+)"/)?.[1] ?? ''}`;
  return node.type;
}

/** A device's widths. `params` is how many knobs it shows, for a panel that's only knobs (or a component's pinned
    controls). */
export function deviceWidth(type: string, params: number): DeviceWidth {
  if (type === NODE_TYPES.pluginInstrument || type === NODE_TYPES.pluginEffect || type === NODE_TYPES.voiceInstrument) return NAMED;
  if (type === NODE_TYPES.component) {
    const knobs = knobsWidth(params);
    return { initial: Math.max(COMPONENT_HEAD, knobs.initial), min: 200 };
  }
  return WIDTHS[type] ?? knobsWidth(params);
}

/** A remembered width, kept to what the device can use. */
export function clampDeviceWidth(width: number, device: DeviceWidth): number {
  return Math.max(device.min, Math.round(width));
}
