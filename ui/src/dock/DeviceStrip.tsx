import { ChevronLeft, Code, CornerLeftUp, ExternalLink, Layers, Music, Plus, RotateCw, Waves } from 'lucide-react';
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { NODE_TYPES, type GraphNode, type Track } from '../bridge/protocol';
import { isInstrument, nodeName, reorderIndex, trackChain } from '../components/chain';
import { DeviceMenu, isRemovable, removeDevice } from '../components/DeviceMenu';
import { addToTrack, chainRole, draggedPaletteNode, isPaletteDrag } from '../components/paletteDrag';
import { PluginPicker, pickedComponent, type PickedNode } from '../components/PluginPicker';
import { isOutputsHeader } from '../arrangement/groups';
import { focusOf } from '../graph/focus';
import { paramApplies } from '../graph/lfo';
import { infoOf } from '../graph/nodeInfo';
import { findNode, graphOf, ownerChain } from '../graph/voice';
import { actions, dispatch, reportError, useDawg } from '../store/store';
import { layout, useLayout } from '../store/layoutStore';
import { ui, useUi } from '../store/uiStore';
import { useWatchNodes } from './analysis';
import { clampDeviceWidth, deviceWidth, widthKey, type DeviceWidth } from './deviceWidths';
import { FaustControls } from './FaustControls';
import { KeySelector } from './KeySelector';
import { ParamSlot } from './Knob';
import { panelFor } from './panels';
import { PanelMinWidth, useMeasuredMinWidth } from './panelMinWidth';
import { hasPresetsFor } from './presets';
import { PresetSelector } from './PresetSelector';
import { stripDevices, stripLinks, type StripLink } from './strip';
import { useScrollFade } from '../ui/useScrollFade';
import { Button, IconButton, Power } from '../ui/controls';
import { Menu } from '../ui/overlays';

// The device strip (docs/design/device-strip.md): a track's devices side by side in signal order, scrolling sideways,
// each card as wide as its node type was last dragged to (D5). On a custom graph the cards stand apart, and an arrow in
// the gap between two says the first feeds the second, a route line above the cards draws the cables that run around others, and hovering a card
// marks what it connects to (D2). Like the graph view, it shows what a component or Voice Instrument holds while the graph
// view is inside it (D4). A card can be collapsed to its name (D5). Only the cards on screen or close to it mount their
// panels, and only those on screen run their analysers (D8): every spectrum redraws 30 times a second.

/** How far beyond the visible part of the strip a card still counts as near, and keeps its panel. */
const NEAR_MARGIN = '0px 400px';

/** The ids of the cards within `margin` of the strip's visible part, kept up to date as it scrolls. */
function useCardsIn(scroller: React.RefObject<HTMLDivElement | null>, ids: string, margin: string): ReadonlySet<number> {
  const [inside, setInside] = useState<ReadonlySet<number>>(new Set());
  useEffect(() => {
    const root = scroller.current;
    if (!root) return;
    const observer = new IntersectionObserver(
      (entries) =>
        setInside((previous) => {
          const next = new Set(previous);
          for (const entry of entries) {
            const id = Number((entry.target as HTMLElement).dataset.node);
            if (entry.isIntersecting) next.add(id);
            else next.delete(id);
          }
          return next;
        }),
      { root, rootMargin: margin },
    );
    root.querySelectorAll('[data-node]').forEach((card) => observer.observe(card));
    return () => observer.disconnect();
  }, [scroller, ids, margin]);
  return inside;
}

/** The two lists a simple chain reorders: MIDI effects before the instrument, audio effects after it. */
type Section = 'midi' | 'audio';

/** A card being dragged to another place in a simple chain (D10), and where it would land. */
interface CardDrag {
  dragging: boolean;
  start: (event: React.DragEvent) => void;
  end: () => void;
  over: (event: React.DragEvent) => void;
  drop: (event: React.DragEvent) => void;
}

/** How wide a collapsed card is: its power switch over its name, written upwards. */
const COLLAPSED_WIDTH = 28;

/** Whether a plain wheel over `target` belongs to something inside the strip: a control that takes the wheel, or an
    element between it and the strip that can still scroll the way the wheel turns. */
export function wheelStaysInside(target: EventTarget | null, root: Element, deltaY: number): boolean {
  for (let element = target instanceof Element ? target : null; element && element !== root; element = element.parentElement) {
    if (element instanceof HTMLElement && element.dataset.takesWheel !== undefined) return true;
    const overflow = getComputedStyle(element).overflowY;
    if ((overflow === 'auto' || overflow === 'scroll') && element.scrollHeight > element.clientHeight + 1) {
      const atTop = element.scrollTop <= 0;
      const atBottom = element.scrollTop + element.clientHeight >= element.scrollHeight - 1;
      if ((deltaY < 0 && !atTop) || (deltaY > 0 && !atBottom)) return true;
    }
  }
  return false;
}

