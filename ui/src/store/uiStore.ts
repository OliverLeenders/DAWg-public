import { create } from 'zustand';
import * as bridge from '../bridge/bridge';
import type { LaneId } from '../bridge/protocol';
import { WINDOW, initialRollClip, initialRollTrack } from '../window';
import { BUILTIN_DRUM_MAPS, DEFAULT_DRUM_MAP, type DrumMap, type DrumPiece } from './drumMap';

// UI-only state: never sent to the engine, not undoable.

/** Drops notes without selected points; null when none is left. */
function normalisePoints(selection: PointSelection | null): PointSelection | null {
  if (!selection) return null;
  const notes = new Map([...selection.notes].filter(([, indices]) => indices.length > 0));
  return notes.size > 0 ? { lane: selection.lane, notes } : null;
}

/** The piano roll's fold: every row, the scale's rows, or the pitches in use (docs/design/scale-highlighting.md, SH4). */
export type RollFold = 'off' | 'scale' | 'used';

export type LaneTab = 'velocity' | 'pressure' | 'timbre' | 'pitch' | 'vibrato';

export interface SnapOption {
  label: string;
  /** Grid size in beats; 0 disables snapping. */
  beats: number;
}

export const SNAP_OPTIONS: SnapOption[] = [
  { label: '1/4', beats: 1 },
  { label: '1/8', beats: 1 / 2 },
  { label: '1/8T', beats: 1 / 3 },
  { label: '1/16', beats: 1 / 4 },
  { label: '1/16T', beats: 1 / 6 },
  { label: '1/32', beats: 1 / 8 },
  { label: 'Off', beats: 0 },
];

/** Selected breakpoints in one lane, per note (note id to point indices). A note whose points are selected is
    edited point by point (arrow keys, Delete), even when the note itself is selected too. */
export interface PointSelection {
  lane: LaneId;
  notes: ReadonlyMap<number, number[]>;
}

/** The selected points of one note's curve in a lane (empty if none, or if another lane holds the selection). */
export const pointsOf = (selection: PointSelection | null, noteId: number, lane: LaneId): number[] =>
  (selection?.lane === lane && selection.notes.get(noteId)) || [];

/** What the centre of the window shows (docs/design/phase6-arrangement.md, section 7). The graph replaces only the
    arrangement's timeline, beside the track headers; the code view opens only from a Faust node
    (docs/design/phase11-faust-panels.md, section 5). */
export type CentreView = 'arrangement' | 'graph' | 'code';

/** The views Tab cycles through and the top bar switches between. */
export const CENTRE_ORDER: CentreView[] = ['arrangement', 'graph'];

/** The view after `centre` in the Tab cycle; the code view goes on to the view it was opened from. */
export function nextCentre(centre: CentreView, codeReturn: CentreView): CentreView {
  const from = centre === 'code' ? codeReturn : centre;
  return centre === 'code' ? from : CENTRE_ORDER[(CENTRE_ORDER.indexOf(from) + 1) % CENTRE_ORDER.length];
}

interface UiState {
  centre: CentreView;
  /** The clip the piano roll edits; other clips of its track are shown as ghosts. Null: all clips editable. */
  editedClipId: number | null;
  selectedClips: ReadonlySet<number>;
  selectedTrackId: number | null;
  /** All selected tracks (Ctrl/Shift+click in the headers); selectedTrackId is the last one clicked, which the
      inspector shows (docs/design/groups-and-sends.md, R12). */
  selectedTracks: ReadonlySet<number>;
  selectedNotes: ReadonlySet<number>;
  selectedPoints: PointSelection | null;
  snap: SnapOption;
  /** Quantise swing, 0.5 (straight) to 0.75 (docs/design/midi-effects.md, ME18). */
  swing: number;
  /** Quantise strength, 0..1: how far Q moves notes towards the grid. */
  quantiseStrength: number;
  /** Which rows the piano roll folds to (docs/design/scale-highlighting.md, SH4). */
  rollFold: RollFold;
  /** Whether drawn and dragged notes snap to the project's scale while unfolded (SH5). */
  scaleSnap: boolean;
  follow: boolean;
  /** Whether the arrangement shows the tempo lane; null shows it when the song has tempo changes
      (docs/design/tempo-map.md, TM12). */
  tempoLane: boolean | null;
  lane: LaneTab;
  /** The clip controller the piano roll's lane area shows instead of an expression lane, or null
      (docs/design/phase10-automation.md, section 5). */
  controllerLane: number | null;
  /** Whether the export dialog is open (docs/design/phase9-export-midi-input.md, section 5). */
  exportOpen: boolean;
  /** Whether the humanise popover is open in the piano roll. */
  humaniseOpen: boolean;
  /** Active drum map id (e.g. 'gm', 'roland', 'ableton', or custom). */
  activeDrumMapId: string;
  /** User-customised drum maps. */
  customDrumMaps: Record<string, DrumMap>;
  /** The graph node whose parameters the inspector shows (docs/design/phase8-node-graph.md, section 5). */
  selectedNode: { trackId: number; nodeId: number } | null;
  /** The Faust node the code view edits (docs/design/phase11-faust-panels.md, section 5). */
  codeNode: { trackId: number; nodeId: number } | null;
  /** The view the code view was opened from, which closing it returns to. */
  codeReturn: CentreView;
  /** The Voice Instrument whose voice graph the graph view shows, or null for the track's graph
      (docs/design/phase12-voice-scope.md, section 6). */
  graphScope: { trackId: number; nodeId: number } | null;
}

