import { AppWindow, ArrowLeft, ArrowRight, Code, ExternalLink, FoldHorizontal, Layers, Link2, Power, Replace, RotateCw, Split, Trash2, Workflow, type LucideIcon } from 'lucide-react';
import { Fragment, useState } from 'react';
import { NODE_TYPES, type ComponentGraph, type GraphNode, type Track } from '../bridge/protocol';
import { infoOf } from '../graph/nodeInfo';
import { linkableNodes, linkNodes } from '../dock/links';
import { PLUGIN_CHANNELS, pluginChannels } from '../graph/widths';
import { actions, dispatch, reportError, useDawg } from '../store/store';
import { ui } from '../store/uiStore';
import { isInstrument, isMidiEffect, nodeName, pluginEditorAction, trackChain } from './chain';
import { PluginPicker, pickedComponent, type PickedNode } from './PluginPicker';
import { convertToVoiceGraph } from './voiceGraph';
import { openInstrument } from './instrumentWindow';
import { MenuItem } from './MenuItem';
import { Menu } from '../ui/overlays';

// The one context menu for a device, wherever it's shown: an inspector slot, a device strip card, a mixer insert or a
// graph node. Each offers what applies to the device, so the same actions are in every place.

const BEND_RANGES = [2, 12, 24, 48, 96];

/** Removes a device. A device in a simple chain leaves its neighbours joined up (the engine's removeNode). */
export function removeDevice(track: Track, node: GraphNode) {
  dispatch({ command: 'removeNode', args: { trackId: track.id, nodeId: node.id } });
  ui.selectNode(null);
}

/** Whether a device can be bypassed and removed: not a track's output, nor a voice graph's input or output. */
export function isRemovable(track: Track, node: GraphNode) {
  const { nodeTypes, nodes } = useDawg.getState();
  return infoOf(track.id, node, nodeTypes, nodes)?.userEditable ?? true;
}

