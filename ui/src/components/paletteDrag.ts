import { NODE_TYPES, type NodeTypeInfo, type Project, type SavedComponent, type Track } from '../bridge/protocol';
import { isOutputsHeader } from '../arrangement/groups';
import { dispatch, reportError, useDawg } from '../store/store';
import { builtinChainRole, trackChain } from './chain';
import { pickedComponent, type PickedNode } from './PluginPicker';

// Devices dragged from the browser (the Plugins tab): onto the graph canvas, a track's chain in the device strip, a
// track in the arrangement, or empty arrangement space for a new track. A double-click on a row adds it to the
// selected track.

/** MIME type of node drags from the browser in the right side bar. */
export const NODE_DRAG_TYPE = 'application/x-dawg-node';

/** The browser row being dragged: a drag's data can't be read until the drop, and targets must know it earlier. */
let dragged: PickedNode | null = null;
export const setDraggedPaletteNode = (picked: PickedNode | null) => {
  dragged = picked;
};
export const draggedPaletteNode = () => dragged;

export const isPaletteDrag = (data: DataTransfer) => data.types.includes(NODE_DRAG_TYPE);

/** Where a browser item goes in a track's chain: the instrument slot, the inserts, or only the graph (a modulation
    source, a patch that needs wiring, a node with several audio inputs). */
export function chainRole(
  picked: PickedNode,
  types: Record<string, NodeTypeInfo> = useDawg.getState().nodeTypes,
  saved: SavedComponent[] = useDawg.getState().savedComponents,
): 'instrument' | 'midiEffect' | 'effect' | null {
  if (picked.type === NODE_TYPES.pluginInstrument) return 'instrument';
  if (picked.type === NODE_TYPES.pluginEffect || picked.type === NODE_TYPES.faust) return 'effect';
  if (picked.patch) return picked.type === NODE_TYPES.component ? 'effect' : null;
  if (picked.saved) {
    const component = saved.find((c) => c.file === picked.saved);
    const audio = (ports: { type: string }[] = []) => ports.length === 1 && ports[0].type === 'audio';
    return component && audio(component.inputs) && audio(component.outputs) ? 'effect' : null;
  }
  return builtinChainRole(types[picked.type]);
}

/** Why a browser item can't go into a track's chain, or null when it can. */
export function chainProblem(track: Track, picked: PickedNode, project: Project | null = useDawg.getState().project): string | null {
  const role = chainRole(picked);
  if (role === null) return 'Add this in the graph view: it needs wiring.';
  if (role === 'midiEffect' && track.kind !== 'instrument') return 'Only instrument tracks take MIDI effects.';
  if (role === 'instrument' && track.kind !== 'instrument') return `${track.kind === 'master' ? 'The master' : 'This'} track has no instrument.`;
  if (!trackChain(track)) return "This track's graph is not a simple chain: add it in the graph view.";
  // A track with output tracks is only their header (docs/design/multi-output-instruments.md, MO12).
  if (role === 'effect' && isOutputsHeader(project, track.id)) return 'This track only feeds its output tracks: add effects to those.';
  return null;
}

/** Adds a browser item to a track's chain: an instrument replaces the track's, an effect goes in at `index` (by
    default after the others). */
export function addToTrack(track: Track, picked: PickedNode, index?: number) {
  const problem = chainProblem(track, picked);
  if (problem) return reportError(problem);
  void pickedComponent(picked)
    .then((component) =>
      dispatch({
        command: 'insertNode',
        args: { trackId: track.id, type: picked.type, plugin: picked.plugin, code: picked.code, component, index },
      }),
    )
    .catch(reportError);
}

/** A new track holding a browser item: an instrument track for an instrument, an audio track for an effect. */
export function addAsNewTrack(picked: PickedNode) {
  const role = chainRole(picked);
  if (role === null) return reportError('Add this in the graph view: it needs wiring.');
  void pickedComponent(picked)
    .then((component) =>
      dispatch({
        command: 'addTrack',
        args: {
          ...(role === 'effect' ? { kind: 'audio' as const } : {}),
          device: { type: picked.type, plugin: picked.plugin, code: picked.code, component },
        },
      }),
    )
    .catch(reportError);
}
