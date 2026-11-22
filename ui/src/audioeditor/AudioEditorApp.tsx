import { X } from 'lucide-react';
import { useEffect } from 'react';
import { ErrorToast, useErrorToastShowing } from '../components/ErrorToast';
import { keyBelongsToFocus, sharedShortcut } from '../keys';
import { actions, initialise, useDawg } from '../store/store';
import { AudioEditor } from './AudioEditor';

// The audio editor's own window (docs/design/time-stretching.md, TS13): the same bundle as the main window, loaded
// with ?window=audio. Playback, undo, redo and save work here too.

export function AudioEditorApp() {
  const connected = useDawg((s) => s.connected);
  const projectState = useDawg((s) => s.projectState);
  const lastError = useDawg((s) => s.lastError);
  const toastShowing = useErrorToastShowing();

  useEffect(() => {
    initialise().catch((e) => useDawg.setState({ lastError: String(e) }));
  }, []);
  useEffect(() => {
    document.title = projectState.open ? `Audio Editor - ${projectState.name || 'Untitled'}` : 'Audio Editor';
  }, [projectState.open, projectState.name]);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!keyBelongsToFocus(e)) sharedShortcut(e);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  if (!connected) return <div className="loading">Connecting to engine…</div>;

  return (
    <div className="roll-app audio-editor-app">
      <AudioEditor />
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
