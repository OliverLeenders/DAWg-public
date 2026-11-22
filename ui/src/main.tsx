import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import './tokens.css';
// The controls before the views' stylesheet, so a view's placement of a control wins over its defaults.
import './ui/controls.css';
import './styles/index.css';
import { projectActions, useDawg } from './store/store';
import * as bridge from './bridge/bridge';
import { useUi } from './store/uiStore';

// Development builds expose the stores for debugging and automated UI checks.
if (import.meta.env.DEV) Object.assign(window, { __dawg: { store: useDawg, ui: useUi, projectActions, bridge } });

// Right-clicking only ever shows DAWg's own menus: the browser's menu (Back, Reload, Print…) never appears. Our
// handlers run first (React listens at the root); this only stops the default.
window.addEventListener('contextmenu', (e) => e.preventDefault());

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
