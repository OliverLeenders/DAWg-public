import { NODE_TYPES, type Command, type GraphNode, type PluginNodeStatus, type Project, type Track } from '../bridge/protocol';
import { nodeName } from '../components/chain';
import { dispatch, useDawg } from '../store/store';

// Choosing a keyed node's key source (docs/design/sidechain.md, K8): Internal, or another track, whose key send feeds
// the node's key input (K2). The same send shows in the source track's Sends list.

/** Whether a node has a Detector parameter (Internal or Sidechain) that a key source switches: the dynamics
    processors (sidechain.md, K6; docs/design/gate-expander.md, N4). */
export function hasKeyDetector(type: string) {
  return type === NODE_TYPES.compressor || type === NODE_TYPES.gate || type === NODE_TYPES.expander;
}

/** Whether a node can be keyed: a dynamics processor, an effect plugin with a sidechain bus, or a modulation source's
    audio trigger (K1, K7; docs/design/modulation.md, M3-M4). */
export function isKeyable(node: GraphNode, status?: PluginNodeStatus) {
  return (
    hasKeyDetector(node.type) ||
    node.type === NODE_TYPES.lfo ||
    node.type === NODE_TYPES.adsr ||
    (node.type === NODE_TYPES.pluginEffect && !!status?.hasSidechain)
  );
}

/** The key sends to a node: from which tracks, with which send. */
export function keySendsTo(project: Project, track: Track, node: GraphNode) {
  return project.trackOrder
    .map((id) => project.tracks[String(id)])
    .filter((t): t is Track => !!t)
    .flatMap((source) =>
      Object.values(source.sends ?? {})
        .filter((send) => send.target === track.id && send.targetNode === node.id)
        .map((send) => ({ source, send })),
    );
}

/** The keyable nodes on tracks other than `track`, for a source track's Add send menu. */
export function keyableNodes(project: Project, track: Track, statuses: Record<string, PluginNodeStatus>) {
  return project.trackOrder
    .map((id) => project.tracks[String(id)])
    .filter((t): t is Track => !!t && t.id !== track.id)
    .flatMap((target) =>
      Object.values(target.graph.nodes)
        .filter((node) => isKeyable(node, statuses[`${target.id}:${node.id}`]))
        .map((node) => ({ track: target, node })),
    );
}

export function KeySelector({ track, node, label = 'Sidechain' }: { track: Track; node: GraphNode; label?: string }) {
  const project = useDawg((s) => s.project);
  const types = useDawg((s) => s.nodeTypes);
  if (!project) return null;

  const sends = keySendsTo(project, track, node);
  const sources = project.trackOrder
    .map((id) => project.tracks[String(id)])
    .filter((t): t is Track => !!t && t.id !== track.id);
  const detects = hasKeyDetector(node.type);
  const keyed = detects ? (node.params.detector ?? 0) > 0.5 : sends.length > 0;
  const value = !keyed || sends.length === 0 ? (keyed ? 'none' : 'internal') : sends.length === 1 ? String(sends[0].source.id) : 'several';

  const choose = (choice: string) => {
    // One undo step: drop the current keys, add the new one, and switch a dynamics processor's detector.
    const commands: Command[] = sends.map(({ source, send }) => ({ command: 'removeSend', args: { trackId: source.id, sendId: send.id } }));
    if (choice !== 'internal')
      commands.push({ command: 'addSend', args: { trackId: Number(choice), target: track.id, targetNode: node.id } });
    if (detects)
      commands.push({
        command: 'setNodeParam',
        args: { trackId: track.id, nodeId: node.id, param: 'detector', value: choice === 'internal' ? 0 : 1 },
      });
    dispatch({ command: 'batch', args: { label: choice === 'internal' ? 'Sidechain: internal' : 'Choose sidechain', commands } });
  };

  return (
    <label className="field key-selector" title={`What ${nodeName(node, types)} reacts to: its own input, or another track's sidechain send`}>
      <span>{label}</span>
      <select value={value} onChange={(e) => choose(e.target.value)}>
        <option value="internal">{detects ? 'Internal' : 'None'}</option>
        {value === 'none' && <option value="none">No source</option>}
        {value === 'several' && <option value="several">{sends.map((s) => s.source.name).join(' + ')}</option>}
        {sources.map((source) => (
          <option key={source.id} value={String(source.id)}>
            {source.name}
          </option>
        ))}
      </select>
    </label>
  );
}
