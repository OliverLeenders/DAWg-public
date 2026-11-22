import { X } from 'lucide-react';
import { useEffect, useLayoutEffect, useState } from 'react';
import { call, on } from '../bridge/bridge';
import { nodeName } from '../components/chain';
import { ErrorToast, useErrorToastShowing } from '../components/ErrorToast';
import { useWindowDrag, WindowControls } from '../components/WindowControls';
import { useWatchNodes } from '../dock/analysis';
import { DeviceStrip } from '../dock/DeviceStrip';
import { dockTrack } from '../dock/dockTrack';
import { Knob } from '../dock/Knob';
import { panelFor } from '../dock/panels';
import { paramApplies } from '../graph/lfo';
import { GraphView } from '../graph/GraphView';
import { infoOf } from '../graph/nodeInfo';
import { findNode, heldGraph } from '../graph/voice';
import { keyBelongsToFocus, sharedShortcut } from '../keys';
import { actions, initialise, useDawg } from '../store/store';
import { useUi } from '../store/uiStore';
import { initialInstrument } from '../window';

// An instrument's own window (docs/design/macros-and-modulation-column.md, MC14): the same bundle as the main window,
// loaded with ?window=instrument&track=<id>&node=<id>. A Sampler or Synth shows its whole panel; a Voice Instrument or a
// component its graph, with the strip of what's in it underneath. Opening another instrument retargets it.

interface Target {
  trackId: number;
  nodeId: number;
}

export function InstrumentApp() {
  const connected = useDawg((s) => s.connected);
  const projectState = useDawg((s) => s.projectState);
  const lastError = useDawg((s) => s.lastError);
  const toastShowing = useErrorToastShowing();
  const project = useDawg((s) => s.project);
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const [target, setTarget] = useState<Target | null>(
    initialInstrument?.trackId && initialInstrument.nodeId ? { trackId: initialInstrument.trackId, nodeId: initialInstrument.nodeId } : null,
  );
  const track = target ? dockTrack(project, target.trackId) : null;
  const node = track && target ? findNode(track, target.nodeId) : undefined;
  const holdsGraph = heldGraph(node) !== undefined;
  const startWindowDrag = useWindowDrag();

  useEffect(() => {
    initialise().catch((e) => useDawg.setState({ lastError: String(e) }));
    return on('dawg_instrumentEditor', ({ trackId, nodeId }) => setTarget({ trackId, nodeId }));
  }, []);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!keyBelongsToFocus(e)) sharedShortcut(e);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
  // The graph view here shows what the instrument holds, on its track.
  useEffect(() => {
    if (!target) return;
    useUi.setState({
      selectedTrackId: target.trackId,
      selectedTracks: new Set([target.trackId]),
      centre: 'graph',
      graphScope: holdsGraph ? { trackId: target.trackId, nodeId: target.nodeId } : null,
    });
  }, [target, holdsGraph]);
  const name = node ? nodeName(node, types) : 'Instrument';
  useEffect(() => {
    document.title = track && node ? `${name} - ${track.name}` : 'Instrument';
  }, [name, track, node]);
  useWatchNodes(track && node && !holdsGraph ? [{ trackId: track.id, nodeId: node.id }] : []);

  // The window can't be made narrower than the panel's own minimum width (the Sampler's columns) plus its padding.
  useLayoutEffect(() => {
    const holder = document.querySelector('.instrument-panel');
    const panel = holder?.querySelector(':scope > .device-panel');
    const least = panel && holder ? parseFloat(getComputedStyle(panel).minWidth) || 0 : 0;
    const padding = holder ? parseFloat(getComputedStyle(holder).paddingLeft) + parseFloat(getComputedStyle(holder).paddingRight) : 0;
    call('window.minWidth', { window: 'instrument', width: least > 0 ? Math.ceil(least + padding) : 0 }).catch(() => {});
  }, [node?.type, connected]);

  if (!connected) return <div className="loading">Connecting to engine…</div>;

  const info = track && node ? infoOf(track.id, node, types, statuses) : undefined;
  const Panel = node ? panelFor(node.type) : null;

  return (
    <div className="roll-app instrument-app">
      <div className="roll-toolbar" onPointerDown={startWindowDrag}>
        <strong className="instrument-app-title">{track && node ? `${track.name} › ${name}` : 'Instrument'}</strong>
        <span className="spacer" />
        <WindowControls />
      </div>
      {!projectState.open || !track || !node ? (
        <div className="editor-placeholder centred">{projectState.open ? 'This instrument is gone' : 'No project is open'}</div>
      ) : holdsGraph ? (
        <div className="instrument-graph">
          <div className="instrument-graph-view">
            <GraphView />
          </div>
          <div className="instrument-strip">
            <DeviceStrip key={`${track.id}:${node.id}`} track={track} scope={node.id} />
          </div>
        </div>
      ) : Panel && info ? (
        <div className="instrument-panel">
          <Panel key={`${track.id}:${node.id}`} track={track} node={node} info={info} />
        </div>
      ) : info && info.params.some((p) => paramApplies(node, p.id)) ? (
        // A device with no panel of its own shows its knobs, as its card did.
        <div className="instrument-panel">
          <div className="dock-knobs instrument-knobs">
            {info.params
              .filter((p) => paramApplies(node, p.id))
              .map((param) => (
                <Knob key={param.id} track={track} node={node} param={param} />
              ))}
          </div>
        </div>
      ) : (
        <div className="editor-placeholder centred">{name} has no editor of its own</div>
      )}
      <ErrorToast />
      {lastError && !toastShowing && (
        <footer className="statusbar">
          <span />
          <button className="error" onClick={actions.clearError} title="Dismiss">
            {lastError} <X size={12} />
          </button>
        </footer>
      )}
    </div>
  );
}
