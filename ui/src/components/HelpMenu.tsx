import { CircleHelp } from 'lucide-react';
import { useEffect, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { useUpdates } from '../updates/updateStore';
import { MenuItem } from './MenuItem';
import { SHORTCUTS, type Shortcut } from './shortcuts';
import { Button, IconButton } from '../ui/controls';
import { Dialog, Menu, below, type Point } from '../ui/overlays';

// The "Help" menu in the top bar and the editors' help buttons: the keyboard and mouse reference, one area at a time
// (docs/design/ux-review.md, U22 and U24), and the update check (docs/design/update-checks.md, UC3 to UC6).

export function HelpMenu() {
  const [open, setOpen] = useState<Point | null>(null);
  const [reference, setReference] = useState(false);
  const updates = useUpdates();
  // Interface debugging (docs/design/ui-recovery.md, UR2): a port for a debugger, from the next start.
  const [debugPort, setDebugPort] = useState('');
  useEffect(() => {
    if (open) bridge.call('settings.get', { key: 'debugPort' }).then((v) => setDebugPort(v ?? '')).catch(() => {});
  }, [open]);
  const toggleDebugging = () => {
    const next = debugPort ? '' : '9222';
    setDebugPort(next);
    bridge.call('settings.set', { key: 'debugPort', value: next }).catch(() => {});
  };

  return (
    <div className="file-menu">
      <Button className={`${open ? 'active' : ''}`} onClick={(e) => setOpen(open ? null : below(e.currentTarget))}>
        Help
      </Button>
      {open && (
        <>
          <Menu at={open} onClose={() => setOpen(null)} className="file-menu-popup">
            <MenuItem
              label="Keyboard and mouse…"
              onClick={() => {
                setOpen(null);
                setReference(true);
              }}
            />
            <div className="menu-separator" />
            <MenuItem
              label={updates.checking ? 'Checking for updates…' : 'Check for updates…'}
              disabled={updates.checking}
              onClick={() => {
                setOpen(null);
                void updates.check(true);
              }}
            />
            <MenuItem label="Check automatically" checked={updates.auto} onClick={() => updates.setAuto(!updates.auto)} />
            <MenuItem label="Include beta versions" checked={updates.beta} onClick={() => updates.setBeta(!updates.beta)} />
            <div className="menu-separator" />
            <MenuItem
              label="Reload interface"
              onClick={() => {
                setOpen(null);
                bridge.call('app.reloadUi').catch(() => {});
              }}
            />
            <MenuItem
              label="Show the log file"
              onClick={() => {
                setOpen(null);
                bridge.call('app.logFile').catch(() => {});
              }}
            />
            <MenuItem label="Allow interface debugging (from the next start)" checked={debugPort !== ''} onClick={toggleDebugging} />
          </Menu>
        </>
      )}
      {reference && <ShortcutReference onClose={() => setReference(false)} />}
    </div>
  );
}

/** A help button for one editor's toolbar: the reference, open at that editor. */
export function HelpButton({ area }: { area: string }) {
  const [open, setOpen] = useState(false);
  return (
    <>
      <IconButton size="l" title="Keyboard and mouse" aria-label="Help" onClick={() => setOpen(true)}>
        <CircleHelp size={16} />
      </IconButton>
      {open && <ShortcutReference initial={area} onClose={() => setOpen(false)} />}
    </>
  );
}

function Table({ title, rows }: { title: string; rows: Shortcut[] }) {
  if (rows.length === 0) return null;
  return (
    <section>
      <h4>{title}</h4>
      <dl>
        {rows.map(([gesture, effect]) => (
          <div key={gesture}>
            <dt>{gesture}</dt>
            <dd>{effect}</dd>
          </div>
        ))}
      </dl>
    </section>
  );
}

function ShortcutReference({ initial = SHORTCUTS[0].title, onClose }: { initial?: string; onClose: () => void }) {
  const [shown, setShown] = useState(initial);
  const area = SHORTCUTS.find((a) => a.title === shown) ?? SHORTCUTS[0];

  return (
    <Dialog className="shortcut-reference" onClose={onClose} closeOnBackdrop aria-labelledby="shortcuts-title">
      <h3 id="shortcuts-title">Keyboard and mouse</h3>
      <div className="shortcut-body">
        <nav className="shortcut-areas">
          {SHORTCUTS.map((a) => (
            <button key={a.title} className={a.title === area.title ? 'active' : ''} onClick={() => setShown(a.title)}>
              {a.title}
            </button>
          ))}
        </nav>
        <div className="shortcut-tables">
          <Table title="Mouse" rows={area.mouse} />
          <Table title="Keys" rows={area.keys} />
        </div>
      </div>
      <div className="modal-buttons">
        <span className="spacer" />
        <Button className="primary" onClick={onClose}>
          Close
        </Button>
      </div>
    </Dialog>
  );
}
