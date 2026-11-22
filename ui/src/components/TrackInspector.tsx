import { AlertTriangle, Code, LoaderCircle, Plus, RotateCw, Workflow, X } from 'lucide-react';
import { useState } from 'react';
import { NODE_TYPES, type ComponentGraph, type GraphNode, type Track } from '../bridge/protocol';
import { isOutputsHeader } from '../arrangement/groups';
import { actions, dispatch, useDawg } from '../store/store';
import { ui, useUi } from '../store/uiStore';
import { infoOf } from '../graph/nodeInfo';
import { inspectorGroups, isModulationSource, nodeName, reorderIndex, trackChain } from './chain';
import { PluginPicker, pickedComponent, type PickedNode } from './PluginPicker';
import { openDevice } from './openDevice';
import { DeviceMenu, isRemovable, removeDevice } from './DeviceMenu';
import { InspectorMacros, InstrumentSection } from './InstrumentSection';
import { Button, IconButton, Power } from '../ui/controls';

// The selected track's instrument and insert effects, Cubase-inspector style
// (docs/design/phase4-plugin-hosting.md, section 10).

type Picker = { kind: 'instrument' | 'effect'; x: number; y: number; index?: number };
type Menu = { node: GraphNode; x: number; y: number };

/** Drag handlers for an effect slot: dragging reorders the chain (docs/design/phase4-plugin-hosting.md, section 10). */
interface SlotDrag {
  dragging: boolean;
  dropBefore: boolean;
  onStart: (e: React.DragEvent<HTMLDivElement>) => void;
  onOver: (e: React.DragEvent<HTMLDivElement>) => void;
  onDrop: (e: React.DragEvent<HTMLDivElement>) => void;
  onEnd: () => void;
}

function Slot({
  track,
  node,
  index,
  onMenu,
  drag,
}: {
  track: Track;
  node: GraphNode;
  index?: number;
  onMenu: (e: React.MouseEvent) => void;
  drag?: SlotDrag;
}) {
  const status = useDawg((s) => s.nodes[`${track.id}:${node.id}`]);
  const selected = useUi((s) => s.selectedNode?.trackId === track.id && s.selectedNode.nodeId === node.id);
  const types = useDawg((s) => s.nodeTypes);
  const isPlugin = node.plugin !== undefined;
  const crashed = isPlugin && status?.status === 'crashed';
  const failed = isPlugin && (status?.status === 'failed' || crashed);
  const loading = isPlugin && (status === undefined || status.status === 'loading');
  const late = status?.lateBlocks ?? 0;
  const title = failed
    ? status.error
    : late > 0
      ? `${late} blocks came back too late, and that much sound was lost`
      : isPlugin
        ? `${node.plugin?.manufacturer} · ${node.plugin?.format}`
        : 'Built-in';

  return (
    <div
      className={`slot ${node.bypassed ? 'bypassed' : ''} ${failed ? 'failed' : ''} ${status?.editorOpen ? 'open' : ''} ${selected ? 'selected' : ''} ${drag?.dragging ? 'dragging' : ''} ${drag?.dropBefore ? 'drop-before' : ''}`}
      draggable={drag !== undefined}
      onDragStart={drag?.onStart}
      onDragOver={drag?.onOver}
      onDrop={drag?.onDrop}
      onDragEnd={drag?.onEnd}
      onContextMenu={(e) => {
        e.preventDefault();
        onMenu(e);
      }}
    >
      {index !== undefined && (
        <span className="slot-index" title={drag ? 'Drag to reorder' : undefined}>
          {index}
        </span>
      )}
      {failed && <AlertTriangle size={13} className="slot-warning" />}
      {loading && <LoaderCircle size={13} className="slot-loading" />}
      <button
        className="slot-name"
        title={title}
        // Shows the node's panel in the device dock; a plugin also opens or closes its own editor. Replacing a node
        // is in the context menu.
        onClick={() => openDevice(track, node)}
      >
        {nodeName(node, types)}
      </button>
      {crashed && (
        <IconButton title="Reload the plugin" onClick={() => actions.reloadPlugin(track.id, node.id)}>
          <RotateCw size={12} />
        </IconButton>
      )}
      <CodeButton track={track} node={node} />
      <Power
        name={nodeName(node, types)}
        on={!node.bypassed}
        onChange={(on) => dispatch({ command: 'setNodeBypass', args: { trackId: track.id, nodeId: node.id, bypassed: !on } })}
      />
      {/* Apart from the power switch and shown on hover, so bypassing doesn't land on removing (3.9). */}
      <IconButton className="slot-remove" title="Remove" onClick={() => removeDevice(track, node)}>
        <X size={12} />
      </IconButton>
    </div>
  );
}