export const useUi = create<UiState>()(() => ({
  centre: 'arrangement',
  editedClipId: initialRollClip,
  selectedClips: new Set<number>(),
  selectedTrackId: initialRollTrack,
  selectedTracks: new Set<number>(initialRollTrack === null ? [] : [initialRollTrack]),
  selectedNotes: new Set<number>(),
  selectedPoints: null,
  snap: SNAP_OPTIONS[3],
  swing: 0.5,
  quantiseStrength: 1,
  rollFold: 'off',
  scaleSnap: false,
  follow: true,
  tempoLane: null,
  lane: 'velocity',
  selectedNode: null,
  exportOpen: false,
  humaniseOpen: false,
  activeDrumMapId: 'gm',
  customDrumMaps: {},
  controllerLane: null,
  codeNode: null,
  codeReturn: 'arrangement',
  graphScope: null,
}));

export const ui = {
  setCentre: (centre: CentreView) => useUi.setState({ centre }),
  /** Cycles arrangement, piano roll and graph (Tab); in the code view, Tab closes it. */
  toggleCentre: () => useUi.setState((s) => ({ centre: nextCentre(s.centre, s.codeReturn) })),
  selectNode: (selectedNode: { trackId: number; nodeId: number } | null) =>
    useUi.setState((s) =>
      // Unchanged selections keep their object: the graph reports selection on every render it changes.
      s.selectedNode?.trackId === selectedNode?.trackId && s.selectedNode?.nodeId === selectedNode?.nodeId
        ? s
        : { selectedNode },
    ),
  /** Opens a Faust node's script in the code view (double-click in the graph, the Code button in the dock or the
      inspector). */
  editCode: (trackId: number, nodeId: number) =>
    useUi.setState((s) => ({
      centre: 'code',
      codeReturn: s.centre === 'code' ? s.codeReturn : s.centre,
      codeNode: { trackId, nodeId },
      selectedNode: { trackId, nodeId },
    })),
  /** Shows what a Voice Instrument or component holds in the graph view (double-click, or Edit voice graph). */
  enterGraph: (trackId: number, nodeId: number) =>
    useUi.setState({ centre: 'graph', graphScope: { trackId, nodeId }, selectedNode: null }),
  /** Leaves the graph shown for the one holding it, or with no `parent` the track's (the breadcrumb, and the exit door). */
  leaveGraph: (parent?: number) =>
    useUi.setState((s) => ({
      graphScope: parent !== undefined && s.graphScope ? { trackId: s.graphScope.trackId, nodeId: parent } : null,
      selectedNode: s.graphScope ? { trackId: s.graphScope.trackId, nodeId: s.graphScope.nodeId } : s.selectedNode,
    })),
  /** Closes the code view, back to the view it was opened from. */
  closeCode: () => useUi.setState((s) => (s.centre === 'code' ? { centre: s.codeReturn } : s)),
  setCodeNode: (codeNode: { trackId: number; nodeId: number } | null) =>
    useUi.setState({ codeNode, selectedNode: codeNode }),
  /** Opens a clip in the piano roll window (double-click in the arrangement, or a ghost note in the roll). */
  editClip: (trackId: number, clipId: number) => {
    useUi.setState((s) => ({
      editedClipId: clipId,
      selectedTrackId: trackId,
      selectedNotes: s.editedClipId === clipId ? s.selectedNotes : new Set(),
      selectedPoints: s.editedClipId === clipId ? s.selectedPoints : null,
    }));
    if (WINDOW === 'main') {
      bridge.call('editor.show', { open: true, track: trackId, clip: clipId }).catch(() => {});
    }
  },
  selectClips: (ids: Iterable<number>) => useUi.setState({ selectedClips: new Set(ids) }),
  selectTrack: (selectedTrackId: number | null) => {
    useUi.setState({
      selectedTrackId,
      selectedTracks: new Set(selectedTrackId === null ? [] : [selectedTrackId]),
      selectedNotes: new Set(),
      selectedPoints: null,
    });
    // The selected instrument track plays MIDI input when none is armed (phase 9, M4).
    if (bridge.isConnected()) bridge.call('track.select', { trackId: selectedTrackId ?? 0 }).catch(() => {});
  },
  /** Adds a track to the selection or takes it out (Ctrl+click). */
  toggleTrack: (trackId: number) => {
    const selected = new Set(useUi.getState().selectedTracks);
    if (selected.has(trackId)) selected.delete(trackId);
    else selected.add(trackId);
    const primary = selected.has(trackId) ? trackId : ([...selected].pop() ?? null);
    useUi.setState({ selectedTracks: selected, selectedTrackId: primary });
  },
  /** Selects tracks (a Shift+click range), showing `primary` in the inspector. */
  selectTracks: (trackIds: Iterable<number>, primary: number) =>
    useUi.setState({ selectedTracks: new Set(trackIds), selectedTrackId: primary }),
  /** Selects notes; selected points survive only in notes that stay selected (all of them with `keepPoints`, for
      Shift+click additions). */
  selectNotes: (ids: Iterable<number>, keepPoints = false) =>
    useUi.setState((s) => {
      const selectedNotes = new Set(ids);
      if (keepPoints || !s.selectedPoints) return { selectedNotes };
      const kept = [...s.selectedPoints.notes].filter(([id]) => selectedNotes.has(id));
      return { selectedNotes, selectedPoints: normalisePoints({ lane: s.selectedPoints.lane, notes: new Map(kept) }) };
    }),
  selectPoints: (selection: PointSelection | null) => useUi.setState({ selectedPoints: normalisePoints(selection) }),
  /** Sets the note and point selections together (box selection). */
  selectNotesAndPoints: (ids: Iterable<number>, points: PointSelection | null) =>
    useUi.setState({ selectedNotes: new Set(ids), selectedPoints: normalisePoints(points) }),
  setSnap: (snap: SnapOption) => useUi.setState({ snap }),
  setSwing: (swing: number) => useUi.setState({ swing }),
  setQuantiseStrength: (quantiseStrength: number) => useUi.setState({ quantiseStrength }),
  setRollFold: (rollFold: RollFold) => useUi.setState({ rollFold }),
  setScaleSnap: (scaleSnap: boolean) => useUi.setState({ scaleSnap }),
  setExportOpen: (exportOpen: boolean) => useUi.setState({ exportOpen }),
  setFollow: (follow: boolean) => useUi.setState({ follow }),
  setTempoLane: (tempoLane: boolean) => useUi.setState({ tempoLane }),
  setLane: (lane: LaneTab) => useUi.setState({ lane, controllerLane: null }),
  setControllerLane: (controllerLane: number | null) => useUi.setState({ controllerLane }),
  setHumaniseOpen: (humaniseOpen: boolean) => useUi.setState({ humaniseOpen }),
  setActiveDrumMapId: (activeDrumMapId: string) => {
    useUi.setState({ activeDrumMapId });
    saveDrumSettings();
  },
  setCustomDrumPiece: (mapId: string, pitch: number, piece: DrumPiece) => {
    useUi.setState((s) => {
      const existing = s.customDrumMaps[mapId] ?? BUILTIN_DRUM_MAPS.find((m) => m.id === mapId) ?? DEFAULT_DRUM_MAP;
      const updated: DrumMap = {
        ...existing,
        id: mapId,
        pieces: { ...existing.pieces, [pitch]: piece },
      };
      return { customDrumMaps: { ...s.customDrumMaps, [mapId]: updated } };
    });
    saveDrumSettings();
  },
};