export function DeviceStrip({ track, scope }: { track: Track; scope?: number }) {
  const types = useDawg((s) => s.nodeTypes);
  const projectPath = useDawg((s) => s.projectState.path);
  const collapsedKeys = useLayout((s) => s.collapsedDevices);
  // The level shown, as a track of its own: a component's or Voice Instrument's graph, or the track's.
  const inner = scope === undefined ? undefined : graphOf(track, scope);
  const shown = useMemo(() => (inner ? { ...track, graph: inner } : track), [track, inner]);
  // At the track's level, instruments are in the inspector and their own window, and sources in the modulation column
  // (docs/design/macros-and-modulation-column.md, MC12, MC14); inside a component or Voice Instrument they stay here.
  const { devices, modulation, simple } = useMemo(() => {
    const all = stripDevices(shown, types);
    return scope === undefined ? { ...all, devices: all.devices.filter((n) => !isInstrument(n)), modulation: [] } : all;
  }, [shown, types, scope]);
  const links = useMemo(() => (simple ? [] : stripLinks(shown, devices, types)), [simple, shown, devices, types]);
  const [hovered, setHovered] = useState<number | null>(null);
  // Modulation counts too: hovering an LFO marks what it modulates.
  const focus = useMemo(
    () => (hovered === null ? null : focusOf(Object.values(shown.graph.connections), new Set([hovered]))),
    [hovered, shown],
  );
  // On a simple chain the order already shows the audio route, so hovering only marks modulation links (D2).
  const modulationIds = new Set(modulation.map((n) => n.id));
  const linkedTo = (id: number) =>
    id !== hovered &&
    hovered !== null &&
    (focus?.nodes.has(id) ?? false) &&
    (!simple || modulationIds.has(id) || modulationIds.has(hovered));
  const collapseKey = (node: GraphNode) => `${projectPath}|${track.id}:${node.id}`;
  // "Collapse all except selected" (MC15): every card but the selected one, or the one right-clicked.
  const [stripMenu, setStripMenu] = useState<{ x: number; y: number } | null>(null);
  const collapseOthers = (keep: number | null) =>
    layout.setDevicesCollapsed(
      [...devices, ...modulation].map((n) => ({ key: collapseKey(n), collapsed: n.id !== keep })),
    );
  // Adding and reordering only on a simple chain, where the order is the signal's route (D10). Indices are the
  // effects', as insertNode and reorderNode take them.
  const chain = simple && scope === undefined ? trackChain(track) : null;
  const effects = chain?.effects ?? [];
  // MIDI effects have a section of their own before the instrument (docs/design/midi-effects.md, ME3).
  const midiEffects = chain?.midiEffects ?? [];
  // A track with output tracks is only their header: effects go on its output tracks (multi-output-instruments.md, MO12).
  const header = useDawg((s) => isOutputsHeader(s.project, track.id));
  const [dragId, setDragId] = useState<number | null>(null);
  const [dropAt, setDropAt] = useState<{ section: Section; before: number } | null>(null);
  const [picker, setPicker] = useState<{ kind: 'instrument' | 'midiEffect' | 'effect'; x: number; y: number; index?: number } | null>(null);
  const [menu, setMenu] = useState<{ node: GraphNode; x: number; y: number } | null>(null);
  // A browser item over the strip, but not over a place in the chain: it goes to the instrument slot or the end.
  const [paletteOver, setPaletteOver] = useState(false);
  const collapsed = new Set(collapsedKeys);
  const row = useRef<HTMLDivElement>(null);
  const selected = useUi((s) => (s.selectedNode?.trackId === track.id ? s.selectedNode.nodeId : null));
  const scroller = useRef<HTMLDivElement>(null);
  useScrollFade(scroller);
  const ids = [...devices, ...modulation].map((n) => n.id).join(',');
  const near = useCardsIn(scroller, ids, NEAR_MARGIN);
  const visible = useCardsIn(scroller, ids, '0px');
  const spans = useCardSpans(row, ids, links.length > 0);
  const lane = links.filter((link) => !link.adjacent);
  const faded = (link: StripLink) => (focus && !focus.cables.has(link.key) ? 'faded' : '');
  // A selection made in the strip itself is already in view: scrolling to it would move the card under the pointer.
  const selectedHere = useRef<number | null>(null);

  // The wheel scrolls the strip sideways (D9): Shift+wheel anywhere, a plain wheel unless it's over a control that takes
  // it (a field clicked to nudge, the EQ's graph: marked data-takes-wheel) or a list in a card that can still scroll
  // that way. React's wheel listeners are passive, so this one is added by hand to keep the page from scrolling too.
  useEffect(() => {
    const root = scroller.current;
    if (!root) return;
    const onWheel = (event: WheelEvent) => {
      const step = event.deltaMode === WheelEvent.DOM_DELTA_LINE ? 16 : event.deltaMode === WheelEvent.DOM_DELTA_PAGE ? root.clientWidth : 1;
      if (event.shiftKey) {
        event.preventDefault();
        event.stopPropagation();
        root.scrollLeft += (event.deltaY || event.deltaX) * step;
        return;
      }
      // A touchpad's sideways swipe scrolls the strip by itself.
      if (Math.abs(event.deltaX) >= Math.abs(event.deltaY)) return;
      if (wheelStaysInside(event.target, root, event.deltaY)) return;
      event.preventDefault();
      root.scrollLeft += event.deltaY * step;
    };
    root.addEventListener('wheel', onWheel, { capture: true, passive: false });
    return () => root.removeEventListener('wheel', onWheel, { capture: true });
  }, []);

  // A node selected elsewhere (the graph, the inspector) is scrolled to (D6).
  useEffect(() => {
    const here = selectedHere.current === selected;
    selectedHere.current = null;
    if (selected === null || here) return;
    scroller.current
      ?.querySelector(`[data-node="${selected}"]`)
      ?.scrollIntoView({ inline: 'nearest', block: 'nearest', behavior: 'smooth' });
  }, [selected]);

  // Plain knobs, plugin cards and collapsed cards draw no analysis.
  useWatchNodes(
    [...devices, ...modulation]
      .filter((n) => visible.has(n.id) && panelFor(n.type) && !collapsed.has(collapseKey(n)))
      .map((n) => ({ trackId: track.id, nodeId: n.id })),
  );

  const openPicker = (event: React.MouseEvent, kind: 'instrument' | 'midiEffect' | 'effect', index?: number) => {
    const rect = event.currentTarget.getBoundingClientRect();
    setPicker({ kind, x: rect.left, y: rect.bottom, index });
  };

  const pick = (picked: PickedNode) => {
    const at = picker;
    setPicker(null);
    if (!at) return;
    void pickedComponent(picked)
      .then((component) =>
        dispatch({
          command: 'insertNode',
          args: { trackId: track.id, type: picked.type, plugin: picked.plugin, code: picked.code, component, index: at.index },
        }),
      )
      .catch(reportError);
  };

  const listOf = (section: Section) => (section === 'midi' ? midiEffects : effects);

  // What can land in a gap or on a card of a section: one of its effects, or an effect of its kind from the browser.
  const accepts = (event: React.DragEvent, section: Section) => {
    if (dragId !== null) return listOf(section).some((e) => e.id === dragId);
    const picked = draggedPaletteNode();
    if (section === 'audio' && header) return false;
    return isPaletteDrag(event.dataTransfer) && picked !== null && chainRole(picked) === (section === 'midi' ? 'midiEffect' : 'effect');
  };

  const isDropAt = (section: Section, before: number) => dropAt?.section === section && dropAt.before === before;

  const drop = (section: Section, before: number) => {
    const nodeId = dragId;
    setDragId(null);
    setDropAt(null);
    const picked = draggedPaletteNode();
    if (nodeId === null) {
      if (picked) addToTrack(track, picked, before);
      return;
    }
    const from = listOf(section).findIndex((e) => e.id === nodeId);
    if (from < 0) return;
    const to = reorderIndex(from, before);
    if (to !== from) dispatch({ command: 'reorderNode', args: { trackId: track.id, nodeId: nodeId!, index: to } });
  };

  /** Where a dragged card can land: before the effect at `before`, or after the last at its list's length. */
  const dropTarget = (section: Section, before: number) => ({
    onDragOver: (event: React.DragEvent) => {
      if (!accepts(event, section)) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = dragId === null ? 'copy' : 'move';
      setDropAt({ section, before });
    },
    onDrop: (event: React.DragEvent) => {
      event.preventDefault();
      drop(section, before);
    },
  });

  // Over a card, its left half lands the dragged one before it, its right half after it.
  const dragOf = (effect: GraphNode, i: number, section: Section = 'audio'): CardDrag => ({
    dragging: dragId === effect.id,
    start: (event) => {
      event.dataTransfer.effectAllowed = 'move';
      event.dataTransfer.setData('text/plain', String(effect.id));
      setDragId(effect.id);
      setDropAt(null);
    },
    end: () => {
      setDragId(null);
      setDropAt(null);
    },
    over: (event) => {
      if (!accepts(event, section)) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = dragId === null ? 'copy' : 'move';
      const rect = event.currentTarget.getBoundingClientRect();
      setDropAt({ section, before: event.clientX < rect.left + rect.width / 2 ? i : i + 1 });
    },
    drop: (event) => {
      event.preventDefault();
      drop(section, dropAt?.section === section ? dropAt.before : i);
    },
  });

  /** The gap before the effect at `before`: an effect is added there, and a dragged card lands there. */
  const gap = (before: number, section: Section = 'audio') => (
    <div key={`gap:${section}:${before}`} className={`strip-gap ${isDropAt(section, before) ? 'drop' : ''}`} {...dropTarget(section, before)}>
      {(section === 'midi' || !header) && (
        <button
          className="strip-insert"
          title={section === 'midi' ? 'Add a MIDI effect here' : 'Add an effect here'}
          onClick={(event) => openPicker(event, section === 'midi' ? 'midiEffect' : 'effect', before)}
        >
          <Plus size={11} />
        </button>
      )}
    </div>
  );

  const card = (node: GraphNode, drag?: CardDrag) => (
    <DeviceCard
      key={node.id}
      drag={drag}
      track={track}
      node={node}
      near={near.has(node.id)}
      collapsed={collapsed.has(collapseKey(node))}
      onCollapse={() => layout.toggleDeviceCollapsed(collapseKey(node))}
      selected={selected === node.id}
      linked={linkedTo(node.id)}
      onHover={(over) => setHovered((current) => (over ? node.id : current === node.id ? null : current))}
      onSelect={() => {
        selectedHere.current = node.id;
        ui.selectNode({ trackId: track.id, nodeId: node.id });
        // Delete then removes it, unless a control in the card takes the focus (an EQ curve deletes its band).
        if (!scroller.current?.contains(document.activeElement)) scroller.current?.focus({ preventScroll: true });
      }}
      onMenu={(event) => {
        // A panel inside the card with a menu of its own (EQ bands, sampler zones) has handled the click already.
        if (event.defaultPrevented) return;
        event.preventDefault();
        setMenu({ node, x: event.clientX, y: event.clientY });
      }}
    />
  );

  const onKeyDown = (event: React.KeyboardEvent) => {
    // Only Delete: Backspace is for text, and a slip in a value field mustn't take a device with it.
    if (event.key !== 'Delete') return;
    const target = event.target as HTMLElement;
    if (target !== scroller.current && !target.closest('.device-card-header')) return;
    const node = [...devices, ...modulation].find((n) => n.id === selected);
    if (!node || !isRemovable(track, node)) return;
    event.preventDefault();
    removeDevice(track, node);
  };

  return (
    <>
      {/* Inside a component: where the strip is, as a row over it, read left to right (design-system.md, step 7). */}
      {scope !== undefined && <ScopeBar track={track} scope={scope} />}
      <div
        className={`device-strip scroll-fade ${paletteOver ? 'drop' : ''}`}
        ref={scroller}
        tabIndex={-1}
        onKeyDown={onKeyDown}
        onContextMenu={(event) => {
          // A card's own menu has it too; this is the strip's empty part.
          if (event.defaultPrevented) return;
          event.preventDefault();
          setStripMenu({ x: event.clientX, y: event.clientY });
        }}
        onDragOver={(event) => {
          // Anywhere else in the strip, a browser item goes where it belongs: an instrument in the instrument slot, an
          // effect after the others. Inside a component or Voice Instrument it's wired in the graph view.
          if (!isPaletteDrag(event.dataTransfer) || scope !== undefined) return;
          // A gap or a card has taken it.
          if (event.defaultPrevented) return setPaletteOver(false);
          event.preventDefault();
          event.dataTransfer.dropEffect = 'copy';
          setDropAt(null);
          setPaletteOver(true);
        }}
        onDragLeave={(event) => {
          if (scroller.current?.contains(event.relatedTarget as Node)) return;
          setPaletteOver(false);
          setDropAt(null);
        }}
        onDrop={(event) => {
          setPaletteOver(false);
          const picked = draggedPaletteNode();
          if (event.defaultPrevented || !picked || !isPaletteDrag(event.dataTransfer) || scope !== undefined) return;
          event.preventDefault();
          setDropAt(null);
          addToTrack(track, picked);
        }}
      >
        {!chain && devices.length === 0 && modulation.length === 0 && (
          <p className="dock-empty strip-empty">{scope === undefined ? 'This track has no devices.' : 'Nothing in here yet.'}</p>
        )}
        <div className="strip-content">
          {lane.length > 0 && <RouteLine spans={spans} links={lane} faded={faded} />}
          <div className={`strip-row apart ${simple ? 'simple' : ''}`} ref={row} style={simple ? undefined : { gap: CARD_GAP }}>
            {links
              .filter((link) => link.adjacent && spans.has(link.to))
              .map((link) => (
                <svg key={link.key} className={`strip-step ${faded(link)}`} style={{ left: spans.get(link.to)!.left - CARD_GAP }} width={CARD_GAP} height={9}>
                  <path d={`M 0 4.5 H ${CARD_GAP} M ${CARD_GAP - 0.5} 4.5 l -4 -3.5 M ${CARD_GAP - 0.5} 4.5 l -4 3.5`} />
                </svg>
              ))}
            {chain ? (
              <>
                {track.kind === 'instrument' &&
                  (midiEffects.length === 0 ? (
                    <button
                      className={`strip-add strip-add-midi ${isDropAt('midi', 0) ? 'drop' : ''}`}
                      title="Add a MIDI effect: notes in, notes out, before the instrument"
                      {...dropTarget('midi', 0)}
                      onClick={(event) => openPicker(event, 'midiEffect', 0)}
                    >
                      <Plus size={14} />
                      <span>MIDI effect</span>
                    </button>
                  ) : (
                    <>
                      <div className="strip-divider" title="MIDI effects: notes in, notes out" aria-label="MIDI effects">
                        <Music size={12} />
                      </div>
                      {midiEffects.map((effect, i) => [gap(i, 'midi'), card(effect, dragOf(effect, i, 'midi'))])}
                      {gap(midiEffects.length, 'midi')}
                    </>
                  ))}
                {/* The instrument is in the inspector and its own window (MC14); a track without one can still choose it here. */}
                {!chain.instrument && track.kind === 'instrument' && (
                  <button className="strip-add" title="Choose the track's instrument" onClick={(event) => openPicker(event, 'instrument')}>
                    <Plus size={14} />
                    <span>Instrument</span>
                  </button>
                )}
                {effects.map((effect, i) => [gap(i), card(effect, dragOf(effect, i))])}
                {header ? (
                  <p className="strip-header-note">This track only feeds its output tracks: add effects to those.</p>
                ) : (
                  <button
                    className={`strip-add ${isDropAt('audio', effects.length) ? 'drop' : ''}`}
                    {...dropTarget('audio', effects.length)}
                    title="Add an audio effect at the end of the chain"
                    onClick={(event) => openPicker(event, 'effect', effects.length)}
                  >
                    <Plus size={14} />
                    <span>Effect</span>
                  </button>
                )}
                {/* The Gain node, while it's modulated. */}
                {devices.filter((n) => n !== chain.instrument && !effects.includes(n) && !midiEffects.includes(n)).map((n) => card(n))}
              </>
            ) : (
              devices.map((n) => card(n))
            )}
            {modulation.length > 0 && (
              <div className="strip-divider" title="Modulation: not in the signal path" aria-label="Modulation">
                <Waves size={12} />
              </div>
            )}
            {modulation.map((n) => card(n))}
          </div>
        </div>
        {picker && <PluginPicker kind={picker.kind} x={picker.x} y={picker.y} onPick={pick} onClose={() => setPicker(null)} />}
        {menu && (
          <DeviceMenu
            track={track}
            node={menu.node}
            x={menu.x}
            y={menu.y}
            onClose={() => setMenu(null)}
            onCollapseOthers={() => collapseOthers(menu.node.id)}
          />
        )}
        {stripMenu && (
          <>
            <Menu onClose={() => setStripMenu(null)} at={{ x: stripMenu.x, y: stripMenu.y }}>
              <button
                className="menu-item"
                onClick={() => {
                  setStripMenu(null);
                  collapseOthers(selected);
                }}
              >
                Collapse all except selected
              </button>
            </Menu>
          </>
        )}
      </div>
    </>
  );
}

