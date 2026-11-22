import { Plus } from 'lucide-react';
import { Power } from '../ui/controls';
import { useState } from 'react';
import type { GraphNode, Track } from '../bridge/protocol';
import { nodeName, reorderIndex, trackChain } from '../components/chain';
import { DeviceMenu } from '../components/DeviceMenu';
import { PluginPicker, pickedComponent, type PickedNode } from '../components/PluginPicker';
import { actions, dispatch, reportError, useDawg } from '../store/store';
import { ui, useUi } from '../store/uiStore';

// A strip's inserts (docs/design/mixer.md, X3): one slot per node of the track's chain, with a power switch to bypass it and
// its name. Clicking shows the node in the main window's dock (and opens a plugin's editor), dragging reorders,
// right-clicking opens the device menu every view shares, and "+" adds one. Narrow strips show the power switches only, names in tooltips (X5).

type Picker = { x: number; y: number };
type Menu = { node: GraphNode; x: number; y: number };

/** The insert being dragged: reordering stays within its track. */
let dragged: { trackId: number; nodeId: number } | null = null;

export function MixerInserts({ track, narrow }: { track: Track; narrow: boolean }) {
  const [picker, setPicker] = useState<Picker | null>(null);
  const [menu, setMenu] = useState<Menu | null>(null);
  const [dropAt, setDropAt] = useState<number | null>(null);
  const chain = trackChain(track);

  if (!chain)
    return (
      <div className="strip-section strip-inserts">
        <span className="strip-custom" title="This track's graph is not a simple chain: edit it in the graph view">
          Custom routing
        </span>
      </div>
    );

  const pick = (picked: PickedNode) => {
    const p = picker;
    setPicker(null);
    if (!p) return;
    void pickedComponent(picked)
      .then((component) =>
        dispatch({ command: 'insertNode', args: { trackId: track.id, type: picked.type, plugin: picked.plugin, code: picked.code, component } }),
      )
      .catch(reportError);
  };

  const drop = (index: number) => {
    const d = dragged;
    dragged = null;
    setDropAt(null);
    if (!d || d.trackId !== track.id) return;
    const from = chain.effects.findIndex((n) => n.id === d.nodeId);
    const to = reorderIndex(from, index);
    if (from >= 0 && to !== from) dispatch({ command: 'reorderNode', args: { trackId: track.id, nodeId: d.nodeId, index: to } });
  };

  return (
    <div className="strip-section strip-inserts">
      {chain.instrument && (
        <InsertSlot
          track={track}
          node={chain.instrument}
          narrow={narrow}
          instrument
          onMenu={(e) => setMenu({ node: chain.instrument!, x: e.clientX, y: e.clientY })}
        />
      )}
      {chain.effects.map((node, index) => (
        <InsertSlot
          key={node.id}
          track={track}
          node={node}
          narrow={narrow}
          dropBefore={dropAt === index}
          onMenu={(e) => setMenu({ node, x: e.clientX, y: e.clientY })}
          onDragStart={() => (dragged = { trackId: track.id, nodeId: node.id })}
          onDragOver={(e) => {
            if (dragged?.trackId !== track.id) return;
            e.preventDefault();
            const rect = e.currentTarget.getBoundingClientRect();
            setDropAt(e.clientY < rect.top + rect.height / 2 ? index : index + 1);
          }}
          onDrop={() => dropAt !== null && drop(dropAt)}
        />
      ))}
      <button
        className={`insert-add ${dropAt === chain.effects.length ? 'drop-before' : ''}`}
        title="Add an insert"
        onClick={(e) => {
          const rect = e.currentTarget.getBoundingClientRect();
          setPicker({ x: rect.left, y: rect.bottom + 4 });
        }}
        onDragOver={(e) => {
          if (dragged?.trackId !== track.id) return;
          e.preventDefault();
          setDropAt(chain.effects.length);
        }}
        onDragLeave={() => setDropAt(null)}
        onDrop={() => drop(chain.effects.length)}
      >
        <Plus size={11} />
        {!narrow && ' Insert'}
      </button>

      {picker && <PluginPicker kind="effect" x={picker.x} y={picker.y} onPick={pick} onClose={() => setPicker(null)} />}

      {menu && <DeviceMenu track={track} node={menu.node} x={menu.x} y={menu.y} onClose={() => setMenu(null)} />}
    </div>
  );
}

interface SlotProps {
  track: Track;
  node: GraphNode;
  narrow: boolean;
  instrument?: boolean;
  dropBefore?: boolean;
  onMenu?: (e: React.MouseEvent) => void;
  onDragStart?: () => void;
  onDragOver?: (e: React.DragEvent<HTMLDivElement>) => void;
  onDrop?: () => void;
}

function InsertSlot({ track, node, narrow, instrument = false, dropBefore = false, onMenu, onDragStart, onDragOver, onDrop }: SlotProps) {
  const types = useDawg((s) => s.nodeTypes);
  const status = useDawg((s) => s.nodes[`${track.id}:${node.id}`]);
  const selected = useUi((s) => s.selectedNode?.trackId === track.id && s.selectedNode.nodeId === node.id);
  const name = nodeName(node, types);
  const failed = node.plugin !== undefined && status?.status === 'failed';
  const show = () => {
    ui.selectNode({ trackId: track.id, nodeId: node.id });
    if (node.plugin && status?.status === 'ready') status.editorOpen ? actions.closeEditor(track.id, node.id) : actions.openEditor(track.id, node.id);
  };

  return (
    <div
      className={`insert-slot ${node.bypassed ? 'bypassed' : ''} ${selected ? 'selected' : ''} ${failed ? 'failed' : ''} ${instrument ? 'instrument' : ''} ${dropBefore ? 'drop-before' : ''}`}
      title={failed ? `${name}: ${status.error}` : narrow ? name : undefined}
      draggable={!instrument}
      onDragStart={(e) => {
        e.dataTransfer.effectAllowed = 'move';
        onDragStart?.();
      }}
      onDragOver={onDragOver}
      onDrop={onDrop}
      onContextMenu={(e) => {
        e.preventDefault();
        onMenu?.(e);
      }}
      onClick={narrow ? show : undefined}
    >
      {/* The same power switch as the dock's and the inspector's (docs/ui-review-2026-11-10.md, 3.9). */}
      <Power
        size="s"
        className="insert-power"
        name={name}
        on={!node.bypassed}
        onChange={(on) => dispatch({ command: 'setNodeBypass', args: { trackId: track.id, nodeId: node.id, bypassed: !on } })}
      />
      {!narrow && (
        <button className="insert-name" onClick={show} title={`${name}: show in the dock`}>
          {name}
        </button>
      )}
    </div>
  );
}
