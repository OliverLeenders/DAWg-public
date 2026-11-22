import { Copy, Minus, Square, X } from 'lucide-react';
import { useEffect, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import type { WindowAction } from '../bridge/protocol';
import { useDawg } from '../store/store';
import { WINDOW } from '../window';

// The title bar's job where the UI draws it (docs/design/window-chrome.md): the empty parts of a window's top bar move
// the window, and these buttons minimise, maximise and close it.

const DOUBLE_CLICK_MS = 400;

/** What moves the window: the bar itself and its empty or purely informative parts, never a control. */
const DRAG_TARGETS = '.spacer, .brand, .brand *, .group, .bar-item, .bar-grow, .project-controls, .mixer-title, .roll-title, .stat';

const control = (action: WindowAction) => bridge.call('window.control', { window: WINDOW, action });

/** Pointer handler for a top bar: drags the window from its empty parts; a double-click maximises or restores it. */
export function useWindowDrag() {
  const custom = useDawg((s) => s.app?.customTitleBar ?? false);
  const lastPress = useRef(0);

  return (e: React.PointerEvent<HTMLElement>) => {
    if (!custom || e.button !== 0) return;
    const target = e.target as Element;
    if (target !== e.currentTarget && !target.matches(DRAG_TARGETS)) return;

    const now = performance.now();
    const double = now - lastPress.current < DOUBLE_CLICK_MS;
    lastPress.current = double ? 0 : now;
    control(double ? 'maximise' : 'drag').catch(() => {});
  };
}

/** The minimise, maximise and close buttons; nothing where the window has a native title bar. */
export function WindowControls() {
  const custom = useDawg((s) => s.app?.customTitleBar ?? false);
  const [maximised, setMaximised] = useState(false);

  useEffect(() => {
    if (!custom) return;
    const update = () => {
      control('state')
        .then((state) => setMaximised(state.maximised))
        .catch(() => {});
    };
    update();
    window.addEventListener('resize', update);
    return () => window.removeEventListener('resize', update);
  }, [custom]);

  if (!custom) return null;

  const press = (action: WindowAction) => () => {
    control(action).catch(() => {});
  };

  return (
    <div className="window-controls">
      <button title="Minimise" aria-label="Minimise" onClick={press('minimise')}>
        <Minus size={15} />
      </button>
      <button title={maximised ? 'Restore' : 'Maximise'} aria-label={maximised ? 'Restore' : 'Maximise'} onClick={press('maximise')}>
        {maximised ? <Copy size={12} /> : <Square size={12} />}
      </button>
      <button className="window-close" title="Close" aria-label="Close" onClick={press('close')}>
        <X size={15} />
      </button>
    </div>
  );
}