/** Where the strip is, when inside a component or Voice Instrument (D4): the way back, and the path from the track,
    written upwards and kept at the strip's left edge as it scrolls. */
function ScopeBar({ track, scope }: { track: Track; scope: number }) {
  const types = useDawg((s) => s.nodeTypes);
  const owner = findNode(track, scope);
  const outer = ownerChain(track, scope) ?? [];
  const parent = outer.at(-1);
  const parentName = parent ? nodeName(parent, types) : track.kind === 'master' ? 'Master' : track.name;

  return (
    <nav className="strip-scope" aria-label="Devices of">
      <IconButton size="s" title={`Back to ${parentName}`} onClick={() => ui.leaveGraph(parent?.id)}>
        <CornerLeftUp size={12} />
      </IconButton>
      <span className="strip-scope-path">
        <Button title="Back to the track's devices" onClick={() => ui.leaveGraph()}>
          {track.kind === 'master' ? 'Master' : track.name}
        </Button>
        {outer.map((node) => (
          <span key={node.id}>
            {' › '}
            <Button onClick={() => ui.enterGraph(track.id, node.id)}>
              {nodeName(node, types)}
            </Button>
          </span>
        ))}
        {owner && <strong>{` › ${nodeName(owner, types)}`}</strong>}
      </span>
    </nav>
  );
}

