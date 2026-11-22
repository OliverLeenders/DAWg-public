import { X } from 'lucide-react';
import { useEffect } from 'react';
import { actions, initialise, useDawg } from '../store/store';
import { useWindowDrag, WindowControls } from '../components/WindowControls';
import { PianoRoll } from './PianoRoll';
import { ErrorToast, useErrorToastShowing } from '../components/ErrorToast';
import { keyBelongsToFocus, sharedShortcut } from '../keys';

// The MIDI editor's own window: the same bundle as the main window, loaded with ?window=roll. Playback, undo, redo
// and save work here too; the selection relay keeps the edited clip in step with the arrangement.

function useRollShortcuts() {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      // The project and dialogs live in the main window (new, open, close, export); the rest is shared.
      if (!keyBelongsToFocus(e)) sharedShortcut(e);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
}

export function PianoRollApp() {
  const connected = useDawg((s) => s.connected);
  const projectState = useDawg((s) => s.projectState);
  const lastError = useDawg((s) => s.lastError);
  const toastShowing = useErrorToastShowing();

  useEffect(() => {
    initialise().catch((e) => useDawg.setState({ lastError: String(e) }));
  }, []);
  useEffect(() => {
    document.title = projectState.open ? `MIDI Editor - ${projectState.name || 'Untitled'}` : 'MIDI Editor';
  }, [projectState.open, projectState.name]);
  useRollShortcuts();
  const startWindowDrag = useWindowDrag();

  if (!connected) return <div className="loading">Connecting to engine…</div>;

  return (
    <div className="roll-app">
      {projectState.open ? (
        <PianoRoll />
      ) : (
        <>
          <div className="roll-toolbar" onPointerDown={startWindowDrag}>
            <span className="spacer" />
            <WindowControls />
          </div>
          <div className="editor-placeholder centred">No project is open</div>
        </>
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
