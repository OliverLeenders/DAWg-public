import { create } from 'zustand';
import * as bridge from '../bridge/bridge';
import type {
  AppInfo,
  AudioDeviceInfo,
  Command,
  History,
  DiscardMode,
  ExportState,
  FaustScript,
  MidiDevice,
  NodeWidths,
  PluginNodeStatus,
  PluginOptions,
  NodeTypeInfo,
  PluginScanState,
  Project,
  ProjectState,
  ProjectTemplate,
  RecentProject,
  Telemetry,
  MidiFileContents,
SavedComponent } from '../bridge/protocol';
import { startMidiImport } from '../components/MidiImportDialog';
import { applyPatch } from './applyPatch';
import { coalesceKey, createCommandQueue } from './commandQueue';
import { loadDrumSettings, startSelectionRelay, useUi } from './uiStore';
import { addedClipIds } from '../arrangement/clipSelection';
import { WINDOW } from '../window';

// Mirror of the C++ model. The C++ Session is the source of truth: the UI sends
// commands and applies the patches that come back. Never mutate `project` locally.

interface DawgState {
  connected: boolean;
  rev: number;
  project: Project | null;
  history: History;
  telemetry: Telemetry;
  audio: AudioDeviceInfo;
  plugins: PluginScanState;
  /** Runtime state of plugin nodes, keyed "<trackId>:<nodeId>". */
  nodes: Record<string, PluginNodeStatus>;
  /** Port widths from the last compilation, for nodes with a mono port (docs/design/mono-ports.md, MP8). */
  widths: Record<string, NodeWidths>;
  /** Plugins whose blocks last came back late, and when. */
  latePlugins: { names: string[]; at: number };
  pluginOptions: PluginOptions;
  /** The engine's node types, keyed by type (docs/design/phase8-node-graph.md). */
  nodeTypes: Record<string, NodeTypeInfo>;
  /** Faust examples and the user's script library, offered like built-ins (docs/design/phase11-faust-panels.md, F6). */
  faustScripts: FaustScript[];
  savedComponents: SavedComponent[];
  projectState: ProjectState;
  recent: RecentProject[];
  templates: ProjectTemplate[];
  /** Set while the user must decide what happens to unsaved changes; resolve() continues the action. */
  confirm: { action: string; resolve: (mode: DiscardMode) => void } | null;
  /** Pool files that can't be found (docs/design/phase7-audio-tracks.md, T11). */
  missingAudio: Array<{ id: number; name: string; path: string }>;
  /** Tracks armed for recording (session state, not saved). */
  armed: ReadonlySet<number>;
  /** Names of the device's active input channels. */
  inputs: string[];
  /** MIDI input devices (docs/design/phase9-export-midi-input.md, M1). */
  midiDevices: MidiDevice[];
  /** The parameter waiting for a controller in MIDI learn (M8). */
  learning: { trackId: number; nodeId: number; param: string } | null;
  /** The running or finished export (X1–X5); null when the dialog shows no export. */
  exportState: ExportState | null;
  /** Clips whose stretched audio is still rendering; they play a repitch preview (docs/design/time-stretching.md,
      TS9). */
  stretchPending: ReadonlySet<number>;
  /** Tracks (master included) whose meter reached 0 dBFS; cleared by clicking the indicator (mixing-basics.md, M3). */
  clipped: ReadonlySet<number>;
  /** Whether the mixer window is open (docs/design/mixer.md, X1). */
  mixerOpen: boolean;
  /** Whether the MIDI editor's own window is open. */
  editorOpen: boolean;
  /** A MIDI file waiting for the choice about its tempo. */
  midiImport: MidiFileContents | null;
  /** The audio clip the Slice to Sampler dialog is open for (audio-features.md, AU3). */
  slice: number | null;
  /** A running track render (freeze, bounce, render selection; audio-features.md, AU14). */
  render: { label?: string; progress?: number } | null;
  /** Audio clips waiting for the Normalise dialog's target (docs/design/audio-features.md, AU2). */
  normalise: number[] | null;
  app: AppInfo | null;
  lastError: string | null;
  /** A hint about how something works, shown once in an info style; nothing failed (docs/ui-review-2026-11-10.md, 3.3). */
  lastHint: string | null;
}

