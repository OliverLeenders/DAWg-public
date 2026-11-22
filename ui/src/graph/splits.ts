import { NODE_TYPES, type GraphNode, type NodeTypeInfo, type PortDescriptor } from '../bridge/protocol';

// Split nodes in the UI (docs/design/split-nodes.md, section 6): their ports and parameters named for the band count or
// the mode, and the crossovers kept in order while they are dragged.

/** The bands' names for each count (SP1). */
export const BAND_NAMES: Record<number, string[]> = {
  2: ['Low', 'High'],
  3: ['Low', 'Mid', 'High'],
  4: ['Low', 'Low mid', 'High mid', 'High'],
};

/** The Band Split's Dry port: after the four band ports, whatever the count. */
export const DRY_PORT = 4;

/** The lowest and highest crossover, and how close two may come: a third of an octave (SP2). */
export const CROSSOVER_MIN = 20;
export const CROSSOVER_MAX = 20000;
export const CROSSOVER_GAP = Math.pow(2, 1 / 3);

export const isSplit = (type: string) => type === NODE_TYPES.bandSplit || type === NODE_TYPES.stereoSplit || type === NODE_TYPES.stereoMerge;

export const bandCount = (node: GraphNode): number => node.bands ?? 3;

export const isLeftRight = (node: GraphNode): boolean => node.mode === 'leftRight';

/** The two parts' names in a mode. */
export const partNames = (node: GraphNode): [string, string] => (isLeftRight(node) ? ['Left', 'Right'] : ['Mid', 'Side']);

/** A split node's description as it shows: band ports named for the count and the others hidden, the stereo nodes'
    ports and gains named for the mode, each port with its colour (`tone`). */
export function splitInfo(node: GraphNode, info: NodeTypeInfo): NodeTypeInfo {
  if (node.type === NODE_TYPES.bandSplit) {
    const count = bandCount(node);
    const names = BAND_NAMES[count] ?? BAND_NAMES[3];
    const outputs = info.outputs.map((port, i): PortDescriptor =>
      i === DRY_PORT ? { ...port, tone: 'dry' } : i < count ? { ...port, name: names[i], tone: `band-${i}` } : { ...port, hidden: true },
    );
    // Crossovers the count doesn't use, and Solo's bands past it, leave the inspector.
    const params = info.params
      .filter((param) => !/^crossover(\d)$/.test(param.id) || Number(param.id.slice(-1)) < count)
      .map((param) => (param.id === 'solo' ? { ...param, choices: ['Off', ...names], max: count } : param));
    return { ...info, outputs, params };
  }

  const names = partNames(node);
  const tones = isLeftRight(node) ? ['left', 'right'] : ['mid', 'side'];
  const name = (ports: PortDescriptor[]) =>
    ports.map((port, i): PortDescriptor => (port.type === 'audio' && i < 2 ? { ...port, name: names[i], tone: tones[i] } : port));

  if (node.type === NODE_TYPES.stereoSplit) return { ...info, outputs: name(info.outputs) };
  return {
    ...info,
    inputs: name(info.inputs),
    params: info.params.map((param, i) => (i < 2 ? { ...param, name: names[i] } : param)),
  };
}

/** The crossovers after moving one: within range, and the others pushed along to stay a third of an octave apart. */
export function orderCrossovers(values: number[], index: number, value: number): number[] {
  const count = values.length;
  const result = [...values];
  const low = CROSSOVER_MIN * Math.pow(CROSSOVER_GAP, index);
  const high = CROSSOVER_MAX / Math.pow(CROSSOVER_GAP, count - 1 - index);
  result[index] = Math.min(high, Math.max(low, value));

  for (let i = index + 1; i < count; i++) result[i] = Math.max(result[i], result[i - 1] * CROSSOVER_GAP);
  for (let i = index - 1; i >= 0; i--) result[i] = Math.min(result[i], result[i + 1] / CROSSOVER_GAP);
  return result;
}

/** A frequency's place across the view, 0 to 1 on a log scale from 20 Hz to 20 kHz, and back. */
export const frequencyToPosition = (hz: number) => Math.log(hz / CROSSOVER_MIN) / Math.log(CROSSOVER_MAX / CROSSOVER_MIN);
export const positionToFrequency = (position: number) => CROSSOVER_MIN * Math.pow(CROSSOVER_MAX / CROSSOVER_MIN, Math.min(1, Math.max(0, position)));
