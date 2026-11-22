import { NODE_TYPES, type GraphNode, type NodeTypeInfo, type PluginNodeStatus, type Track } from '../bridge/protocol';
import { isSplit } from '../graph/splits';

// Chain view of a track graph, mirroring findChain() in engine/src/Commands.cpp
// (docs/design/phase4-plugin-hosting.md, P2): Sequencer -> [MIDI effects...] -> [instrument] -> [effects...] -> [Gain] ->
// Output. The Gain is optional (docs/design/phase8-node-graph.md, G4); MIDI effects are docs/design/midi-effects.md, ME2.

export interface Chain {
  /** Between the sequencer and the instrument, in order. */
  midiEffects: GraphNode[];
  instrument: GraphNode | null;
  effects: GraphNode[];
  /** The Gain feeding the output, the chain's own (not one of its effects), if it has one. */
  gain: GraphNode | null;
}

/** The MIDI effects' node types. */
export const MIDI_EFFECT_TYPES: string[] = [NODE_TYPES.stepSequencer, NODE_TYPES.arpeggiator, NODE_TYPES.chord, NODE_TYPES.scaleLock, NODE_TYPES.noteRepeat];

/** MIDI effects take notes and give notes (docs/design/midi-effects.md, ME1). */
export const isMidiEffect = (node: GraphNode) => MIDI_EFFECT_TYPES.includes(node.type);

/** A simple chain's MIDI effects that aren't bypassed: what plays the track's clips differently from how they look. */
export const activeMidiEffects = (track: Track) =>
  (trackChain(track)?.midiEffects ?? []).filter((n) => !n.bypassed && n.type !== NODE_TYPES.stepSequencer);

/** Returns the track's instrument and effects in order, or null if its graph is not a simple chain. */
export function trackChain(track: Track): Chain | null {
  const nodes = Object.values(track.graph.nodes);
  const connections = Object.values(track.graph.connections);
  const byId = new Map(nodes.map((n) => [n.id, n]));
  // The node feeding `id`'s main input, if that is its only connection there. Other inputs (a key, the modulation
  // pool) don't affect the chain.
  const inputOf = (id: number) => {
    const incoming = connections.filter((c) => c.toNode === id && c.toPort === 0);
    return incoming.length === 1 && incoming[0].fromPort === 0 ? byId.get(incoming[0].fromNode) ?? null : null;
  };

  const output = nodes.find((n) => n.type === NODE_TYPES.trackOutput);
  if (!output) return null;
  // Nothing feeding the output is an empty chain (once the gain is gone, removing the instrument leaves that).
  const last = inputOf(output.id);
  if (!last && connections.some((c) => c.toNode === output.id && c.toPort === 0)) return null;
  const gain = last?.type === NODE_TYPES.gain ? last : null;

  const chain: Chain = { midiEffects: [], instrument: null, effects: [], gain };
  const seen = new Set<number>();
  const sequencer = nodes.find((n) => n.type === NODE_TYPES.sequencer);

  for (let node = inputOf((gain ?? output).id); node; node = inputOf(node.id)) {
    if (node.type === NODE_TYPES.sequencer || seen.has(node.id)) return null;
    // An audio track's chain starts at its audio player, the master track's at its mix input; neither is a
    // slot of its own.
    if (node.type === NODE_TYPES.audioPlayer || node.type === NODE_TYPES.mixInput) break;
    seen.add(node.id);

    if (isInstrument(node)) {
      chain.instrument = node;
      // Back from its note input through the MIDI effects to the sequencer.
      for (let from = inputOf(node.id); from && from.id !== sequencer?.id; from = inputOf(from.id)) {
        if (!isMidiEffect(from) || seen.has(from.id)) return null;
        seen.add(from.id);
        chain.midiEffects.unshift(from);
      }
      if (chain.midiEffects.length > 0 && inputOf(chain.midiEffects[0].id)?.id !== sequencer?.id) return null;
      break;
    }

    chain.effects.unshift(node);
  }

  // Without an instrument, MIDI effects hang off the sequencer in a row.
  if (!chain.instrument && sequencer) {
    for (let from = sequencer; ; ) {
      const next = connections.filter((c) => c.fromNode === from.id && c.fromPort === 0 && c.toPort === 0).map((c) => byId.get(c.toNode));
      const midi = next.filter((n): n is GraphNode => !!n && isMidiEffect(n));
      if (midi.length !== 1 || seen.has(midi[0].id)) break;
      seen.add(midi[0].id);
      chain.midiEffects.push(midi[0]);
      from = midi[0];
    }
  }

  // Like the engine's findChain(): every audio node belongs to the chain (modulation sources are extras outside
  // it, docs/design/modulation.md, M7), and the sequencer feeds only the instrument or a source's note input.
  const fixedTypes: string[] = [NODE_TYPES.sequencer, NODE_TYPES.trackOutput, NODE_TYPES.audioPlayer, NODE_TYPES.mixInput];
  const fixed = nodes.filter((n) => fixedTypes.includes(n.type)).length + (gain ? 1 : 0);
  // Output to track nodes fed straight from the instrument are extras too (docs/design/multi-output-instruments.md, MO3).
  const outputExtra = (n: GraphNode) =>
    n.type === NODE_TYPES.busOutput && !!chain.instrument && connections.every((c) => c.toNode !== n.id || c.fromNode === chain.instrument?.id);
  const chainNodes = nodes.filter((n) => !isModulationSource(n) && !outputExtra(n)).length;
  if (chainNodes !== fixed + chain.midiEffects.length + chain.effects.length + (chain.instrument ? 1 : 0)) return null;

  const afterSequencer = chain.midiEffects[0] ?? chain.instrument;
  const feedsOther = (c: { fromNode: number; toNode: number }) => {
    const target = byId.get(c.toNode);
    return c.fromNode === sequencer?.id && c.toNode !== afterSequencer?.id && !(target && isModulationSource(target));
  };
  if (sequencer && connections.some(feedsOther)) return null;

  return chain;
}

