import type { GraphNode, Track } from '../bridge/protocol';
import { dispatch } from '../store/store';
import { ui } from '../store/uiStore';

/** Turns an MPE Synth into a Voice Instrument with the default voice graph, one undo step, and shows the voice graph
    (docs/design/phase12-voice-scope.md, V4). */
export function convertToVoiceGraph(track: Track, node: GraphNode) {
  dispatch({ command: 'convertToVoiceGraph', args: { trackId: track.id, nodeId: node.id } });
  ui.enterGraph(track.id, node.id);
}
