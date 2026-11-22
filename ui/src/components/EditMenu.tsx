import { useState } from 'react';
import { actions, dispatch, useDawg } from '../store/store';
import { useUi } from '../store/uiStore';
import { sendKeyToView, type ViewKey } from '../keys';
import { Button } from '../ui/controls';
import { Menu, below, type Point } from '../ui/overlays';

// The "Edit" menu in the top bar: undo and redo, and the editing commands of the centre view (the arrangement or
// the graph). Those are sent to the view as the key presses they're bound to, so a menu item and its shortcut can't
// drift apart (docs/design/ux-review.md, U24).


function Item({ label, shortcut, disabled, onClick }: { label: string; shortcut?: string; disabled?: boolean; onClick: () => void }) {
  return (
    <button className="menu-item with-shortcut" disabled={disabled} onClick={onClick}>
      <span>{label}</span>
      {shortcut && <kbd>{shortcut}</kbd>}
    </button>
  );
}

export function EditMenu() {
  const [open, setOpen] = useState<Point | null>(null);
  const history = useDawg((s) => s.history);
  const projectOpen = useDawg((s) => s.projectState.open);
  const graph = useUi((s) => s.centre === 'graph');
  const code = useUi((s) => s.centre === 'code');
  const autoCrossfadeMs = useDawg((s) => s.project?.autoCrossfadeMs ?? 0);

  const run = (action: () => void) => () => {
    setOpen(null);
    action();
  };
  const viewItem = (label: string, shortcut: string, key: ViewKey) => (
    <Item label={label} shortcut={shortcut} disabled={!projectOpen || code} onClick={run(() => sendKeyToView(graph, key))} />
  );

  return (
    <div className="file-menu">
      <Button className={`${open ? 'active' : ''}`} onClick={(e) => setOpen(open ? null : below(e.currentTarget))}>
        Edit
      </Button>
      {open && (
        <>
          <Menu at={open} onClose={() => setOpen(null)} className="file-menu-popup">
            <Item label={history.canUndo ? `Undo ${history.undoLabel}` : 'Undo'} shortcut="Ctrl+Z" disabled={!history.canUndo} onClick={run(actions.undo)} />
            <Item label={history.canRedo ? `Redo ${history.redoLabel}` : 'Redo'} shortcut="Ctrl+Shift+Z" disabled={!history.canRedo} onClick={run(actions.redo)} />
            <div className="menu-separator" />
            <div className="menu-heading">{graph ? 'Graph' : 'Arrangement'}</div>
            {viewItem('Cut', 'Ctrl+X', { key: 'x', ctrlKey: true })}
            {viewItem('Copy', 'Ctrl+C', { key: 'c', ctrlKey: true })}
            {viewItem('Paste', 'Ctrl+V', { key: 'v', ctrlKey: true })}
            {viewItem('Duplicate', 'Ctrl+D', { key: 'd', ctrlKey: true })}
            {viewItem('Delete', 'Del', { key: 'Delete' })}
            {viewItem('Select all', 'Ctrl+A', { key: 'a', ctrlKey: true })}
            <div className="menu-separator" />
            {graph ? (
              <>
                {viewItem('Bypass selected', 'Ctrl+B', { key: 'b', ctrlKey: true })}
                {viewItem('Group in a component', 'Ctrl+G', { key: 'g', ctrlKey: true })}
                {viewItem('Ungroup', 'Ctrl+Shift+G', { key: 'g', ctrlKey: true, shiftKey: true })}
              </>
            ) : (
              <>
                {viewItem('Split at playhead', 'S', { key: 's' })}
                {viewItem('Glue', 'Ctrl+J', { key: 'j', ctrlKey: true })}
                {viewItem('Loop or unloop clips', 'Ctrl+L', { key: 'l', ctrlKey: true })}
                <div className="menu-separator" />
                <Item
                  label={autoCrossfadeMs > 0 ? `Automatic crossfades: on (${autoCrossfadeMs} ms)` : 'Automatic crossfades: off'}
                  disabled={!projectOpen}
                  onClick={run(() => dispatch({ command: 'setAutoCrossfade', args: { ms: autoCrossfadeMs > 0 ? 0 : 10 } }))}
                />
              </>
            )}
          </Menu>
        </>
      )}
    </div>
  );
}