export function isInstrument(node: GraphNode) {
  return (
    node.type === NODE_TYPES.mpeSynth ||
    node.type === NODE_TYPES.sampler ||
    node.type === NODE_TYPES.drumSynth ||
    node.type === NODE_TYPES.pluginInstrument ||
    node.type === NODE_TYPES.voiceInstrument
  );
}

/** Node types that are modulation sources (LFO, ADSR, Note Expression, Macros), not inserts. */
export function isModulationSourceType(type: string) {
  return type === NODE_TYPES.lfo || type === NODE_TYPES.adsr || type === NODE_TYPES.noteExpression || type === NODE_TYPES.macros;
}

/** Where a built-in node type goes in a track's chain (the engine's insertNode), or null when it only goes in a graph:
    modulation sources sit outside the chain, a split has several outputs (split-nodes.md, SP9) and a Sample Voice only
    runs in a voice graph. Effects take one audio input, besides a keyable node's key (sidechain.md, K1). The one place
    that decides it, for the add menus and for dragging from the browser alike. */
export function builtinChainRole(info: NodeTypeInfo | undefined): 'instrument' | 'midiEffect' | 'effect' | null {
  if (!info || !info.userEditable || isModulationSourceType(info.type) || isSplit(info.type)) return null;
  if (info.type === NODE_TYPES.pluginInstrument || info.type === NODE_TYPES.pluginEffect || info.type === NODE_TYPES.sampleVoice) return null;
  if (info.category === 'instrument') return 'instrument';
  if (info.category === 'midiEffect') return 'midiEffect';
  const audioInputs = info.inputs.filter((port) => port.type === 'audio').length;
  return audioInputs === 1 || audioInputs === 2 ? 'effect' : null;
}

/** Modulation sources are not inserts: the inspector lists them in their own section. */
export function isModulationSource(node: GraphNode) {
  return isModulationSourceType(node.type);
}

export interface InspectorGroups {
  instruments: GraphNode[];
  effects: GraphNode[];
  modulation: GraphNode[];
}

function isModulationConnection(
  connection: { fromNode: number; toNode: number; toPort: number },
  byId: Map<number, GraphNode>,
  infoFor: (node: GraphNode) => NodeTypeInfo | undefined,
) {
  const target = byId.get(connection.toNode);
  return target ? infoFor(target)?.inputs[connection.toPort]?.type === 'modulation' : false;
}

/**
 * The read-only node list for a custom graph. Instruments come first and effects follow in signal-flow
 * order, so an effect that feeds another is always listed above it. Modulation sources are kept out of
 * the insert list and returned separately (docs/design/phase8-node-graph.md, G6).
 */
