import { AudioWaveform, Drum, Expand, Link, LocateFixed, Magnet, Sparkles, Spline, Split, ZoomIn, ZoomOut } from 'lucide-react';
import { useEffect, useMemo, useRef, useState, type SetStateAction } from 'react';
import { TICKS_PER_BEAT, type Clip, type Track } from '../bridge/protocol';
import { actions, dispatch, reportError, useDawg } from '../store/store';
import { SNAP_OPTIONS, ui, useUi, type LaneTab, type RollFold } from '../store/uiStore';
import { BUILTIN_DRUM_MAPS, DEFAULT_DRUM_MAP, drumSynthMap, explicitDrumMode, isDrumTrack } from '../store/drumMap';
import { HumaniseDialog } from './HumaniseDialog';
import { Auditioner } from './audition';
import type { LaneId } from '../bridge/protocol';
import { CLIP_HANDLE_PX, drawBand, drawClipSpans, LANE_RANGES, LOOP_ZONE, drawClips, drawGrid, drawKeyboard, drawLane, drawLinkPreview, drawLinks, drawNotes, drawOutsideMarks, drawRuler } from './draw';
import { readTheme } from '../canvas/theme';
import { foldRows, keyChangeAt, keySpans, scaleAt, scalesOf } from './fold';
import { SCALES } from '../music/scales';
import { LinkInspector } from './LinkInspector';
import { linkInRowCommand, linkLines, withGlides } from './links';
import {
  bottomRowOf,
  DEFAULT_VIEW,
  pitchAtY,
  pitchName,
  pitchToRow,
  pitchToY,
  tickToX,
  topRowOf,
  MAX_PITCH,
  MIN_PITCH,
  ghostNotes,
  withRows,
  placeNotes,
  snapTicks,
  sortedClips,
  type View,
  xToTick,
} from './geometry';
import { useCanvas } from '../canvas/useCanvas';
import { useRollEditing } from './useRollEditing';
import { useRulerInput } from '../timeline/useRulerInput';
import { clipEnd, resizedClip } from '../timeline/clips';
import { useClickCount } from '../timeline/useClickCount';
import { ghostAt } from './ghosts';
import { applyVibratoPreset, VIBRATO_PRESETS } from './vibrato';
import { VibratoInspector } from './VibratoInspector';
import { TextPrompt } from '../components/ConfirmDiscard';
import { useWindowDrag, WindowControls } from '../components/WindowControls';
import { controllerLabel, drawControllerLane, MOD_WHEEL, SUSTAIN, useControllerEditing } from './controllers';
import { tempoMapOf } from '../time/tempoMap';
import { PlayheadOverlay } from '../components/PlayheadOverlay';
import { HelpButton } from '../components/HelpMenu';
import { MenuItem } from '../components/MenuItem';
import { DragNumber } from '../components/DragNumber';
import { clampSwing, swingApplies } from '../time/swing';
import { activeMidiEffects } from '../components/chain';
import { useScrollFade } from '../ui/useScrollFade';
import { Bar } from '../ui/Bar';
import { Button, IconButton } from '../ui/controls';
import { Menu } from '../ui/overlays';
import { Tab, Tabs } from '../ui/Tabs';

const KEYBOARD_WIDTH = 56;
const REVEAL_MARGIN_PX = 80; // how far from the left edge a note lands when selecting it scrolls the roll
const LANE_TABS: Array<{ id: LaneTab; label: string }> = [
  { id: 'velocity', label: 'Velocity' },
  { id: 'pressure', label: 'Pressure' },
  { id: 'timbre', label: 'Timbre' },
  { id: 'pitch', label: 'Pitch' },
  { id: 'vibrato', label: 'Vibrato' },
];
const DEFAULT_LANE_HEIGHT = 140;
const MIN_LANE_HEIGHT = 60;
const MIN_ROLL_HEIGHT = 120;
/** Narrowest value span a lane can be zoomed to. */
const MIN_LANE_SPAN: Record<LaneId | 'velocity', number> = {
  velocity: 0.05,
  pressure: 0.05,
  timbre: 0.05,
  vibrato: 0.05,
  pitch: 1,
};
const MIN_PX_PER_BEAT = 8;
const MAX_PX_PER_BEAT = 1200;
const FOLLOW_FRACTION = 1 / 3;

const clampView = (view: View, rollHeight: number): View => {
  const visibleRows = rollHeight / view.keyHeight;
  const pxPerBeat = Math.min(MAX_PX_PER_BEAT, Math.max(MIN_PX_PER_BEAT, view.pxPerTick * TICKS_PER_BEAT));
  return {
    ...view,
    pxPerTick: pxPerBeat / TICKS_PER_BEAT,
    startTick: Math.max(0, view.startTick),
    topPitch: Math.min(topRowOf(view) + 0.5, Math.max(bottomRowOf(view) - 0.5 + visibleRows, view.topPitch)),
  };
};

const ROOT_NAMES = Array.from({ length: 12 }, (_, i) => pitchName(60 + i).replace(/-?\d+$/, ''));
const FOLDS: Array<{ id: RollFold; label: string }> = [
  { id: 'off', label: 'Off' },
  { id: 'scale', label: 'Scale' },
  { id: 'used', label: 'Used' },
];