const initialState: DawgState = {
  connected: false,
  rev: 0,
  project: null,
  history: { canUndo: false, canRedo: false, undoLabel: '', redoLabel: '' },
  telemetry: {
    playing: false,
    positionBeats: 0,
    peakLeft: 0,
    peakRight: 0,
    sampleRate: 0,
    latencySamples: 0,
    cpu: 0,
    allocationViolations: 0,
    pluginAllocations: 0,
    recording: false,
    previewSeconds: -1,
    recordStartBeats: -1,
    inputPeak: 0,
    meters: {},
    midiActivity: 0,
    automation: {},
  },
  audio: { open: false },
  plugins: { scanning: false, progress: 0, current: '', plugins: [], failed: [] },
  nodes: {},
  widths: {},
  latePlugins: { names: [], at: 0 },
  pluginOptions: { available: false, outOfProcess: false },
  nodeTypes: {},
  faustScripts: [],
  savedComponents: [],
  projectState: { open: false, name: '', path: '', dirty: false, untitled: true, recovery: null },
  recent: [],
  templates: [],
  confirm: null,
  missingAudio: [],
  armed: new Set<number>(),
  inputs: [],
  clipped: new Set<number>(),
  midiDevices: [],
  learning: null,
  exportState: null,
  stretchPending: new Set(),
  mixerOpen: false,
  editorOpen: false,
  midiImport: null,
  normalise: null,
  render: null,
  slice: null,
  app: null,
  lastError: null,
  lastHint: null,
};

export const useDawg = create<DawgState>()(() => initialState);

const set = useDawg.setState;

export function reportError(error: unknown) {
  set({ lastError: error instanceof Error ? error.message : String(error) });
}

function pluginOf(key: string) {
  const project = useDawg.getState().project;
  const [trackId, nodeId] = key.split(':').map(Number);
  const track = project?.master?.id === trackId ? project.master : project?.tracks[trackId];
  return track?.graph.nodes[nodeId];
}

function noteLatePlugins(before: Record<string, PluginNodeStatus>, after: Record<string, PluginNodeStatus>) {
  const late = Object.keys(after).filter((key) => (after[key].lateBlocks ?? 0) > (before[key]?.lateBlocks ?? 0));
  if (late.length > 0) set({ latePlugins: { names: late.map((key) => pluginOf(key)?.plugin?.name ?? 'A plugin'), at: Date.now() } });
}

/** The user hears once of a plugin going away; it stays in the project, with a Reload button on its slot. */
function reportCrashes(before: Record<string, PluginNodeStatus>, after: Record<string, PluginNodeStatus>) {
  for (const [key, status] of Object.entries(after)) {
    if (status.status !== 'crashed' || before[key]?.status === 'crashed') continue;
    
    const plugin = pluginOf(key)?.plugin;
    reportError(`${plugin?.name ?? 'A plugin'} stopped working: ${status.error ?? 'its process has gone'}. Reload it from its slot.`);
  }
}

async function resync() {
  const snapshot = await bridge.call('model.get');
  set({ rev: snapshot.rev, project: snapshot.project, history: snapshot.history });
}

let initialising: Promise<void> | null = null;
/** Set while recording runs, until the patch that adds its takes (or another patch after the stop). */
let awaitingTakes = false;

/** Connects to the engine, loads the initial state and subscribes to events.
    Idempotent (React StrictMode runs effects twice in development). */
export function initialise(): Promise<void> {
  initialising ??= doInitialise();
  return initialising;
}

