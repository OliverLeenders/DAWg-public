import { useEffect, useMemo, useRef, useState } from 'react';
import { TICKS_PER_BEAT, type CurvePoint, type LaneId, type Track } from '../bridge/protocol';
import { dispatch, useDawg } from '../store/store';
import { linkCommand } from './links';
import { pointsOf, ui, useUi, type PointSelection } from '../store/uiStore';
import { quantiseNote, swingApplies } from '../time/swing';
import type { Auditioner } from './audition';
import type { TempoMap } from '../time/tempoMap';
import {
  addPoint,
  bendSegmentToPointer,
  boxSelect,
  deletePoints,
  handlePosition,
  hitCurve,
  movePoints,
  pastePoints,
  pointsInRect,
  shiftPoints,
  type CurveBounds,
  type CurveMapping,
  straightenSegment,
} from './curveEditing';
import { LANE_PAD } from './draw';
import { resizeEnds, resizeStarts, snappedTravel, type Grid } from './dragSnap';
import { useClickCount } from '../timeline/useClickCount';
import {
  addNotesCommand,
  clampToClips,
  combineCommands,
  removeNotesCommand,
  setCurvesCommand,
  splitNotesCommand,
  toEditNote,
  updateNotesCommand,
  type EditNote,
} from './edit';
import {
  adjacentNote,
  evaluateCurve,
  isBent,
  LANE_DEFAULTS,
  MAX_PITCH,
  MIN_PITCH,
  NO_VIBRATO,
  noteInPitchDirection,
  pitchAtTick,
  pitchAtY,
  pitchToY,
  rowAtY,
  stepRows,
  tickToX,
  vibratoHandles,
  xToTick,
  yToPitch,
  type PlacedNote,
  type View,
} from './geometry';
import { lockToScale, type ScaleDirection } from '../music/scales';
import type { ScaleShown } from './fold';

// Cubase-style editing for the piano roll (docs/design/phase3-piano-roll.md, "Editing scheme"):
//   click note            select (Shift+click toggles)
//   drag note             move the selection (Ctrl/Cmd places it off the grid; Shift locks to time or pitch)
//   drag note edge        resize the selection's starts or ends
//   drag empty space      box selection (Shift adds)
//   Alt+click empty       create a note (last used length); keep dragging right to set its length (also double-click)
//   Alt+drag note         copy the selection
//   Alt+click note        split it there (with the selected notes under the pointer)
//   Ctrl+Alt+drag note    link it to the note it's dropped on (docs/design/linked-notes.md)
//   Shift+Alt+click note  add a pitch point there
//   S                     split the selected notes at the playhead, as S does in the arrangement
//   Ctrl+Shift+drag note  velocity
// Curve points (pitch in the roll, all curves in the lanes): click selects (Shift toggles), drag moves the
// selected points, double-click deletes, Alt+click adds (Shift+Alt+click on a note in the roll), dragging empty
// lane space box-selects points. A box in the roll selects a bent note only once it holds all of the note's pitch
// points, and otherwise the points inside it. Up and Down (Shift: an octave), Ctrl/Cmd+Left and Right, and Delete act
// on the selected points and on the selected notes without selected points, together. Plain Left and Right select the
// previous and next note in timeline order, Ctrl/Cmd+Up and Down the note above and below.
// Points snap to the grid in time and pitch points to semitones (Ctrl/Cmd for free values), and never leave
// their note. Edits are previewed locally and committed as one command on release.

const EDGE_PX = 5;
const VELOCITY_PX = 150; // vertical pixels for the full velocity range
const DRAG_THRESHOLD_PX = 4;

type Zone = 'start' | 'end' | 'body';

export interface Band {
  x0: number;
  y0: number;
  x1: number;
  y1: number;
}

interface CurveRef {
  placed: PlacedNote;
  lane: LaneId;
  map: CurveMapping;
  /** The same lane's mapping for another note (for the other notes of a multi-note point drag). */
  mapFor: (placed: PlacedNote) => CurveMapping;
}

/** One note's selected points in a point drag. */
interface PointGroup {
  placed: PlacedNote;
  map: CurveMapping;
  points: CurvePoint[];
  indices: number[];
}

type Drag =
  | { kind: 'create'; note: EditNote; originTick: number }
  | {
      kind: 'move';
      anchorTick: number;
      anchorPitch: number;
      anchorX: number;
      anchorY: number;
      originals: EditNote[];
      grabbed: EditNote;
      moved: boolean;
      copy: boolean;
      lockable: boolean;
      lock: 'time' | 'pitch' | null;
      /** Shift-clicked note that was already selected: deselect it on release if this stays a click. */
      toggleOnClick: number | null;
    }
  | { kind: 'altNote'; placed: PlacedNote; x: number; y: number; addPoint: boolean; selection: ReadonlySet<number> }
  | { kind: 'resizeStart' | 'resizeEnd'; anchorTick: number; originals: EditNote[]; grabbed: EditNote }
  | { kind: 'velocity'; anchorY: number; originals: EditNote[] }
  | { kind: 'link'; placed: PlacedNote; x0: number; y0: number; x: number; y: number }
  | { kind: 'vibrato'; handle: 'depth' | 'rate'; placed: PlacedNote; originals: EditNote[] }
  | { kind: 'band'; band: Band; base: { notes: ReadonlySet<number>; points: ReadonlyMap<number, number[]> } }
  /** The first group holds the grabbed point (`grab` indexes its points); the others follow its travel. */
  | { kind: 'points'; lane: LaneId; groups: PointGroup[]; grab: number; moved: boolean }
  | (CurveRef & { kind: 'bend'; points: CurvePoint[]; segment: number; grab: { x: number; y: number } })
  | (CurveRef & { kind: 'pointBand'; band: Band; base: ReadonlyMap<number, number[]> });

type Clipboard = { kind: 'notes'; notes: EditNote[] } | { kind: 'curve'; lane: LaneId; points: CurvePoint[] };

interface Options {
  track: Track | undefined;
  notes: PlacedNote[];
  view: View;
  gridTicks: number;
  map: TempoMap;
  selected: ReadonlySet<number>;
  selectedPoints: PointSelection | null;
  auditioner: Auditioner;
  /** Visible value range of each lane (zoomable). */
  laneRanges: Record<LaneId | 'velocity', [number, number]>;
  /** The clip new notes go into (null: the clip around them, as in phase 3). */
  editedClipId: number | null;
  /** Scrolls a note into view. */
  reveal: (note: PlacedNote) => void;
  /**
   * Snap to scale (docs/design/scale-highlighting.md, SH5, SH9): the scale at a tick, which drawn, dragged and nudged
   * notes move into; null while Snap to scale is off. A folded roll needs none: notes land on its rows.
   */
  snapScaleAt?: ((tick: number) => ScaleShown | null) | null;
}