function DeviceCard({
  track,
  node,
  drag,
  near,
  collapsed,
  onCollapse,
  selected,
  linked,
  onHover,
  onSelect,
  onMenu,
}: {
  track: Track;
  node: GraphNode;
  /** On a simple chain, an effect is dragged by its header (or a collapsed card as a whole) to reorder it. */
  drag?: CardDrag;
  near: boolean;
  /** Shown as its name only (D5). */
  collapsed: boolean;
  onCollapse: () => void;
  selected: boolean;
  /** Connected to the hovered card. */
  linked: boolean;
  onHover: (over: boolean) => void;
  onSelect: () => void;
  /** The device menu every view shares. */
  onMenu: (event: React.MouseEvent) => void;
}) {
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const info = infoOf(track.id, node, types, statuses);
  const status = statuses[`${track.id}:${node.id}`];
  // A component's own controls are the ones pinned on it.
  const knobs = node.component ? node.component.exposed.length : (info?.params.filter((param) => paramApplies(node, param.id)).length ?? 0);
  // The card is never narrower than its panel needs at the dock's current height (panelMinWidth.ts): measured, or
  // reported by a panel that trades space itself (the compressor, whose curve shrinks). The type's minimum is the
  // floor, and the card's padding and borders come on top.
  const body = useRef<HTMLDivElement>(null);
  const [reported, setReported] = useState(0);
  const reportMin = useCallback((min: number) => setReported(Math.ceil(min)), []);
  const measured = useMeasuredMinWidth(body, near && !collapsed && reported === 0);
  const panelMin = (reported || measured) > 0 ? (reported || measured) + CARD_CHROME : 0;
  const typeWidth = deviceWidth(node.type, knobs);
  const device = panelMin > typeWidth.min ? { ...typeWidth, min: panelMin } : typeWidth;
  const sizeKey = widthKey(node);
  const saved = useLayout((s) => s.deviceWidths[sizeKey]);
  const width = clampDeviceWidth(saved ?? device.initial, device);
  const named = node.plugin !== undefined || node.type === NODE_TYPES.voiceInstrument;
  const isFaust = node.type === NODE_TYPES.faust;
  const editing = useUi((s) => s.centre === 'code' && s.codeNode?.trackId === track.id && s.codeNode.nodeId === node.id);
  const name = nodeName(node, types);
  const power = info?.userEditable && (
    <Power
      size="s"
      name={name}
      on={!node.bypassed}
      onChange={(on) => dispatch({ command: 'setNodeBypass', args: { trackId: track.id, nodeId: node.id, bypassed: !on } })}
    />
  );
  const classes = `device-card ${selected ? 'selected' : ''} ${linked ? 'linked' : ''} ${node.bypassed ? 'bypassed' : ''} ${drag?.dragging ? 'dragging' : ''}`;
  const dropHere = drag ? { onDragOver: drag.over, onDrop: drag.drop } : {};
  const dragFrom = drag ? { draggable: true, onDragStart: drag.start, onDragEnd: drag.end } : {};

  if (collapsed) {
    return (
      <section
        data-node={node.id}
        className={`${classes} collapsed`}
        style={{ width: COLLAPSED_WIDTH }}
        {...dropHere}
        {...dragFrom}
        onContextMenu={onMenu}
        onPointerDownCapture={onSelect}
        onPointerEnter={() => onHover(true)}
        onPointerLeave={() => onHover(false)}
      >
        {power}
        <button className="device-card-expand" title={`Show ${name}`} onClick={onCollapse}>
          {name}
        </button>
      </section>
    );
  }

  return (
    <section
      data-node={node.id}
      className={classes}
      style={{ width }}
      {...dropHere}
      onContextMenu={onMenu}
      onPointerDownCapture={onSelect}
      onPointerEnter={() => onHover(true)}
      onPointerLeave={() => onHover(false)}
    >
      <header className="device-card-header" {...dragFrom} title={drag ? 'Drag to move it in the chain · right-click for more' : 'Right-click for more'}>
        {power}
        {node.type === NODE_TYPES.feedback && (
          <span className="device-card-loop" title="Feeds a loop back to an earlier device">
            ↺
          </span>
        )}
        <span className="device-card-name" title={name}>
          {name}
        </span>
        {isFaust && status?.status === 'failed' && <span className="badge failed">Error</span>}
        {info && hasPresetsFor(node.type) && <PresetSelector key={`${track.id}:${node.id}`} track={track} node={node} info={info} compact />}
        {(node.type === NODE_TYPES.lfo || node.type === NODE_TYPES.adsr) && <KeySelector track={track} node={node} label="Trigger" />}
        {isFaust && (
          // The code editor opens only from a Faust node (docs/design/phase11-faust-panels.md, section 5).
          <IconButton
            size="s"
            className={`${editing ? 'active' : ''}`}
            title={editing ? 'Close the code editor' : "Edit this node's script in the code editor"}
            onClick={() => (editing ? ui.closeCode() : ui.editCode(track.id, node.id))}
          >
            <Code size={12} />
          </IconButton>
        )}
        <IconButton size="s" className="device-card-collapse" title="Collapse to its name" onClick={onCollapse}>
          <ChevronLeft size={12} />
        </IconButton>
      </header>
      <div className="device-card-body" ref={body}>
        {near && (
          <PanelMinWidth.Provider value={reportMin}>
            <DeviceBody track={track} node={node} />
          </PanelMinWidth.Provider>
        )}
      </div>
      {!named && <ResizeEdge sizeKey={sizeKey} width={width} device={device} />}
    </section>
  );
}