const DRUM_SETTINGS_KEY = 'drumMaps';

// The drum map in use and the edited maps belong to the user, not to a project: they live in the app's settings.
function saveDrumSettings() {
  const { activeDrumMapId, customDrumMaps } = useUi.getState();
  bridge
    .call('settings.set', { key: DRUM_SETTINGS_KEY, value: JSON.stringify({ activeDrumMapId, customDrumMaps }) })
    .catch(() => {});
}

/** Loads the drum settings saved by saveDrumSettings(). */
export async function loadDrumSettings() {
  const saved = await bridge.call('settings.get', { key: DRUM_SETTINGS_KEY });
  if (saved) useUi.setState(JSON.parse(saved) as Pick<UiState, 'activeDrumMapId' | 'customDrumMaps'>);
}

let applyingRemote = false;

/** Relays this window's selection to the other windows and follows theirs, so the main window, the mixer window
    and the detached MIDI editor show one selection (docs/design/mixer.md, X6 and X11). */
export function startSelectionRelay() {
  useUi.subscribe((s, prev) => {
    if (applyingRemote) return;
    if (
      s.selectedTracks === prev.selectedTracks &&
      s.selectedTrackId === prev.selectedTrackId &&
      s.selectedNode === prev.selectedNode &&
      s.editedClipId === prev.editedClipId
    )
      return;
    bridge
      .call('ui.select', {
        source: WINDOW,
        tracks: [...s.selectedTracks],
        primary: s.selectedTrackId,
        node: s.selectedNode,
        clip: s.editedClipId,
      })
      .catch(() => {});
  });
  bridge.on('dawg_selection', (selection) => {
    if (selection.source === WINDOW) return;
    applyingRemote = true;
    useUi.setState((s) => ({
      selectedTracks: new Set(selection.tracks),
      selectedTrackId: selection.primary,
      selectedNode: selection.node,
      editedClipId: selection.clip,
      ...(selection.primary !== s.selectedTrackId ? { selectedNotes: new Set<number>(), selectedPoints: null } : {}),
    }));
    applyingRemote = false;
  });
}
