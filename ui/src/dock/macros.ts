import { NODE_TYPES, type GraphNode, type ModulationSlot, type NodeTypeInfo, type ParamDescriptor, type Track, type TrackGraph } from '../bridge/protocol';
import { depthBetween, modulatedValue } from '../graph/params';
import { heldGraph } from '../graph/voice';

// Macros and the links between modulation sources and the parameters they drive (docs/design/macros-and-modulation-column.md,
// MC5 to MC13): what the modulation column, the knobs' rings and Map mode work out from the model.

/** How many knobs a Macros device has (maxMacros in engine/include/dawg/Model.h). */
export const MACRO_COUNT = 8;

/** The neutral positions, by the `neutralN` choice's value (MC6). */
export const NEUTRALS = ['Bottom', 'Centre', 'Top'] as const;

/** The track's Macros device: in its own graph, never in a component or voice graph (MC5). */
export function macrosOf(track: Track): GraphNode | undefined {
  return Object.values(track.graph.nodes).find((n) => n.type === NODE_TYPES.macros);
}

/** A knob's name: its own, or "Macro N". */
export function macroName(node: GraphNode | undefined, knob: number): string {
  return node?.macroNames?.[knob] || `Macro ${knob + 1}`;
}

/** The parameter id of a knob's value or neutral position. */
export const macroParam = (knob: number) => `macro${knob + 1}`;
export const neutralParam = (knob: number) => `neutral${knob + 1}`;

/** A knob's neutral position: 0 Bottom, 1 Centre, 2 Top. */
export const neutralOf = (node: GraphNode | undefined, knob: number) => Math.round(node?.params[neutralParam(knob)] ?? 0);

/** What a knob sends at a value of 0..100 % for its neutral position (MC6): 0 at the neutral position, ±1 turned all the
    way from it. Mirrors macroSignal in engine/src/ModulationNodes.cpp. */
export function macroSignal(percent: number, neutral: number): number {
  const value = Math.min(1, Math.max(0, percent / 100));
  return neutral === 1 ? value * 2 - 1 : neutral === 2 ? value - 1 : value;
}

/** Every graph of a track, at any depth, with the node holding it (none for the track's own). */
export function graphsOf(track: Track): Array<{ graph: TrackGraph; owner?: GraphNode }> {
  const result: Array<{ graph: TrackGraph; owner?: GraphNode }> = [];
  const visit = (graph: TrackGraph, owner?: GraphNode) => {
    result.push({ graph, owner });
    for (const node of Object.values(graph.nodes)) {
      const inner = heldGraph(node);
      if (inner) visit(inner, node);
    }
  };
  visit(track.graph);
  return result;
}

/** One modulation slot on a track and what feeds it. */
export interface SlotLink {
  /** The graph holding the target. */
  graph: TrackGraph;
  node: GraphNode;
  port: number;
  slot: ModulationSlot;
  /** A macro slot's knob, or undefined for one fed by cables. */
  macro?: number;
  /** The nodes feeding the slot's port by cable, with their output port. */
  sources: Array<{ node: GraphNode; port: number }>;
}

/** Every assigned modulation slot on the track. */
export function slotLinks(track: Track): SlotLink[] {
  const links: SlotLink[] = [];
  for (const { graph } of graphsOf(track)) {
    const connections = Object.values(graph.connections);
    for (const node of Object.values(graph.nodes)) {
      for (const [key, slot] of Object.entries(node.modulations ?? {})) {
        if (!slot.param) continue;
        const port = Number(key);
        const macro = (slot.macro ?? -1) >= 0 ? slot.macro : undefined;
        const sources = connections
          .filter((c) => c.toNode === node.id && c.toPort === port)
          .flatMap((c) => (graph.nodes[String(c.fromNode)] ? [{ node: graph.nodes[String(c.fromNode)], port: c.fromPort }] : []));
        links.push({ graph, node, port, slot, macro, sources });
      }
    }
  }
  return links;
}

/** The slots a macro knob feeds (MC11), or a whole Macros device's with no knob. */
export function macroTargets(track: Track, knob?: number): SlotLink[] {
  const macros = macrosOf(track);
  return slotLinks(track).filter(
    (link) =>
      (link.macro !== undefined && (knob === undefined || link.macro === knob)) ||
      (macros !== undefined && link.sources.some((s) => s.node.id === macros.id && (knob === undefined || s.port === knob))),
  );
}