/** A graph that is no simple chain (docs/design/phase8-node-graph.md, G6): its nodes, and the way to the graph. */
function CustomRouting({ track }: { track: Track }) {
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const graphShown = useUi((s) => s.centre === 'graph');
  const groups = inspectorGroups(track, types, (node) => infoOf(track.id, node, types, statuses));
  const section = (label: string, nodes: GraphNode[]) =>
    nodes.length > 0 ? (
      <>
        <div className="inspector-label">{label}</div>
        {nodes.map((n) => <CustomRoutingNode key={n.id} track={track} node={n} />)}
      </>
    ) : null;

  return (
    <div className="inspector custom-routing">
      <div className="inspector-label" title="This track's graph is not a simple chain, so it is rewired in the graph view">
        Custom routing
      </div>
      {section(groups.instruments.length === 1 ? 'Instrument' : 'Instruments', groups.instruments)}
      {groups.instruments.length === 1 && <InstrumentSection track={track} node={groups.instruments[0]} />}
      {section('Inserts', groups.effects)}
      {section('Modulation', groups.modulation)}
      {!graphShown && (
        <Button onClick={() => ui.setCentre('graph')}>
          <Workflow size={13} /> Open graph
        </Button>
      )}
    </div>
  );
}

/** A node of a custom graph, or a modulation source: a click shows its panel in the dock, as a chain slot does, and it can be bypassed. */
function CustomRoutingNode({ track, node }: { track: Track; node: GraphNode }) {
  const types = useDawg((s) => s.nodeTypes);
  const status = useDawg((s) => s.nodes[`${track.id}:${node.id}`]);
  const selected = useUi((s) => s.selectedNode?.trackId === track.id && s.selectedNode.nodeId === node.id);
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);

  return (
    <div
      className={`slot ${status?.editorOpen ? 'open' : ''} ${selected ? 'selected' : ''} ${node.bypassed ? 'bypassed' : ''}`}
      onContextMenu={(e) => {
        e.preventDefault();
        setMenu({ x: e.clientX, y: e.clientY });
      }}
    >
      <button className="slot-name" title={node.plugin ? `${node.plugin.manufacturer} · ${node.plugin.format}` : undefined} onClick={() => openDevice(track, node)}>
        {nodeName(node, types)}
      </button>
      <CodeButton track={track} node={node} />
      <Power
        name={nodeName(node, types)}
        on={!node.bypassed}
        onChange={(on) => dispatch({ command: 'setNodeBypass', args: { trackId: track.id, nodeId: node.id, bypassed: !on } })}
      />
      {isRemovable(track, node) && (
        <IconButton title="Remove" onClick={() => removeDevice(track, node)}>
          <X size={12} />
        </IconButton>
      )}
      {menu && <DeviceMenu track={track} node={node} x={menu.x} y={menu.y} onClose={() => setMenu(null)} />}
    </div>
  );
}

/** A Faust node's way to its script: the code editor opens only from a Faust node (phase11-faust-panels.md, section 5). */
function CodeButton({ track, node }: { track: Track; node: GraphNode }) {
  if (node.type !== NODE_TYPES.faust) return null;
  return (
    <IconButton title="Edit the script" onClick={() => ui.editCode(track.id, node.id)}>
      <Code size={12} />
    </IconButton>
  );
}

