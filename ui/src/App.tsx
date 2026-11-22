import { X } from 'lucide-react';
import { lazy, Suspense, useEffect } from 'react';
import { actions, dispatch, initialise, projectActions, useDawg } from './store/store';
import { ui, useUi } from './store/uiStore';
import { isConnected } from './bridge/bridge';
import { TopBar } from './components/TopBar';
import { Inspector } from './components/Inspector';
import { Splitter } from './components/Splitter';
import { layout, useLayout } from './store/layoutStore';
import { Arrangement } from './arrangement/Arrangement';
import { RightPanel } from './components/RightPanel';
import { WelcomeScreen } from './components/WelcomeScreen';
import { ConfirmDiscard } from './components/ConfirmDiscard';
import { MidiImportDialog } from './components/MidiImportDialog';
import { NormaliseDialog } from './components/NormaliseDialog';
import { RenderProgress } from './components/RenderProgress';
import { SliceDialog } from './components/SliceDialog';
import { OutputMuteBanner } from './components/OutputMuteBanner';
import { AltDragNotice } from './components/AltDragNotice';
import { UpdateNotice } from './updates/UpdateNotice';
import { ErrorToast, useErrorToastShowing } from './components/ErrorToast';
import { ExportDialog } from './components/ExportDialog';
import { DeviceDock } from './dock/DeviceDock';
import { BrandMark } from './components/BrandMark';
import { isAudioWindow, isInstrumentWindow, isMixerWindow, isRollWindow } from './window';
import { InstrumentApp } from './instrument/InstrumentApp';
import { AudioEditorApp } from './audioeditor/AudioEditorApp';
import { focusInPanel, focusNextPanel, keyBelongsToFocus, sharedShortcut } from './keys';
import { MixerApp } from './mixer/MixerApp';
import { PianoRollApp } from './pianoroll/PianoRollApp';

// Monaco is large: the code view loads when first shown.
const CodeView = lazy(() => import('./faust/CodeView').then((m) => ({ default: m.CodeView })));