async function doInitialise(): Promise<void> {
  if (!(await bridge.connect())) return;

  bridge.on('dawg_patch', (patch) => {
    const { rev, project } = useDawg.getState();
    if (project === null || patch.rev !== rev + 1) {
      // Missed a patch (or not loaded yet): fetch a full snapshot instead.
      resync().catch(reportError);
      return;
    }
    const next = applyPatch(project, patch.ops);
    set({ rev: patch.rev, project: next, history: patch.history });
    // A recording adds its takes in one undo step labelled "Record" after it stops: select exactly those
    // (docs/design/takes-lanes.md, T3). The first other patch after the stop means it recorded nothing.
    if (awaitingTakes && patch.history.undoLabel === 'Record') {
      awaitingTakes = false;
      const added = addedClipIds(project, next);
      if (added.length > 0 && WINDOW === 'main') useUi.setState({ selectedClips: new Set(added) });
    } else if (awaitingTakes && !useDawg.getState().telemetry.recording) {
      awaitingTakes = false;
    }
    if (next.audioFiles !== project.audioFiles) refreshMissing();
  });
  bridge.on('dawg_telemetry', (telemetry) => {
    if (telemetry.recording) awaitingTakes = true;
    // Clip indicators stay lit until clicked.
    const { clipped } = useDawg.getState();
    const newly = Object.entries(telemetry.meters ?? {})
      .filter(([id, [left, right]]) => Math.max(left, right) >= 1 && !clipped.has(Number(id)))
      .map(([id]) => Number(id));
    set(newly.length > 0 ? { telemetry, clipped: new Set([...clipped, ...newly]) } : { telemetry });

    // Output protection (docs/design/output-protection.md, S1): say which tracks produced invalid audio.
    const invalid = telemetry.invalidTracks ?? [];
    if (invalid.length > 0) {
      const { project } = useDawg.getState();
      const names = invalid.map((id) =>
        project?.master?.id === id ? 'Master' : (project?.tracks[String(id)]?.name ?? `Track ${id}`),
      );
      set({ lastError: `Invalid audio (NaN or infinity) from ${names.join(', ')}: silenced and reset.` });
    }
  });
  bridge.on('dawg_audio', (audio) => {
    set({ audio });
    refreshInputs();
  });
  bridge.on('dawg_armed', (ids) => set({ armed: new Set(ids) }));
  bridge.on('dawg_midi', (midiDevices) => set({ midiDevices }));
  if (WINDOW === 'main') bridge.on('dawg_midiFile', (file) => startMidiImport(file, 0));
  bridge.on('dawg_learned', ({ ok, error }) => set({ learning: null, ...(ok ? {} : { lastError: error }) }));
  bridge.on('dawg_export', (exportState) => set({ exportState }));
  bridge.on('dawg_render', (render) =>
    set((s) => ({
      render: render.state === 'stretching' || render.state === 'rendering' ? { ...s.render, ...render } : null,
      ...(render.error ? { lastError: render.error } : {}),
    })),
  );
  bridge.on('dawg_stretch', ({ pending }) => set({ stretchPending: new Set(pending) }));
  bridge.on('dawg_plugins', (plugins) => set({ plugins }));
  bridge.on('dawg_nodes', (nodes) => {
    reportCrashes(useDawg.getState().nodes, nodes);
    noteLatePlugins(useDawg.getState().nodes, nodes);
    set({ nodes });
  });
  bridge.on('dawg_widths', (widths) => set({ widths }));
  bridge.on('dawg_error', (message) => set({ lastError: message }));
  bridge.on('dawg_hint', (message) => set({ lastHint: message }));
  bridge.on('dawg_projectState', (projectState) => {
    set({ projectState });
    refreshLists();
    refreshMissing();
  });
  // Unsaved changes are decided in the main window (the mixer and MIDI editor windows show no prompts).
  if (WINDOW === 'main')
    bridge.on('dawg_confirm', ({ action }) => askToDiscard(action, (mode) => bridge.call('project.quit', { mode })));
  // The page says it is alive, so the app can rebuild it when it stops (docs/design/ui-recovery.md, UR3).
  if (WINDOW === 'main') {
    const alive = () => bridge.call('ui.alive').catch(() => {});
    alive();
    window.setInterval(alive, HEARTBEAT_MS);
  }
  bridge.on('dawg_mixer', ({ open }) => set({ mixerOpen: open }));
  bridge.on('dawg_editor', ({ open }) => set({ editorOpen: open }));
  // The instrument window keeps to its instrument's track (macros-and-modulation-column.md, MC14).
  if (WINDOW !== 'instrument') startSelectionRelay();
  if (WINDOW === 'roll') loadDrumSettings().catch(reportError);

  const [app, audio, plugins, nodes, types, pluginOptions] = await Promise.all([
    bridge.call('app.info'),
    bridge.call('audio.get'),
    bridge.call('plugins.get'),
    bridge.call('plugins.status'),
    bridge.call('nodes.types'),
    bridge.call('plugins.options'),
    resync(),
  ]);
  const projectState = await bridge.call('project.state');
  bridge
    .call('mixer.state')
    .then(({ open }) => set({ mixerOpen: open }))
    .catch(reportError);
  bridge
    .call('editor.state')
    .then(({ open }) => set({ editorOpen: open }))
    .catch(reportError);
  const nodeTypes = Object.fromEntries(types.map((t) => [t.type, t]));
  bridge
    .call('midi.devices')
    .then((midiDevices) => set({ midiDevices }))
    .catch(reportError);
  bridge
    .call('nodes.widths')
    .then((widths) => set({ widths }))
    .catch(reportError);
  set({ connected: true, app, audio, plugins, nodes, nodeTypes, projectState, pluginOptions });
  refreshFaustScripts();
  refreshSavedComponents();
  refreshInputs();
  refreshLists();
}