export function inspectorGroups(
  track: Track,
  types: Record<string, NodeTypeInfo>,
  infoFor: (node: GraphNode) => NodeTypeInfo | undefined = (node) => types[node.type],
): InspectorGroups {
  const nodes = Object.values(track.graph.nodes);
  const connections = Object.values(track.graph.connections);
  const byId = new Map(nodes.map((n) => [n.id, n]));
  const position = new Map(nodes.map((n, i) => [n.id, i]));
  const visible = nodes.filter((n) => types[n.type]?.userEditable !== false);
  const visibleIds = new Set(visible.map((n) => n.id));

  // Modulation sources are not part of the audio graph; list them in their original order.
  const modulation = visible.filter(isModulationSource).sort((a, b) => position.get(a.id)! - position.get(b.id)!);
  const audioNodes = nodes.filter((n) => !isModulationSource(n));
  const audioIds = new Set(audioNodes.map((n) => n.id));
  const incoming = new Map(audioNodes.map((n) => [n.id, 0]));
  const outgoing = new Map(audioNodes.map((n) => [n.id, [] as number[]]));

  for (const connection of connections) {
    if (!audioIds.has(connection.fromNode) || !audioIds.has(connection.toNode)) continue;
    if (isModulationConnection(connection, byId, infoFor)) continue;
    outgoing.get(connection.fromNode)!.push(connection.toNode);
    incoming.set(connection.toNode, incoming.get(connection.toNode)! + 1);
  }

  const rank = (node: GraphNode) => {
    if (isInstrument(node)) return 0;
    if (types[node.type]?.userEditable === false) return 2;
    return 1;
  };
  const ready = audioNodes.filter((n) => incoming.get(n.id) === 0);
  const order: GraphNode[] = [];

  // Kahn's algorithm with a category priority: among nodes whose inputs are already listed, an
  // instrument comes before an effect, and hidden fixed nodes (sequencer, output) last.
  while (ready.length > 0) {
    ready.sort((a, b) => rank(a) - rank(b) || position.get(a.id)! - position.get(b.id)!);
    const node = ready.shift()!;
    order.push(node);

    for (const to of outgoing.get(node.id)!) {
      const left = incoming.get(to)! - 1;
      incoming.set(to, left);
      if (left === 0) {
        const target = byId.get(to);
        if (target) ready.push(target);
      }
    }
  }

  // A malformed or cyclic graph should still show every node rather than lose some.
  if (order.length < audioNodes.length) {
    const listed = new Set(order.map((n) => n.id));
    order.push(...audioNodes.filter((n) => !listed.has(n.id)));
  }

  const visibleOrder = order.filter((n) => visibleIds.has(n.id));
  return {
    instruments: visibleOrder.filter(isInstrument),
    effects: visibleOrder.filter((n) => !isInstrument(n)),
    modulation,
  };
}

/** The index to pass to reorderNode when the effect at `from` is dropped before `before` in the current list: the
    effect is taken out first, so the indices after it shift down by one. */
export function reorderIndex(from: number, before: number) {
  return from < before ? before - 1 : before;
}

/** Display name of a node: the plugin's name, or the type's name from the registry (nodes.types). */
export function nodeName(node: GraphNode, types?: Record<string, NodeTypeInfo>) {
  if (node.plugin) return node.plugin.name;
  if (node.component?.name) return node.component.name;
  // A Faust script's own name (docs/design/phase11-faust-panels.md, F6).
  const declared = node.code?.match(/declare\s+name\s+"([^"]+)"/)?.[1];
  if (declared) return declared;
  // The stereo nodes are named by their mode, as the node menu offers them (docs/design/split-nodes.md, SP12).
  if (node.type === NODE_TYPES.stereoSplit || node.type === NODE_TYPES.stereoMerge)
    return `${node.mode === 'leftRight' ? 'Left/right' : 'Mid/side'} ${node.type === NODE_TYPES.stereoSplit ? 'split' : 'merge'}`;
  if (node.type === NODE_TYPES.bandSplit) return 'Band split';
  if (types?.[node.type]) return types[node.type].name;
  if (node.type === NODE_TYPES.mpeSynth) return 'MPE Synth';
  return node.type.replace(/^dawg\./, '');
}

/** What clicking a device in the inspector does to a plugin's own editor: a ready plugin's editor opens, or closes
    if it is open; other nodes (and plugins still loading or failed) have none. */
export function pluginEditorAction(node: GraphNode, status: PluginNodeStatus | undefined): 'open' | 'close' | null {
  if (node.plugin === undefined || status?.status !== 'ready') return null;
  return status.editorOpen ? 'close' : 'open';
}
