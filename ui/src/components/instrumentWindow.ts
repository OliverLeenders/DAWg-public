import { NODE_TYPES, type GraphNode, type Track } from '../bridge/protocol';
import * as bridge from '../bridge/bridge';
import { actions, reportError, useDawg } from '../store/store';

// An instrument's full editor opens in a window of its own (docs/design/macros-and-modulation-column.md, MC14): the
// instrument window for a built-in instrument, a Voice Instrument or a component; a plugin's own window for a plugin.

export function openInstrument(track: Track, node: GraphNode) {
  if (node.type === NODE_TYPES.pluginInstrument) {
    const status = useDawg.getState().nodes[`${track.id}:${node.id}`];
    if (status?.editorOpen) actions.closeEditor(track.id, node.id);
    else actions.openEditor(track.id, node.id);
    return;
  }
  bridge.call('instrumentEditor.show', { open: true, trackId: track.id, nodeId: node.id }).catch(reportError);
}
