import { create } from 'zustand';
import type { ParamAddress } from '../bridge/protocol';
import type { SourceRef } from './macros';

// What the modulation column is doing (docs/design/macros-and-modulation-column.md, MC9, MC12, MC13): which source's
// editor is open on each track, the source being pointed at, and Map mode. Per window, not in the project or undo.

interface ModulationUi {
  /** The source whose editor the column shows, by track. */
  open: Record<number, number>;
  /** The chip under the pointer: the parameters it drives are marked. */
  hovered: SourceRef | null;
  /** Map mode: the macro knob that the next parameters moved are mapped to. */
  mapping: { trackId: number; knob: number } | null;
  /** Link mode (docs/design/parameter-linking.md, L15): the parameter the next one moved will follow. */
  linking: ParamAddress | null;
  /** The parameters marked while a link's mark is under the pointer (L16). */
  marked: ParamAddress[];
}

export const useModulationUi = create<ModulationUi>()(() => ({ open: {}, hovered: null, mapping: null, linking: null, marked: [] }));

// Escape ends Link mode wherever the pointer is.
const endLinkingOnEscape = (event: KeyboardEvent) => {
  if (event.key === 'Escape') modulationUi.stopLinking();
};

export const modulationUi = {
  openSource: (trackId: number, nodeId: number | null) =>
    useModulationUi.setState((s) => {
      const open = { ...s.open };
      if (nodeId === null) delete open[trackId];
      else open[trackId] = nodeId;
      return { open };
    }),
  hover: (hovered: SourceRef | null) => useModulationUi.setState({ hovered }),
  /** Starts Map mode on a knob, or ends it when it's already on that knob. */
  toggleMapping: (trackId: number, knob: number) =>
    useModulationUi.setState((s) => ({
      mapping: s.mapping?.trackId === trackId && s.mapping.knob === knob ? null : { trackId, knob },
    })),
  stopMapping: () => useModulationUi.setState({ mapping: null }),
  startLinking: (source: ParamAddress) => {
    useModulationUi.setState({ linking: source, mapping: null });
    window.addEventListener('keydown', endLinkingOnEscape);
  },
  stopLinking: () => {
    useModulationUi.setState({ linking: null });
    window.removeEventListener('keydown', endLinkingOnEscape);
  },
  mark: (marked: ParamAddress[]) => useModulationUi.setState({ marked }),
};

/** The drag type of a modulation source dragged from the column onto a knob (MC13). */
export const SOURCE_DRAG_TYPE = 'application/x-dawg-modulation-source';

/** What a dragged source carries. */
export interface DraggedSource extends SourceRef {
  /** For a cable source: the output port to connect. */
  port: number;
}

/** The source being dragged, for drop targets that only see the drag's types while it moves. */
let dragged: DraggedSource | null = null;

export const sourceDrag = {
  start: (event: React.DragEvent, source: DraggedSource) => {
    dragged = source;
    event.dataTransfer.effectAllowed = 'link';
    event.dataTransfer.setData(SOURCE_DRAG_TYPE, JSON.stringify(source));
  },
  end: () => {
    dragged = null;
  },
  current: (event: React.DragEvent): DraggedSource | null => (event.dataTransfer.types.includes(SOURCE_DRAG_TYPE) ? dragged : null),
};