let clipboard: Clipboard | null = null;

const clampPitch = (p: number) => Math.min(MAX_PITCH, Math.max(MIN_PITCH, p));
const clampVelocity = (v: number) => Math.min(1, Math.max(0, v));

export function useRollEditing({
  track,
  notes,
  view,
  gridTicks,
  map,
  selected,
  selectedPoints,
  auditioner,
  laneRanges,
  editedClipId,
  reveal,
  snapScaleAt = null,
}: Options) {
  // A pitch moved into the scale where the note starts, while Snap to scale is on and the roll unfolded.
  const scaleFor = (tick: number) => (snapScaleAt && !view.rows ? snapScaleAt(tick) : null);
  const toScale = (pitch: number, tick: number, direction: ScaleDirection = 'nearest') => {
    const scale = scaleFor(tick);
    return clampPitch(scale ? lockToScale(pitch, scale.root, scale.mask, direction) : pitch);
  };
  const [preview, setPreviewState] = useState<EditNote[] | null>(null);
  // Pointer-up can run before React re-renders after the last move, so the latest preview lives in a ref.
  const previewRef = useRef<EditNote[] | null>(null);
  const setPreview = (next: EditNote[] | null) => {
    previewRef.current = next;
    setPreviewState(next);
  };
  const [band, setBand] = useState<Band | null>(null);
  const [linkDrag, setLinkDrag] = useState<Band | null>(null);
  const [bandTarget, setBandTarget] = useState<'roll' | 'lane'>('roll');
  const [cursor, setCursor] = useState('default');
  const drag = useRef<Drag | null>(null);
  // Free tension-handle times, keyed by note id, lane and segment while the editor is open.
  const lastLength = useRef(0);
  const awaitingRev = useRef<number | null>(null);
  const rev = useDawg((s) => s.rev);

  const step = gridTicks > 0 ? gridTicks : TICKS_PER_BEAT / 4;
  if (lastLength.current === 0) lastLength.current = step;

  // Notes that existed before a commit that creates notes; the new ones get selected once it lands.
  const selectNewAfter = useRef<ReadonlySet<number> | null>(null);

  // Keep the preview on screen until the committed change comes back from the engine.
  useEffect(() => {
    if (awaitingRev.current === null || rev <= awaitingRev.current) return;
    awaitingRev.current = null;
    setPreview(null);

    const before = selectNewAfter.current;
    selectNewAfter.current = null;
    if (before) ui.selectNotes(notes.map((n) => n.note.id).filter((id) => !before.has(id)));
  }, [rev]);

  const commit = (command: ReturnType<typeof addNotesCommand>, options: { selectNew?: boolean } = {}) => {
    if (!command) {
      setPreview(null);
      return;
    }
    if (options.selectNew) selectNewAfter.current = new Set(notes.map((n) => n.note.id));
    awaitingRev.current = useDawg.getState().rev;
    dispatch(command);
  };

  const remove = (toRemove: EditNote[]) => {
    commit(removeNotesCommand(toRemove));
    ui.selectNotes([]);
  };

  const selection = () => notes.filter((n) => selected.has(n.note.id)).map(toEditNote);

  const playheadTick = () => useDawg.getState().telemetry.positionBeats * TICKS_PER_BEAT;

  // -------------------------------------------------------------------------------------------------------
  // Curve mappings

  /** Grid snapping for curve times: note-relative in and out, on the roll's absolute grid. */
  const timeSnapper = (placed: PlacedNote) =>
    gridTicks > 0 ? (t: number) => map.snap(placed.start + t, gridTicks, true) - placed.start : undefined;

  /** The limits of a note's curve in a lane: the model's full range, within the note. */
  const curveBounds = (placed: PlacedNote, lane: LaneId): CurveBounds => ({
    min: lane === 'pitch' ? -48 : 0,
    max: lane === 'pitch' ? 48 : 1,
    maxTime: placed.end - placed.start,
    anchorStart: lane !== 'vibrato',
  });

  /** Maps a note's pitch curve onto the roll. */
  const rollPitchMapping = (placed: PlacedNote): CurveMapping => ({
    ...curveBounds(placed, 'pitch'),
    toX: (t) => tickToX(view, placed.start + t),
    toY: (v) => pitchToY(view, placed.note.pitch + v) + view.keyHeight / 2,
    fromX: (x) => xToTick(view, x) - placed.start,
    fromY: (y) => yToPitch(view, y) - placed.note.pitch,
    neutral: 0,
    snapValue: Math.round,
    snapTime: timeSnapper(placed),
  });

  /** Maps a note's curve onto an expression lane of the given height. */
  const laneMapping = (placed: PlacedNote, lane: LaneId, height: number): CurveMapping => {
    const [min, max] = laneRanges[lane];
    const span = Math.max(1, height - 2 * LANE_PAD);
    return {
      ...curveBounds(placed, lane),
      toX: (t) => tickToX(view, placed.start + t),
      toY: (v) => LANE_PAD + (1 - (v - min) / (max - min)) * span,
      fromX: (x) => xToTick(view, x) - placed.start,
      fromY: (y) => min + (1 - (y - LANE_PAD) / span) * (max - min),
      neutral: LANE_DEFAULTS[lane],
      snapValue: lane === 'pitch' ? Math.round : undefined,
      snapTime: timeSnapper(placed),
    };
  };

  const clickCount = useClickCount();

  const curveEdit = (placed: PlacedNote, lane: LaneId, points: CurvePoint[]): EditNote => ({
    ...toEditNote(placed),
    expression: { ...placed.note.expression, [lane]: points },
  });

  const previewCurve = (placed: PlacedNote, lane: LaneId, points: CurvePoint[]) =>
    setPreview([curveEdit(placed, lane, points)]);

  const commitCurve = (placed: PlacedNote, lane: LaneId, points: CurvePoint[], label = 'Edit expression') =>
    commit(setCurvesCommand([{ placed, lane, points }], label));

  /** Notes with selected points, with their curves in the selected lane. */
  const selectedPointGroups = () => {
    if (!selectedPoints) return [];
    const { lane } = selectedPoints;
    return notes.flatMap((placed) => {
      const indices = selectedPoints.notes.get(placed.note.id);
      return indices ? [{ placed, lane, points: placed.note.expression[lane], indices }] : [];
    });
  };

  /** Selected notes edited as a whole: those without selected points. */
  const wholeNotes = () => selection().filter((n) => !selectedPoints?.notes.has(n.id!));

  /** The groups of a point drag: the grabbed point's note first, then the other notes with selected points. */
  const dragGroups = (ref: CurveRef, selection: PointSelection): PointGroup[] => {
    const own: PointGroup = {
      placed: ref.placed,
      map: ref.map,
      points: ref.placed.note.expression[ref.lane],
      indices: selection.notes.get(ref.placed.note.id) ?? [],
    };
    const others = notes
      .filter((n) => n.note.id !== ref.placed.note.id && selection.notes.has(n.note.id))
      .map((placed) => ({
        placed,
        map: ref.mapFor(placed),
        points: placed.note.expression[ref.lane],
        indices: selection.notes.get(placed.note.id)!,
      }));
    return [own, ...others];
  };

  /**
   * Handles a press on a curve's point or handle (and, with `addOnAlt`, an Alt+click that adds a point).
   * Returns false if nothing on the curve was hit.
   */
  const curvePointerDown = (
    ref: CurveRef,
    x: number,
    y: number,
    mods: { alt: boolean; shift: boolean; free: boolean; clicks: number },
    addOnAlt: boolean,
  ) => {
    const { placed, lane, map } = ref;
    const points = placed.note.expression[lane];
    const hit = hitCurve(points, x, y, map);
    const current = pointsOf(selectedPoints, placed.note.id, lane);

    if (hit?.kind === 'point') {
      if (mods.clicks >= 2) {
        const remaining = deletePoints(points, [hit.index], map.anchorStart);
        previewCurve(placed, lane, remaining);
        commitCurve(placed, lane, remaining, 'Delete points');
        ui.selectPoints(null);
        return true;
      }

      if (mods.shift) {
        const indices = current.includes(hit.index) ? current.filter((i) => i !== hit.index) : [...current, hit.index];
        const others = selectedPoints?.lane === lane ? selectedPoints.notes : [];
        ui.selectPoints({ lane, notes: new Map([...others, [placed.note.id, indices]]) });
        return true;
      }

      // A selected point drags the whole point selection; another point becomes the only selection.
      let selection = selectedPoints;
      if (!selection || !current.includes(hit.index)) {
        selection = { lane, notes: new Map([[placed.note.id, [hit.index]]]) };
        ui.selectNotesAndPoints([placed.note.id], selection);
      }
      drag.current = { kind: 'points', lane, groups: dragGroups(ref, selection), grab: hit.index, moved: false };
      return true;
    }

    if (hit?.kind === 'handle' && mods.clicks >= 2) {
      const straight = straightenSegment(points, hit.segment);
      previewCurve(placed, lane, straight);
      commitCurve(placed, lane, straight, 'Straighten segment');
      return true;
    }

    if (hit?.kind === 'handle') {
      const handle = handlePosition(points, hit.segment, map);
      drag.current = {
        kind: 'bend',
        ...ref,
        points,
        segment: hit.segment,
        grab: { x: x - handle.x, y: y - handle.y },
      };
      return true;
    }

    // Alt+click or a double-click adds a point, picked up right away.
    if ((mods.alt || mods.clicks >= 2) && addOnAlt) {
      const added = addPoint(points, x, y, map, mods.free);
      ui.selectPoints({ lane, notes: new Map([[placed.note.id, [added.index]]]) });
      previewCurve(placed, lane, added.points);
      const group = { placed, map, points: added.points, indices: [added.index] };
      drag.current = { kind: 'points', lane, groups: [group], grab: added.index, moved: true };
      return true;
    }

    return false;
  };

  // -------------------------------------------------------------------------------------------------------
  // Hit testing

  /** Notes whose vibrato is audible somewhere (intensity lane above zero). */
  const findVibratoHandle = (x: number, y: number) => {
    for (const placed of notes) {
      if (!selected.has(placed.note.id)) continue;
      const handles = vibratoHandles(view, placed, map);
      if (!handles) continue;
      if (Math.abs(handles.depth.x - x) <= 6 && Math.abs(handles.depth.y - y) <= 6) return { handle: 'depth' as const, placed };
      if (Math.abs(handles.rate.x - x) <= 5 && Math.abs(handles.rate.y - y) <= 8) return { handle: 'rate' as const, placed };
    }
    return null;
  };

  const hit = (x: number, y: number): { placed: PlacedNote; zone: Zone } | null => {
    for (let i = notes.length - 1; i >= 0; i--) {
      const placed = notes[i];
      const x0 = tickToX(view, placed.start);
      const x1 = tickToX(view, placed.end);
      if (x < x0 || x > x1) continue;
      const pitch = isBent(placed.note) ? pitchAtTick(placed, xToTick(view, x), map) : placed.note.pitch;
      const top = pitchToY(view, pitch);
      if (y < top || y > top + view.keyHeight) continue;
      const edge = Math.min(EDGE_PX, (x1 - x0) / 3);
      return { placed, zone: x - x0 < edge ? 'start' : x1 - x < edge ? 'end' : 'body' };
    }
    return null;
  };

  /** Notes whose pitch points the roll shows: the selected ones and those with selected pitch points. */
  const showsPitchPoints = (placed: PlacedNote) =>
    selected.has(placed.note.id) || pointsOf(selectedPoints, placed.note.id, 'pitch').length > 0;

  /** A press on a shown pitch point or handle in the roll (never adds points). */
  const rollCurveDown = (x: number, y: number, mods: { alt: boolean; shift: boolean; free: boolean; clicks: number }) => {
    for (const placed of notes) {
      if (!showsPitchPoints(placed)) continue;
      const map = rollPitchMapping(placed);
      if (hitCurve(placed.note.expression.pitch, x, y, map))
        return curvePointerDown({ placed, lane: 'pitch', map, mapFor: rollPitchMapping }, x, y, mods, false);
    }
    return false;
  };

  // -------------------------------------------------------------------------------------------------------
  // Pointer (roll)

  const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!track || e.button !== 0) return; // right-click opens the context menu
    const x = e.nativeEvent.offsetX;
    const y = e.nativeEvent.offsetY;
    e.currentTarget.setPointerCapture(e.pointerId);
    e.currentTarget.focus({ preventScroll: true });

    const mods = { alt: e.altKey, shift: e.shiftKey, free: e.ctrlKey || e.metaKey, clicks: clickCount(x, y) };

    // Vibrato handles of selected notes: depth (up/down) and rate (left/right).
    const vibratoHit = findVibratoHandle(x, y);
    if (vibratoHit) {
      const originals = notes.filter((n) => selected.has(n.note.id)).map(toEditNote);
      drag.current = { kind: 'vibrato', handle: vibratoHit.handle, placed: vibratoHit.placed, originals };
      return;
    }

    if (e.altKey && (e.ctrlKey || e.metaKey)) {
      const from = hit(x, y);
      if (from) {
        drag.current = { kind: 'link', placed: from.placed, x0: x, y0: y, x, y };
        return;
      }
    }

    if (rollCurveDown(x, y, mods)) return;

    const target = hit(x, y);

    if (!target) {
      if (!e.shiftKey) ui.selectPoints(null);
      // Alt+click, or a double-click as in other DAWs: a note, as long as the drag that follows.
      if (e.altKey || mods.clicks >= 2) {
        const start =
          e.ctrlKey || e.metaKey
            ? Math.round(xToTick(view, x))
            : map.snap(xToTick(view, x), gridTicks);
        const pitch = toScale(pitchAtY(view, y), start);
        const note: EditNote = { start, length: lastLength.current, pitch, velocity: 0.8 };
        ui.selectNotes([]);
        drag.current = { kind: 'create', note, originTick: start };
        setPreview([note]);
        auditioner.play(track.id, pitch);
      } else {
        const base = e.shiftKey
          ? { notes: selected, points: selectedPoints?.lane === 'pitch' ? selectedPoints.notes : new Map<number, number[]>() }
          : { notes: new Set<number>(), points: new Map<number, number[]>() };
        drag.current = { kind: 'band', band: { x0: x, y0: y, x1: x, y1: y }, base };
      }
      return;
    }

    const id = target.placed.note.id;

    // Shift on a note edge still toggles the selection. On the body it starts a direction-locked move; if it turns
    // out to be a click, a selected note is deselected on release (the usual Shift+click toggle).
    if (e.shiftKey && !e.altKey && !(e.ctrlKey || e.metaKey) && target.zone !== 'body') {
      const next = new Set(selected);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      ui.selectNotes(next, true);
      return;
    }

    let activeSelection: ReadonlySet<number> = selected.has(id) ? selected : new Set([id]);
    let toggleOnClick: number | null = null;

    if (e.shiftKey && !e.altKey && !(e.ctrlKey || e.metaKey) && target.zone === 'body') {
      if (selected.has(id)) {
        toggleOnClick = id;
      } else {
        activeSelection = new Set([...selected, id]);
        ui.selectNotes(activeSelection, true);
      }
    } else if (!selected.has(id)) {
      // Clicking a note selects it as a whole, even if a box selection took some of its points.
      ui.selectNotesAndPoints(activeSelection, null);
    }

    // A double-click on a note's body adds a pitch point there, as Shift+Alt+click does.
    if (mods.clicks >= 2 && target.zone === 'body' && !e.altKey) {
      addPitchPointAt(x, y);
      return;
    }

    // Alt on a note: a click splits it (Shift+Alt: adds a pitch point), a drag copies the selection (decided on
    // the first move).
    if (e.altKey) {
      drag.current = { kind: 'altNote', placed: target.placed, x, y, addPoint: e.shiftKey, selection: activeSelection };
      return;
    }

    const originals = notes.filter((n) => activeSelection.has(n.note.id)).map(toEditNote);
    const grabbed = toEditNote(target.placed);
    const anchorTick = xToTick(view, x);
    const mod = e.ctrlKey || e.metaKey;

    if (mod && e.shiftKey) drag.current = { kind: 'velocity', anchorY: y, originals };
    else if (target.zone === 'start') drag.current = { kind: 'resizeStart', anchorTick, originals, grabbed };
    else if (target.zone === 'end') drag.current = { kind: 'resizeEnd', anchorTick, originals, grabbed };
    else {
      drag.current = moveDrag(anchorTick, x, y, originals, grabbed, false, e.shiftKey && !e.altKey, toggleOnClick);
      auditioner.play(track.id, grabbed.pitch, grabbed.velocity);
    }
  };

  const moveDrag = (
    anchorTick: number,
    x: number,
    y: number,
    originals: EditNote[],
    grabbed: EditNote,
    copy: boolean,
    lockable: boolean,
    toggleOnClick: number | null = null,
  ): Drag => ({
    kind: 'move',
    anchorTick,
    // The row grabbed: a pitch, or in a folded roll a row index (docs/design/scale-highlighting.md, SH5).
    anchorPitch: rowAtY(view, y),
    anchorX: x,
    anchorY: y,
    originals,
    grabbed,
    moved: false,
    copy,
    lockable,
    lock: null,
    toggleOnClick,
  });

  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const x = e.nativeEvent.offsetX;
    const y = e.nativeEvent.offsetY;
    const d = drag.current;

    if (!d) {
      updateCursor(x, y, e);
      return;
    }

    // Ctrl/Cmd places notes, clips and curve points off the grid.
    dragTo(d, x, y, e.ctrlKey || e.metaKey);
  };

  const updateCursor = (x: number, y: number, e: React.PointerEvent) => {
    const vibratoHit = findVibratoHandle(x, y);
    if (vibratoHit) {
      setCursor(vibratoHit.handle === 'depth' ? 'ns-resize' : 'ew-resize');
      return;
    }
    if (notes.some((n) => showsPitchPoints(n) && hitCurve(n.note.expression.pitch, x, y, rollPitchMapping(n)))) {
      setCursor('pointer');
      return;
    }
    const target = hit(x, y);
    const mod = e.ctrlKey || e.metaKey;
    if (!target) setCursor(e.altKey ? 'crosshair' : 'default');
    else if (mod && e.shiftKey) setCursor('ns-resize');
    else if (e.altKey) setCursor('copy');
    else setCursor(target.zone === 'body' ? 'grab' : 'ew-resize');
  };

  /** Applies pointer travel to the current gesture (roll and lanes). */
  const dragTo = (d: Drag, x: number, y: number, free: boolean) => {
    const tick = xToTick(view, x);
    // Dragged edges snap to the roll's absolute grid (bar-relative through the tempo map), unless free.
    const gridOf = (unsnapped: boolean): Grid => (unsnapped || gridTicks <= 0 ? null : (t, round) => map.snap(t, gridTicks, round));

    switch (d.kind) {
      case 'create': {
        const end = free
          ? Math.max(d.originTick + 1, Math.round(tick))
          : Math.max(d.originTick + step, map.snap(tick, gridTicks, true));
        d.note = { ...d.note, length: end - d.originTick };
        setPreview([d.note]);
        break;
      }
      case 'altNote': {
        if (Math.hypot(x - d.x, y - d.y) <= DRAG_THRESHOLD_PX) break;
        // Alt+drag on a note: copy the selection.
        const originals = notes.filter((n) => selected.has(n.note.id) || n.note.id === d.placed.note.id).map(toEditNote);
        drag.current = moveDrag(xToTick(view, d.x), d.x, d.y, originals, toEditNote(d.placed), true, false);
        dragTo(drag.current, x, y, free);
        break;
      }
      case 'move': {
        // Shift locks the move to whichever direction the pointer went first.
        if (d.lockable && d.lock === null && Math.hypot(x - d.anchorX, y - d.anchorY) > DRAG_THRESHOLD_PX)
          d.lock = Math.abs(x - d.anchorX) >= Math.abs(y - d.anchorY) ? 'time' : 'pitch';

        // The grabbed note's start lands on a grid line; the others keep their places around it.
        const delta = d.lock === 'pitch' ? 0 : snappedTravel(d.grabbed.start, tick - d.anchorTick, gridOf(free));
        const rowDelta = d.lock === 'time' ? 0 : rowAtY(view, y) - d.anchorPitch;
        d.moved = d.moved || delta !== 0 || rowDelta !== 0;
        const minStart = Math.min(...d.originals.map((n) => n.start));
        const safeDelta = Math.max(delta, -minStart);
        // Folded, each note moves by rows, so onto shown rows. Unfolded with Snap to scale, the grabbed note goes to the
        // scale key nearest where it was dragged and the others move with it, each into the scale where it lands.
        const grabbedTo = view.rows
          ? stepRows(view, d.grabbed.pitch, rowDelta)
          : toScale(d.grabbed.pitch + rowDelta, d.grabbed.start + safeDelta);
        const pitchDelta = grabbedTo - d.grabbed.pitch;
        const pitchOf = (n: EditNote) =>
          view.rows
            ? stepRows(view, n.pitch, rowDelta)
            : rowDelta === 0 && !scaleFor(n.start + safeDelta)
              ? n.pitch
              : toScale(n.pitch + pitchDelta, n.start + safeDelta);
        const moved = d.originals.map((n) => ({ ...n, start: n.start + safeDelta, pitch: clampPitch(pitchOf(n)) }));
        // A copy previews as new notes (no ids), so the originals stay where they are.
        setPreview(d.copy ? moved.map((n) => ({ ...n, id: undefined, clipId: undefined })) : moved);
        if (track) auditioner.play(track.id, clampPitch(d.grabbed.pitch + pitchDelta), d.grabbed.velocity);
        break;
      }
      case 'resizeStart': {
        setPreview(resizeStarts(d.originals, d.grabbed, tick - d.anchorTick, step, gridOf(free)));
        break;
      }
      case 'resizeEnd': {
        setPreview(resizeEnds(d.originals, d.grabbed, tick - d.anchorTick, step, gridOf(free)));
        break;
      }
      case 'velocity': {
        const delta = (d.anchorY - y) / VELOCITY_PX;
        setPreview(d.originals.map((n) => ({ ...n, velocity: clampVelocity(n.velocity + delta) })));
        break;
      }
      case 'link': {
        d.x = x;
        d.y = y;
        setLinkDrag({ x0: d.x0, y0: d.y0, x1: x, y1: y });
        break;
      }
      case 'band': {
        d.band = { ...d.band, x1: x, y1: y };
        setBandTarget('roll');
        setBand({ ...d.band });
        break;
      }
      case 'points': {
        d.moved = true;
        const [first, ...others] = d.groups;
        const moved = movePoints(first.points, first.indices, d.grab, x, y, first.map, free);
        // The other notes' points follow the grabbed point's travel.
        const grabbed = moved.points[moved.indices[first.indices.indexOf(d.grab)]];
        const dt = grabbed[0] - first.points[d.grab][0];
        const dv = grabbed[1] - first.points[d.grab][1];
        const results = [moved, ...others.map((g) => shiftPoints(g.points, g.indices, dt, dv, g.map))];
        setPreview(d.groups.map((g, i) => curveEdit(g.placed, d.lane, results[i].points)));
        ui.selectPoints({ lane: d.lane, notes: new Map(d.groups.map((g, i) => [g.placed.note.id, results[i].indices])) });
        break;
      }
      case 'bend': {
        const bent = bendSegmentToPointer(d.points, d.segment, d.map, x, y, d.grab);
        previewCurve(d.placed, d.lane, bent.points);
        break;
      }
      case 'pointBand': {
        d.band = { ...d.band, x1: x, y1: y };
        setBandTarget('lane');
        setBand({ ...d.band });
        break;
      }
      case 'vibrato': {
        const v = d.placed.note.vibrato ?? NO_VIBRATO;
        let change: Partial<typeof v>;
        if (d.handle === 'depth') {
          const depth = Math.min(2, Math.max(0.05, Math.abs(yToPitch(view, y) - d.placed.note.pitch)));
          change = { depth: Math.round(depth * 100) / 100 };
        } else {
          const begin = d.placed.start + v.delay;
          const cycle = Math.max(TICKS_PER_BEAT / 64, tick - begin);
          if (v.syncBeats > 0) {
            const options = [2, 1, 1 / 2, 1 / 3, 1 / 4, 1 / 6, 1 / 8];
            const beats = cycle / TICKS_PER_BEAT;
            change = { syncBeats: options.reduce((best, o) => (Math.abs(o - beats) < Math.abs(best - beats) ? o : best)) };
          } else {
            const rate = 1 / (map.secondsAt(begin + cycle) - map.secondsAt(begin));
            change = { rateHz: Math.round(Math.min(12, Math.max(0.5, rate)) * 100) / 100 };
          }
        }
        setPreview(d.originals.map((n) => ({ ...n, vibrato: { ...(n.vibrato ?? NO_VIBRATO), ...change } })));
        break;
      }
    }
  };

  const onPointerUp = () => {
    const d = drag.current;
    drag.current = null;
    auditioner.stop();
    if (!d || !track) return;
    const current = previewRef.current;

    switch (d.kind) {
      case 'create':
        lastLength.current = d.note.length;
        commit(addNotesCommand(track, [d.note], map, 'Add note', editedClipId), { selectNew: true });
        break;
      case 'altNote': {
        // Alt+click on a note (no drag) splits it there; Shift+Alt+click adds a pitch point (snapped: Shift is
        // taken, so a free value comes from dragging the point with Shift afterwards).
        if (!d.addPoint) {
          splitAt(d.x, d.y, false, d.selection);
          break;
        }
        const map = rollPitchMapping(d.placed);
        const added = addPoint(d.placed.note.expression.pitch, d.x, d.y, map);
        previewCurve(d.placed, 'pitch', added.points);
        commitCurve(d.placed, 'pitch', added.points, 'Add pitch point');
        ui.selectPoints({ lane: 'pitch', notes: new Map([[d.placed.note.id, [added.index]]]) });
        break;
      }
      case 'move':
        if (!d.moved && d.toggleOnClick !== null) {
          const next = new Set(selected);
          next.delete(d.toggleOnClick);
          ui.selectNotes(next, true);
          setPreview(null);
        } else if (!d.moved || !current) setPreview(null);
        else if (d.copy) commit(addNotesCommand(track, current, map, 'Copy notes', editedClipId), { selectNew: true });
        else commit(updateNotesCommand(track, clampToClips(track, current), map, 'Move notes'));
        break;
      case 'resizeStart':
      case 'resizeEnd': {
        const edited = current ?? [];
        const grabbed = edited.find((n) => n.id === d.grabbed.id);
        if (grabbed) lastLength.current = grabbed.length;
        commit(updateNotesCommand(track, clampToClips(track, edited), map, 'Resize notes'));
        break;
      }
      case 'velocity':
        commit(current ? updateNotesCommand(track, current, map, 'Change velocity') : null);
        break;
      case 'vibrato':
        commit(current ? updateNotesCommand(track, current, map, 'Edit vibrato') : null);
        break;
      case 'points': {
        if (!d.moved) {
          setPreview(null);
          break;
        }
        const edits = d.groups.flatMap(({ placed }) => {
          const points = current?.find((n) => n.id === placed.note.id)?.expression?.[d.lane];
          return points ? [{ placed, lane: d.lane, points }] : [];
        });
        commit(setCurvesCommand(edits, 'Edit expression'));
        break;
      }
      case 'bend': {
        const edited = current?.[0]?.expression?.[d.lane];
        commit(edited ? setCurvesCommand([{ placed: d.placed, lane: d.lane, points: edited }], 'Edit expression') : null);
        break;
      }
      case 'link': {
        setLinkDrag(null);
        const onto = hit(d.x, d.y);
        if (onto && onto.placed.note.id !== d.placed.note.id) {
          const command = linkCommand(d.placed, onto.placed);
          if (command) dispatch(command);
          else useDawg.setState({ lastHint: 'Only notes of one clip that start at different times can be linked' });
        }
        break;
      }
      case 'band': {
        const { band: b, base } = d;
        const [left, right] = [Math.min(b.x0, b.x1), Math.max(b.x0, b.x1)];
        const [top, bottom] = [Math.min(b.y0, b.y1), Math.max(b.y0, b.y1)];
        const chosen = boxSelect(
          notes.map((n) => {
            // A flat curve (the engine keeps an anchor on every note) counts as no points: the box selects the note.
            const curve = n.note.expression.pitch;
            const bent = curve.some((p) => p[1] !== 0);
            const y = pitchToY(view, n.note.pitch);
            return {
              id: n.note.id,
              pointCount: bent ? curve.length : 0,
              inside: bent ? pointsInRect(curve, rollPitchMapping(n), b.x0, b.y0, b.x1, b.y1) : [],
              touched:
                tickToX(view, n.end) >= left && tickToX(view, n.start) <= right && y + view.keyHeight >= top && y <= bottom,
            };
          }),
          base,
        );
        ui.selectNotesAndPoints(chosen.notes, { lane: 'pitch', notes: chosen.points });
        setBand(null);
        break;
      }
      case 'pointBand': {
        const { band: b } = d;
        const id = d.placed.note.id;
        const inside = pointsInRect(d.placed.note.expression[d.lane], d.map, b.x0, b.y0, b.x1, b.y1);
        const indices = [...new Set([...(d.base.get(id) ?? []), ...inside])];
        ui.selectPoints({ lane: d.lane, notes: new Map([...d.base, [id, indices]]) });
        setBand(null);
        break;
      }
    }
  };

  // -------------------------------------------------------------------------------------------------------
  // Keyboard

  const applyToSelection = (label: string, change: (n: EditNote) => EditNote) => {
    if (!track || selected.size === 0) return;
    commit(updateNotesCommand(track, clampToClips(track, selection().map(change)), map, label));
  };

  /**
   * Up and Down, Ctrl/Cmd+Left and Right: shifts the selected points by `dt` ticks and `dv`, and changes the selected notes without selected
   * points, together as one undo step.
   */
  const nudge = (noteLabel: string, change: (n: EditNote) => EditNote, dt: number, dv: number) => {
    if (!track) return;
    const whole = clampToClips(track, wholeNotes().map(change));
    const groups = selectedPointGroups().map((g) => ({
      ...g,
      ...shiftPoints(g.points, g.indices, dt, dv, curveBounds(g.placed, g.lane)),
    }));
    const label = groups.length === 0 ? noteLabel : whole.length === 0 ? 'Move points' : 'Move notes and points';
    setPreview([...whole, ...groups.map((g) => curveEdit(g.placed, g.lane, g.points))]);
    commit(
      combineCommands(label, [
        whole.length > 0 ? updateNotesCommand(track, whole, map, label) : null,
        groups.length > 0 ? setCurvesCommand(groups, label) : null,
      ]),
    );
    if (selectedPoints && groups.length > 0)
      ui.selectPoints({ lane: selectedPoints.lane, notes: new Map(groups.map((g) => [g.placed.note.id, g.indices])) });
  };

  const pasteNotes = (copied: EditNote[], at: number, label: string) => {
    if (!track || copied.length === 0) return;
    const pasted = copied.map((n) => ({ ...n, id: undefined, clipId: undefined, copiedFrom: n.id, start: n.start + at }));
    commit(addNotesCommand(track, pasted, map, label, editedClipId), { selectNew: true });
  };

  const copy = () => {
    // Points copy from one note only.
    const groups = selectedPointGroups();
    if (groups.length > 1) return false;
    if (groups.length === 1) {
      const [{ lane, points, indices }] = groups;
      const chosen = points.filter((_, i) => indices.includes(i));
      if (chosen.length === 0) return false;
      const origin = chosen[0][0];
      clipboard = { kind: 'curve', lane, points: chosen.map(([t, ...rest]) => [t - origin, ...rest]) };
      return true;
    }

    const notesToCopy = selection();
    if (notesToCopy.length === 0) return false;
    const origin = Math.min(...notesToCopy.map((n) => n.start));
    clipboard = { kind: 'notes', notes: notesToCopy.map((n) => ({ ...n, start: n.start - origin })) };
    return true;
  };

  /** Deletes the selected points and the selected notes without selected points, as one undo step. */
  const deleteSelection = () => {
    const groups = selectedPointGroups();
    if (groups.length === 0) {
      remove(selection());
      return;
    }
    const whole = wholeNotes();
    const curves = groups.map((g) => ({ ...g, points: deletePoints(g.points, g.indices, g.lane !== 'vibrato') }));
    const label = whole.length > 0 ? 'Delete' : 'Delete points';
    commit(combineCommands(label, [setCurvesCommand(curves, 'Delete points'), removeNotesCommand(whole)]));
    const removed = new Set(whole.map((n) => n.id));
    ui.selectNotesAndPoints([...selected].filter((id) => !removed.has(id)), null);
  };

  /** Pastes copied curve points at the playhead into the selected notes that span it (else any that do). */
  const pasteCurve = (lane: LaneId, copied: CurvePoint[]) => {
    const tick = map.snap(playheadTick(), gridTicks, true);
    const spanning = notes.filter((n) => tick >= n.start && tick < n.end);
    const targets = spanning.some((n) => selected.has(n.note.id)) ? spanning.filter((n) => selected.has(n.note.id)) : spanning;
    if (targets.length === 0) return;

    commit(
      setCurvesCommand(
        targets.map((placed) => ({
          placed,
          lane,
          points: pastePoints(
            placed.note.expression[lane],
            copied,
            tick - placed.start,
            placed.end - placed.start,
            LANE_DEFAULTS[lane],
          ),
        })),
        'Paste points',
      ),
    );
  };

  /** Splits notes at a roll position, snapped to the grid unless free: the selected notes there, or else the
      note under it. */
  const splitAt = (x: number, y: number, free: boolean, among: ReadonlySet<number> = selected) => {
    const tick = xToTick(view, x);
    const at = free ? Math.round(tick) : map.snap(tick, gridTicks, true);
    const inside = (n: PlacedNote) => at > n.start && at < n.end;
    const chosen = notes.filter((n) => among.has(n.note.id) && inside(n));
    const under = hit(x, y)?.placed;
    const targets = chosen.length > 0 ? chosen : under && inside(under) ? [under] : [];
    commit(splitNotesCommand(targets, at));
  };

  /** Adds a pitch point to the note at a roll position, as Shift+Alt+click does (for the context menu). */
  const addPitchPointAt = (x: number, y: number) => {
    const placed = hit(x, y)?.placed;
    if (!placed) return;
    const added = addPoint(placed.note.expression.pitch, x, y, rollPitchMapping(placed));
    commitCurve(placed, 'pitch', added.points, 'Add pitch point');
    ui.selectPoints({ lane: 'pitch', notes: new Map([[placed.note.id, [added.index]]]) });
  };

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (!track) return;
    const mod = e.ctrlKey || e.metaKey;
    const key = e.key;
    let handled = true;

    if (key === 'Delete' || key === 'Backspace') deleteSelection();
    else if (mod && key.toLowerCase() === 'a') ui.selectNotesAndPoints(notes.map((n) => n.note.id), null);
    else if (key === 'Escape') {
      if (selectedPoints) ui.selectPoints(null);
      else ui.selectNotes([]);
    } else if (mod && key.toLowerCase() === 'c') copy();
    else if (mod && key.toLowerCase() === 'x') {
      if (copy()) deleteSelection();
    } else if (mod && key.toLowerCase() === 'v') {
      if (clipboard?.kind === 'curve') pasteCurve(clipboard.lane, clipboard.points);
      else if (clipboard?.kind === 'notes') pasteNotes(clipboard.notes, map.snap(playheadTick(), gridTicks), 'Paste notes');
    } else if (mod && key.toLowerCase() === 'd') {
      const current = selection();
      if (current.length > 0) {
        const origin = Math.min(...current.map((n) => n.start));
        const end = Math.max(...current.map((n) => n.start + n.length));
        const relative = current.map((n) => ({ ...n, start: n.start - origin }));
        pasteNotes(relative, origin + Math.ceil((end - origin) / step) * step, 'Duplicate notes');
      }
    } else if (key.toLowerCase() === 's' && !mod) {
      const at = Math.round(playheadTick());
      commit(splitNotesCommand(notes.filter((n) => selected.has(n.note.id) && at > n.start && at < n.end), at));
    } else if (key.toLowerCase() === 'q' && !mod) {
      const { swing, quantiseStrength } = useUi.getState();
      const swung = swingApplies(step / TICKS_PER_BEAT) ? swing : 0.5;
      applyToSelection('Quantise', (n) => quantiseNote(n, map, step, swung, quantiseStrength, e.shiftKey));
    } else if (key.toLowerCase() === 'h' && !mod) {
      ui.setHumaniseOpen(true);
    } else if (e.altKey && (key === 'ArrowUp' || key === 'ArrowDown')) {
      const delta = key === 'ArrowUp' ? 0.05 : -0.05;
      applyToSelection('Change velocity', (n) => ({ ...n, velocity: clampVelocity(n.velocity + delta) }));
    } else if (e.altKey && (key === 'ArrowLeft' || key === 'ArrowRight')) {
      const delta = key === 'ArrowRight' ? step : -step;
      if (e.shiftKey)
        applyToSelection('Resize notes', (n) => ({ ...n, length: Math.max(step, n.length + delta) }));
      else
        applyToSelection('Resize notes', (n) => {
          const end = n.start + n.length;
          const start = Math.min(Math.max(0, n.start + delta), end - step);
          return { ...n, start, length: end - start };
        });
    } else if ((!mod && (key === 'ArrowLeft' || key === 'ArrowRight')) || (mod && (key === 'ArrowUp' || key === 'ArrowDown'))) {
      const next =
        key === 'ArrowLeft' || key === 'ArrowRight'
          ? adjacentNote(notes, selected, key === 'ArrowRight' ? 1 : -1)
          : noteInPitchDirection(notes, selected, key === 'ArrowUp' ? 1 : -1);
      if (next) {
        ui.selectNotesAndPoints([next.note.id], null);
        reveal(next);
      }
    } else if (key === 'ArrowLeft' || key === 'ArrowRight') {
      const delta = key === 'ArrowRight' ? step : -step;
      nudge('Move notes', (n) => ({ ...n, start: Math.max(0, n.start + delta) }), delta, 0);
    } else if (key === 'ArrowUp' || key === 'ArrowDown') {
      // Notes and pitch points by a semitone (Shift: an octave), the other lanes' points by 5 % (Shift: 25 %).
      const sign = key === 'ArrowUp' ? 1 : -1;
      const delta = sign * (e.shiftKey ? 12 : 1);
      const pointDelta = selectedPoints?.lane === 'pitch' ? delta : sign * (e.shiftKey ? 0.25 : 0.05);
      // Folded, a step is a shown row; unfolded with Snap to scale, the next key in the scale (SH5). An octave is
      // always twelve semitones.
      const step = (n: EditNote) =>
        e.shiftKey
          ? n.pitch + delta
          : view.rows
            ? stepRows(view, n.pitch, sign)
            : toScale(n.pitch + sign, n.start, sign > 0 ? 'up' : 'down');
      nudge('Transpose', (n) => ({ ...n, pitch: clampPitch(step(n)) }), 0, pointDelta);
    } else handled = false;

    if (handled) {
      e.preventDefault();
      e.stopPropagation();
    }
  };

  // -------------------------------------------------------------------------------------------------------
  // Velocity lane: paint velocities by dragging across the stems.

  const laneEdits = useRef<Map<number, EditNote> | null>(null);
  const laneLastX = useRef(0);

  const onVelocityLane = (phase: 'down' | 'move' | 'up', x: number, y: number, height: number) => {
    if (!track) return;

    if (phase === 'up') {
      const edits = laneEdits.current;
      laneEdits.current = null;
      if (edits && edits.size > 0) commit(updateNotesCommand(track, [...edits.values()], map, 'Change velocity'));
      return;
    }

    if (phase === 'down') {
      laneEdits.current = new Map();
      laneLastX.current = x;
    }
    const edits = laneEdits.current;
    if (!edits) return;

    // Paint everything between the previous and the current pointer position, so fast drags don't skip stems.
    const [from, to] = [Math.min(laneLastX.current, x) - 5, Math.max(laneLastX.current, x) + 5];
    laneLastX.current = x;

    const [min, max] = laneRanges.velocity;
    const value = clampVelocity(min + (1 - (y - LANE_PAD) / Math.max(1, height - 2 * LANE_PAD)) * (max - min));
    const underPointer = notes.filter((n) => {
      const stem = tickToX(view, n.start);
      return stem >= from && stem <= to;
    });
    const preferSelected = underPointer.some((n) => selected.has(n.note.id));

    for (const placed of underPointer)
      if (!preferSelected || selected.has(placed.note.id))
        edits.set(placed.note.id, { ...toEditNote(placed), velocity: value });

    setPreview([...edits.values()]);
  };

  // -------------------------------------------------------------------------------------------------------
  // Expression lanes (pressure, timbre, pitch, vibrato): point editing.

  const onCurveLane = (
    phase: 'down' | 'move' | 'up',
    lane: LaneId,
    x: number,
    y: number,
    height: number,
    mods: { alt: boolean; shift: boolean; free: boolean } = { alt: false, shift: false, free: false },
  ) => {
    if (!track) return;
    const d = drag.current;

    if (phase === 'move') {
      if (d) dragTo(d, x, y, mods.free);
      return;
    }

    if (phase === 'up') {
      if (d) onPointerUp();
      return;
    }

    const tick = xToTick(view, x);
    const candidates = notes.filter((n) => tick >= n.start && tick <= n.end);
    const preferred = candidates.some((n) => selected.has(n.note.id))
      ? candidates.filter((n) => selected.has(n.note.id))
      : candidates;
    const clicks = clickCount(x, y);

    // A point or handle under the pointer wins.
    const mapFor = (n: PlacedNote) => laneMapping(n, lane, height);
    for (const placed of preferred) {
      const ref = { placed, lane, map: mapFor(placed), mapFor };
      if (hitCurve(placed.note.expression[lane], x, y, ref.map)) {
        if (!selected.has(placed.note.id)) ui.selectNotes([placed.note.id]);
        curvePointerDown(ref, x, y, { ...mods, clicks }, true);
        return;
      }
    }

    if (preferred.length === 0) {
      ui.selectPoints(null);
      return;
    }

    // Otherwise work on the note whose curve passes closest to the pointer.
    const distance = (n: PlacedNote) => {
      const map = laneMapping(n, lane, height);
      return Math.abs(map.toY(evaluateCurve(n.note.expression[lane], map.fromX(x), LANE_DEFAULTS[lane])) - y);
    };
    const placed = preferred.reduce((best, n) => (distance(n) < distance(best) ? n : best));
    if (!selected.has(placed.note.id)) ui.selectNotes([placed.note.id]);
    const ref = { placed, lane, map: mapFor(placed), mapFor };

    if (mods.alt || clicks >= 2) {
      curvePointerDown(ref, x, y, { ...mods, clicks }, true);
      return;
    }

    // Drag on empty lane space: box-select this note's points (Shift adds).
    const base = mods.shift && selectedPoints?.lane === lane ? selectedPoints.notes : new Map<number, number[]>();
    if (!mods.shift) ui.selectPoints(null);
    drag.current = { kind: 'pointBand', ...ref, band: { x0: x, y0: y, x1: x, y1: y }, base };
  };

  // -------------------------------------------------------------------------------------------------------
  // Display: the model's notes with the preview applied.

  const displayNotes = useMemo(() => {
    if (!preview) return notes;
    const byId = new Map(preview.filter((n) => n.id !== undefined).map((n) => [n.id!, n]));
    const shown = notes.map((placed) => {
      const edit = byId.get(placed.note.id);
      if (!edit) return placed;
      return {
        ...placed,
        start: edit.start,
        end: edit.start + edit.length,
        note: {
          ...placed.note,
          pitch: edit.pitch,
          velocity: edit.velocity,
          length: edit.length,
          expression: { ...placed.note.expression, ...edit.expression },
          vibrato: edit.vibrato ?? placed.note.vibrato,
        },
      };
    });
    const created = preview
      .filter((n) => n.id === undefined)
      .map(
        (n): PlacedNote => ({
          start: n.start,
          end: n.start + n.length,
          clip: { id: -1, start: 0, length: Infinity, offset: 0, loopStart: 0, loopLength: 0, name: '', colour: '', linkId: 0, fileId: 0, gainDb: 0, fadeIn: 0, fadeOut: 0, fadeInShape: 0, fadeOutShape: 0, transpose: 0, lane: 0, order: 0, sequencerPattern: -1, sequencerStartStep: 0, controllers: {}, notes: {} },
          note: {
            id: -1,
            start: n.start,
            length: n.length,
            pitch: n.pitch,
            velocity: n.velocity,
            releaseVelocity: 0.5,
            expression: { pitch: [], pressure: [], timbre: [], vibrato: [], ...n.expression },
            vibrato: n.vibrato ?? NO_VIBRATO,
          },
        }),
      );
    return [...shown, ...created];
  }, [notes, preview]);

  return {
    displayNotes,
    band,
    linkDrag,
    bandTarget,
    cursor,
    onPointerDown,
    onPointerMove,
    onPointerUp,
    onKeyDown,
    onVelocityLane,
    onCurveLane,
    noteAt: (x: number, y: number) => hit(x, y)?.placed ?? null,
    splitAt,
    addPitchPointAt,
  };
}
