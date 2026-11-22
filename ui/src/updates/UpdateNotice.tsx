import { Download, Info } from 'lucide-react';
import { useEffect } from 'react';
import { useUpdates } from './updateStore';
import { Button } from '../ui/controls';
import { Dialog } from '../ui/overlays';

// Update checks in the main window (docs/design/update-checks.md, UC4 and UC5): the first-start notice, the quiet
// notice when a release is on offer, and the dialog with its notes.

export function UpdateNotice() {
  const s = useUpdates();

  useEffect(() => {
    void useUpdates.getState().init();
  }, []);

  if (!s.loaded) return null;

  return (
    <>
      {!s.asked && (
        <div className="alt-drag-notice update-notice" role="status">
          <Info size={16} />
          <span>
            DAWg checks GitHub once a day for a new release and tells you here. It sends nothing about you or your projects,
            and it never installs anything itself.
          </span>
          <Button onClick={() => s.answerFirstRun(false)}>
            Don't check
          </Button>
          <Button className="primary" onClick={() => s.answerFirstRun(true)}>
            Check for updates
          </Button>
        </div>
      )}

      {s.asked && s.offer && !s.dialogOpen && (
        <button className="update-pill" onClick={() => useUpdates.setState({ dialogOpen: true })} title="See what's new">
          <Download size={13} />
          {s.offer.name.toLowerCase().startsWith('dawg') ? s.offer.name : `DAWg ${s.offer.name}`} is available
        </button>
      )}

      {s.dialogOpen && <UpdateDialog />}
    </>
  );
}

function UpdateDialog() {
  const s = useUpdates();
  const close = s.offer ? s.later : s.closeDialog;

  return (
    <Dialog className="update-dialog" onClose={close} closeOnBackdrop aria-labelledby="update-title">
      {s.offer ? (
        <>
          <h3 id="update-title">{s.offer.name} is available</h3>
          <p className="hint">
            {s.offer.prerelease ? 'A beta version. ' : ''}Download opens its page on GitHub; DAWg doesn't install anything itself.
          </p>
          {s.offer.notes.trim() !== '' && <pre className="update-notes">{s.offer.notes.trim()}</pre>}
          <div className="modal-buttons">
            <Button onClick={s.skip}>
              Skip this version
            </Button>
            <span className="spacer" />
            <Button onClick={s.later}>
              Later
            </Button>
            <Button className="primary" autoFocus onClick={s.download}>
              Download
            </Button>
          </div>
        </>
      ) : (
        <>
          <h3 id="update-title">{s.manualResult?.kind === 'failed' ? 'Could not check for updates' : 'DAWg is up to date'}</h3>
          <p>{s.manualResult?.kind === 'failed' ? `${s.manualResult.message}.` : 'There is no newer release.'}</p>
          <div className="modal-buttons">
            <span className="spacer" />
            <Button className="primary" autoFocus onClick={close}>
              Close
            </Button>
          </div>
        </>
      )}
    </Dialog>
  );
}