/** Model commands, undo and redo, in order and one at a time (see commandQueue.ts). */
const commands = createCommandQueue();

/** Sends a model command. Pass the same gesture id for every update of one drag
    so the whole drag becomes a single undo step. `key` overrides what the command
    queue uses to replace a waiting value command (see commandQueue.ts). */
export function dispatch(command: Command, gesture?: string, key?: string): void {
  commands.push(
    () => bridge.call('model.apply', { ...command, gesture }).catch(reportError),
    key ?? coalesceKey(command, gesture),
    gesture,
  );
}

/** Reloads the Faust examples and library (after saving a script). */
export function refreshFaustScripts() {
  bridge
    .call('faust.scripts')
    .then((faustScripts) => set({ faustScripts }))
    .catch(reportError);
}

/** Reloads the list of saved components (after saving or deleting one). */
export function refreshSavedComponents() {
  bridge
    .call('components.list')
    .then((savedComponents) => set({ savedComponents }))
    .catch(reportError);
}

function refreshInputs() {
  bridge
    .call('audio.inputs')
    .then((inputs) => set({ inputs }))
    .catch(reportError);
}

function refreshMissing() {
  if (!useDawg.getState().projectState.open) {
    set({ missingAudio: [] });
    return;
  }
  bridge
    .call('audio.missing')
    .then((missingAudio) => set({ missingAudio }))
    .catch(reportError);
}

function refreshLists() {
  Promise.all([bridge.call('project.recent'), bridge.call('project.templates')])
    .then(([recent, templates]) => set({ recent, templates }))
    .catch(reportError);
}

/** Runs an action that may discard unsaved changes; if the native side says a decision is needed, asks first. */
function discarding(action: string, call: (mode: DiscardMode) => Promise<{ needsDecision: boolean }>) {
  call('ask')
    .then((r) => {
      if (r.needsDecision) askToDiscard(action, call);
    })
    .catch(reportError);
}

/** How often the main window says it is alive; the app reloads it after 30 s of silence (UR3). */
const HEARTBEAT_MS = 5000;

function askToDiscard(action: string, call: (mode: DiscardMode) => Promise<unknown>) {
  set({
    confirm: {
      action,
      resolve: (mode) => {
        set({ confirm: null });
        // The app stops waiting for this prompt (UR6).
        if (action === 'quit') bridge.call('project.confirmDone').catch(() => {});
        if (mode !== 'ask') call(mode).catch(reportError);
      },
    },
  });
}

export const projectActions = {
  newProject: (template = 'empty') => discarding('new', (mode) => bridge.call('project.new', { template, mode })),
  open: (path?: string) => discarding('open', (mode) => bridge.call('project.open', { path, mode })),
  close: () => discarding('close', (mode) => bridge.call('project.close', { mode })),
  save: (saveAs = false) => bridge.call('project.save', { saveAs }).catch(reportError),
  saveAsTemplate: (name: string) => bridge.call('project.saveAsTemplate', { name }).catch(reportError),
  recover: (accept: boolean) => bridge.call('project.recover', { accept }).catch(reportError),
  importMidi: () => bridge.call('midi.importDialog').catch(reportError),
  exportMidi: () => bridge.call('midi.exportDialog').catch(reportError),
  importAudio: (trackId?: number, start?: number) => bridge.call('audio.importDialog', { trackId, start }).catch(reportError),
  locateAudio: (fileId: number) => bridge.call('audio.locate', { fileId }).catch(reportError),
  removeRecent: (path: string) =>
    bridge
      .call('project.removeRecent', { path })
      .then(refreshLists)
      .catch(reportError),
};

