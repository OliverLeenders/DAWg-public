import { NODE_TYPES, type Connection, type GraphNode, type NodeTypeInfo, type Project, type TrackGraph } from '../bridge/protocol';

// Connection rules for the graph view, mirroring validateConnection() in engine/src/Commands.cpp
// (docs/design/phase8-node-graph.md, G7). The engine still has the final say.

/** True if `to` can already reach `from`, so connecting from -> to would close a loop. A loop through a node in `through` doesn't
    count: Feedback nodes are what a loop may close through (docs/design/feedback-routing.md, R1). */
export function wouldCycle(connections: Connection[], from: number, to: number, through: ReadonlySet<number> = new Set()): boolean {
  if (through.has(from) || through.has(to)) return false;
  if (from === to) return true;
  const stack = [to];
  const seen = new Set<number>();
  while (stack.length > 0) {
    const node = stack.pop()!;
    if (node === from) return true;
    if (seen.has(node) || through.has(node)) continue;
    seen.add(node);
    for (const c of connections) if (c.fromNode === node) stack.push(c.toNode);
  }
  return false;
}

/** The Feedback nodes of a graph. */
export function feedbackNodes(graph: TrackGraph): Set<number> {
  return new Set(Object.values(graph.nodes).filter((n) => n.type === NODE_TYPES.feedback).map((n) => n.id));
}

/** True if a connection would close a loop with no Feedback node in it, so that it needs one. */
export function closesLoop(graph: TrackGraph, c: Connection): boolean {
  return wouldCycle(Object.values(graph.connections), c.fromNode, c.toNode, feedbackNodes(graph));
}

/** True for a cable leaving a Feedback node that comes round to it again: the way back of a loop. */
export function isLoopBack(graph: TrackGraph, c: Connection): boolean {
  return graph.nodes[String(c.fromNode)]?.type === NODE_TYPES.feedback && wouldCycle(Object.values(graph.connections), c.fromNode, c.toNode);
}

/** A loop's Feedback nodes that are drawn small, on the cable going back: one way in and one way out, with that way out the way back,
    and nothing modulating them. By node, with the two cables through each. */
export function feedbackChips(graph: TrackGraph): Map<number, { into: Connection; out: Connection }> {
  const connections = Object.values(graph.connections);
  const feedback = feedbackNodes(graph);
  const chips = new Map<number, { into: Connection; out: Connection }>();

  for (const id of feedback) {
    const ins = connections.filter((c) => c.toNode === id);
    const outs = connections.filter((c) => c.fromNode === id);
    if (ins.length !== 1 || outs.length !== 1 || ins[0].toPort !== 0) continue;
    if (feedback.has(ins[0].fromNode) || feedback.has(outs[0].toNode) || !isLoopBack(graph, outs[0])) continue;
    chips.set(id, { into: ins[0], out: outs[0] });
  }

  return chips;
}

/** Where a new Feedback node goes for a cable closing a loop: under the two ends, between them. */
export function loopNodePosition(graph: TrackGraph, c: Connection): { x: number; y: number } {
  const from = graph.nodes[String(c.fromNode)];
  const to = graph.nodes[String(c.toNode)];
  return { x: Math.round(((from?.x ?? 0) + (to?.x ?? 0)) / 2), y: Math.round(Math.max(from?.y ?? 0, to?.y ?? 0) + 140) };
}

/** True if any Feedback node in a project, voice graphs included, may ring for ever. */
export function growingLoop(project: Project | null): boolean {
  const grows = (graph: TrackGraph): boolean =>
    Object.values(graph.nodes).some(
      (n) =>
        (n.type === NODE_TYPES.feedback && (n.params.above ?? 0) >= 0.5) ||
        (n.voice !== undefined && grows(n.voice)) ||
        (n.component !== undefined && grows(n.component.graph)),
    );
  if (!project) return false;
  return [...Object.values(project.tracks), ...(project.master ? [project.master] : [])].some((t) => grows(t.graph));
}

/** Why a connection is not allowed in a track's graph or a voice graph, or null if it is. */
export function connectionProblem(graph: TrackGraph, infoFor: (node: GraphNode) => NodeTypeInfo | undefined, c: Connection): string | null {
  const from = graph.nodes[String(c.fromNode)];
  const to = graph.nodes[String(c.toNode)];
  if (!from || !to) return 'No such node';

  const out = infoFor(from)?.outputs[c.fromPort];
  const input = infoFor(to)?.inputs[c.toPort];
  if (!out || !input) return 'No such port';
  if (out.type === 'any' && input.type === 'any') return 'Connect a spare port to something that says what it carries';
  if (out.type !== input.type && out.type !== 'any' && input.type !== 'any') return 'Ports carry different kinds of data';

  const connections = Object.values(graph.connections);
  if (connections.some((e) => e.fromNode === c.fromNode && e.fromPort === c.fromPort && e.toNode === c.toNode && e.toPort === c.toPort))
    return 'These ports are already connected';
  if (input.type === 'events' && connections.some((e) => e.toNode === c.toNode && e.toPort === c.toPort))
    return 'An event input accepts only one connection';
  // An audio cable closing a loop gets a Feedback node put in it (R1).
  if (input.type !== 'audio' && closesLoop(graph, c)) return 'This connection would create a feedback loop';
  return null;
}