/** What a card shows under its header: the device's panel, or a way to open what has a window or graph of its own. */
function DeviceBody({ track, node }: { track: Track; node: GraphNode }) {
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const info = infoOf(track.id, node, types, statuses);
  const status = statuses[`${track.id}:${node.id}`];
  const Panel = panelFor(node.type);

  if (node.plugin) return <PluginBody track={track} node={node} />;
  if (node.type === NODE_TYPES.faust) {
    if (status?.program) return <FaustControls track={track} node={node} program={status.program} />;
    return <p className="dock-empty">{status?.status === 'compiling' ? 'Compiling…' : 'The script has not compiled yet.'}</p>;
  }
  if (Panel && info) return <Panel key={`${track.id}:${node.id}`} track={track} node={node} info={info} />;
  if (node.type === NODE_TYPES.voiceInstrument) {
    return (
      <div className="device-card-named">
        <Button title="Plays its voice graph once per note: open it" onClick={() => ui.enterGraph(track.id, node.id)}>
          <Layers size={12} /> Open
        </Button>
      </div>
    );
  }
  const params = info?.params.filter((param) => paramApplies(node, param.id)) ?? [];
  if (params.length === 0) return <p className="dock-empty">No parameters.</p>;
  return (
    <div className="dock-knobs">
      {params.map((param) => (
        <ParamSlot key={param.id} track={track} node={node} param={param} />
      ))}
    </div>
  );
}