/** Piano roll for the selected track. See docs/design/phase3-piano-roll.md. */
export function PianoRoll() {
  const project = useDawg((s) => s.project);
  // Only the fields that affect the roll: the whole telemetry object changed identity every 100 ms, so the roll
  // re-rendered and redrew every note 30 times a second even when nothing moved. The playhead lives on its own
  // overlay (components/PlayheadOverlay.tsx) and is not subscribed here.
  const playing = useDawg((s) => s.telemetry.playing);
  const selectedTrackId = useUi((s) => s.selectedTrackId);
  const selectedNotes = useUi((s) => s.selectedNotes);
  const snapOption = useUi((s) => s.snap);
  const swing = useUi((s) => s.swing);
  const nodeTypes = useDawg((s) => s.nodeTypes);
  const quantiseStrength = useUi((s) => s.quantiseStrength);
  const follow = useUi((s) => s.follow);
  const lane = useUi((s) => s.lane);
  const controllerLane = useUi((s) => s.controllerLane);
  const selectedPoints = useUi((s) => s.selectedPoints);
  const activeDrumMapId = useUi((s) => s.activeDrumMapId);
  const customDrumMaps = useUi((s) => s.customDrumMaps);
  const humaniseOpen = useUi((s) => s.humaniseOpen);
  const fold = useUi((s) => s.rollFold);
  const scaleSnap = useUi((s) => s.scaleSnap);
  // The key change the toolbar edits and the keyboard shows: the one in effect at the playhead
  // (docs/design/scale-highlighting.md, SH9). Only its bar is subscribed, so playing doesn't re-render the roll.
  const playheadBar = useDawg((s) => tempoMapOf(s.project).barAt(s.telemetry.positionBeats * TICKS_PER_BEAT).bar);
  const keys = project?.keys;
  const keyNow = keyChangeAt(keys, playheadBar);
  const keyBar = keyNow?.bar ?? 0;
  const [addingController, setAddingController] = useState(false);
  const startWindowDrag = useWindowDrag();

  // The roll's window stays open whatever is selected: while the selection isn't a track with notes (the master, or a
  // track of the previous project), it keeps showing the last track it showed, or else the first track.
  const lastTrackId = useRef<number | null>(null);
  const track: Track | undefined = useMemo(() => {
    if (!project) return undefined;
    const found = [selectedTrackId, lastTrackId.current, project.trackOrder[0]]
      .map((id) => (id === null || id === undefined ? undefined : project.tracks[String(id)]))
      .find((candidate) => candidate !== undefined);
    lastTrackId.current = found?.id ?? null;
    return found;
  }, [project, selectedTrackId]);

  const isDrum = track ? isDrumTrack(track.name, explicitDrumMode(track)) : false;
  // A Drum Synth's pads name its rows (docs/design/drum-synth.md, DR10), whatever map is chosen.
  const activeDrumMap = useMemo(
    () =>
      (track && drumSynthMap(track)) ??
      customDrumMaps[activeDrumMapId] ??
      BUILTIN_DRUM_MAPS.find((m) => m.id === activeDrumMapId) ??
      DEFAULT_DRUM_MAP,
    [customDrumMaps, activeDrumMapId, track],
  );

  const editedClipIdRaw = useUi((s) => s.editedClipId);
  // Only a clip of this track can be edited; otherwise every clip is (as in phase 3).
  const editedClipId = track && editedClipIdRaw !== null && track.clips[String(editedClipIdRaw)] ? editedClipIdRaw : null;
  const notes = useMemo(() => (track ? placeNotes(track, editedClipId) : []), [track, editedClipId]);
  const ghosts = useMemo(() => (track ? ghostNotes(track, editedClipId) : []), [track, editedClipId]);
  // A clip edge dragged in the ruler (docs/design/phase6-arrangement.md, section 5): the clips drawn with it applied.
  const [clipDrag, setClipDrag] = useState<{ clip: Clip; edge: 'start' | 'end'; to: number } | null>(null);
  const storedClips = useMemo(() => (track ? sortedClips(track) : []), [track]);
  const clips = useMemo(
    () => (clipDrag ? storedClips.map((c) => (c.id === clipDrag.clip.id ? resizedClip(c, clipDrag.edge, clipDrag.to) : c)) : storedClips),
    [storedClips, clipDrag],
  );
  const markers = useMemo(() => Object.values(project?.markers ?? {}), [project]);

  // The rows a folded roll shows (docs/design/scale-highlighting.md, SH4), from the stored notes, so a drag in progress
  // never moves them; kept as the same array while they don't change, which keeps the view from converting.
  const spans = useMemo(() => keySpans(keys, tempoMapOf(project)), [keys, project]);
  const anyScale = spans.some((span) => span.scale !== null);
  const keyboardScale = scaleAt(spans, tempoMapOf(project).tickOfBar(playheadBar));
  const foldKey = useMemo(
    () => foldRows(fold, notes.map((n) => n.note.pitch), scalesOf(spans))?.join(',') ?? '',
    [fold, notes, spans],
  );
  const rows = useMemo(() => (foldKey === '' ? undefined : foldKey.split(',').map(Number)), [foldKey]);
  const rowsRef = useRef(rows);
  rowsRef.current = rows;
  // The view keeps the rows its top edge refers to; on other rows it is converted, keeping the pitch at the top.
  const [storedView, setStoredView] = useState<View>(DEFAULT_VIEW);
  const view = withRows(storedView, rows);
  const setView = (update: SetStateAction<View>) =>
    setStoredView((v) => {
      const current = withRows(v, rowsRef.current);
      return typeof update === 'function' ? update(current) : update;
    });
  // Development builds expose the view so automated UI checks can compute screen positions.
  if (import.meta.env.DEV) Object.assign(window, { __dawgView: view });
  const [pressedKey, setPressedKey] = useState<number | null>(null);
  const [laneHeight, setLaneHeight] = useState(DEFAULT_LANE_HEIGHT);
  const [laneRanges, setLaneRanges] = useState(LANE_RANGES);
  // The roll position of the right-click goes with it, for commands that act there.
  const [menu, setMenu] = useState<{ x: number; y: number; roll: { x: number; y: number } } | null>(null);
  const theme = useMemo(readTheme, []);

  const roll = useCanvas();
  // PointerEvent.detail is always 0 in Chromium: the controller lanes count their double-clicks here.
  const laneClicks = useClickCount();
  const ruler = useCanvas();
  const keyboard = useCanvas();
  const laneCanvas = useCanvas();

  // The clip whose controllers the controller lanes edit: the edited clip, else the clip of the selected notes,
  // else the track's only clip (docs/design/phase10-automation.md, section 5).
  const controllerClip = useMemo(() => {
    if (!track) return undefined;
    if (editedClipId !== null) return track.clips[String(editedClipId)];
    const owner = notes.find((n) => selectedNotes.has(n.note.id));
    if (owner) return owner.clip;
    const all = Object.values(track.clips).filter((c) => c.fileId === 0);
    return all.length === 1 ? all[0] : undefined;
  }, [track, editedClipId, notes, selectedNotes]);
  const controllerTabs = useMemo(() => {
    const present = Object.keys(controllerClip?.controllers ?? {}).map(Number);
    return [...new Set([SUSTAIN, MOD_WHEEL, ...present, ...(controllerLane !== null ? [controllerLane] : [])])].sort(
      (a, b) => (a === SUSTAIN ? -1 : b === SUSTAIN ? 1 : a - b),
    );
  }, [controllerClip, controllerLane]);

  const map = tempoMapOf(project);
  const gridTicks = snapTicks(snapOption.beats);
  const controllerEditing = useControllerEditing(controllerClip, controllerLane ?? SUSTAIN, view, gridTicks, laneCanvas.size.height);
  const auditioner = useMemo(() => new Auditioner(), []);
  const editing = useRollEditing({
    track,
    notes,
    view,
    gridTicks,
    map,
    selected: selectedNotes,
    selectedPoints,
    auditioner,
    laneRanges,
    editedClipId,
    snapScaleAt: anyScale && scaleSnap ? (tick) => scaleAt(spans, tick) : null,
    reveal: (placed) =>
      setView((v) => {
        // Only as far as needed, with some of what comes before the note still in sight.
        const left = tickToX(v, placed.start);
        const y = pitchToY(v, placed.note.pitch);
        const visible = (roll.size.height - v.keyHeight) / v.keyHeight;
        return clampView(
          {
            ...v,
            startTick:
              left < 0 || left > roll.size.width - REVEAL_MARGIN_PX ? placed.start - REVEAL_MARGIN_PX / v.pxPerTick : v.startTick,
            topPitch:
              y < 0
                ? pitchToRow(v.rows, placed.note.pitch) + 0.5
                : y > roll.size.height - v.keyHeight
                  ? pitchToRow(v.rows, placed.note.pitch) + visible + 0.5
                  : v.topPitch,
          },
          roll.size.height,
        );
      }),
  });
  const shownNotes = editing.displayNotes;
  const rulerInput = useRulerInput({ view, gridTicks, map, loop: project?.loop });

  // Opening a clip scrolls the roll to it (docs/design/phase6-arrangement.md, section 5).
  const scrolledToClip = useRef<number | null>(null);
  useEffect(() => {
    if (!track || editedClipId === null || editedClipId === scrolledToClip.current || roll.size.height === 0) return;
    scrolledToClip.current = editedClipId;
    const clip = track.clips[String(editedClipId)];
    const pitches = notes.map((n) => n.note.pitch);
    setView((v) =>
      clampView(
        {
          ...v,
          startTick: Math.max(0, clip.start - TICKS_PER_BEAT),
          topPitch:
            pitches.length > 0
              ? (pitchToRow(v.rows, Math.max(...pitches)) + pitchToRow(v.rows, Math.min(...pitches))) / 2 + roll.size.height / v.keyHeight / 2
              : v.topPitch,
        },
        roll.size.height,
      ),
    );
  }, [track, editedClipId, notes, roll.size.height]);

  // Continuous follow: keep the playhead a third of the way across while playing. The overlay calls this at the
  // telemetry rate; the view only re-renders (and the roll only redraws) when the playhead actually moves it.
  const followTick = (tick: number) => {
    if (!follow || !playing || roll.size.width === 0) return;
    const visibleTicks = roll.size.width / view.pxPerTick;
    const startTick = Math.max(0, tick - visibleTicks * FOLLOW_FRACTION);
    if (Math.abs(startTick - view.startTick) > 1) setView((v) => ({ ...v, startTick }));
  };

  // Redraw everything when anything it depends on changes (batched to one frame).
  const frame = useRef(0);
  const laneTabs = useRef<HTMLDivElement>(null);
  useScrollFade(laneTabs);
  useEffect(() => {
    cancelAnimationFrame(frame.current);
    frame.current = requestAnimationFrame(() => {
      const colour = track?.colour ?? theme.accent;

      const rollCtx = roll.context();
      if (rollCtx) {
        drawGrid(rollCtx, roll.size, view, theme, map, gridTicks, spans);
        drawClips(rollCtx, roll.size, view, clips, colour, editedClipId);
        if (ghosts.length > 0) {
          rollCtx.save();
          rollCtx.globalAlpha = 0.28;
          drawNotes(rollCtx, roll.size, view, theme, ghosts, colour, new Set(), map, null);
          rollCtx.restore();
        }
        const editedClip = editedClipId === null ? undefined : track?.clips[String(editedClipId)];
        drawNotes(
          rollCtx,
          roll.size,
          view,
          theme,
          withGlides(shownNotes, map),
          colour,
          selectedNotes,
          map,
          selectedPoints,
          editedClip
            ? { start: editedClip.start, end: editedClip.start + editedClip.length }
            : undefined,
          isDrum ? activeDrumMap : undefined,
        );
        drawOutsideMarks(rollCtx, roll.size, view, theme, shownNotes, spans);
        drawLinks(rollCtx, view, colour, linkLines(editing.displayNotes));
        if (editing.linkDrag) drawLinkPreview(rollCtx, colour, editing.linkDrag);
        if (editing.band && editing.bandTarget === 'roll') drawBand(rollCtx, theme, editing.band);
      }

      const rulerCtx = ruler.context();
      if (rulerCtx)
      {
        drawRuler(rulerCtx, ruler.size, view, theme, map, rulerInput.loop ?? undefined, markers, true);
        drawClipSpans(rulerCtx, ruler.size, view, clips, colour, editedClipId);
      }

      const keyboardCtx = keyboard.context();
      if (keyboardCtx)
        drawKeyboard(keyboardCtx, keyboard.size, view, theme, pressedKey, isDrum ? activeDrumMap : undefined, keyboardScale);

      const laneCtx = laneCanvas.context();
      if (laneCtx && controllerLane !== null) {
        drawControllerLane(
          laneCtx,
          laneCanvas.size,
          view,
          theme,
          controllerClip,
          controllerLane,
          controllerEditing.points,
          colour,
          controllerEditing.selected,
        );
        if (controllerEditing.band) drawBand(laneCtx, theme, controllerEditing.band);
      } else if (laneCtx) {
        drawLane(
          laneCtx,
          laneCanvas.size,
          view,
          theme,
          lane,
          laneRanges[lane],
          shownNotes,
          colour,
          selectedNotes,
          selectedPoints,
          map,
          gridTicks,
        );
        if (editing.band && editing.bandTarget === 'lane') drawBand(laneCtx, theme, editing.band);
      }
    });
  });

  // Wheel: scroll pitch; Shift scrolls time; Ctrl/Cmd zooms time; Alt zooms pitch. Registered as a native,
  // non-passive listener so Ctrl+wheel zooms the roll instead of the whole web view.
  const gridRef = useRef<HTMLDivElement>(null);
  const onWheelRef = useRef<(e: WheelEvent, x: number) => void>(() => {});
  useEffect(() => {
    const element = gridRef.current;
    if (!element) return;
    const listener = (e: WheelEvent) => {
      e.preventDefault();
      const laneElement = laneCanvas.ref.current;
      if (laneElement && e.target === laneElement && !e.ctrlKey && !e.metaKey && !e.shiftKey) {
        onLaneWheelRef.current(e, e.clientY - laneElement.getBoundingClientRect().top);
        return;
      }
      const rollRect = roll.ref.current?.getBoundingClientRect();
      onWheelRef.current(e, rollRect ? e.clientX - rollRect.left : 0);
    };
    element.addEventListener('wheel', listener, { passive: false });
    return () => element.removeEventListener('wheel', listener);
  }, [roll.ref, laneCanvas.ref]);

  onWheelRef.current = (e: WheelEvent, x: number) => {
    setView((v) => {
      if (e.ctrlKey || e.metaKey) {
        const anchor = xToTick(v, x);
        const pxPerTick = v.pxPerTick * Math.exp(-e.deltaY * 0.0015);
        return clampView({ ...v, pxPerTick, startTick: anchor - x / pxPerTick }, roll.size.height);
      }
      if (e.altKey) {
        const keyHeight = Math.min(32, Math.max(6, v.keyHeight * Math.exp(-e.deltaY * 0.0015)));
        return clampView({ ...v, keyHeight }, roll.size.height);
      }
      if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
        const delta = e.shiftKey ? e.deltaY : e.deltaX;
        return clampView({ ...v, startTick: v.startTick + delta / v.pxPerTick }, roll.size.height);
      }
      // Wheel down shows lower notes.
      return clampView({ ...v, topPitch: v.topPitch - e.deltaY / v.keyHeight / 3 }, roll.size.height);
    });
  };

  // Lane wheel: Alt zooms the value range around the pointer, the wheel scrolls it (within the lane's limits).
  const onLaneWheelRef = useRef<(e: WheelEvent, y: number) => void>(() => {});
  onLaneWheelRef.current = (e: WheelEvent, y: number) => {
    setLaneRanges((ranges) => {
      const [limitMin, limitMax] = LANE_RANGES[lane];
      const [min, max] = ranges[lane];
      const span = max - min;
      const usable = Math.max(1, laneCanvas.size.height - 12);
      const pointerValue = min + (1 - (y - 6) / usable) * span;

      let next: [number, number];
      if (e.altKey) {
        const factor = Math.exp(e.deltaY * 0.0015);
        const newSpan = Math.min(limitMax - limitMin, Math.max(MIN_LANE_SPAN[lane], span * factor));
        const ratio = (pointerValue - min) / span;
        next = [pointerValue - ratio * newSpan, pointerValue + (1 - ratio) * newSpan];
      } else {
        const shift = (e.deltaY / usable) * span * 0.5;
        next = [min - shift, max - shift];
      }

      // Keep the range inside the lane's limits without changing its span.
      const newSpan = next[1] - next[0];
      const lo = Math.min(Math.max(next[0], limitMin), limitMax - newSpan);
      return { ...ranges, [lane]: [lo, lo + newSpan] };
    });
  };
  const resetLaneRange = () => setLaneRanges((ranges) => ({ ...ranges, [lane]: LANE_RANGES[lane] }));

  // Divider between roll and lanes: drag to resize the lane area.
  const resizing = useRef<{ startY: number; startHeight: number } | null>(null);
  const onDividerDown = (e: React.PointerEvent<HTMLDivElement>) => {
    e.currentTarget.setPointerCapture(e.pointerId);
    resizing.current = { startY: e.clientY, startHeight: laneHeight };
  };
  const onDividerMove = (e: React.PointerEvent<HTMLDivElement>) => {
    const r = resizing.current;
    const grid = gridRef.current;
    if (!r || !grid) return;
    const maxHeight = grid.clientHeight - MIN_ROLL_HEIGHT - 60;
    setLaneHeight(Math.min(maxHeight, Math.max(MIN_LANE_HEIGHT, r.startHeight - (e.clientY - r.startY))));
  };
  const onDividerUp = () => (resizing.current = null);

  // Ruler: move the playhead (snapped to the grid unless Shift).
  /** The clip edge under the pointer in the ruler's lower half: the edited clip's first, then the nearest. */
  const clipEdgeAt = (x: number, y: number) => {
    if (y <= ruler.size.height * LOOP_ZONE) return null;
    const near = storedClips
      .flatMap((clip) =>
        (['start', 'end'] as const).map((edge) => ({ clip, edge, distance: Math.abs(x - tickToX(view, edge === 'start' ? clip.start : clipEnd(clip))) })),
      )
      .filter((c) => c.distance <= CLIP_HANDLE_PX)
      .sort((a, b) => Number(b.clip.id === editedClipId) - Number(a.clip.id === editedClipId) || a.distance - b.distance);
    return near[0] ?? null;
  };

  const seekTo = (x: number, free: boolean) => {
    const tick = Math.max(0, xToTick(view, x));
    const target = free || gridTicks <= 0 ? tick : Math.round(tick / gridTicks) * gridTicks;
    actions.seek(target / TICKS_PER_BEAT);
  };

  // Keyboard audition: press a key to hear it through the track's instrument.
  const releaseKey = () => {
    setPressedKey(null);
    auditioner.stop();
  };
  const pressKey = (y: number) => {
    if (!track) return;
    const pitch = pitchAtY(view, y);
    if (pitch < MIN_PITCH || pitch > MAX_PITCH) return;
    setPressedKey(pitch);
    auditioner.play(track.id, pitch);
  };

  const zoom = (factor: number) =>
    setView((v) => {
      const centre = xToTick(v, roll.size.width / 2);
      const pxPerTick = v.pxPerTick * factor;
      return clampView({ ...v, pxPerTick, startTick: centre - roll.size.width / 2 / pxPerTick }, roll.size.height);
    });

  const zoomToFit = () => {
    if (notes.length === 0 || roll.size.width === 0) return;
    const start = Math.min(...notes.map((n) => n.start));
    const end = Math.max(...notes.map((n) => n.end));
    const low = Math.min(...notes.map((n) => n.note.pitch));
    const high = Math.max(...notes.map((n) => n.note.pitch));
    const pxPerTick = (roll.size.width * 0.9) / Math.max(TICKS_PER_BEAT, end - start);
    setView((v) =>
      clampView(
        {
          ...v,
          pxPerTick,
          startTick: start - (roll.size.width * 0.05) / pxPerTick,
          topPitch: (pitchToRow(v.rows, high) + pitchToRow(v.rows, low)) / 2 + roll.size.height / v.keyHeight / 2,
        },
        roll.size.height,
      ),
    );
  };

  if (!track) {
    return (
      <div className="piano-roll">
        <div className="roll-toolbar" onPointerDown={startWindowDrag}>
          <span className="spacer" />
          <WindowControls />
        </div>
        <div className="editor-placeholder">
          <h2>Piano roll</h2>
          <p>Add a track to start.</p>
        </div>
      </div>
    );
  }

  return (
    <div className="piano-roll">
      {/* One row that never wraps: groups that don't fit fold into "⋯" (design-system.md, DS14); grouped as the grid, the
          scale, the tools (docs/ui-review-2026-11-10.md, 3.12). */}
      <Bar
        className="roll-toolbar"
        onPointerDown={startWindowDrag}
        items={[
          {
            key: 'title',
            node: (
              <span className="roll-title" style={{ color: track.colour }}>
                {track.name}
                {editedClipId !== null && track.clips[String(editedClipId)]?.name
                  ? ` · ${track.clips[String(editedClipId)].name}`
                  : ''}
              </span>
            ),
          },
          activeMidiEffects(track).length > 0 && {
            key: 'midi-effects',
            fold: 5,
            // The clip shows what goes in; its MIDI effects change what plays (docs/design/midi-effects.md, ME15).
            node: (
              <span className="roll-midi-effects" title="What plays goes through these MIDI effects first">
                MIDI effects: {activeMidiEffects(track).map((n) => nodeTypes[n.type]?.name ?? n.type).join(', ')}
                {editedClipId !== null && (
                  <Button
                    title="Replace the clip's notes with what the MIDI effects play, and bypass them"
                    onClick={() => dispatch({ command: 'renderMidiEffects', args: { clipId: editedClipId } })}
                  >
                    Render
                  </Button>
                )}
              </span>
            ),
          },
          {
            key: 'grid',
            fold: 1,
            node: (
              <div className="toolbar-group grid" role="group" aria-label="Grid">
                <label className="field">
                  <span>Snap</span>
                  <select
                    value={snapOption.label}
                    onChange={(e) => ui.setSnap(SNAP_OPTIONS.find((o) => o.label === e.target.value) ?? SNAP_OPTIONS[3])}
                  >
                    {SNAP_OPTIONS.map((o) => (
                      <option key={o.label}>{o.label}</option>
                    ))}
                  </select>
                </label>
                <label
                  className={`field ${swingApplies(snapOption.beats) ? '' : 'disabled'}`}
                  title={swingApplies(snapOption.beats) ? 'Quantise swing: 50 % is straight, 66 % a triplet feel' : 'Swing needs a straight grid'}
                >
                  <span>Swing</span>
                  <DragNumber
                    className="roll-quantise-number"
                    value={Math.round(swing * 100)}
                    min={50}
                    max={75}
                    integer
                    resetValue={50}
                    format={(v) => `${Math.round(v)} %`}
                    onChange={(v) => ui.setSwing(clampSwing(v / 100))}
                  />
                </label>
                <label className="field" title="Quantise strength: how far Q moves notes towards the grid">
                  <span>Strength</span>
                  <DragNumber
                    className="roll-quantise-number"
                    value={Math.round(quantiseStrength * 100)}
                    min={0}
                    max={100}
                    integer
                    resetValue={100}
                    format={(v) => `${Math.round(v)} %`}
                    onChange={(v) => ui.setQuantiseStrength(Math.min(1, Math.max(0, v / 100)))}
                  />
                </label>
              </div>
            ),
          },
          {
            key: 'scale',
            fold: 3,
            node: (
              <div className="toolbar-group scale" role="group" aria-label="Scale">
                {/* The key at the playhead, the fold and Snap to scale (docs/design/scale-highlighting.md, SH2, SH4, SH5, SH9). */}
                <label
                  className="field"
                  title={
                    (keys?.length ?? 0) > 1
                      ? `The key from bar ${keyBar + 1}, where the playhead is. Key changes are in the arrangement's Key strip.`
                      : "The project's key: the roll shows its scale, and Scale Lock can follow it. Key changes are in the arrangement's Key strip."
                  }
                >
                  <span>{(keys?.length ?? 0) > 1 ? `Key · bar ${keyBar + 1}` : 'Key'}</span>
                  <select
                    value={keyNow?.enabled ? String(keyNow.root) : 'off'}
                    onChange={(e) =>
                      dispatch(
                        e.target.value === 'off'
                          ? { command: 'setKey', args: { bar: keyBar, enabled: false } }
                          : { command: 'setKey', args: { bar: keyBar, enabled: true, root: Number(e.target.value) } },
                      )
                    }
                  >
                    <option value="off">Off</option>
                    {ROOT_NAMES.map((name, root) => (
                      <option key={name} value={root}>
                        {name}
                      </option>
                    ))}
                  </select>
                  {keyNow?.enabled && (
                    <select
                      value={keyNow.scale}
                      onChange={(e) => dispatch({ command: 'setKey', args: { bar: keyBar, scale: Number(e.target.value) } })}
                    >
                      {SCALES.map((s, index) => (
                        <option key={s.name} value={index}>
                          {s.name}
                        </option>
                      ))}
                    </select>
                  )}
                </label>
                <label className="field" title="Fold: only the scale's rows, or only the pitches the notes use">
                  <span>Fold</span>
                  <select value={fold} onChange={(e) => ui.setRollFold(e.target.value as RollFold)}>
                    {FOLDS.map((f) => (
                      <option key={f.id} value={f.id} disabled={f.id === 'scale' && !anyScale}>
                        {f.label}
                      </option>
                    ))}
                  </select>
                </label>
                <IconButton
                  className={`${scaleSnap && anyScale && !rows ? 'active' : ''}`}
                  disabled={!anyScale || rows !== undefined}
                  title={
                    !anyScale
                      ? 'Snap to scale needs a key'
                      : rows
                        ? 'A folded roll already places notes on its rows'
                        : scaleSnap
                          ? 'Snap to scale: on (drawn and dragged notes go to the nearest key in the scale)'
                          : 'Snap to scale: off'
                  }
                  onClick={() => ui.setScaleSnap(!scaleSnap)}
                >
                  <Magnet size={15} />
                </IconButton>
              </div>
            ),
          },
          {
            key: 'tools',
            fold: 3,
            node: (
              <div className="toolbar-group tools" role="group" aria-label="Tools">
                <IconButton
                  className={`${isDrum ? 'active' : ''}`}
                  title={isDrum ? 'Drum part, showing drum names (click for melodic)' : 'Melodic part (click for drum part)'}
                  onClick={() => dispatch({ command: 'setTrackDrumMode', args: { id: track.id, mode: isDrum ? 'melodic' : 'drums' } })}
                >
                  <Drum size={15} />
                </IconButton>
                <IconButton
                  title="Humanise notes (H)"
                  onClick={() => ui.setHumaniseOpen(true)}
                >
                  <Sparkles size={15} />
                </IconButton>
              </div>
            ),
          },
          {
            key: 'vibrato',
            fold: 4,
            node: (
              <VibratoInspector
                track={track}
                notes={notes.filter((n) => selectedNotes.has(n.note.id))}
                map={map}
              />
            ),
          },
          { key: 'links', fold: 4, node: <LinkInspector notes={notes.filter((n) => selectedNotes.has(n.note.id))} /> },
          { key: 'spacer', grow: true, node: <div className="spacer" /> },
          {
            key: 'view',
            fold: 2,
            node: (
              <div className="group">
                <HelpButton area="Piano roll" />
                <IconButton
                  size="l"
                  className={`${follow ? 'active' : ''}`}
                  title="Follow playhead"
                  onClick={() => ui.setFollow(!follow)}
                >
                  <LocateFixed size={16} />
                </IconButton>
                <IconButton size="l" title="Zoom out" onClick={() => zoom(1 / 1.5)}>
                  <ZoomOut size={16} />
                </IconButton>
                <IconButton size="l" title="Zoom in" onClick={() => zoom(1.5)}>
                  <ZoomIn size={16} />
                </IconButton>
                <IconButton size="l" title="Zoom to fit notes" onClick={zoomToFit}>
                  <Expand size={16} />
                </IconButton>
              </div>
            ),
          },
          { key: 'window', end: true, node: <WindowControls /> },
        ]}
      />

      <div
        ref={gridRef}
        className="roll-grid"
        style={{
          gridTemplateColumns: `${KEYBOARD_WIDTH}px minmax(0, 1fr)`,
          gridTemplateRows: `26px minmax(${MIN_ROLL_HEIGHT}px, 1fr) 6px 28px ${laneHeight}px`,
        }}
      >
        <div className="roll-corner" />
        <div className="canvas-stack">
          <canvas
            ref={ruler.ref}
            className="roll-ruler"
            title="Upper half: drag to set the loop range, drag inside it to move it, click to toggle it. Lower half: move the playhead (Ctrl: off the grid), drag a clip's end handle to resize the clip"
            onPointerDown={(e) => {
              e.currentTarget.setPointerCapture(e.pointerId);
              const edge = e.button === 0 ? clipEdgeAt(e.nativeEvent.offsetX, e.nativeEvent.offsetY) : null;
              if (edge) {
                setClipDrag({ clip: edge.clip, edge: edge.edge, to: edge.edge === 'start' ? edge.clip.start : clipEnd(edge.clip) });
                return;
              }
              if (!rulerInput.down(e, ruler.size.height)) seekTo(e.nativeEvent.offsetX, e.ctrlKey || e.metaKey);
            }}
            onPointerMove={(e) => {
              const { offsetX: x, offsetY: y } = e.nativeEvent;
              if (e.buttons !== 1) {
                e.currentTarget.style.cursor = clipEdgeAt(x, y) ? 'ew-resize' : rulerInput.cursorAt(x, y, ruler.size.height);
                return;
              }
              if (clipDrag) {
                // On the grid (Ctrl/Cmd: off it), and never past the other edge.
                const tick = Math.max(0, xToTick(view, x));
                const to = e.ctrlKey || e.metaKey ? Math.round(tick) : map.snap(tick, gridTicks, true);
                const { clip, edge } = clipDrag;
                setClipDrag({ ...clipDrag, to: edge === 'start' ? Math.min(to, clipEnd(clip) - 1) : Math.max(to, clip.start + 1) });
                return;
              }
              if (!rulerInput.move(e)) seekTo(x, e.ctrlKey || e.metaKey);
            }}
            onPointerUp={() => {
              if (clipDrag) {
                const { clip, edge, to } = clipDrag;
                setClipDrag(null);
                if (to !== (edge === 'start' ? clip.start : clipEnd(clip))) dispatch({ command: 'resizeClip', args: { id: clip.id, edge, to } });
                return;
              }
              rulerInput.up();
            }}
          />
          <PlayheadOverlay view={view} theme={theme} ruler />
        </div>

        <canvas
          ref={keyboard.ref}
          className="roll-keyboard"
         
          onPointerDown={(e) => {
            e.currentTarget.setPointerCapture(e.pointerId);
            pressKey(e.nativeEvent.offsetY);
          }}
          onPointerMove={(e) => e.buttons === 1 && pressKey(e.nativeEvent.offsetY)}
          onPointerUp={releaseKey}
          onPointerCancel={releaseKey}
        />
        <div className="canvas-stack working-area">
          <canvas
            ref={roll.ref}
            className="roll-canvas"
            tabIndex={0}
            style={{ cursor: editing.cursor }}
            onPointerDown={(e) => {
              // A click on a ghost note opens its clip (unless an editable note is there).
              const x = e.nativeEvent.offsetX;
              const y = e.nativeEvent.offsetY;
              if (e.button === 0 && !e.altKey && !editing.noteAt(x, y)) {
                const ghost = ghostAt(ghosts, view, x, y);
                if (ghost && track) {
                  ui.editClip(track.id, ghost.clip.id);
                  return;
                }
              }
              editing.onPointerDown(e);
            }}
            onPointerMove={editing.onPointerMove}
            onPointerUp={editing.onPointerUp}
            onPointerCancel={editing.onPointerUp}
            onKeyDown={editing.onKeyDown}
            onContextMenu={(e) => {
              e.preventDefault();
              const placed = editing.noteAt(e.nativeEvent.offsetX, e.nativeEvent.offsetY);
              if (!placed) return;
              if (!selectedNotes.has(placed.note.id)) ui.selectNotes([placed.note.id]);
              // In the window's coordinates: the menu is drawn into the document's body (ui/overlays.tsx).
              setMenu({
                x: e.clientX,
                y: e.clientY,
                roll: { x: e.nativeEvent.offsetX, y: e.nativeEvent.offsetY },
              });
            }}
          />
          <PlayheadOverlay view={view} theme={theme} onTick={followTick} />
        </div>

        <div
          className="lane-divider"
          title="Drag to resize the lanes"
          onPointerDown={onDividerDown}
          onPointerMove={onDividerMove}
          onPointerUp={onDividerUp}
          onPointerCancel={onDividerUp}
        />
        <div className="lane-tabs-corner" />
        <Tabs variant="lane" label="Lanes" className="lane-tabs scroll-fade" ref={laneTabs}>
          {LANE_TABS.map((tab) => (
            <Tab key={tab.id} selected={controllerLane === null && lane === tab.id} onClick={() => ui.setLane(tab.id)}>
              {tab.label}
            </Tab>
          ))}
          <span className="lane-tabs-separator" />
          {controllerTabs.map((controller) => (
            <Tab
              key={`cc${controller}`}
              className="controller"
              selected={controllerLane === controller}
              title="A clip controller: moves and loops with the clip"
              onClick={() => ui.setControllerLane(controller)}
            >
              {controllerLabel(controller)}
            </Tab>
          ))}
          <button className="tab" title="Show another MIDI controller (CC number)" onClick={() => setAddingController(true)}>
            +
          </button>
        </Tabs>

        <div className="lane-label" title="Double-click to reset the zoom" onDoubleClick={resetLaneRange}>
          {controllerLane !== null ? controllerLabel(controllerLane) : LANE_TABS.find((t) => t.id === lane)?.label}
        </div>
        <div className="canvas-stack">
          <canvas
            ref={laneCanvas.ref}
            className="lane-canvas"
            tabIndex={0}
            onKeyDown={(e) => {
              if (controllerLane !== null) {
                if ((e.key === 'Delete' || e.key === 'Backspace') && controllerEditing.removeSelected()) e.preventDefault();
                return;
              }
              editing.onKeyDown(e);
            }}
            onPointerDown={(e) => {
              if (e.button !== 0) return;
              e.currentTarget.setPointerCapture(e.pointerId);
              e.currentTarget.focus({ preventScroll: true });
              const { offsetX: x, offsetY: y } = e.nativeEvent;
              if (controllerLane !== null)
                controllerEditing.down(x, y, laneClicks(x, y), {
                  alt: e.altKey,
                  shift: e.shiftKey,
                  free: e.ctrlKey || e.metaKey,
                });
              else if (lane === 'velocity') editing.onVelocityLane('down', x, y, laneCanvas.size.height);
              else editing.onCurveLane('down', lane, x, y, laneCanvas.size.height, { alt: e.altKey, shift: e.shiftKey, free: e.ctrlKey || e.metaKey });
            }}
            onPointerMove={(e) => {
              if (e.buttons !== 1) return;
              const { offsetX: x, offsetY: y } = e.nativeEvent;
              if (controllerLane !== null) controllerEditing.move(x, y, e.ctrlKey || e.metaKey);
              else if (lane === 'velocity') editing.onVelocityLane('move', x, y, laneCanvas.size.height);
              else editing.onCurveLane('move', lane, x, y, laneCanvas.size.height, { alt: e.altKey, shift: e.shiftKey, free: e.ctrlKey || e.metaKey });
            }}
            onPointerUp={() => {
              if (controllerLane !== null) controllerEditing.up();
              else if (lane === 'velocity') editing.onVelocityLane('up', 0, 0, 0);
              else editing.onCurveLane('up', lane, 0, 0, 0);
            }}
            onPointerCancel={() => {
              if (controllerLane !== null) controllerEditing.up();
              else if (lane === 'velocity') editing.onVelocityLane('up', 0, 0, 0);
              else editing.onCurveLane('up', lane, 0, 0, 0);
            }}
          />
          <PlayheadOverlay view={view} theme={theme} />
        </div>
      </div>

      {addingController && (
        <TextPrompt
          title="Show a MIDI controller"
          label="CC number (0–127)"
          initial="11"
          confirmLabel="Show"
          onDone={(value) => {
            setAddingController(false);
            const cc = Number(value);
            if (value !== null && Number.isInteger(cc) && cc >= 0 && cc <= 127) ui.setControllerLane(cc);
          }}
        />
      )}

      {menu && (
        <>
          <Menu onClose={() => setMenu(null)} at={{ x: menu.x, y: menu.y }}>
            <MenuItem
              icon={Split}
              label="Split here"
              shortcut="Alt+click"
              onClick={() => {
                editing.splitAt(menu.roll.x, menu.roll.y, false);
                setMenu(null);
              }}
            />
            {/* Every Alt gesture has a way without Alt: some Linux desktops move the window on Alt+drag. */}
            <MenuItem
              icon={Spline}
              label="Add pitch point here"
              shortcut="Shift+Alt+click"
              onClick={() => {
                editing.addPitchPointAt(menu.roll.x, menu.roll.y);
                setMenu(null);
              }}
            />
            <MenuItem
              icon={Link}
              label={selectedNotes.size > 1 ? 'Link the selected notes in a row' : 'Link to the next note'}
              shortcut="Ctrl+Alt+drag"
              onClick={() => {
                const chosen = notes.filter((n) => useUi.getState().selectedNotes.has(n.note.id));
                const command = linkInRowCommand(chosen, notes);
                if (command) dispatch(command);
                else reportError('There is no later note in this clip to link to');
                setMenu(null);
              }}
            />
            <MenuItem
              icon={Sparkles}
              label="Humanise…"
              shortcut="H"
              onClick={() => {
                ui.setHumaniseOpen(true);
                setMenu(null);
              }}
            />
            <div className="menu-separator" />
            <div className="menu-heading">Vibrato</div>
            {VIBRATO_PRESETS.map((preset) => (
              <MenuItem
                key={preset.label}
                icon={AudioWaveform}
                label={preset.label}
                onClick={() => {
                  applyVibratoPreset(
                    track,
                    notes.filter((n) => useUi.getState().selectedNotes.has(n.note.id)),
                    preset.vibrato,
                    map,
                  );
                  setMenu(null);
                }}
              />
            ))}
          </Menu>
        </>
      )}

      {humaniseOpen && track && (
        <HumaniseDialog
          track={track}
          notes={notes}
          selectedNotes={selectedNotes}
          map={map}
          onClose={() => ui.setHumaniseOpen(false)}
        />
      )}
    </div>
  );
}
