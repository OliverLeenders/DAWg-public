import { Check } from 'lucide-react';
import { useState } from 'react';
import { layout, useLayout } from '../store/layoutStore';
import { ui, useUi } from '../store/uiStore';
import { actions, useDawg } from '../store/store';
import { Button } from '../ui/controls';
import { Menu, below, type Point } from '../ui/overlays';

// The "View" menu in the top bar: show or hide the side panels, switch the centre view.

function CheckItem({ checked, label, shortcut, onClick }: { checked: boolean; label: string; shortcut?: string; onClick: () => void }) {
  return (
    <button className="menu-item with-shortcut" onClick={onClick}>
      <span className="menu-check-label">
        <span className="menu-check">{checked && <Check size={12} />}</span>
        {label}
      </span>
      {shortcut && <kbd>{shortcut}</kbd>}
    </button>
  );
}

export function ViewMenu() {
  const [open, setOpen] = useState<Point | null>(null);
  const inspectorVisible = useLayout((s) => s.inspectorVisible);
  const pluginsVisible = useLayout((s) => s.pluginsVisible);
  const dockVisible = useLayout((s) => s.dockVisible);
  const centre = useUi((s) => s.centre);
  const mixerOpen = useDawg((s) => s.mixerOpen);
  const editorOpen = useDawg((s) => s.editorOpen);

  const run = (action: () => void) => () => {
    setOpen(null);
    action();
  };

  return (
    <div className="file-menu">
      <Button className={`${open ? 'active' : ''}`} onClick={(e) => setOpen(open ? null : below(e.currentTarget))}>
        View
      </Button>
      {open && (
        <>
          <Menu at={open} onClose={() => setOpen(null)} className="file-menu-popup">
            <CheckItem checked={inspectorVisible} label="Inspector" onClick={run(layout.toggleInspector)} />
            <CheckItem checked={pluginsVisible} label="Sounds & plugins" onClick={run(layout.togglePlugins)} />
            <CheckItem checked={dockVisible} label="Device dock" onClick={run(layout.toggleDock)} />
            <CheckItem checked={mixerOpen} label="Mixer window" shortcut="Ctrl+M" onClick={run(() => actions.showMixer(!mixerOpen))} />
            <CheckItem
              checked={editorOpen}
              label="Piano roll window"
              onClick={run(() => actions.showEditor(!editorOpen))}
            />
            <div className="menu-separator" />
            <CheckItem checked={centre === 'arrangement'} label="Arrangement" shortcut="Tab" onClick={run(() => ui.setCentre('arrangement'))} />
            <CheckItem checked={centre === 'graph'} label="Graph" shortcut="Tab" onClick={run(() => ui.setCentre('graph'))} />
          </Menu>
        </>
      )}
    </div>
  );
}
