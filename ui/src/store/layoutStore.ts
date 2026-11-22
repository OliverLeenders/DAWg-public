import { create } from 'zustand';
import type { Span } from '../dock/levelHistory';
import { isMixerWindow } from '../window';

// Window layout: which side panels are shown and how wide things are. Per user (browser storage), not part
// of the project; falls back to the defaults when storage is unavailable.

/** Which way signal flows on the graph canvas: left to right, or top to bottom. */
export type GraphDirection = 'horizontal' | 'vertical';

export interface Layout {
  inspectorVisible: boolean;
  pluginsVisible: boolean;
  inspectorWidth: number;
  pluginsWidth: number;
  /** Width of the arrangement's track header column. */
  headerWidth: number;
  /** The right panel's tab. */
  rightTab: 'sounds' | 'midiFiles' | 'plugins';
  /** The device dock under the centre view (docs/design/phase11-faust-panels.md, section 5). */
  dockVisible: boolean;
  dockHeight: number;
  /** Which way signal flows on the graph canvas. */
  graphDirection: GraphDirection;
  /** Whether nodes and cable bends snap to the graph's grid (docs/design/graph-grid-and-cables.md, G3). */
  graphSnap: boolean;
  /** The width of each node type's card in the device strip, once dragged (docs/design/device-strip.md, D5). */
  deviceWidths: Record<string, number>;
  /** The device strip's collapsed cards, as "<project path>|<track id>:<node id>" (D5), the latest last. */
  collapsedDevices: string[];
  /** The seconds the compressor panel's level history shows. */
  compressorSpan: Span;
  /** What the top bar's position display shows: bars and beats, or minutes and seconds (tempo-map.md, TM13). */
  timeDisplay: 'bars' | 'time';
  /** The notice about a Linux desktop that takes Alt+drag was dismissed (AltDragNotice.tsx). */
  altDragNoticeSeen: boolean;
}

export const LIMITS = {
  side: [200, 560] as const,
  header: [120, 380] as const,
  dock: [140, 520] as const,
};

const DEFAULTS: Layout = {
  inspectorVisible: true,
  pluginsVisible: true,
  inspectorWidth: 280,
  pluginsWidth: 320,
  headerWidth: 190,
  rightTab: 'sounds',
  dockVisible: true,
  dockHeight: 220,
  graphDirection: 'horizontal',
  graphSnap: true,
  deviceWidths: {},
  collapsedDevices: [],
  compressorSpan: 4,
  timeDisplay: 'bars',
  altDragNoticeSeen: false,
};

const STORAGE_KEY = 'dawg.layout';

/** How many collapsed cards are remembered. */
const COLLAPSED_KEPT = 500;

function load(): Layout {
  try {
    const saved = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '{}') as Partial<Layout>;
    return { ...DEFAULTS, ...saved };
  } catch {
    return DEFAULTS;
  }
}

export const useLayout = create<Layout>()(load);

useLayout.subscribe((state) => {
  // The main window's layout; the mixer window keeps its own settings (mixerStore.ts) and never writes this one.
  if (isMixerWindow) return;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(state));
  } catch {
    // Storage unavailable: the layout just isn't remembered.
  }
});

const clamp = (value: number, [min, max]: readonly [number, number]) => Math.round(Math.min(max, Math.max(min, value)));

export const layout = {
  toggleInspector: () => useLayout.setState((s) => ({ inspectorVisible: !s.inspectorVisible })),
  togglePlugins: () => useLayout.setState((s) => ({ pluginsVisible: !s.pluginsVisible })),
  setInspectorWidth: (width: number) => useLayout.setState({ inspectorWidth: clamp(width, LIMITS.side) }),
  setPluginsWidth: (width: number) => useLayout.setState({ pluginsWidth: clamp(width, LIMITS.side) }),
  setHeaderWidth: (width: number) => useLayout.setState({ headerWidth: clamp(width, LIMITS.header) }),
  setRightTab: (rightTab: Layout['rightTab']) => useLayout.setState({ rightTab }),
  toggleDock: () => useLayout.setState((s) => ({ dockVisible: !s.dockVisible })),
  setDockHeight: (height: number) => useLayout.setState({ dockHeight: clamp(height, LIMITS.dock) }),
  setGraphDirection: (graphDirection: GraphDirection) => useLayout.setState({ graphDirection }),
  setGraphSnap: (graphSnap: boolean) => useLayout.setState({ graphSnap }),
  /** Collapses a card in the device strip or opens it again. Only the latest are remembered, so the list can't grow without end. */
  toggleDeviceCollapsed: (key: string) =>
    useLayout.setState((s) => ({
      collapsedDevices: s.collapsedDevices.includes(key)
        ? s.collapsedDevices.filter((k) => k !== key)
        : [...s.collapsedDevices, key].slice(-COLLAPSED_KEPT),
    })),
  /** Collapses or opens several cards at once ("Collapse all except selected", macros-and-modulation-column.md, MC15). */
  setDevicesCollapsed: (cards: Array<{ key: string; collapsed: boolean }>) =>
    useLayout.setState((s) => {
      const open = new Set(cards.filter((c) => !c.collapsed).map((c) => c.key));
      const kept = s.collapsedDevices.filter((k) => !open.has(k) && !cards.some((c) => c.key === k));
      return { collapsedDevices: [...kept, ...cards.filter((c) => c.collapsed).map((c) => c.key)].slice(-COLLAPSED_KEPT) };
    }),
  setDeviceWidth: (type: string, width: number) =>
    useLayout.setState((s) => ({ deviceWidths: { ...s.deviceWidths, [type]: Math.round(width) } })),
};
