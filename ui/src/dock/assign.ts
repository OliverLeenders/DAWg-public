import { NODE_TYPES, type GraphNode, type Track } from '../bridge/protocol';
import { infoOf } from '../graph/nodeInfo';
import { findNode, graphOf, ownerOf } from '../graph/voice';
import { dispatch, reportError, useDawg } from '../store/store';
import { freeModulationPort, macrosOf, mappedDepth, neutralOf } from './macros';
import type { DraggedSource } from './modulationStore';

// Assigning a modulation source to a parameter from the column (docs/design/macros-and-modulation-column.md, MC9, MC13).

/** The depth a right-click mapping or a dropped source starts at. */
export const DEFAULT_DEPTH = 0.5;

/** Maps a parameter of any node on the track to a macro knob (mapMacro adds the Macros device if there is none). */
export function mapToMacro(trackId: number, nodeId: number, param: string, knob: number, depth = DEFAULT_DEPTH) {
  dispatch({ command: 'mapMacro', args: { trackId, nodeId, param, macro: knob, depth } });
}

/** A source dropped on a parameter: a macro knob maps it; an LFO, ADSR or Expression output is cabled to a free modulation
    input of the node, when both are in one graph, and targets the parameter. */
export function assignSource(track: Track, source: DraggedSource, node: GraphNode, param: string) {
  const macros = macrosOf(track);
  if (macros && source.nodeId === macros.id) {
    mapToMacro(track.id, node.id, param, source.knob ?? source.port);
    return;
  }

  const owner = ownerOf(track, node.id);
  const graph = graphOf(track, owner?.id);
  if (!graph || !graph.nodes[String(source.nodeId)]) {
    reportError(new Error('A source can only be connected to a device in its own graph: add one at that level in the graph view'));
    return;
  }

  const info = useDawg.getState().nodeTypes[node.type];
  const port = freeModulationPort(graph, node, info);
  if (port === undefined) {
    reportError(new Error('Every modulation input of this device is in use'));
    return;
  }

  const scope = owner ? { voiceNodeId: owner.id } : {};
  dispatch({
    command: 'batch',
    args: {
      label: 'Modulate',
      commands: [
        { command: 'connect', args: { trackId: track.id, ...scope, fromNode: source.nodeId, fromPort: source.port, toNode: node.id, toPort: port } },
        { command: 'setModulation', args: { trackId: track.id, ...scope, nodeId: node.id, port, param, depth: DEFAULT_DEPTH } },
      ],
    },
  });
}

/** Whether a parameter of a node can be a macro's target: a modulatable one, not on the Macros device itself. */
export function mappable(node: GraphNode, modulatable: boolean) {
  return modulatable && node.type !== NODE_TYPES.macros;
}

/** The smallest depth Map mode maps: a parameter that only wobbled along with another isn't meant. */
const MAP_THRESHOLD = 0.005;

/** Map mode (MC9) for a move of several parameters of a node at once (a panel's graph): each one moved is mapped to the
    knob at the depth it was moved, in one undo step. */
export function mapMoves(trackId: number, nodeId: number, moves: Record<string, [number | undefined, number | undefined]>, knob: number) {
  const s = useDawg.getState();
  const track = s.project?.master?.id === trackId ? s.project.master : s.project?.tracks[String(trackId)];
  const node = track ? findNode(track, nodeId) : undefined;
  const info = track && node ? infoOf(track.id, node, s.nodeTypes, s.nodes) : undefined;
  if (!track || !node || !info) return;
  const neutral = neutralOf(macrosOf(track), knob);
  const commands = Object.entries(moves).flatMap(([id, [from, to]]) => {
    const param = info.params.find((p) => p.id === id);
    if (!param || !mappable(node, param.modulatable)) return [];
    // A parameter at its default has no value in the model.
    const start = from ?? param.default;
    const depth = mappedDepth(param, start, to ?? start, neutral);
    return Math.abs(depth) < MAP_THRESHOLD ? [] : [{ command: 'mapMacro' as const, args: { trackId, nodeId, param: id, macro: knob, depth } }];
  });
  if (commands.length === 1) dispatch(commands[0]);
  else if (commands.length > 1) dispatch({ command: 'batch', args: { label: 'Map to macro', commands } });
}
