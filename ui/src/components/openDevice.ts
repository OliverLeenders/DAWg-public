import { NODE_TYPES, type GraphNode, type Track } from '../bridge/protocol';
import { actions, useDawg } from '../store/store';
import { layout, useLayout } from '../store/layoutStore';
import { ui } from '../store/uiStore';
import { pluginEditorAction } from './chain';
import { openInstrument } from './instrumentWindow';

/** Clicking a device in the inspector, on a simple chain or a custom graph alike: its panel shows in the device dock
    (which opens if hidden), and a plugin's own editor opens, or closes if it is open. A Voice Instrument opens its voice graph
    instead, which is where its sound is made. */
export function openDevice(track: Track, node: GraphNode) {
  if (node.type === NODE_TYPES.voiceInstrument) {
    ui.enterGraph(track.id, node.id);
    return;
  }

  ui.selectNode({ trackId: track.id, nodeId: node.id });
  // A built-in instrument's editor is in its own window now (docs/design/macros-and-modulation-column.md, MC14).
  if ((node.type === NODE_TYPES.mpeSynth || node.type === NODE_TYPES.sampler) && track.graph.nodes[String(node.id)]) {
    openInstrument(track, node);
    return;
  }
  if (!useLayout.getState().dockVisible) layout.toggleDock();
  const action = pluginEditorAction(node, useDawg.getState().nodes[`${track.id}:${node.id}`]);
  if (action === 'open') actions.openEditor(track.id, node.id);
  else if (action === 'close') actions.closeEditor(track.id, node.id);
}