/** A source being pointed at in the column: a node, and for the Macros device perhaps one knob. */
export interface SourceRef {
  trackId: number;
  nodeId: number;
  knob?: number;
}

/** Whether a slot is fed by a source (MC13's highlight). */
export function drivenBy(link: SlotLink, source: SourceRef, macros: GraphNode | undefined): boolean {
  if (link.macro !== undefined)
    return macros !== undefined && source.nodeId === macros.id && (source.knob === undefined || source.knob === link.macro);
  return link.sources.some((s) => s.node.id === source.nodeId && (source.knob === undefined || s.port === source.knob));
}

/** The range a source's signal covers, for a ring's swing: a macro's by its neutral position, an LFO's by its zero
    position, an envelope's and the expression's unipolar outputs 0..1. */
export function signalRange(source: GraphNode | undefined, port: number): [number, number] {
  if (!source) return [-1, 1];
  if (source.type === NODE_TYPES.macros) {
    const neutral = neutralOf(source, port);
    return neutral === 1 ? [-1, 1] : neutral === 2 ? [-1, 0] : [0, 1];
  }
  if (source.type === NODE_TYPES.lfo) {
    const zero = Math.round(source.params.zero ?? 1);
    return zero === 0 ? [-1, 0] : zero === 2 ? [0, 1] : [-1, 1];
  }
  if (source.type === NODE_TYPES.adsr) return [0, 1];
  // Note Expression: Pitch is bipolar; pressure, timbre, velocity and gate run 0..1.
  if (source.type === NODE_TYPES.noteExpression) return port === 0 ? [-1, 1] : [0, 1];
  return [-1, 1];
}

/** How a parameter's modulation is shown on its knob (MC13): the positions (0..1 on the knob) its sources can swing it
    between, and where a macro puts it now. Null when nothing modulates it. */
export interface Swing {
  low: number;
  high: number;
  /** Where the macros (not the cable sources, which move) put it now, when only macros drive it. */
  now?: number;
}

export function swingOf(
  track: Track,
  node: GraphNode,
  param: ParamDescriptor,
  base: number,
  toPosition: (value: number) => number,
): Swing | null {
  const macros = macrosOf(track);
  const links = slotLinks(track).filter((l) => l.node.id === node.id && l.slot.param === param.id && l.slot.active !== false);
  if (links.length === 0) return null;
  let low = 0;
  let high = 0;
  let now = 0;
  let onlyMacros = true;
  for (const link of links) {
    const ranges = link.macro !== undefined ? [signalRange(macros, link.macro)] : link.sources.map((s) => signalRange(s.node, s.port));
    if (link.macro === undefined) onlyMacros = false;
    for (const [a, b] of ranges) {
      const x = a * link.slot.depth;
      const y = b * link.slot.depth;
      low += Math.min(x, y);
      high += Math.max(x, y);
    }
    if (link.macro !== undefined && macros) now += macroSignal(macros.params[macroParam(link.macro)] ?? 0, neutralOf(macros, link.macro)) * link.slot.depth;
  }
  if (Math.abs(high - low) < 1e-9) return null;
  return {
    low: toPosition(modulatedValue(param, base, low)),
    high: toPosition(modulatedValue(param, base, high)),
    now: onlyMacros ? toPosition(modulatedValue(param, base, now)) : undefined,
  };
}

/** The depth Map mode gives a parameter moved from `from` to `to` (MC9): the move in its modulation units, signed so
    that turning the macro away from its neutral position moves the parameter the way it was moved. */
export function mappedDepth(param: ParamDescriptor, from: number, to: number, neutral: number): number {
  const depth = depthBetween(param, from, to);
  return neutral === 2 ? -depth : depth;
}

/** A node's first modulation input with no cable and no slot, for a source dragged onto one of its knobs (MC13). */
export function freeModulationPort(graph: TrackGraph, node: GraphNode, info: NodeTypeInfo | undefined): number | undefined {
  const used = new Set(Object.keys(node.modulations ?? {}).map(Number));
  for (const c of Object.values(graph.connections)) if (c.toNode === node.id) used.add(c.toPort);
  const index = (info?.inputs ?? []).findIndex((port, i) => port.type === 'modulation' && !used.has(i));
  return index >= 0 ? index : undefined;
}
