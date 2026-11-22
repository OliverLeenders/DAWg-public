import { ArrowDown, ArrowRight, ChevronRight, TriangleAlert, Workflow } from 'lucide-react';
import { infoOf } from './nodeInfo';
import {
  Background,
  BackgroundVariant,
  Controls,
  MiniMap,
  ReactFlow,
  ReactFlowProvider,
  applyEdgeChanges,
  applyNodeChanges,
  useReactFlow,
  type Connection as FlowConnection,
  type Edge,
  type EdgeChange,
  type NodeChange,
} from '@xyflow/react';
import '@xyflow/react/dist/style.css';
import { Fragment, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { NODE_TYPES, type Command, type Connection, type GraphNode, type Track, type TrackGraph } from '../bridge/protocol';
import { PluginPicker, pickedComponent, type PickedNode } from '../components/PluginPicker';
import { actions, dispatch, newGestureId, reportError, useDawg } from '../store/store';
import { ui, useUi } from '../store/uiStore';
import { layout, useLayout } from '../store/layoutStore';
import { focusOf } from './focus';
import { GraphNodeView, inputHandle, outputHandle, portOf, type GraphFlowNode } from './GraphNodeView';
import { chipFacing, layoutGraph, type MeasuredNode, type Point, type Side } from './layout';
import { facing, translate } from './cables';
import { curveHits, routeAround, type Box as RouteBox, type CableEnd } from './routing';
import { RoutedEdge } from './RoutedEdge';
import { detachment, graphAfter, insertionCables, makeRoom, ShakeDetector, type Box, type Detachment } from './insertNode';
import { PATCHES, patchCommand } from './patches';
import { closesLoop, connectionProblem, feedbackChips, feedbackNodes, isLoopBack, loopNodePosition } from './validate';
import { findNode, graphOf, ownerChain, runsPerVoice } from './voice';
import { nodeName } from '../components/chain';
import { DeviceMenu } from '../components/DeviceMenu';
import { openInstrument } from '../components/instrumentWindow';
import { HelpButton } from '../components/HelpMenu';
import { draggedPaletteNode, NODE_DRAG_TYPE } from '../components/paletteDrag';
import { isMono } from './widths';
import { Button, Segmented, Toggle } from '../ui/controls';
import { dotOffset, firstPort, firstPortOf, GRID, MAJOR, snapLayout, snapNode } from './grid';
import { align, type AlignKind, type AlignNode } from './align';
import { Menu } from '../ui/overlays';

// The node graph view (docs/design/phase8-node-graph.md): the selected track's graph, or the master track's,
// on a React Flow canvas, in place of the arrangement's timeline beside the track headers. Every edit is a model command; the canvas only keeps node positions locally while
// they're dragged. Double-clicking a Voice Instrument shows its voice graph on the same canvas, with a breadcrumb back
// (docs/design/phase12-voice-scope.md, section 6); its commands carry the Voice Instrument as voiceNodeId.

const nodeTypes = { dawg: GraphNodeView };
const edgeTypes = { routed: RoutedEdge };

const PASTE_OFFSET = 40;
/** The size assumed for a node that isn't on the canvas yet, in flow units. */
const NEW_NODE_SIZE = { width: 180, height: 80 };

const edgeId = (c: Connection) => `${c.fromNode}:${c.fromPort}>${c.toNode}:${c.toPort}`;

/** Copied nodes (types, positions, plugins), the track they came from, and the connections between them, by index. */
let clipboard: {
  trackId: number;
  nodes: GraphNode[];
  connections: Array<{ from: number; fromPort: number; to: number; toPort: number }>;
} | null = null;

/** The track the graph view (and the inspector) works on: the selected track, the master track, or the first track. */
export function useGraphTrack(): Track | undefined {
  const project = useDawg((s) => s.project);
  const selectedTrackId = useUi((s) => s.selectedTrackId);
  const id = selectedTrackId ?? project?.trackOrder[0];
  if (id === undefined || !project) return undefined;
  return project.master?.id === id ? project.master : project.tracks[String(id)];
}

function Canvas({ track, graph, voiceNodeId }: { track: Track; graph: TrackGraph; voiceNodeId?: number }) {
  // Every graph command names the graph it edits.
  const scope = voiceNodeId === undefined ? {} : { voiceNodeId };
  // The Sampler and the MPE Synth at the track's level edit in the instrument window; inside a component or Voice
  // Instrument they have their card in the dock, as any other node there.
  const builtInInstrument = (node: GraphNode) =>
    voiceNodeId === undefined && (node.type === NODE_TYPES.sampler || node.type === NODE_TYPES.mpeSynth);
  // The component whose insides are shown, whose ports its Input and Output have.
  const inside = voiceNodeId === undefined ? undefined : findNode(track, voiceNodeId)?.component;
  const ownerParent = voiceNodeId === undefined ? undefined : ownerChain(track, voiceNodeId)?.at(-1)?.id;
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const widths = useDawg((s) => s.widths);
  const selectedNode = useUi((s) => s.selectedNode);
  const direction = useLayout((s) => s.graphDirection);
  const flow = useReactFlow();
  const [nodes, setNodes] = useState<GraphFlowNode[]>([]);
  const [menu, setMenu] = useState<{ x: number; y: number; flowX: number; flowY: number } | null>(null);
  const [nodeMenu, setNodeMenu] = useState<{ node: GraphNode; x: number; y: number } | null>(null);
  const [problem, setProblem] = useState<string | null>(null);
  const [alignMenu, setAlignMenu] = useState<DOMRect | null>(null);
  const wrapper = useRef<HTMLDivElement>(null);
  const [hovered, setHovered] = useState<number | null>(null);
  // The way around what it would cross of each cable without bends of its own, for where the nodes are now.
  const [routes, setRoutes] = useState<Map<string, Point[]>>(new Map());
  const [edgeMenu, setEdgeMenu] = useState<{ cable: Connection; x: number; y: number } | null>(null);

  // The model is the source of truth: rebuild the nodes when it changes, keeping the local selection.
  useEffect(() => {
    setNodes((current) => {
      const currentMap = new Map(current.map((n) => [n.id, n]));
      const selected = new Set(current.filter((n) => n.selected).map((n) => n.id));
      if (selectedNode?.trackId === track.id) selected.add(String(selectedNode.nodeId));
      const chips = feedbackChips(graph);
      const flowAt = (id: number | undefined) => {
        const n = id === undefined ? undefined : graph.nodes[String(id)];
        return n && (direction === 'vertical' ? { x: n.y, y: n.x } : { x: n.x, y: n.y });
      };
      return Object.values(graph.nodes).map((node) => {
        const existing = currentMap.get(String(node.id));
        const chip = chips.get(node.id);
        const info = infoOf(track.id, node, types, statuses, inside);
        return {
          ...existing,
          id: String(node.id),
          type: 'dawg',
          position: direction === 'vertical' ? { x: node.y, y: node.x } : { x: node.x, y: node.y },
          data: { trackId: track.id, node, info, connections: Object.values(graph.connections), direction, chip: chip && chipFacing(flowAt(chip.into.fromNode), flowAt(chip.out.toNode), direction === 'vertical'), owner: voiceNodeId, ownerParent },
          selected: selected.has(String(node.id)),
          deletable: info?.userEditable ?? true,
        };
      });
    });
    // selectedNode only seeds the selection when the track changes.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [graph, types, statuses, direction, inside]);

  const [edges, setEdges] = useState<Edge[]>([]);
  // The cable a dragged node would be inserted into if dropped now.
  const [insertInto, setInsertInto] = useState<string | null>(null);
  const insertion = useRef<{ id: string; cable: Connection; cables: [Connection, Connection] } | null>(null);
  // A node shaken loose while being dragged: until the drop its cables only look gone.
  const shake = useRef<ShakeDetector | null>(null);
  const [detached, setDetached] = useState<Detachment | null>(null);
  const detachedRef = useRef<Detachment | null>(null);
  // Hovering or selecting nodes brightens them, what they connect to and those cables, and quietens the rest.
  const focusIds = useMemo(() => {
    const ids = new Set(nodes.filter((n) => n.selected).map((n) => Number(n.id)));
    if (hovered !== null) ids.add(hovered);
    return ids;
  }, [nodes, hovered]);
  const focus = useMemo(() => focusOf(Object.values(graph.connections), focusIds), [graph, focusIds]);

  const shownEdges = useMemo(() => {
    const gone = new Set(detached?.remove.map(edgeId));
    const closing = (detached?.add ?? []).map((c): Edge => {
      const id = edgeId(c);
      const like = edges.find((e) => e.id === edgeId(detached!.remove.find((r) => r.toNode === c.toNode)!));
      return {
        id,
        source: String(c.fromNode),
        sourceHandle: outputHandle(c.fromPort),
        target: String(c.toNode),
        targetHandle: inputHandle(c.toPort),
        className: like?.className,
        data: { ...c },
      };
    });
    const dragging = new Map(nodes.filter((n) => n.dragging).map((n) => [n.id, n]));
    const vertical = direction === 'vertical';

    return [...edges.filter((e) => !gone.has(e.id)), ...closing].map((e) => {
      const cable = e.data as unknown as Connection;
      const route = detached || dragging.has(e.source) || dragging.has(e.target) ? undefined : routes.get(e.id);
      // A cable's own bends, on the canvas; while both its nodes are dragged together they go along (G8).
      let bends = cable.bends?.length ? cable.bends.map((p) => facing(p, vertical)) : undefined;
      const [source, target] = [dragging.get(e.source), dragging.get(e.target)];
      if (bends && source && target) {
        const was = facing(source.data.node, vertical);
        bends = translate(bends, source.position.x - was.x, source.position.y - was.y);
      }
      const focused = focus?.cables.has(e.id) ?? false;
      const look = `${focused ? 'focused' : focus ? 'faded' : ''} ${e.id === insertInto ? 'insert-target' : ''}`;
      const onBends = detached ? undefined : (next: Point[] | null) => setBends(cable, next);
      // A cable's input end can be dragged to another port; its output end once it is selected, as a drag from an output otherwise makes a
      // new cable.
      const reconnectable: boolean | 'target' = e.selected ? true : 'target';
      return { ...e, type: 'routed', reconnectable, data: { ...e.data, route, bends, onBends }, className: `${e.className} ${look}` };
    });
    // setBends only reads the track and scope, which a new Canvas comes with.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [edges, insertInto, detached, focus, routes, nodes, direction]);

  /** Stores a cable's bends from the canvas, or clears them (G9). */
  function setBends(cable: Connection, bends: Point[] | null) {
    const vertical = direction === 'vertical';
    const { fromNode, fromPort, toNode, toPort } = cable;
    dispatch({
      command: 'setConnectionBends',
      args: {
        trackId: track.id,
        ...scope,
        fromNode,
        fromPort,
        toNode,
        toPort,
        bends: bends?.map((p) => facing({ x: Math.round(p.x), y: Math.round(p.y) }, vertical)),
      },
    });
  }

  /** Clears the bends of the cables at the selected nodes, or of all cables with none selected (G6), as one undo step. */
  const resetCables = (only?: Connection[]) => {
    const chosen = new Set(nodes.filter((n) => n.selected).map((n) => Number(n.id)));
    const cables = (only ?? Object.values(graph.connections)).filter(
      (c) => c.bends?.length && (only || chosen.size === 0 || chosen.has(c.fromNode) || chosen.has(c.toNode)),
    );
    const commands: Command[] = cables.map(({ fromNode, fromPort, toNode, toPort }) => ({
      command: 'setConnectionBends',
      args: { trackId: track.id, ...scope, fromNode, fromPort, toNode, toPort },
    }));
    if (commands.length === 1) dispatch(commands[0]);
    else if (commands.length > 1) dispatch({ command: 'batch', args: { label: 'Reset cables', commands } });
  };
  const anyBends = Object.values(graph.connections).some((c) => c.bends?.length);

  const shownNodes = useMemo(
    () => nodes.map((n) => (focus && !focus.nodes.has(Number(n.id)) ? { ...n, className: 'faded' } : n.className ? { ...n, className: undefined } : n)),
    [nodes, focus],
  );

  useEffect(() => {
    setEdges((current) => {
      const currentMap = new Map(current.map((e) => [e.id, e]));
      const selected = new Set(current.filter((e) => e.selected).map((e) => e.id));
      const chips = feedbackChips(graph);
      return Object.values(graph.connections).map((c) => {
        const id = edgeId(c);
        const existing = currentMap.get(id);
        const from = graph.nodes[String(c.fromNode)];
        const port = (from ? infoOf(track.id, from, types, statuses, inside) : undefined)?.outputs[c.fromPort];
        const kind = port?.type ?? 'audio';
        // A cable from a mono port is drawn thinner (docs/design/mono-ports.md, MP8).
        const mono = port !== undefined && isMono(port, 'out', c.fromPort, widths[`${track.id}:${c.fromNode}`]);
        return {
          ...existing,
          id,
          source: String(c.fromNode),
          sourceHandle: outputHandle(c.fromPort),
          target: String(c.toNode),
          targetHandle: inputHandle(c.toPort),
          className: `graph-edge ${kind} ${mono ? 'mono' : ''} ${isLoopBack(graph, c) || chips.has(c.toNode) ? 'loop-back' : ''}`,
          selected: selected.has(id),
          data: { ...c },
        };
      });
    });
  }, [graph, types, statuses, inside, widths, track.id]);

  // A Feedback node put in a closing cable is selected, so that its panel shows.
  const feedbackBefore = useRef<Set<number> | null>(null);
  useEffect(() => {
    const before = feedbackBefore.current;
    if (!before) return;
    feedbackBefore.current = null;
    const added = [...feedbackNodes(graph)].find((id) => !before.has(id));
    if (added === undefined) return;
    setNodes((current) => current.map((n) => ({ ...n, selected: n.id === String(added) })));
    ui.selectNode({ trackId: track.id, nodeId: added });
  }, [graph]);

  // The node under the pointer in a drag, which the others move with, and whether Shift skips snapping for this drag (G3).
  const dragLead = useRef<string | null>(null);
  const loose = useRef(false);

  /** A drag's moves, with the lead node's first port on a grid point and the others keeping their places around it (G1). */
  const snapped = (changes: NodeChange<GraphFlowNode>[]) => {
    if (!useLayout.getState().graphSnap || loose.current || dragLead.current === null) return changes;
    const lead = changes.find((c) => c.type === 'position' && c.id === dragLead.current && c.dragging && c.position);
    const port = firstPort(flow.getInternalNode(dragLead.current ?? '')?.internals.handleBounds);
    if (!lead || lead.type !== 'position' || !lead.position || !port) return changes;
    const to = snapNode(lead.position, port);
    const [dx, dy] = [to.x - lead.position.x, to.y - lead.position.y];
    return changes.map((c) =>
      c.type === 'position' && c.dragging && c.position ? { ...c, position: { x: c.position.x + dx, y: c.position.y + dy } } : c,
    );
  };

  // Removal goes through onBeforeDelete; everything else (drag, selection, size) is local until committed.
  const onNodesChange = useCallback((changes: NodeChange<GraphFlowNode>[]) => {
    const kept = snapped(changes.filter((c) => c.type !== 'remove'));
    setNodes((current) => applyNodeChanges(kept, current));
    // snapped reads the drag's refs and the store as they are now.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  const onEdgesChange = useCallback((changes: EdgeChange[]) => {
    setEdges((current) => applyEdgeChanges(changes.filter((c) => c.type !== 'remove'), current));
  }, []);

  const toConnection = (c: FlowConnection): Connection => ({
    fromNode: Number(c.source),
    fromPort: portOf(c.sourceHandle),
    toNode: Number(c.target),
    toPort: portOf(c.targetHandle),
  });

  // Nodes added from the browser or the add menu land with their first port on the grid (G1): once they are drawn and measured, they move
  // there in the same undo step as the adding.
  const adding = useRef<{ gesture: string; before: Set<number>; at: number } | null>(null);
  const placeNew = () => {
    adding.current = { gesture: newGestureId(), before: new Set(Object.values(graph.nodes).map((n) => n.id)), at: Date.now() };
    return adding.current.gesture;
  };

  // The cable whose end is being dragged to another port (G8), and whether it was dropped on one.
  const reconnecting = useRef<{ cable: Connection; done: boolean } | null>(null);
  /** The graph without a cable: what its moved end is checked against. */
  const without = (cable: Connection): TrackGraph => ({
    ...graph,
    connections: Object.fromEntries(Object.entries(graph.connections).filter(([, c]) => edgeId(c) !== edgeId(cable))),
  });

  /** A cable moved to another port, as one undo step: it keeps its bends, unless it now closes a loop and gets a Feedback node. */
  const reconnect = (was: Connection, cable: Connection) => {
    if (edgeId(cable) === edgeId(was)) return;
    const rest = without(was);
    const loop = closesLoop(rest, cable);
    const { fromNode, fromPort, toNode, toPort } = was;
    const commands: Command[] = [
      { command: 'disconnect', args: { trackId: track.id, ...scope, fromNode, fromPort, toNode, toPort } },
      loop
        ? { command: 'connectOrCloseLoop', args: { trackId: track.id, ...scope, ...cable, ...loopNodePosition(rest, cable) } }
        : { command: 'connect', args: { trackId: track.id, ...scope, ...cable } },
    ];
    if (!loop && was.bends?.length) commands.push({ command: 'setConnectionBends', args: { trackId: track.id, ...scope, ...cable, bends: was.bends } });
    if (loop) feedbackBefore.current = feedbackNodes(graph);
    dispatch({ command: 'batch', args: { label: 'Reconnect cable', commands } });
  };

  const addNode = (picked: PickedNode, x: number, y: number) =>
    dispatch(
      { command: 'addNode', args: { trackId: track.id, ...scope, type: picked.type, plugin: picked.plugin, code: picked.code, mode: picked.mode, x, y } },
      placeNew(),
    );

  /** A picked factory patch or saved component, or else one node. */
  const addPicked = (picked: PickedNode, x: number, y: number) => {
    const patch = PATCHES.find((p) => p.key === picked.patch);
    if (patch) dispatch(patchCommand(patch, track.id, scope, x, y), placeNew());
    else if (picked.saved)
      void pickedComponent(picked)
        .then((component) =>
          dispatch({ command: 'addNodes', args: { trackId: track.id, ...scope, nodes: [{ type: NODE_TYPES.component, component, x, y }] } }, placeNew()),
        )
        .catch(reportError);
    else addNode(picked, x, y);
  };

  /** The cable under the pointer that a dragged node can be inserted into, and the cables that would replace it. */
  const insertionAt = (pointer: { clientX: number; clientY: number }, node: GraphNode) => {
    const current = graphAfter(graph, detachedRef.current);
    for (const element of document.elementsFromPoint(pointer.clientX, pointer.clientY)) {
      const id = element.closest('.react-flow__edge')?.getAttribute('data-id');
      const cable = Object.values(current.connections).find((c) => edgeId(c) === id);
      const cables = cable && insertionCables(current, node, cable, (n) => infoOf(track.id, n, types, statuses, inside));
      if (cable && cables) return { id: edgeId(cable), cable, cables };
    }
    return null;
  };

  /** A node on the canvas along the signal's direction, as the model stores positions. */
  const boxOf = (n: GraphFlowNode): Box => {
    const [width, height] = [n.measured?.width ?? NEW_NODE_SIZE.width, n.measured?.height ?? NEW_NODE_SIZE.height];
    return direction === 'vertical'
      ? { x: n.position.y, y: n.position.x, width: height, height: width }
      : { x: n.position.x, y: n.position.y, width, height };
  };

  /** Where a node inserted into a cable goes, and the moves that clear the nodes after it. */
  const roomFor = (box: Box, cable: Connection, nodeId: number) => {
    const boxes = new Map(nodes.filter((n) => Number(n.id) !== nodeId).map((n) => [Number(n.id), boxOf(n)]));
    const [from, to] = [boxes.get(cable.fromNode), boxes.get(cable.toNode)];
    if (!from || !to) return { x: box.x, moves: [] as Command[] };

    const room = makeRoom(box, from, to, [...boxes.values()]);
    const moves = [...boxes].flatMap(([id, b]): Command[] =>
      room.moved.includes(b)
        ? [
            {
              command: 'setNodePosition',
              args: { trackId: track.id, ...scope, nodeId: id, x: Math.round(b.x + room.shift), y: Math.round(b.y) },
            },
          ]
        : [],
    );
    return { x: Math.round(room.x), moves };
  };

  const copySelection = () => {
    const chosen = nodes.filter((n) => n.selected && n.data.info?.userEditable !== false).map((n) => n.data.node);
    if (chosen.length === 0) return false;
    const index = new Map(chosen.map((n, i) => [n.id, i]));
    const connections = Object.values(graph.connections)
      .filter((c) => index.has(c.fromNode) && index.has(c.toNode))
      .map((c) => ({ from: index.get(c.fromNode)!, fromPort: c.fromPort, to: index.get(c.toNode)!, toPort: c.toPort }));
    clipboard = { trackId: track.id, nodes: chosen, connections };
    return true;
  };

  const paste = () => {
    if (!clipboard) return;
    const copied = clipboard;
    dispatch({
      command: 'addNodes',
      args: {
        trackId: track.id,
        ...scope,
        nodes: clipboard.nodes.map((n) => ({
          type: n.type,
          plugin: n.plugin,
          source: n.plugin ? { trackId: copied.trackId, nodeId: n.id } : undefined,
          params: n.params,
          bypassed: n.bypassed,
          code: n.code,
          zones: n.zones,
          steps: n.steps,
          midi: n.midi,
          modulations: n.modulations,
          modulationShape: n.modulationShape,
          voice: n.voice,
          component: n.component,
          x: n.x + PASTE_OFFSET,
          y: n.y + PASTE_OFFSET,
        })),
        connections: clipboard.connections,
      },
    });
  };

  const deleteNodes = (nodeIds: number[], label = 'Delete') => {
    // The track output and a voice graph's Voice Input and Output stay.
    const editable = new Set(
      nodeIds.filter((id) => {
        const node = graph.nodes[String(id)];
        return node !== undefined && (infoOf(track.id, node, types, statuses, inside)?.userEditable ?? true);
      }),
    );
    const gone = new Set(editable);
    const commands: Command[] = Object.values(graph.connections)
      .filter((c) => gone.has(c.fromNode) || gone.has(c.toNode))
      .map((c) => ({ command: 'disconnect', args: { trackId: track.id, ...scope, ...c } }));
    if (editable.size > 0) commands.push({ command: 'removeNodes', args: { trackId: track.id, ...scope, nodeIds: [...editable] } });
    if (commands.length === 1) dispatch(commands[0]);
    else if (commands.length > 1) dispatch({ command: 'batch', args: { label, commands } });
    if ([...editable].some((id) => id === selectedNode?.nodeId)) ui.selectNode(null);
  };

  const deleteSelection = (removed: { nodes: GraphFlowNode[]; edges: Edge[] }) => {
    const nodeIds = removed.nodes.filter((n) => n.data.info?.userEditable !== false).map((n) => n.data.node.id);
    const gone = new Set(nodeIds);
    const commands: Command[] = removed.edges
      .map((e) => e.data as unknown as Connection)
      .filter((c) => !gone.has(c.fromNode) && !gone.has(c.toNode))
      .map((c) => ({ command: 'disconnect', args: { trackId: track.id, ...scope, ...c } }));
    if (nodeIds.length > 0) commands.push({ command: 'removeNodes', args: { trackId: track.id, ...scope, nodeIds } });
    if (commands.length === 1) dispatch(commands[0]);
    else if (commands.length > 1) dispatch({ command: 'batch', args: { label: 'Delete', commands } });
    if (nodeIds.some((id) => id === selectedNode?.nodeId)) ui.selectNode(null);
  };

  const measureNodes = () =>
    nodes.flatMap((n): MeasuredNode[] => {
      const internal = flow.getInternalNode(n.id);
      const bounds = internal?.internals.handleBounds;
      if (!internal || !bounds) return [];
      const handles = [...(bounds.source ?? []), ...(bounds.target ?? [])];
      return [
        {
          id: Number(n.id),
          width: n.measured?.width ?? NEW_NODE_SIZE.width,
          height: n.measured?.height ?? NEW_NODE_SIZE.height,
          ports: handles.flatMap((h) =>
            h.id ? [{ handle: h.id, side: h.position as Side, x: h.x + h.width / 2, y: h.y + h.height / 2 }] : [],
          ),
        },
      ];
    });

  // Where the nodes are, for when to work the routes out again: not while one is being dragged, as the model hasn't moved yet.
  const placed = Object.values(graph.nodes).map((n) => `${n.id}:${n.x}:${n.y}`).join();
  const sized = nodes.map((n) => `${n.id}:${n.measured?.width}:${n.measured?.height}:${n.data.chip ?? ''}`).join();

  useEffect(() => {
    // Two frames on, as a chip that has turned round measures its ports in the next one.
    let frame = requestAnimationFrame(() => {
      frame = requestAnimationFrame(() => {
        const measured = measureNodes();
        const place = (id: number) => {
          const node = graph.nodes[String(id)];
          if (!node) return undefined;
          return direction === 'vertical' ? { x: node.y, y: node.x } : { x: node.x, y: node.y };
        };
        const boxes = new Map<number, RouteBox>();
        for (const m of measured) {
          const at = place(m.id);
          if (at) boxes.set(m.id, { ...at, width: m.width, height: m.height });
        }
        const endOf = (id: number, handle: string): CableEnd | null => {
          const [box, port] = [boxes.get(id), measured.find((m) => m.id === id)?.ports.find((p) => p.handle === handle)];
          return box && port ? { at: { x: box.x + port.x, y: box.y + port.y }, side: port.side } : null;
        };

        const next = new Map<string, Point[]>();
        for (const c of Object.values(graph.connections)) {
          // A cable with bends of its own goes where they say (G5).
          if (c.bends?.length) continue;
          const id = edgeId(c);
          const [from, to] = [endOf(c.fromNode, outputHandle(c.fromPort)), endOf(c.toNode, inputHandle(c.toPort))];
          if (!from || !to) continue;
          if (curveHits(from, to, [...boxes.values()])) {
            const around = routeAround(from, to, [...boxes.values()]);
            if (around) next.set(id, around);
          }
        }
        setRoutes(next);
      });
    });
    return () => cancelAnimationFrame(frame);
    // measureNodes reads what `sized` and `placed` stand for.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [placed, sized, graph.connections, direction]);

  // Snaps what was just added, once it has been measured: a patch's nodes move together, by what puts the first one's port on the grid.
  useEffect(() => {
    const pending = adding.current;
    if (!pending) return;
    if (Date.now() - pending.at > 5000) {
      adding.current = null;
      return;
    }
    const added = nodes.filter((n) => !pending.before.has(Number(n.id)));
    if (added.length === 0) return;
    const lead = added.reduce((a, b) => (Number(b.id) < Number(a.id) ? b : a));
    const port = firstPort(flow.getInternalNode(lead.id)?.internals.handleBounds);
    if (!port) return;
    adding.current = null;
    if (!useLayout.getState().graphSnap) return;
    const to = snapNode(lead.position, port);
    const [dx, dy] = [to.x - lead.position.x, to.y - lead.position.y];
    if (dx === 0 && dy === 0) return;
    const vertical = direction === 'vertical';
    const commands: Command[] = added.map((n) => {
      const at = facing({ x: n.position.x + dx, y: n.position.y + dy }, vertical);
      return { command: 'setNodePosition', args: { trackId: track.id, ...scope, nodeId: Number(n.id), x: at.x, y: at.y } };
    });
    dispatch(commands.length === 1 ? commands[0] : { command: 'batch', args: { label: 'Place nodes', commands } }, pending.gesture);
    // Runs when the nodes have been measured; the rest is read as it is then.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sized]);

  const arrange = async () => {
    const measured = measureNodes();
    const vertical = direction === 'vertical';
    const result = snapLayout(await layoutGraph(graph, measured, vertical), measured, Object.values(graph.connections), useLayout.getState().graphSnap);
    // Layout's routes become the cables' bends, in the same undo step as the positions, so they outlast a reload (G7).
    const routed: Command[] = Object.values(graph.connections).flatMap((c): Command[] => {
      const route = result.routes.get(edgeId(c)) ?? [];
      if (route.length === 0 && !c.bends?.length) return [];
      const { fromNode, fromPort, toNode, toPort } = c;
      const bends = route.length > 0 ? route.map((p) => facing({ x: Math.round(p.x), y: Math.round(p.y) }, vertical)) : undefined;
      return [{ command: 'setConnectionBends', args: { trackId: track.id, ...scope, fromNode, fromPort, toNode, toPort, bends } }];
    });
    const commands: Command[] = [...result.positions].map(([nodeId, at]) => ({
      command: 'setNodePosition',
      args: {
        trackId: track.id,
        ...scope,
        nodeId,
        x: vertical ? at.y : at.x,
        y: vertical ? at.x : at.y,
      },
    }));
    if (commands.length > 0) dispatch({ command: 'batch', args: { label: 'Layout graph', commands: [...commands, ...routed] } });
  };

  /** Lines the selected nodes up or spaces them evenly (G4), as one undo step. */
  const alignSelection = (kind: AlignKind) => {
    const vertical = direction === 'vertical';
    const chosen = nodes.filter((n) => n.selected).map((n): AlignNode => {
      const bounds = flow.getInternalNode(n.id)?.internals.handleBounds;
      const centres = [...(bounds?.target ?? []), ...(bounds?.source ?? [])].flatMap((h) =>
        h.id ? [{ handle: h.id, x: h.x + h.width / 2, y: h.y + h.height / 2 }] : [],
      );
      return {
        id: Number(n.id),
        ...n.position,
        width: n.measured?.width ?? NEW_NODE_SIZE.width,
        height: n.measured?.height ?? NEW_NODE_SIZE.height,
        input: firstPortOf(centres.filter((c) => c.handle.startsWith('in-'))),
        output: firstPortOf(centres.filter((c) => c.handle.startsWith('out-'))),
      };
    });
    const commands: Command[] = [...align(chosen, kind, vertical, useLayout.getState().graphSnap)].map(([nodeId, at]) => ({
      command: 'setNodePosition',
      args: { trackId: track.id, ...scope, nodeId, x: vertical ? at.y : at.x, y: vertical ? at.x : at.y },
    }));
    if (commands.length > 0) dispatch({ command: 'batch', args: { label: kind === 'distribute' ? 'Distribute nodes' : 'Align nodes', commands } });
  };
  const selectedCount = nodes.filter((n) => n.selected).length;

  const toggleSelectedBypass = () => {
    const selected = nodes.filter((node) => node.selected && node.data.info?.userEditable !== false);
    if (selected.length === 0) return;
    const bypassed = selected.some((node) => !node.data.node.bypassed);
    const commands: Command[] = selected.map((node) => ({
      command: 'setNodeBypass',
      args: { trackId: track.id, ...scope, nodeId: node.data.node.id, bypassed },
    }));
    dispatch({ command: 'batch', args: { label: bypassed ? 'Bypass nodes' : 'Enable nodes', commands } });
  };

  const groupSelection = () => {
    const nodeIds = nodes.filter((node) => node.selected && node.data.info?.userEditable !== false).map((node) => node.data.node.id);
    if (nodeIds.length === 0) return;
    dispatch({ command: 'groupNodes', args: { trackId: track.id, ...scope, nodeIds } });
    ui.selectNode(null);
  };

  const ungroupSelection = () => {
    const commands: Command[] = nodes
      .filter((node) => node.selected && node.data.node.component)
      .map((node) => ({ command: 'ungroupNode', args: { trackId: track.id, ...scope, nodeId: node.data.node.id } }));
    if (commands.length === 0) return;
    dispatch({ command: 'batch', args: { label: 'Ungroup', commands } });
    ui.selectNode(null);
  };

  const selectAll = () => {
    setNodes((current) =>
      current.map((node) => ({ ...node, selected: node.data.info?.userEditable !== false })),
    );
  };

  const cutSelection = () => {
    const selected = nodes.filter((node) => node.selected && node.data.info?.userEditable !== false);
    if (selected.length === 0 || !copySelection()) return;
    deleteNodes(selected.map((node) => node.data.node.id), 'Cut');
  };

  return (
    <div
      className={`graph-canvas ${direction}`}
      ref={wrapper}
      tabIndex={0}
      onKeyDown={(e) => {
        const mod = e.ctrlKey || e.metaKey;
        if (mod && e.key.toLowerCase() === 'c') copySelection();
        else if (mod && e.key.toLowerCase() === 'v') paste();
        else if (mod && e.key.toLowerCase() === 'd') {
          e.preventDefault();
          if (copySelection()) paste();
        } else if (mod && e.key.toLowerCase() === 'b') {
          e.preventDefault();
          toggleSelectedBypass();
        } else if (mod && e.key.toLowerCase() === 'a') {
          e.preventDefault();
          selectAll();
        } else if (mod && e.key.toLowerCase() === 'x') {
          e.preventDefault();
          cutSelection();
        } else if (mod && e.key.toLowerCase() === 'g') {
          e.preventDefault();
          if (e.shiftKey) ungroupSelection();
          else groupSelection();
        } else if (e.key === 'Escape') {
          setNodes((current) => current.map((node) => ({ ...node, selected: false })));
          ui.selectNode(null);
        }
      }}
      onDragOver={(e) => {
        if (!e.dataTransfer.types.includes(NODE_DRAG_TYPE)) return;
        e.preventDefault();
        e.dataTransfer.dropEffect = 'copy';
        const dragged = draggedPaletteNode();
        const palette = dragged ? ({ id: -1, type: dragged.type } as GraphNode) : null;
        setInsertInto(palette ? (insertionAt(e, palette)?.id ?? null) : null);
      }}
      onDragLeave={(e) => {
        if (!e.currentTarget.contains(e.relatedTarget as Node | null)) setInsertInto(null);
      }}
      onDrop={(e) => {
        const raw = e.dataTransfer.getData(NODE_DRAG_TYPE);
        if (!raw) return;
        e.preventDefault();
        setInsertInto(null);
        const picked = JSON.parse(raw) as PickedNode;
        const at = flow.screenToFlowPosition({ x: e.clientX, y: e.clientY });
        if (picked.patch || picked.saved) {
          addPicked(picked, Math.round(at.x), Math.round(at.y));
          return;
        }
        const [x, y] = direction === 'vertical' ? [Math.round(at.y), Math.round(at.x)] : [Math.round(at.x), Math.round(at.y)];
        const target = insertionAt(e, { id: -1, type: picked.type } as GraphNode);
        if (!target) {
          addNode(picked, Math.round(at.x), Math.round(at.y));
          return;
        }
        // Dropped on a cable: the node goes into its path, and what is to the right makes room, as one undo step.
        const room = roomFor({ x, y, ...NEW_NODE_SIZE }, target.cable, -1);
        const command: Command = {
          command: 'addNode',
          args: {
            trackId: track.id,
            ...scope,
            type: picked.type,
            plugin: picked.plugin,
            code: picked.code,
            mode: picked.mode,
            x: room.x,
            y,
            insertInto: { ...target.cable, input: target.cables[0].toPort, output: target.cables[1].fromPort },
          },
        };
        dispatch({ command: 'batch', args: { label: 'Insert node', commands: [...room.moves, command] } }, placeNew());
      }}
    >
      <ReactFlow<GraphFlowNode, Edge>
        role="application"
        aria-label={voiceNodeId === undefined ? `Signal graph for ${track.name}` : `Voice graph on ${track.name}`}
        nodes={shownNodes}
        edges={shownEdges}
        nodeTypes={nodeTypes}
        edgeTypes={edgeTypes}
        onNodeMouseEnter={(_e, node) => setHovered(Number(node.id))}
        onNodeMouseLeave={() => setHovered(null)}
        colorMode="dark"
        fitView
        // On opening, nodes stay readable: a long chain is centred at 60 % rather than shrunk to fit, and a lone node
        // isn't blown up past 100 % (docs/design/ux-review.md, U21). Fit graph still shows everything.
        fitViewOptions={{ minZoom: 0.6, maxZoom: 1 }}
        minZoom={0.2}
        maxZoom={2}
        // Only Delete removes nodes: Backspace is for text, and a slip in a value field mustn't take a device with it.
        // A cable's end is grabbed just outside its port (G8).
        reconnectRadius={14}
        deleteKeyCode="Delete"
        proOptions={{ hideAttribution: true }}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onSelectionChange={({ nodes: chosen }) => {
          const only = chosen.length === 1 ? chosen[0] : null;
          ui.selectNode(only ? { trackId: track.id, nodeId: Number(only.id) } : null);
        }}
        onNodeDragStart={(e, node) => {
          shake.current = new ShakeDetector();
          dragLead.current = node.id;
          loose.current = e.shiftKey;
        }}
        onNodeDrag={(e, _node, dragged) => {
          loose.current = e.shiftKey;
          const pointer = 'changedTouches' in e ? e.changedTouches[0] : e;
          const only = dragged.length === 1 ? dragged[0].data.node : null;

          // Shaking a node takes it out of the cables it is in; the drop makes that real.
          if (only && !detachedRef.current && shake.current?.feed(pointer.clientX, pointer.clientY, e.timeStamp)) {
            detachedRef.current = detachment(graph, only, (n) => infoOf(track.id, n, types, statuses, inside));
            setDetached(detachedRef.current);
          }

          insertion.current = only ? insertionAt(pointer, only) : null;
          setInsertInto(insertion.current?.id ?? null);
        }}
        onNodeDragStop={(_e, _node, released) => {
          // Where the nodes are drawn, which snapping may have moved from where React Flow put them.
          const dragged = released.map((n) => nodes.find((m) => m.id === n.id) ?? n);
          // A snapped position stays exact: a port's centre may be half a pixel into its node.
          const keep = (v: number) => (useLayout.getState().graphSnap && !loose.current ? v : Math.round(v));
          // What the highlight showed last is what the drop does.
          const dropped = insertion.current;
          const shaken = detachedRef.current;
          dragLead.current = null;
          insertion.current = null;
          detachedRef.current = null;
          setInsertInto(null);
          setDetached(null);
          const gesture = newGestureId();
          const commands: Command[] = dragged
            .filter((n) => {
              const x = direction === 'vertical' ? n.position.y : n.position.x;
              const y = direction === 'vertical' ? n.position.x : n.position.y;
              return x !== n.data.node.x || y !== n.data.node.y;
            })
            .map((n) => ({
              command: 'setNodePosition',
              args: {
                trackId: track.id,
                ...scope,
                nodeId: n.data.node.id,
                x: keep(direction === 'vertical' ? n.position.y : n.position.x),
                y: keep(direction === 'vertical' ? n.position.x : n.position.y),
              },
            }));

          const rewire = (command: 'connect' | 'disconnect', c: Connection): Command => ({
            command,
            args: { trackId: track.id, ...scope, ...c },
          });
          const unplug = shaken
            ? [...shaken.remove.map((c) => rewire('disconnect', c)), ...shaken.add.map((c) => rewire('connect', c))]
            : [];

          // Dropped on a cable: the node goes into its path and what is to the right makes room, as one undo step with
          // the move.
          if (dropped) {
            const node = dragged[0];
            const room = roomFor(boxOf(node), dropped.cable, node.data.node.id);
            const y = direction === 'vertical' ? node.position.x : node.position.y;
            dispatch({
              command: 'batch',
              args: {
                label: 'Insert node',
                commands: [
                  ...unplug,
                  ...room.moves,
                  {
                    command: 'setNodePosition',
                    args: { trackId: track.id, ...scope, nodeId: node.data.node.id, x: room.x, y: Math.round(y) },
                  },
                  rewire('disconnect', dropped.cable),
                  ...dropped.cables.map((c) => rewire('connect', c)),
                ],
              },
            });
          } else if (shaken) {
            dispatch({ command: 'batch', args: { label: 'Detach node', commands: [...unplug, ...commands] } });
          } else if (commands.length === 1) dispatch(commands[0], gesture);
          else if (commands.length > 1) {
            // The bends of cables between the moved nodes go with them (G8).
            const moved = new Map(commands.map((c) => [(c.args as { nodeId: number }).nodeId, c.args as { x: number; y: number }]));
            const carried: Command[] = Object.values(graph.connections).flatMap((c): Command[] => {
              const [from, to, node] = [moved.get(c.fromNode), moved.get(c.toNode), graph.nodes[String(c.fromNode)]];
              if (!c.bends?.length || !from || !to || !node) return [];
              const { fromNode, fromPort, toNode, toPort } = c;
              const bends = translate(c.bends, from.x - node.x, from.y - node.y);
              return [{ command: 'setConnectionBends', args: { trackId: track.id, ...scope, fromNode, fromPort, toNode, toPort, bends } }];
            });
            dispatch({ command: 'batch', args: { label: 'Move nodes', commands: [...commands, ...carried] } });
          }
        }}
        onNodeClick={(_e, node) => {
          // A click shows the node's panel (a Faust node's controls) in the device dock, opening it if hidden. A built-in
          // instrument at the track's level has no card there: its editor is its own window (MC14), opened by a double-click.
          if (builtInInstrument(node.data.node)) return;
          if (!useLayout.getState().dockVisible) layout.toggleDock();
        }}
        onNodeDoubleClick={(_e, node) => {
          if (builtInInstrument(node.data.node)) openInstrument(track, node.data.node);
          else if (node.data.node.plugin) actions.openEditor(track.id, node.data.node.id);
          else if (node.data.node.type === NODE_TYPES.faust) ui.editCode(track.id, node.data.node.id);
          else if (node.data.node.type === NODE_TYPES.voiceInstrument || node.data.node.component) ui.enterGraph(track.id, node.data.node.id);
        }}
        isValidConnection={(c) => {
          const connection = c as FlowConnection;
          const base = reconnecting.current ? without(reconnecting.current.cable) : graph;
          return connectionProblem(base, (n) => infoOf(track.id, n, types, statuses, inside), toConnection(connection)) === null;
        }}
        onReconnectStart={(_e, edge) => {
          reconnecting.current = { cable: edge.data as unknown as Connection, done: false };
          setProblem(null);
        }}
        onReconnect={(edge, next) => {
          if (reconnecting.current) reconnecting.current.done = true;
          reconnect(edge.data as unknown as Connection, toConnection(next));
        }}
        onReconnectEnd={(_e, _edge, _end, state) => {
          const moved = reconnecting.current;
          reconnecting.current = null;
          if (!moved || moved.done) return;
          // Let go on empty canvas, the cable comes out, as one pulled off an input does; on a port it can't go to, say why.
          if (!state.toHandle) dispatch({ command: 'disconnect', args: { trackId: track.id, ...scope, ...moved.cable } });
          else if (state.fromHandle) {
            const [source, target] = state.fromHandle.type === 'source' ? [state.fromHandle, state.toHandle] : [state.toHandle, state.fromHandle];
            const c = toConnection({ source: source.nodeId, sourceHandle: source.id ?? null, target: target.nodeId, targetHandle: target.id ?? null });
            setProblem(connectionProblem(without(moved.cable), (n) => infoOf(track.id, n, types, statuses, inside), c));
          }
        }}
        onConnectEnd={(_e, state) => {
          // A cable's end being moved is onReconnectEnd's.
          if (reconnecting.current) return;
          if (state.isValid || !state.fromHandle) return setProblem(null);
          const from = state.fromHandle;

          // Pulling a cable off an input and letting go on empty canvas disconnects it, as in modular patching
          // environments. Dragging from an output only ever makes new cables.
          if (!state.toHandle) {
            if (from.type !== 'target') return setProblem(null);
            const node = Number(from.nodeId);
            const port = portOf(from.id);
            const cables = Object.values(graph.connections).filter((c) => c.toNode === node && c.toPort === port);
            if (cables.length === 1) dispatch({ command: 'disconnect', args: { trackId: track.id, ...scope, ...cables[0] } });
            return setProblem(
              cables.length > 1 ? 'Several cables meet at this input: select one and press Delete to remove it.' : null,
            );
          }

          // A drag can start at either end; the connection always runs from an output to an input.
          const [source, target] = from.type === 'source' ? [from, state.toHandle] : [state.toHandle, from];
          const c = toConnection({
            source: source.nodeId,
            sourceHandle: source.id ?? null,
            target: target.nodeId,
            targetHandle: target.id ?? null,
          });
          setProblem(connectionProblem(graph, (n) => infoOf(track.id, n, types, statuses, inside), c));
        }}
        onConnect={(c) => {
          setProblem(null);
          const cable = toConnection(c);

          const [from, to] = [graph.nodes[String(cable.fromNode)], graph.nodes[String(cable.toNode)]];
          const [out, input] = [from && infoOf(track.id, from, types, statuses, inside)?.outputs[cable.fromPort], to && infoOf(track.id, to, types, statuses, inside)?.inputs[cable.toPort]];

          // A cable to a spare port makes the port, typed and named after what's on the other end.
          if (voiceNodeId !== undefined && (out?.type === 'any' || input?.type === 'any')) {
            const other = out?.type === 'any' ? input : out;
            if (!other || other.type === 'any') return;
            dispatch({
              command: 'batch',
              args: {
                label: 'Add port',
                commands: [
                  { command: 'addComponentPort', args: { trackId: track.id, nodeId: voiceNodeId, side: out?.type === 'any' ? 'input' : 'output', type: other.type, name: other.name } },
                  { command: 'connect', args: { trackId: track.id, ...scope, ...cable } },
                ],
              },
            });
            return;
          }

          feedbackBefore.current = feedbackNodes(graph);
          dispatch({ command: 'connectOrCloseLoop', args: { trackId: track.id, ...scope, ...cable, ...loopNodePosition(graph, cable) } });
        }}
        onBeforeDelete={async (removed) => {
          deleteSelection(removed);
          return false;
        }}
        onNodeContextMenu={(e, node) => {
          e.preventDefault();
          setNodeMenu({ node: node.data.node, x: e.clientX, y: e.clientY });
        }}
        onEdgeContextMenu={(e, edge) => {
          e.preventDefault();
          setEdgeMenu({ cable: edge.data as unknown as Connection, x: e.clientX, y: e.clientY });
        }}
        onPaneContextMenu={(e) => {
          e.preventDefault();
          const at = flow.screenToFlowPosition({ x: e.clientX, y: e.clientY });
          setMenu({ x: e.clientX, y: e.clientY, flowX: at.x, flowY: at.y });
        }}
        onPaneClick={() => setProblem(null)}
      >
        {/* Dots on the grid points (G2), every fifth a little brighter. */}
        <Background id="minor" variant={BackgroundVariant.Dots} gap={GRID} size={1} offset={dotOffset(GRID, 1)} />
        <Background id="major" className="graph-grid-major" variant={BackgroundVariant.Dots} gap={GRID * MAJOR} size={1.5} offset={dotOffset(GRID * MAJOR, 1.5)} />
        <Controls showInteractive={false} />
        <MiniMap pannable zoomable className="graph-minimap" />
      </ReactFlow>
      <div className="graph-actions">
        <Button onClick={() => flow.fitView({ padding: 0.2, duration: 180, maxZoom: 1 })}>
          Fit graph
        </Button>
        <Button
          disabled={!nodes.some((node) => node.selected)}
          onClick={() =>
            flow.fitView({
              nodes: nodes.filter((node) => node.selected).map((node) => ({ id: node.id })),
              padding: 0.35,
              duration: 180,
            })
          }
        >
          Fit selection
        </Button>
        <Button onClick={() => void arrange().catch(reportError)}>
          Layout
        </Button>
        <Button
          title="Line up the selected nodes, or space them evenly"
          disabled={selectedCount < 2}
          aria-haspopup="menu"
          onClick={(e) => setAlignMenu(e.currentTarget.getBoundingClientRect())}
        >
          Align
        </Button>
        <Button
          title="Take the bends out of the selected nodes' cables, or of every cable with none selected"
          disabled={!anyBends}
          onClick={() => resetCables()}
        >
          Reset cables
        </Button>
        <Button disabled={!nodes.some((node) => node.selected)} onClick={toggleSelectedBypass}>
          Bypass selected
        </Button>
        <Button title="Put the selected nodes in a component (Ctrl+G)" disabled={!nodes.some((node) => node.selected && node.data.info?.userEditable !== false)} onClick={groupSelection}>
          Group
        </Button>
        <Button title="Put what's in the selected components back (Ctrl+Shift+G)" disabled={!nodes.some((node) => node.selected && node.data.node.component)} onClick={ungroupSelection}>
          Ungroup
        </Button>
      </div>
      {edgeMenu && (
        <Menu onClose={() => setEdgeMenu(null)} at={{ x: edgeMenu.x, y: edgeMenu.y }} onClick={() => setEdgeMenu(null)}>
          <button className="menu-item" disabled={!edgeMenu.cable.bends?.length} onClick={() => resetCables([edgeMenu.cable])}>
            Reset cable
          </button>
        </Menu>
      )}
      {alignMenu && (
        <Menu onClose={() => setAlignMenu(null)} at={{ x: alignMenu.left, y: alignMenu.bottom + 4 }} onClick={() => setAlignMenu(null)}>
          {(
            [
              ['inputs', 'Align inputs', 'Their first inputs on one line'],
              ['outputs', 'Align outputs', 'Their first outputs on one line'],
              ['flow', direction === 'vertical' ? 'Align in a row' : 'Align in a column', 'The same place along the flow'],
              ['distribute', 'Distribute evenly', 'Even gaps across the flow (three or more nodes)'],
            ] as Array<[AlignKind, string, string]>
          ).map(([kind, label, title]) => (
            <button key={kind} className="menu-item" title={title} disabled={kind === 'distribute' && selectedCount < 3} onClick={() => alignSelection(kind)}>
              {label}
            </button>
          ))}
        </Menu>
      )}
      {problem && <div className="graph-problem">{problem}</div>}
      {nodeMenu && <DeviceMenu track={track} node={nodeMenu.node} x={nodeMenu.x} y={nodeMenu.y} onClose={() => setNodeMenu(null)} />}
      {menu && (
        <PluginPicker
          kind="any"
          instruments={track.kind === 'instrument'}
          voice={runsPerVoice(track, voiceNodeId)}
          x={menu.x}
          y={menu.y}
          onPick={(picked) => {
            setMenu(null);
            addPicked(picked, Math.round(menu.flowX), Math.round(menu.flowY));
          }}
          onClose={() => setMenu(null)}
        />
      )}
    </div>
  );
}


export function GraphView() {
  const track = useGraphTrack();
  const direction = useLayout((s) => s.graphDirection);
  const snap = useLayout((s) => s.graphSnap);
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const graphScope = useUi((s) => s.graphScope);

  // The voice graph shown, while its Voice Instrument is on the shown track; otherwise the track's graph.
  const voiceGraph = track && graphScope?.trackId === track.id ? graphOf(track, graphScope.nodeId) : undefined;
  const voiceNodeId = voiceGraph ? graphScope?.nodeId : undefined;
  const graph = voiceGraph ?? track?.graph;
  const owner = track && voiceNodeId !== undefined ? findNode(track, voiceNodeId) : undefined;
  const inside = owner?.component;
  const sink = inside ? NODE_TYPES.componentOutput : voiceGraph ? NODE_TYPES.voiceOutput : NODE_TYPES.trackOutput;
  // From the track's graph down to the one shown.
  const path = track && owner ? [...(ownerChain(track, owner.id) ?? []), owner] : [];

  const unreachable = track && graph
    ? (() => {
        const incoming = new Map<number, number[]>();
        for (const edge of Object.values(graph.connections))
          incoming.set(edge.toNode, [...(incoming.get(edge.toNode) ?? []), edge.fromNode]);
        const reachable = new Set(
          Object.values(graph.nodes)
            .filter((node) => node.type === sink)
            .map((node) => node.id),
        );
        const queue = [...reachable];
        for (let index = 0; index < queue.length; index += 1) {
          for (const source of incoming.get(queue[index]) ?? [])
            if (!reachable.has(source)) {
              reachable.add(source);
              queue.push(source);
            }
        }
        return Object.values(graph.nodes).filter((node) => {
          const info = infoOf(track.id, node, types, statuses, inside);
          const producesSignal = info?.outputs.some((port) => port.type === 'audio' || port.type === 'events');
          return producesSignal && info?.userEditable !== false && node.type !== sink && !reachable.has(node.id);
        }).length;
      })()
    : 0;

  return (
    <div className="graph-view">
      {/* Where you are, what is wrong, how it reads: three parts apart, with as few words as do (the owner, 2026-11-15). */}
      <div className="roll-toolbar graph-header">
        <Workflow className="graph-header-icon" size={14} aria-label="Graph" />
        {track && (
          <nav className="graph-breadcrumb" aria-label="Graph of">
            {[{ id: null as number | null, name: track.kind === 'master' ? 'Master' : track.name }, ...path.map((node) => ({ id: node.id as number | null, name: nodeName(node, types) }))].map(
              (step, index, steps) => (
                <Fragment key={step.id ?? 'track'}>
                  {index > 0 && <ChevronRight className="graph-breadcrumb-separator" size={12} aria-hidden />}
                  {index + 1 < steps.length ? (
                    <button
                      className="graph-crumb"
                      title={index === 0 ? "Back to the track's graph" : `Back to ${step.name}`}
                      onClick={() => (step.id === null ? ui.leaveGraph() : ui.enterGraph(track.id, step.id))}
                    >
                      {step.name}
                    </button>
                  ) : (
                    <span className="graph-crumb current" aria-current="page">
                      {step.name}
                    </span>
                  )}
                </Fragment>
              ),
            )}
          </nav>
        )}
        {unreachable > 0 && (
          <span className="graph-warning" title="They make no sound until they are connected">
            <TriangleAlert size={12} />
            {unreachable} not connected to {inside ? 'its Output' : voiceGraph ? 'Voice Output' : 'the output'}
          </span>
        )}
        <span className="spacer" />
        <Toggle on={snap} onChange={layout.setGraphSnap} title="Snap nodes and cable bends to the grid (hold Shift while dragging to skip it)">
          Snap
        </Toggle>
        <Segmented
          label="Flow"
          className="graph-direction"
          value={direction}
          onChange={(next) => layout.setGraphDirection(next)}
          options={[
            { value: 'horizontal', label: <ArrowRight size={13} />, title: 'Flow: left to right' },
            { value: 'vertical', label: <ArrowDown size={13} />, title: 'Flow: top to bottom' },
          ]}
        />
        <HelpButton area="Graph" />
      </div>
      {track && graph ? (
        <ReactFlowProvider key={`${track.id}:${voiceNodeId ?? ''}`}>
          <Canvas track={track} graph={graph} voiceNodeId={voiceNodeId} />
        </ReactFlowProvider>
      ) : (
        <p className="empty">No track selected.</p>
      )}
    </div>
  );
}