/** A plugin's card: it's edited in its own window (D3), so this only opens it and says how it's doing. */
function PluginBody({ track, node }: { track: Track; node: GraphNode }) {
  const status = useDawg((s) => s.nodes[`${track.id}:${node.id}`]);
  const crashed = status?.status === 'crashed';
  const failed = status?.status === 'failed';

  return (
    <div className="device-card-named">
      {node.plugin?.manufacturer && <span className="dim">{node.plugin.manufacturer}</span>}
      {status?.status === 'ready' && (
        <Button
          className={`${status.editorOpen ? 'active' : ''}`}
          title={status.editorOpen ? "Close the plugin's window" : "Open the plugin's window"}
          onClick={() => (status.editorOpen ? actions.closeEditor(track.id, node.id) : actions.openEditor(track.id, node.id))}
        >
          <ExternalLink size={12} /> {status.editorOpen ? 'Close' : 'Open'}
        </Button>
      )}
      {status?.status === 'loading' && <span className="dim">Loading…</span>}
      {(crashed || failed) && (
        <span className="badge failed" title={status?.error}>
          {crashed ? 'Crashed' : 'Failed to load'}
        </span>
      )}
      {crashed && (
        <Button title="Load the plugin again" onClick={() => actions.reloadPlugin(track.id, node.id)}>
          <RotateCw size={12} /> Reload
        </Button>
      )}
      {(status?.latency ?? 0) > 0 && <span className="badge">{status!.latency} samples latency</span>}
      {node.type === NODE_TYPES.pluginEffect && status?.hasSidechain && <KeySelector track={track} node={node} />}
    </div>
  );
}

