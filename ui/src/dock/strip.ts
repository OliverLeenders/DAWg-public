import { NODE_TYPES, type Connection, type GraphNode, type NodeTypeInfo, type Track } from '../bridge/protocol';
import { isModulationSource, trackChain } from '../components/chain';
import { cableId } from '../graph/layout';
import { isLoopBack } from '../graph/validate';

// What the device strip shows of a track (docs/design/device-strip.md, D1 to D3): its devices in signal order, the
// cables between them for the route line, and its modulation sources after them, as they aren't in the signal path.

export interface StripDevices {
  devices: GraphNode[];
  modulation: GraphNode[];
  /** Whether the track's graph is a simple chain, where the strip may add and reorder devices (D10). */
  simple: boolean;
}

/** A cable between two devices in the strip, for the route line. */
export interface StripLink {
  key: string;
  from: number;
  to: number;
  /** A loop's way back, from a Feedback node to a device before it. */
  back: boolean;
  /** From a card to the one right after it: drawn as a small arrow between them, not in the route line above (D2). */
  adjacent: boolean;
}

/** Whether a cable goes into a modulation input: modulation isn't part of the signal's route. */
function isModulation(track: Track, c: Connection, types: Record<string, NodeTypeInfo>) {
  const target = track.graph.nodes[String(c.toNode)];
  return target ? types[target.type]?.inputs[c.toPort]?.type === 'modulation' : false;
}

/** The audio nodes in signal order, depth first: a branch is followed to its end before the next one starts, so parallel
    branches follow one another in the row (D1). A loop's way back is left out, so a loop can't break the order. */
export function stripOrder(track: Track, types: Record<string, NodeTypeInfo>): GraphNode[] {
  const graph = track.graph;
  const nodes = Object.values(graph.nodes)
    .filter((n) => !isModulationSource(n))
    .sort((a, b) => a.id - b.id);
  const ids = new Set(nodes.map((n) => n.id));
  const forward = Object.values(graph.connections).filter(
    (c) => ids.has(c.fromNode) && ids.has(c.toNode) && !isModulation(track, c, types) && !isLoopBack(graph, c),
  );
  const waiting = new Map(nodes.map((n) => [n.id, 0]));
  for (const c of forward) waiting.set(c.toNode, waiting.get(c.toNode)! + 1);
  const placed = new Map<number, number>();
  const order: GraphNode[] = [];

  // The node to place next: of those whose inputs are all placed, the one fed by the most recently placed node. Between
  // equals a device comes before the fixed nodes that end a track, so a Feedback node follows what feeds it, not the Gain.
  const latestFeed = (node: GraphNode) => Math.max(-1, ...forward.filter((c) => c.toNode === node.id).map((c) => placed.get(c.fromNode)!));
  const fixed = (node: GraphNode) => (node.type === NODE_TYPES.gain || types[node.type]?.userEditable === false ? 1 : 0);
  const before = (a: GraphNode, b: GraphNode) => latestFeed(a) - latestFeed(b) || fixed(b) - fixed(a);

  while (order.length < nodes.length) {
    const ready = nodes.filter((n) => !placed.has(n.id) && waiting.get(n.id) === 0);
    // A malformed graph still shows every node rather than lose some.
    const candidates = ready.length > 0 ? ready : nodes.filter((n) => !placed.has(n.id)).slice(0, 1);
    const next = candidates.reduce((best, n) => (before(n, best) > 0 ? n : best));
    placed.set(next.id, order.length);
    order.push(next);
    for (const c of forward) if (c.fromNode === next.id) waiting.set(c.toNode, waiting.get(c.toNode)! - 1);
  }

  return order;
}

/** Whether something modulates a node: a connection into one of its modulation inputs. */
function isModulated(track: Track, node: GraphNode, types: Record<string, NodeTypeInfo>) {
  return Object.values(track.graph.connections).some((c) => c.toNode === node.id && isModulation(track, c, types));
}

export function stripDevices(track: Track, types: Record<string, NodeTypeInfo>): StripDevices {
  const editable = (node: GraphNode) => types[node.type]?.userEditable !== false;
  const modulation = Object.values(track.graph.nodes)
    .filter((n) => isModulationSource(n) && editable(n))
    .sort((a, b) => a.id - b.id);
  const chain = trackChain(track);

  if (chain) {
    // The fader beside the strip already sets the level, so the Gain node is only worth a card while it's modulated.
    const gain = chain.gain;
    const devices = [...chain.midiEffects, ...(chain.instrument ? [chain.instrument] : []), ...chain.effects];
    if (gain && isModulated(track, gain, types)) devices.push(gain);
    return { devices, modulation, simple: true };
  }

  // On a custom graph a Gain node is a device like any other.
  const devices = stripOrder(track, types).filter((n) => editable(n) || n.type === NODE_TYPES.gain);
  return { devices, modulation, simple: false };
}

/** The cables between the strip's devices, modulation left out. */
export function stripLinks(track: Track, devices: GraphNode[], types: Record<string, NodeTypeInfo>): StripLink[] {
  const index = new Map(devices.map((n, i) => [n.id, i]));
  return Object.values(track.graph.connections)
    .filter((c) => index.has(c.fromNode) && index.has(c.toNode) && !isModulation(track, c, types))
    .map((c) => {
      const back = isLoopBack(track.graph, c);
      return { key: cableId(c), from: c.fromNode, to: c.toNode, back, adjacent: !back && index.get(c.toNode) === index.get(c.fromNode)! + 1 };
    });
}
