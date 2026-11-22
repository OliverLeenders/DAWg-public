import {
  NODE_TYPES,
  type ComponentGraph,
  type ExposedParam,
  type GraphNode,
  type NodeTypeInfo,
  type ParamDescriptor,
  type PluginNodeStatus,
  type PortDescriptor,
  type Track,
} from '../bridge/protocol';
import { drumOutputsShown } from '../dock/drumSynth';
import { useDawg } from '../store/store';
import { isSplit, splitInfo } from './splits';
import { findNode } from './voice';

/** The spare port on a component's Input and Output: a cable to it makes a new port (docs/design/component-nodes.md, N2). */
export const SPARE_PORT: PortDescriptor = { type: 'any', name: 'Add port' };

/** The parameters shown on a component, each with the inner node it belongs to. Those of a Faust script yet to compile wait. */
export function exposedControls(
  track: Track,
  component: ComponentGraph,
  types: Record<string, NodeTypeInfo>,
  statuses: Record<string, PluginNodeStatus>,
): Array<{ exposed: ExposedParam; node: GraphNode; param: ParamDescriptor }> {
  return component.exposed.flatMap((exposed) => {
    const node = findNode(track, exposed.nodeId);
    const param = node && infoOf(track.id, node, types, statuses)?.params.find((p) => p.id === exposed.param);
    return node && param ? [{ exposed, node, param }] : [];
  });
}

/** A node's description: a Faust node's ports and parameters are its compiled script's
    (docs/design/phase11-faust-panels.md, F3), a component's ports are its own, and its Input and Output have those of the component
    they're in, `inside`; other nodes have their type's. */
export function infoOf(
  trackId: number,
  node: GraphNode,
  types: Record<string, NodeTypeInfo>,
  statuses: Record<string, PluginNodeStatus> = useDawg.getState().nodes,
  inside?: ComponentGraph,
): NodeTypeInfo | undefined {
  if (node.type === NODE_TYPES.faust) return statuses[`${trackId}:${node.id}`]?.program ?? types[node.type];
  const info = types[node.type];
  if (!info) return info;
  if (node.component) return { ...info, inputs: node.component.inputs, outputs: node.component.outputs };
  if (isSplit(node.type)) return splitInfo(node, info);
  // A Sampler shows the outputs its zones use or that are wired, not all 16 (docs/design/multi-output-instruments.md, MO1).
  if (node.type === NODE_TYPES.sampler) return { ...info, outputs: info.outputs.slice(0, samplerOutputsShown(trackId, node)) };
  // A Drum Synth's likewise, from its pads (docs/design/drum-synth.md, DR9).
  if (node.type === NODE_TYPES.drumSynth) return { ...info, outputs: info.outputs.slice(0, drumOutputsShown(node, wiredPorts(trackId, node))) };
  if (node.type === NODE_TYPES.pluginInstrument) return { ...info, outputs: pluginOutputsShown(trackId, node, info.outputs, statuses) };
  if (node.type === NODE_TYPES.componentInput) return { ...info, outputs: [...(inside?.inputs ?? []), SPARE_PORT] };
  if (node.type === NODE_TYPES.componentOutput) return { ...info, inputs: [...(inside?.outputs ?? []), SPARE_PORT] };
  return info;
}

/** An instrument plugin's outputs: Main, and the plugin's other buses while they're on, named as the plugin names them,
    up to the highest one a cable leaves (docs/design/multi-output-instruments.md, MO1). */
function pluginOutputsShown(
  trackId: number,
  node: GraphNode,
  ports: PortDescriptor[],
  statuses: Record<string, PluginNodeStatus>,
): PortDescriptor[] {
  const buses = statuses[`${trackId}:${node.id}`]?.outputs ?? [];
  const connections = useDawg.getState().project?.tracks[String(trackId)]?.graph.connections ?? {};
  const wired = Object.values(connections)
    .filter((c) => c.fromNode === node.id)
    .map((c) => c.fromPort + 1);
  const on = buses.reduce((last, bus, i) => (bus.on ? i + 1 : last), 1);
  return ports.slice(0, Math.max(1, on, ...wired)).map((port, i) => (i > 0 && buses[i]?.name ? { ...port, name: buses[i].name } : port));
}

/** How many of a Sampler's outputs show: Main, and up to the highest one a zone plays from or a cable leaves. */
/** The output ports of a node that a cable leaves. */
function wiredPorts(trackId: number, node: GraphNode): number[] {
  const connections = useDawg.getState().project?.tracks[String(trackId)]?.graph.connections ?? {};
  return Object.values(connections)
    .filter((c) => c.fromNode === node.id)
    .map((c) => c.fromPort);
}

export function samplerOutputsShown(trackId: number, node: GraphNode): number {
  const connections = useDawg.getState().project?.tracks[String(trackId)]?.graph.connections ?? {};
  const wired = Object.values(connections)
    .filter((c) => c.fromNode === node.id)
    .map((c) => c.fromPort + 1);
  return Math.max(1, ...(node.zones ?? []).map((z) => z.output ?? 1), ...wired);
}