export function TrackInspector({ track }: { track: Track }) {
  const [picker, setPicker] = useState<Picker | null>(null);
  const [menu, setMenu] = useState<Menu | null>(null);
  const [dragId, setDragId] = useState<number | null>(null);
  const [dropAt, setDropAt] = useState<number | null>(null);
  const chain = trackChain(track);
  const modulation = Object.values(track.graph.nodes).filter(isModulationSource);
  // A track with output tracks is only their header: effects go on its output tracks (multi-output-instruments.md, MO12).
  const header = useDawg((s) => isOutputsHeader(s.project, track.id));

  if (!chain) return <CustomRouting track={track} />;

  const openPicker = (e: React.MouseEvent, kind: Picker['kind'], extra: Partial<Picker> = {}) => {
    const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
    setPicker({ kind, x: rect.left, y: rect.bottom + 4, ...extra });
  };

  const dropEffect = (before: number) => {
    const nodeId = dragId;
    setDragId(null);
    setDropAt(null);
    if (nodeId === null) return;
    const from = chain.effects.findIndex((e) => e.id === nodeId);
    if (from < 0) return;
    const to = reorderIndex(from, before);
    if (to !== from) dispatch({ command: 'reorderNode', args: { trackId: track.id, nodeId, index: to } });
  };

  // Dropping on the upper half of a slot puts the dragged effect before it, the lower half after it.
  const dragOf = (effect: GraphNode, i: number): SlotDrag => ({
    dragging: dragId === effect.id,
    dropBefore: dropAt === i,
    onStart: (e) => {
      e.dataTransfer.effectAllowed = 'move';
      e.dataTransfer.setData('text/plain', String(effect.id));
      setDragId(effect.id);
      setDropAt(null);
    },
    onOver: (e) => {
      if (dragId === null) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = 'move';
      const rect = e.currentTarget.getBoundingClientRect();
      setDropAt(e.clientY < rect.top + rect.height / 2 ? i : i + 1);
    },
    onDrop: (e) => {
      e.preventDefault();
      dropEffect(dropAt ?? i);
    },
    onEnd: () => {
      setDragId(null);
      setDropAt(null);
    },
  });

  const pick = (picked: PickedNode) => {
    const p = picker;
    setPicker(null);
    if (!p) return;
    void pickedComponent(picked).then((component) => insertPicked(picked, p, component)).catch(reportError);
  };

  const insertPicked = (picked: PickedNode, p: NonNullable<typeof picker>, component?: ComponentGraph) =>
    dispatch({
      command: 'insertNode',
      args: { trackId: track.id, type: picked.type, plugin: picked.plugin, code: picked.code, component, index: p.index },
    });

  return (
    <div className="inspector" onClick={(e) => e.stopPropagation()}>
      {track.kind === 'instrument' && (
        <>
      <div className="inspector-label">Instrument</div>
      {chain.instrument ? (
        <>
          <Slot
            track={track}
            node={chain.instrument}
            onMenu={(e) => setMenu({ node: chain.instrument!, x: e.clientX, y: e.clientY })}
          />
          <InstrumentSection track={track} node={chain.instrument} />
        </>
      ) : (
        <button className="slot empty-slot" onClick={(e) => openPicker(e, 'instrument')}>
          <Plus size={12} /> Choose instrument
        </button>
      )}

        </>
      )}

      <InspectorMacros track={track} />

      <div className="inspector-label">Inserts</div>
      {chain.effects.map((effect, i) => (
        <Slot
          key={effect.id}
          track={track}
          node={effect}
          index={i + 1}
          drag={dragOf(effect, i)}
          onMenu={(e) => setMenu({ node: effect, x: e.clientX, y: e.clientY })}
        />
      ))}
      {header ? (
        <p className="inspector-note">This track only feeds its output tracks: add effects to those.</p>
      ) : (
        <button
          className={`slot empty-slot ${dropAt === chain.effects.length ? 'drop-before' : ''}`}
          onClick={(e) => openPicker(e, 'effect')}
          onDragOver={(e) => {
            if (dragId === null) return;
            e.preventDefault();
            e.dataTransfer.dropEffect = 'move';
            setDropAt(chain.effects.length);
          }}
          onDrop={(e) => {
            e.preventDefault();
            dropEffect(chain.effects.length);
          }}
        >
          <Plus size={12} /> Add effect
        </button>
      )}

      {/* Modulation sources sit outside the chain (docs/design/modulation.md, M7); they are added in the graph. */}
      {modulation.length > 0 && (
        <>
          <div className="inspector-label">Modulation</div>
          {modulation.map((n) => (
            <CustomRoutingNode key={n.id} track={track} node={n} />
          ))}
        </>
      )}

      {picker && (
        <PluginPicker kind={picker.kind} x={picker.x} y={picker.y} onPick={pick} onClose={() => setPicker(null)} />
      )}

      {menu && <DeviceMenu track={track} node={menu.node} x={menu.x} y={menu.y} onClose={() => setMenu(null)} />}
    </div>
  );
}