/** The card's right edge: dragging it sets the width of every card with its size key (deviceWidths.ts, widthKey),
    double-clicking resets it. */
function ResizeEdge({ sizeKey, width, device }: { sizeKey: string; width: number; device: DeviceWidth }) {
  const start = useRef<{ x: number; width: number } | null>(null);
  return (
    <div
      className="device-card-resize"
      role="separator"
      aria-orientation="vertical"
      aria-label="Resize the device"
      onPointerDown={(event) => {
        event.preventDefault();
        event.stopPropagation();
        event.currentTarget.setPointerCapture(event.pointerId);
        start.current = { x: event.clientX, width };
      }}
      onPointerMove={(event) => {
        if (!start.current || !event.currentTarget.hasPointerCapture(event.pointerId)) return;
        layout.setDeviceWidth(sizeKey, clampDeviceWidth(start.current.width + event.clientX - start.current.x, device));
      }}
      onPointerUp={(event) => {
        start.current = null;
        event.currentTarget.releasePointerCapture(event.pointerId);
      }}
      onPointerCancel={() => (start.current = null)}
      onDoubleClick={() => layout.setDeviceWidth(sizeKey, device.initial)}
    />
  );
}

/** The gap between the cards of a custom graph, which an arrow from one card to the next fills, border to border. */
const CARD_GAP = 14;
/** A card's body padding (12 px each side) and borders (1 px each side), around its panel. */
const CARD_CHROME = 26;
/** How tall the route line is: the run of a cable sits on one of these levels above the cards, and drops at least 8 px
    to its card, so the arrowhead sits on a straight stem rather than in the bend. */