function useGlobalShortcuts() {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (keyBelongsToFocus(e)) return;
      if (sharedShortcut(e)) return;

      const mod = e.ctrlKey || e.metaKey;
      if (mod && e.key.toLowerCase() === 'o') {
        e.preventDefault();
        projectActions.open();
      } else if (mod && e.key.toLowerCase() === 'n') {
        e.preventDefault();
        projectActions.newProject();
      } else if (mod && e.key.toLowerCase() === 'e') {
        e.preventDefault();
        if (useDawg.getState().projectState.open) ui.setExportOpen(true);
      } else if (mod && e.key.toLowerCase() === 'g' && useUi.getState().centre === 'arrangement') {
        // Ctrl+G groups the selected tracks; Ctrl+Shift+G ungroups the selected group (docs/design/groups-and-sends.md).
        const { selectedTracks, selectedTrackId } = useUi.getState();
        const tracks = useDawg.getState().project?.tracks ?? {};
        const ids = [...selectedTracks].filter((id) => tracks[String(id)] !== undefined && tracks[String(id)].kind !== 'return');
        e.preventDefault();
        if (!e.shiftKey && ids.length > 0) dispatch({ command: 'groupTracks', args: { ids } });
        else if (e.shiftKey && selectedTrackId !== null && tracks[String(selectedTrackId)]?.kind === 'group')
          dispatch({ command: 'ungroup', args: { id: selectedTrackId } });
      } else if (mod && e.shiftKey && e.key.toLowerCase() === 'd' && useUi.getState().centre === 'arrangement') {
        // Duplicate the selected track (not the master).
        const id = useUi.getState().selectedTrackId;
        const project = useDawg.getState().project;
        if (id === null || project?.tracks[String(id)] === undefined) return;
        e.preventDefault();
        dispatch({ command: 'duplicateTrack', args: { id } });
      } else if (mod && e.key.toLowerCase() === 'w') {
        e.preventDefault();
        if (useDawg.getState().projectState.open) projectActions.close();
      } else if (e.key === 'F6') {
        e.preventDefault();
        focusNextPanel(e.shiftKey);
      } else if (e.key === 'Tab' && !mod && useDawg.getState().projectState.open && !focusInPanel(e)) {
        // Cycle the arrangement and the graph (the code view closes; the piano roll is its own window). On a control
        // inside a panel (reached with F6), Tab moves the focus on instead.
        e.preventDefault();
        ui.toggleCentre();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
}

export function App() {
  // The mixer and MIDI editor windows load the same bundle (docs/design/mixer.md, X11).
  if (isInstrumentWindow) return <InstrumentApp />;
  return isMixerWindow ? <MixerApp /> : isRollWindow ? <PianoRollApp /> : isAudioWindow ? <AudioEditorApp /> : <MainApp />;
}

function MainApp() {
  const connected = useDawg((s) => s.connected);
  const lastError = useDawg((s) => s.lastError);
  const toastShowing = useErrorToastShowing();
  const app = useDawg((s) => s.app);
  const projectOpen = useDawg((s) => s.projectState.open);
  const centre = useUi((s) => s.centre);
  const exportOpen = useUi((s) => s.exportOpen);
  const panels = useLayout();
  const dockVisible = panels.dockVisible;
  const workspaceColumns = [
    panels.inspectorVisible ? `${panels.inspectorWidth}px 4px` : '',
    'minmax(0, 1fr)',
    panels.pluginsVisible ? `4px ${panels.pluginsWidth}px` : '',
  ]
    .filter(Boolean)
    .join(' ');

  useEffect(() => {
    initialise().catch((e) => useDawg.setState({ lastError: String(e) }));
  }, []);
  useGlobalShortcuts();

  if (!isConnected()) {
    return (
      <div className="disconnected">
        <h1>
          <BrandMark size={56} />
          <span>
            DAW<span>g</span>
          </span>
        </h1>
        <p>This UI runs inside the DAWg app. Start it with <code>DAWg --dev</code> to load this dev server.</p>
      </div>
    );
  }

  if (!connected) return <div className="loading">Connecting to engine…</div>;

  return (
    <div className="app">
      <TopBar />
      {projectOpen ? (
        <main className="workspace" style={{ gridTemplateColumns: workspaceColumns }}>
          {panels.inspectorVisible && (
            <>
              <Inspector />
              <Splitter width={panels.inspectorWidth} direction={1} onResize={layout.setInspectorWidth} />
            </>
          )}
          <section className={`panel editor ${dockVisible ? 'with-dock' : ''}`}>
            <div className="centre-view">
              {/* The graph replaces the arrangement's timeline, beside the track headers. */}
              {(centre === 'arrangement' || centre === 'graph') && <Arrangement graph={centre === 'graph'} />}
              {centre === 'code' && (
                <Suspense fallback={<div className="loading">Loading the editor…</div>}>
                  <CodeView />
                </Suspense>
              )}
            </div>
            {dockVisible && <DeviceDock />}
          </section>
          {panels.pluginsVisible && (
            <>
              <Splitter width={panels.pluginsWidth} direction={-1} onResize={layout.setPluginsWidth} />
              <RightPanel />
            </>
          )}
        </main>
      ) : (
        <WelcomeScreen />
      )}
      <ConfirmDiscard />
      <MidiImportDialog />
      <NormaliseDialog />
      <RenderProgress />
      <SliceDialog />
      <OutputMuteBanner />
      <AltDragNotice />
      <UpdateNotice />
      <ErrorToast />
      {exportOpen && projectOpen && <ExportDialog onClose={() => ui.setExportOpen(false)} />}
      <footer className="statusbar">
        <span>
          DAWg {app?.version} · JUCE {app?.juceVersion?.replace('JUCE v', '')}
          {app?.vst2Enabled ? ' · VST2 on' : ''}
        </span>
        {lastError && !toastShowing && (
          <button className="error" onClick={actions.clearError} title="Dismiss">
            {lastError} <X size={12} />
          </button>
        )}
      </footer>
    </div>
  );
}
