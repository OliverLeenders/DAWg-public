import { TICKS_PER_BEAT } from './bridge/protocol';
import { actions, dispatch, projectActions, useDawg } from './store/store';

/**
 * Whether a key press belongs to what has the focus rather than to DAWg's window-wide shortcuts: a text field, a
 * dropdown (where typing a letter picks an option), editable text, or an open dialog. Without this, typing R in a
 * focused dropdown started a recording.
 */
export function keyBelongsToFocus(e: KeyboardEvent): boolean {
  const target = e.target;
  if (target instanceof HTMLElement) {
    if (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.tagName === 'SELECT') return true;
    if (target.isContentEditable) return true;
  }
  return document.querySelector('.modal-backdrop') !== null;
}

export type ViewKey = Pick<KeyboardEventInit, 'key' | 'ctrlKey' | 'shiftKey' | 'altKey' | 'metaKey'>;

/** Sends a key press to the centre view, the arrangement's timeline or the graph canvas, as if it had the focus there:
    the Edit menu's items go through the view's own handler. */
export function sendKeyToView(graph: boolean, key: ViewKey) {
  const target = document.querySelector<HTMLElement>(graph ? '.graph-canvas' : '.arrangement-canvas');
  if (!target) return;
  target.focus({ preventScroll: true });
  target.dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, cancelable: true, ...key }));
}

/**
 * The shortcuts every project window shares, the main window and the piano roll's (docs/design/ux-review.md, U19):
 * undo, redo, save, play, record, loop, a marker and the mixer. Returns whether it took the key.
 */
export function sharedShortcut(e: KeyboardEvent): boolean {
  const mod = e.ctrlKey || e.metaKey;
  const key = e.key.toLowerCase();
  const open = useDawg.getState().projectState.open;
  const take = (action: () => void) => {
    e.preventDefault();
    action();
    return true;
  };

  if (mod && key === 'z') return take(() => (e.shiftKey ? actions.redo() : actions.undo()));
  if (mod && key === 'y') return take(actions.redo);
  if (mod && key === 's') return take(() => open && projectActions.save(e.shiftKey));
  if (mod && key === 'm') return take(() => actions.showMixer(!useDawg.getState().mixerOpen));
  if (!open) return false;
  if (e.code === 'Space') return take(actions.togglePlay);
  if (!mod && key === 'l') return take(() => dispatch({ command: 'setLoop', args: { enabled: !useDawg.getState().project?.loop?.enabled } }));
  if (!mod && key === 'r') return take(() => actions.record(e.shiftKey));
  if (!mod && key === 'p') return take(() => dispatch({ command: 'setPunch', args: { enabled: !useDawg.getState().project?.punch?.enabled } }));
  if (e.key === 'Insert') {
    const position = Math.round(useDawg.getState().telemetry.positionBeats * TICKS_PER_BEAT);
    return take(() => dispatch({ command: 'addMarker', args: { position } }));
  }
  return false;
}

/** The main window's panels, in the order F6 moves through them. */
const PANELS = ['.panel.tracks', '.centre-view', '.device-dock', '.panel.plugins'];

/** Whether the keyboard has put the focus on a control inside one of the panels, where Tab moves on to the next
    control. A control focused by a click (not :focus-visible) leaves Tab to switch the view. */
export function focusInPanel(e: KeyboardEvent) {
  const target = e.target;
  return (
    target instanceof HTMLElement &&
    target.matches(':focus-visible') &&
    !target.matches('canvas, .graph-canvas') &&
    target.closest(PANELS.join(',')) !== null
  );
}

/** F6 / Shift+F6: the focus to the next or previous panel's first control (docs/design/ux-review.md, U25). */
export function focusNextPanel(back: boolean) {
  const panels = PANELS.map((s) => document.querySelector<HTMLElement>(s)).filter((p): p is HTMLElement => p !== null);
  if (panels.length === 0) return;
  const current = panels.findIndex((p) => p.contains(document.activeElement));
  const next = panels[(current + (back ? -1 : 1) + panels.length) % panels.length] ?? panels[0];
  const first = next.querySelector<HTMLElement>('button:not(:disabled), input, select, [tabindex="0"], canvas[tabindex]');
  (first ?? next).focus({ preventScroll: true });
}