const ROUTE_HEIGHT = 18;
const ROUTE_LEVELS = [10, 6, 2];
/** How far in from a card's edge a cable leaves or arrives. */
const ROUTE_INSET = 14;

interface CardSpan {
  left: number;
  right: number;
}

/** The cards' places in the row, measured again whenever the row or a card changes size. */
function useCardSpans(row: React.RefObject<HTMLDivElement | null>, ids: string, wanted: boolean): Map<number, CardSpan> {
  const [spans, setSpans] = useState<Map<number, CardSpan>>(new Map());
  useLayoutEffect(() => {
    const element = row.current;
    if (!element || !wanted) return;
    const measure = () => {
      const next = new Map<number, CardSpan>();
      element.querySelectorAll<HTMLElement>('[data-node]').forEach((card) =>
        next.set(Number(card.dataset.node), { left: card.offsetLeft, right: card.offsetLeft + card.offsetWidth }),
      );
      setSpans(next);
    };
    measure();
    // A card can change size without the row doing so: one collapsed while the row fills the strip.
    const observer = new ResizeObserver(measure);
    observer.observe(element);
    element.querySelectorAll('[data-node]').forEach((card) => observer.observe(card));
    return () => observer.disconnect();
  }, [row, ids, wanted]);
  return spans;
}

/** The cables that run around other cards, drawn above them (D2): a cable leaves near its card's right edge and drops in
    near the left edge of the card it feeds; a loop's way back runs dashed from centre to centre. Clicking it opens the
    graph. */
function RouteLine({
  spans,
  links,
  faded,
}: {
  spans: Map<number, CardSpan>;
  links: StripLink[];
  faded: (link: StripLink) => string;
}) {
  const routes = useMemo(() => layoutRoutes(links, spans), [links, spans]);
  const width = Math.max(0, ...[...spans.values()].map((span) => span.right));

  return (
    <svg className="strip-route" width={width} height={ROUTE_HEIGHT} onClick={() => ui.setCentre('graph')}>
      <title>The cables between these devices. Click to open the graph.</title>
      {routes.map(({ link, x1, x2, y }) => (
        <path
          key={link.key}
          className={`strip-cable ${link.back ? 'back' : ''} ${faded(link)}`}
          d={`M ${x1} ${ROUTE_HEIGHT} V ${y} H ${x2} V ${ROUTE_HEIGHT - 1} m -3 -3 l 3 3 l 3 -3`}
        />
      ))}
    </svg>
  );
}

/** Where each cable runs: the shortest ones on the lowest level, and a longer one above any it would run into. */
function layoutRoutes(links: StripLink[], spans: Map<number, CardSpan>) {
  const placed: Array<{ link: StripLink; x1: number; x2: number; y: number }> = [];
  const taken: Array<Array<[number, number]>> = ROUTE_LEVELS.map(() => []);
  const ends = links.flatMap((link) => {
    const from = spans.get(link.from);
    const to = spans.get(link.to);
    if (!from || !to) return [];
    const x1 = link.back ? (from.left + from.right) / 2 : from.right - ROUTE_INSET;
    const x2 = link.back ? (to.left + to.right) / 2 : to.left + ROUTE_INSET;
    return [{ link, x1, x2 }];
  });

  for (const end of ends.sort((a, b) => Math.abs(a.x2 - a.x1) - Math.abs(b.x2 - b.x1))) {
    const low = Math.min(end.x1, end.x2) - 2;
    const high = Math.max(end.x1, end.x2) + 2;
    const free = taken.findIndex((level) => level.every(([a, b]) => high < a || low > b));
    const level = free === -1 ? ROUTE_LEVELS.length - 1 : free;
    taken[level].push([low, high]);
    placed.push({ ...end, y: ROUTE_LEVELS[level] });
  }

  return placed;
}
