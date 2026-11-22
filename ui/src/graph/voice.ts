import { NODE_TYPES, type GraphNode, type Track, type TrackGraph } from '../bridge/protocol';

// The scopes of the graph view (docs/design/phase12-voice-scope.md; component-nodes.md, N7): a Voice Instrument's voice graph or what's
// in a component, nested in its node, at any depth. Node ids are unique in the project, so a node is found by id.

/** Node types that can't run per voice (V6), mirroring nodetype::allowedInVoiceGraph in engine/include/dawg/NodeRegistry.h. */
const TRACK_ONLY = new Set<string>([
  NODE_TYPES.pluginInstrument,
  NODE_TYPES.pluginEffect,
  NODE_TYPES.sequencer,
  NODE_TYPES.audioPlayer,
  NODE_TYPES.mixInput,
  NODE_TYPES.trackOutput,
  NODE_TYPES.mpeSynth,
  NODE_TYPES.sampler,
  NODE_TYPES.drumSynth,
  NODE_TYPES.voiceInstrument,
]);

/** Whether a node of this type may be added to a voice graph. */
export const allowedInVoiceGraph = (type: string) => !TRACK_ONLY.has(type);

/** The graph a node holds: a Voice Instrument's voice graph, or what's in a component. */
export const heldGraph = (node: GraphNode | undefined): TrackGraph | undefined => node?.voice ?? node?.component?.graph;

/** The nodes holding a node, outermost first, or undefined when it's nowhere. Empty when it's in the track's graph. */
export function ownerChain(track: Track, nodeId: number): GraphNode[] | undefined {
  const search = (graph: TrackGraph): GraphNode[] | undefined => {
    if (graph.nodes[String(nodeId)]) return [];
    for (const owner of Object.values(graph.nodes)) {
      const inner = heldGraph(owner);
      const found = inner && search(inner);
      if (found) return [owner, ...found];
    }
    return undefined;
  };
  return search(track.graph);
}

/** The Voice Instrument or component whose graph directly holds a node, or undefined when it's in the track's graph or nowhere. */
export function ownerOf(track: Track, nodeId: number): GraphNode | undefined {
  return ownerChain(track, nodeId)?.at(-1);
}

/** A node at any depth. */
export function findNode(track: Track, nodeId: number): GraphNode | undefined {
  const search = (graph: TrackGraph): GraphNode | undefined => {
    if (graph.nodes[String(nodeId)]) return graph.nodes[String(nodeId)];
    for (const owner of Object.values(graph.nodes)) {
      const inner = heldGraph(owner);
      const found = inner && search(inner);
      if (found) return found;
    }
    return undefined;
  };
  return search(track.graph);
}

/** The graph a scope shows: what a node holds, or the track's graph. Undefined when the node is gone. */
export function graphOf(track: Track, ownerId: number | undefined): TrackGraph | undefined {
  return ownerId === undefined ? track.graph : heldGraph(findNode(track, ownerId));
}

/** Whether what a node holds runs per voice: it's a Voice Instrument, or inside one. */
export function runsPerVoice(track: Track, ownerId: number | undefined): boolean {
  if (ownerId === undefined) return false;
  const chain = [...(ownerChain(track, ownerId) ?? []), findNode(track, ownerId)];
  return chain.some((node) => node?.type === NODE_TYPES.voiceInstrument);
}

/** The component a node's parameters can be shown on: the one directly holding it (docs/design/component-nodes.md, N6). */
export function componentHolding(track: Track, nodeId: number): GraphNode | undefined {
  const owner = ownerOf(track, nodeId);
  return owner?.component ? owner : undefined;
}
