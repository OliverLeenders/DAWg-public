import { useWindowDrag, WindowControls } from '../components/WindowControls';
import { Pause, Play, Redo2, Undo2, X } from 'lucide-react';
import { useEffect } from 'react';
import { actions, initialise, projectActions, useDawg } from '../store/store';
import { toggleMixer, useMixer, type MixerSettings } from './mixerStore';
import { Mixer } from './Mixer';
import { GlobalSwitches } from '../components/GlobalSwitches';
import { IconButton } from '../ui/controls';
import { Bar } from '../ui/Bar';

// The mixer window's UI (docs/design/mixer.md, X1 and X11): the same bundle as the main window, loaded with
// ?window=mixer, showing only the mixer under a toolbar with the global mute and solo switches and the section and width toggles (X4, X5). Space, undo,
// redo and save work here too; Ctrl+M closes the window.

const SECTIONS: Array<{ key: keyof MixerSettings; label: string; title: string }> = [
  { key: 'io', label: 'I/O', title: 'Inputs and outputs' },
  { key: 'inserts', label: 'Inserts', title: 'Insert effects' },
  { key: 'sends', label: 'Sends', title: 'Sends to returns and sidechains' },
  { key: 'offsets', label: 'Offset', title: 'Track timing offsets' },
];

function useMixerShortcuts() {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement;
      if (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA') return;
      const mod = e.ctrlKey || e.metaKey;
      const open = useDawg.getState().projectState.open;
      const key = e.key.toLowerCase();
      if (mod && key === 'z') {
        e.preventDefault();
        if (e.shiftKey) actions.redo();
        else actions.undo();
      } else if (mod && key === 'y') {
        e.preventDefault();
        actions.redo();
      } else if (mod && key === 's') {
        e.preventDefault();
        if (open) projectActions.save(e.shiftKey);
      } else if (mod && key === 'm') {
        e.preventDefault();
        actions.showMixer(false);
      } else if (e.code === 'Space' && open && target.tagName !== 'SELECT') {
        e.preventDefault();
        actions.togglePlay();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
}

export function MixerApp() {
  const connected = useDawg((s) => s.connected);
  const projectState = useDawg((s) => s.projectState);
  const playing = useDawg((s) => s.telemetry.playing);
  const history = useDawg((s) => s.history);
  const lastError = useDawg((s) => s.lastError);
  const settings = useMixer();

  useEffect(() => {
    initialise().catch((e) => useDawg.setState({ lastError: String(e) }));
  }, []);
  useEffect(() => {
    document.title = projectState.open ? `Mixer - ${projectState.name}` : 'Mixer';
  }, [projectState.open, projectState.name]);
  useMixerShortcuts();
  const startWindowDrag = useWindowDrag();

  if (!connected) return <div className="loading">Connecting to engine…</div>;

  return (
    <div className="mixer-app">
      {/* One row that never wraps: what doesn't fit folds into "⋯" (design-system.md, DS14). */}
      <Bar
        className="mixer-toolbar"
        onPointerDown={startWindowDrag}
        items={[
          {
            key: 'transport',
            fold: 3,
            node: (
              <div className="group">
                <IconButton
                  size="l"
                  className={`${playing ? 'active' : ''}`}
                  title={playing ? 'Stop (Space)' : 'Play (Space)'}
                  disabled={!projectState.open}
                  onClick={actions.togglePlay}
                >
                  {playing ? <Pause size={14} /> : <Play size={14} />}
                </IconButton>
                <IconButton size="l" title={`Undo ${history.undoLabel} (Ctrl+Z)`} disabled={!history.canUndo} onClick={actions.undo}>
                  <Undo2 size={14} />
                </IconButton>
                <IconButton size="l" title={`Redo ${history.redoLabel} (Ctrl+Shift+Z)`} disabled={!history.canRedo} onClick={actions.redo}>
                  <Redo2 size={14} />
                </IconButton>
              </div>
            ),
          },
          projectState.open && { key: 'switches', fold: 4, node: <GlobalSwitches /> },
          { key: 'title', node: <span className="mixer-title">{projectState.open ? projectState.name || 'Untitled' : 'Mixer'}</span> },
          { key: 'spacer', grow: true, node: <span className="spacer" /> },
          {
            key: 'sections',
            fold: 2,
            node: (
              <div className="segmented toggles" title="Sections shown">
                {SECTIONS.map(({ key, label, title }) => (
                  <button key={key} className={settings[key] ? 'active' : ''} title={`${title}: ${settings[key] ? 'shown' : 'hidden'}`} onClick={() => toggleMixer(key)}>
                    {label}
                  </button>
                ))}
              </div>
            ),
          },
          {
            key: 'width',
            fold: 1,
            node: (
              <div className="segmented" title="Strip width">
                <button className={settings.narrow ? '' : 'active'} onClick={() => useMixer.setState({ narrow: false })}>
                  Wide
                </button>
                <button className={settings.narrow ? 'active' : ''} onClick={() => useMixer.setState({ narrow: true })}>
                  Narrow
                </button>
              </div>
            ),
          },
          { key: 'window', end: true, node: <WindowControls /> },
        ]}
      />
      {projectState.open ? <Mixer /> : <div className="mixer-empty centred">No project is open</div>}
      {lastError && (
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
