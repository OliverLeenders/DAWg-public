import { useEffect, useRef } from 'react';
import * as bridge from '../bridge/bridge';
import { useDawg } from '../store/store';
import { Button } from '../ui/controls';
import { Dialog } from '../ui/overlays';

const ACTION_TEXT: Record<string, string> = {
  new: 'before creating a new project',
  open: 'before opening another project',
  close: 'before closing',
  quit: 'before quitting',
};

/**
 * The save / don't save / cancel prompt for unsaved changes (docs/design/phase5-project-workflow.md, section 8).
 * Enter saves, Escape cancels.
 */
export function ConfirmDiscard() {
  const confirm = useDawg((s) => s.confirm);
  const name = useDawg((s) => s.projectState.name);
  const saveRef = useRef<HTMLButtonElement>(null);

  useEffect(() => saveRef.current?.focus(), [confirm]);
  // The app waits for this before asking with its own prompt (docs/design/ui-recovery.md, UR6).
  useEffect(() => {
    if (confirm?.action === 'quit') bridge.call('project.confirmShown').catch(() => {});
  }, [confirm]);

  if (!confirm) return null;

  return (
    <Dialog onClose={() => confirm.resolve('ask')} aria-labelledby="confirm-title">
      <h3 id="confirm-title">Save changes to “{name}”?</h3>
      <p>Your changes will be lost if you don’t save them {ACTION_TEXT[confirm.action] ?? ''}.</p>
      <div className="modal-buttons">
        <Button onClick={() => confirm.resolve('discard')}>
          Don’t save
        </Button>
        <span className="spacer" />
        <Button onClick={() => confirm.resolve('ask')}>
          Cancel
        </Button>
        <Button ref={saveRef} className="primary" onClick={() => confirm.resolve('save')}>
          Save
        </Button>
      </div>
    </Dialog>
  );
}

/** A small prompt for a single text value (e.g. a template name). */
export function TextPrompt({
  title,
  label,
  initial,
  confirmLabel,
  onDone,
}: {
  title: string;
  label: string;
  initial: string;
  confirmLabel: string;
  onDone: (value: string | null) => void;
}) {
  return (
    <Dialog
      onClose={() => onDone(null)}
      onSubmit={(e) => {
        e.preventDefault();
        const value = String(new FormData(e.currentTarget).get('value') ?? '').trim();
        if (value) onDone(value);
      }}
    >
      <h3>{title}</h3>
      <label className="modal-field">
        <span>{label}</span>
        <input name="value" className="search" autoFocus defaultValue={initial} onFocus={(e) => e.target.select()} />
      </label>
      <div className="modal-buttons">
        <span className="spacer" />
        <Button type="button" onClick={() => onDone(null)}>
          Cancel
        </Button>
        <Button type="submit" className="primary">
          {confirmLabel}
        </Button>
      </div>
    </Dialog>
  );
}

/** A yes/no question, such as deleting a group with its tracks. Enter confirms, Escape cancels. */
export function ConfirmDialog({
  title,
  message,
  confirmLabel,
  onDone,
}: {
  title: string;
  message: string;
  confirmLabel: string;
  onDone: (confirmed: boolean) => void;
}) {
  const confirmRef = useRef<HTMLButtonElement>(null);
  useEffect(() => confirmRef.current?.focus(), []);

  return (
    <Dialog onClose={() => onDone(false)} aria-labelledby="confirm-dialog-title">
      <h3 id="confirm-dialog-title">{title}</h3>
      <p>{message}</p>
      <div className="modal-buttons">
        <span className="spacer" />
        <Button onClick={() => onDone(false)}>
          Cancel
        </Button>
        <Button ref={confirmRef} className="primary danger" onClick={() => onDone(true)}>
          {confirmLabel}
        </Button>
      </div>
    </Dialog>
  );
}