export const actions = {
  undo: () => commands.push(() => bridge.call('model.undo').catch(reportError)),
  redo: () => commands.push(() => bridge.call('model.redo').catch(reportError)),
  play: () => bridge.call('transport.play').catch(reportError),
  /** Stops playback (the playhead returns to where playback started); when already stopped, goes to the start. */
  stop: () =>
    (useDawg.getState().telemetry.playing ? bridge.call('transport.stop') : bridge.call('transport.seek', { beats: 0 })).catch(
      reportError,
    ),
  seek: (beats: number) => bridge.call('transport.seek', { beats }).catch(reportError),
  /** Starts recording armed tracks (with a one-bar count-in), or stops a running recording. */
  record: (countIn = false) => bridge.call('transport.record', { countIn }).catch(reportError),
  arm: (trackId: number, armed: boolean) => bridge.call('track.arm', { trackId, armed }).catch(reportError),
  togglePlay: () => bridge.call('transport.toggle').catch(reportError),
  /** Opens or closes the mixer window (docs/design/mixer.md, X1). */
  showMixer: (open: boolean) => bridge.call('mixer.show', { open }).catch(reportError),
  /** Opens or closes the MIDI editor's own window, on the current track and clip. */
  showEditor: (open: boolean) => {
    const { selectedTrackId, editedClipId } = useUi.getState();
    bridge
      .call('editor.show', { open, track: selectedTrackId ?? 0, clip: editedClipId ?? 0 })
      .catch(reportError);
  },
  openAudioSettings: () => bridge.call('audio.openSettings').catch(reportError),
  scanPlugins: () => bridge.call('plugins.scan').catch(reportError),
  setPluginsOutOfProcess: (enabled: boolean) => {
    set({ pluginOptions: { ...useDawg.getState().pluginOptions, outOfProcess: enabled } });
    return bridge.call('plugins.setOutOfProcess', { enabled }).catch(reportError);
  },
  setPluginRunsInside: (identifier: string, inside: boolean) =>
    bridge.call('plugin.setRunInside', { identifier, inside }).catch(reportError),
  reloadPlugin: (trackId: number, nodeId: number) => bridge.call('plugin.reload', { trackId, nodeId }).catch(reportError),
  clearNode: (trackId: number, nodeId: number) => bridge.call('node.clear', { trackId, nodeId }).catch(reportError),
  openEditor: (trackId: number, nodeId: number) => bridge.call('plugin.openEditor', { trackId, nodeId }).catch(reportError),
  closeEditor: (trackId: number, nodeId: number) =>
    bridge.call('plugin.closeEditor', { trackId, nodeId }).catch(reportError),

  /** Turns a clip indicator off; without an id, all of them. */
  resetClip: (trackId?: number) => {
    const { clipped } = useDawg.getState();
    if (trackId === undefined) set({ clipped: new Set() });
    else if (clipped.has(trackId)) set({ clipped: new Set([...clipped].filter((id) => id !== trackId)) });
  },

  /** MIDI input devices: enable and settings (M1). */
  configureMidi: (identifier: string, fields: Partial<Omit<MidiDevice, 'identifier' | 'name'>>) =>
    bridge
      .call('midi.configure', { identifier, ...fields })
      .then((midiDevices) => set({ midiDevices }))
      .catch(reportError),
  /** Starts MIDI learn for a parameter, or cancels it (M8). */
  midiLearn: (target: { trackId: number; nodeId: number; param: string } | null) => {
    set({ learning: target });
    bridge.call('midi.learn', target ?? {}).catch(reportError);
  },

  clearError: () => set({ lastError: null }),
};

let gestureCounter = 0;
// The window's name keeps the ids of two windows apart (docs/design/mixer.md, X11).
export const newGestureId = () => `${WINDOW}${++gestureCounter}-${Date.now()}`;