export function DeviceMenu({
  track,
  node,
  x,
  y,
  onClose,
  onCollapseOthers,
}: {
  track: Track;
  node: GraphNode;
  x: number;
  y: number;
  onClose: () => void;
  /** The device strip's "Collapse all except selected" (macros-and-modulation-column.md, MC15). */
  onCollapseOthers?: () => void;
}) {
  const types = useDawg((s) => s.nodeTypes);
  const status = useDawg((s) => s.nodes[`${track.id}:${node.id}`]);
  const options = useDawg((s) => s.pluginOptions);
  const [replacing, setReplacing] = useState(false);
  // Replacing and moving apply to the track's own chain, where the order is the signal's route.
  const chain = track.graph.nodes[String(node.id)] ? trackChain(track) : null;
  // A MIDI effect moves and is replaced among the MIDI effects, an effect among the effects.
  const midi = isMidiEffect(node);
  const list = chain ? (midi ? chain.midiEffects : chain.effects) : [];
  const effectIndex = list.findIndex((e) => e.id === node.id);
  const inChain = chain !== null && (chain.instrument?.id === node.id || effectIndex >= 0);
  const removable = isRemovable(track, node);
  const editor = pluginEditorAction(node, status);
  const project = useDawg((s) => s.project);
  // Mirroring this device on another of its kind (docs/design/parameter-linking.md, L5, L15): the other follows it.
  const mirrors = linkableNodes(project, node);
  const params = infoOf(track.id, node, types, useDawg.getState().nodes)?.params ?? [];
  const hasLevels = params.some((p) => p.unit === 'dB' && p.kind === 'continuous' && p.min < 0 && Math.abs(p.min + p.max) < 1e-6);

  if (replacing) {
    const pick = (picked: PickedNode) => {
      onClose();
      void pickedComponent(picked)
        .then((component) => replace(track, node, picked, effectIndex, component))
        .catch(reportError);
    };
    return <PluginPicker kind={isInstrument(node) ? 'instrument' : midi ? 'midiEffect' : 'effect'} x={x} y={y} onPick={pick} onClose={onClose} />;
  }

  const item = (icon: LucideIcon, label: string, run: () => void, danger = false) => (
    <MenuItem icon={icon} label={label} onClick={run} danger={danger} />
  );

  return (
    <>
      <Menu
        onClose={onClose}
        at={{ x: Math.min(x, window.innerWidth - 220), y: Math.min(y, window.innerHeight - 360) }}
        className="device-menu"
        onClick={onClose}
        onContextMenu={(e) => e.preventDefault()}
      >
        <div className="menu-heading">{nodeName(node, types)}</div>
        {editor && item(ExternalLink, editor === 'open' ? 'Open plugin window' : 'Close plugin window', () => (editor === 'open' ? actions.openEditor(track.id, node.id) : actions.closeEditor(track.id, node.id)))}
        {node.plugin && status?.status === 'crashed' && item(RotateCw, 'Reload plugin', () => actions.reloadPlugin(track.id, node.id))}
        {isInstrument(node) && !node.plugin && track.graph.nodes[String(node.id)] && item(AppWindow, 'Open in its window', () => openInstrument(track, node))}
        {(node.type === NODE_TYPES.voiceInstrument || node.component) && item(Layers, node.component ? 'Open component' : 'Edit voice graph', () => ui.enterGraph(track.id, node.id))}
        {node.type === NODE_TYPES.faust && item(Code, 'Edit script', () => ui.editCode(track.id, node.id))}
        {(node.type === NODE_TYPES.mpeSynth || node.type === NODE_TYPES.sampler) && track.graph.nodes[String(node.id)] && item(Workflow, 'Convert to voice graph', () => convertToVoiceGraph(track, node))}
        {node.plugin && options.available && options.outOfProcess && (
          <>
            <div className="menu-separator" />
            <MenuItem
              checked={!status?.separateProcess}
              label="Run inside DAWg"
              onClick={() => actions.setPluginRunsInside(node.plugin!.identifier, status?.separateProcess === true)}
            />
          </>
        )}
        {node.type === NODE_TYPES.pluginInstrument && (status?.outputs?.length ?? 0) > 1 && (
          <>
            <div className="menu-separator" />
            <MenuItem
              checked={node.allOutputs === true}
              label="Use all outputs"
              onClick={() => dispatch({ command: 'setPluginOutputs', args: { trackId: track.id, nodeId: node.id, all: !node.allOutputs } })}
            />
            {track.graph.nodes[String(node.id)] && item(Split, 'Each output to its own track', () => splitPluginOutputs(track, node, status?.outputs ?? []))}
          </>
        )}
        {node.type === NODE_TYPES.pluginEffect && (
          <>
            <div className="menu-separator" />
            <div className="menu-heading">Channels</div>
            {PLUGIN_CHANNELS.map((option) => (
              <MenuItem
                key={option.value}
                checked={pluginChannels(node) === option.value}
                label={option.label}
                onClick={() => dispatch({ command: 'setPluginChannels', args: { trackId: track.id, nodeId: node.id, channels: option.value } })}
              />
            ))}
          </>
        )}
        {node.type === NODE_TYPES.pluginInstrument && node.midi && !status?.nativeNotes && (
          <>
            <div className="menu-separator" />
            <MenuItem checked={node.midi.mpe} label="MPE (one channel per note)" onClick={() => setMidi(track, node, { mpe: true })} />
            <MenuItem checked={!node.midi.mpe} label="Plain MIDI (channel 1)" onClick={() => setMidi(track, node, { mpe: false })} />
            <div className="menu-heading">Pitch-bend range</div>
            {BEND_RANGES.map((range) => (
              <MenuItem key={range} checked={node.midi?.bendRange === range} label={`±${range} semitones`} onClick={() => setMidi(track, node, { bendRange: range })} />
            ))}
          </>
        )}
        {mirrors.length > 0 && (
          <>
            <div className="menu-separator" />
            <div className="menu-heading">Make another follow this one</div>
            {mirrors.slice(0, 8).map(({ track: other, node: follower }) => {
              const name = `${other.name} · ${nodeName(follower, types)}`;
              const to = { trackId: other.id, nodeId: follower.id };
              const from = { trackId: track.id, nodeId: node.id };
              return (
                <Fragment key={`${other.id}:${follower.id}`}>
                  {item(Link2, `Mirror on ${name}`, () => linkNodes(from, to, 'mirror'))}
                  {hasLevels && item(Link2, `Mirror on ${name}, gains inverted`, () => linkNodes(from, to, 'mirrorInverted'))}
                </Fragment>
              );
            })}
          </>
        )}
        {onCollapseOthers && item(FoldHorizontal, 'Collapse all except selected', onCollapseOthers)}
        <div className="menu-separator" />
        {inChain && (
          <MenuItem
            icon={Replace}
            label="Replace…"
            onClick={(e) => {
              e.stopPropagation();
              setReplacing(true);
            }}
          />
        )}
        {effectIndex > 0 && item(ArrowLeft, 'Move earlier in the chain', () => reorder(track, node, effectIndex - 1))}
        {chain && effectIndex >= 0 && effectIndex < list.length - 1 && item(ArrowRight, 'Move later in the chain', () => reorder(track, node, effectIndex + 1))}
        {removable &&
          item(Power, node.bypassed ? 'Enable' : 'Bypass', () =>
            dispatch({ command: 'setNodeBypass', args: { trackId: track.id, nodeId: node.id, bypassed: !node.bypassed } }),
          )}
        {removable && <MenuItem icon={Trash2} label="Remove" shortcut="Del" danger onClick={() => removeDevice(track, node)} />}
      </Menu>
    </>
  );
}

/** An output track for each of a plugin's buses after the main one, named after the bus, and every bus on
    (docs/design/multi-output-instruments.md, MO5). Buses already wired keep their cables. */
function splitPluginOutputs(track: Track, node: GraphNode, buses: Array<{ name: string }>) {
  const outputs = buses.slice(1).map((bus, i) => ({ port: i + 1, name: bus.name || `${track.name} ${i + 2}` }));
  dispatch({ command: 'splitPluginOutputs', args: { trackId: track.id, nodeId: node.id, outputs } });
}

function setMidi(track: Track, node: GraphNode, settings: { mpe?: boolean; bendRange?: number }) {
  dispatch({ command: 'setPluginMidi', args: { trackId: track.id, nodeId: node.id, ...settings } });
}

function reorder(track: Track, node: GraphNode, index: number) {
  dispatch({ command: 'reorderNode', args: { trackId: track.id, nodeId: node.id, index } });
}

/** Puts the picked device in the replaced one's place, as one undo step. An instrument takes the instrument slot. */
function replace(track: Track, node: GraphNode, picked: PickedNode, effectIndex: number, component?: ComponentGraph) {
  const insert = {
    command: 'insertNode' as const,
    args: { trackId: track.id, type: picked.type, plugin: picked.plugin, code: picked.code, component, index: effectIndex >= 0 ? effectIndex : undefined },
  };
  if (isInstrument(node)) dispatch(insert);
  else
    dispatch({
      command: 'batch',
      args: { label: 'Replace device', commands: [insert, { command: 'removeNode', args: { trackId: track.id, nodeId: node.id } }] },
    });
}
