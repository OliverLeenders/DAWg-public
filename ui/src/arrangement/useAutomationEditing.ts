import { useRef, useState } from 'react';
import type { CurvePoint } from '../bridge/protocol';
import { manualPosition } from '../automation/automation';
import {
  addPoint,
  bendSegmentToPointer,
  deletePoints,
  handlePosition,
  hitCurve,
  movePoints,
  pointsInRect,
  type CurveMapping,
  straightenSegment,
} from '../pianoroll/curveEditing';
import { tickToX, xToTick, type View } from '../pianoroll/geometry';
import type { TempoMap } from '../time/tempoMap';
import { dispatch, useDawg } from '../store/store';
import { AUTOMATION_PAD } from './draw';
import { automationAt, type AutomationRow, type Lane } from './layout';
import { useClickCount } from '../timeline/useClickCount';

// Editing automation lanes in the arrangement (docs/design/phase10-automation.md, section 5), with the piano roll's
// curve tools: click selects a point, Shift toggles it, Alt+click or double-click on empty space adds one, drag moves points (Ctrl/Cmd:
// off the grid), the diamond between points bends a segment, dragging empty space box-selects points (Shift adds),
// and double-click or Delete removes points.

interface Box {
  x0: number;
  y0: number;
  x1: number;
  y1: number;
}

type Drag =
  | { kind: 'points'; row: AutomationRow; points: CurvePoint[]; indices: number[]; grab: number; moved: boolean }
  | { kind: 'bend'; row: AutomationRow; points: CurvePoint[]; segment: number; grab: { x: number; y: number } }
  | { kind: 'band'; row: AutomationRow; box: Box; base: ReadonlySet<number> };

export interface AutomationPreview {
  laneId: number;
  points: CurvePoint[];
}

export function useAutomationEditing({
  lanes,
  view,
  scrollY,
  gridTicks,
  map,
}: {
  lanes: Lane[];
  view: View;
  scrollY: number;
  gridTicks: number;
  map: TempoMap;
}) {
  const types = useDawg((s) => s.nodeTypes);
  const [preview, setPreview] = useState<AutomationPreview | null>(null);
  const [selection, setSelection] = useState<{ laneId: number; indices: Set<number> } | null>(null);
  const [box, setBox] = useState<Box | null>(null);
  const clickCount = useClickCount();
  const drag = useRef<Drag | null>(null);

  const mapping = (row: AutomationRow): CurveMapping => {
    const top = row.top - scrollY + AUTOMATION_PAD;
    const span = Math.max(1, row.height - 2 * AUTOMATION_PAD);
    return {
      toX: (t) => tickToX(view, t),
      toY: (v) => top + (1 - v) * span,
      fromX: (x) => xToTick(view, x),
      fromY: (y) => 1 - (y - top) / span,
      min: 0,
      max: 1,
      maxTime: Number.MAX_SAFE_INTEGER,
      neutral: manualPosition(row.lane, row.track, types),
      snapTime: (t) => Math.max(0, map.snap(t, gridTicks, true)),
    };
  };

  const commit = (row: AutomationRow, points: CurvePoint[]) => {
    setPreview(null);
    dispatch({ command: 'setAutomationPoints', args: { trackId: row.track.id, laneId: row.lane.id, points } });
  };

  /** Handles a press in the lanes canvas; returns false when it isn't on an automation lane. */
  const onPointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (e.button !== 0) return false;
    const { offsetX: x, offsetY: y } = e.nativeEvent;
    const row = automationAt(lanes, y, scrollY);
    if (!row) return false;

    e.currentTarget.setPointerCapture(e.pointerId);
    const map = mapping(row);
    const points = row.lane.points;
    const hit = hitCurve(points, x, y, map);
    const clicks = clickCount(x, y);

    if (hit?.kind === 'point' && clicks >= 2) {
      const remaining = deletePoints(points, [hit.index]);
      setSelection(null);
      commit(row, remaining);
      return true;
    }

    if (hit?.kind === 'handle' && clicks >= 2) {
      commit(row, straightenSegment(points, hit.segment));
      return true;
    }

    if (hit?.kind === 'handle') {
      const handle = handlePosition(points, hit.segment, map);
      drag.current = {
        kind: 'bend',
        row,
        points,
        segment: hit.segment,
        grab: { x: x - handle.x, y: y - handle.y },
      };
      return true;
    }

    if (hit?.kind === 'point') {
      if (e.shiftKey) {
        const indices = selection?.laneId === row.lane.id ? new Set(selection.indices) : new Set<number>();
        if (indices.has(hit.index)) indices.delete(hit.index);
        else indices.add(hit.index);
        setSelection(indices.size > 0 ? { laneId: row.lane.id, indices } : null);
        return true;
      }
      const indices =
        selection?.laneId === row.lane.id && selection.indices.has(hit.index) ? [...selection.indices] : [hit.index];
      setSelection({ laneId: row.lane.id, indices: new Set(indices) });
      drag.current = { kind: 'points', row, points, indices, grab: hit.index, moved: false };
      return true;
    }

    if (e.altKey || clicks >= 2) {
      // Alt+click or a double-click on empty space adds a point, picked up right away.
      const added = addPoint(points, x, y, map, e.ctrlKey || e.metaKey);
      setSelection({ laneId: row.lane.id, indices: new Set([added.index]) });
      setPreview({ laneId: row.lane.id, points: added.points });
      drag.current = { kind: 'points', row, points: added.points, indices: [added.index], grab: added.index, moved: true };
      return true;
    }

    // Empty space starts a box selection; Shift adds to the current selection.
    const base =
      e.shiftKey && selection?.laneId === row.lane.id ? new Set(selection.indices) : new Set<number>();
    if (!e.shiftKey) setSelection(null);
    const next = { x0: x, y0: y, x1: x, y1: y };
    setBox(next);
    drag.current = { kind: 'band', row, box: next, base };
    return true;
  };

  const onPointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const d = drag.current;
    if (!d) return false;
    const { offsetX: x, offsetY: y } = e.nativeEvent;

    if (d.kind === 'band') {
      // Keep the box in its lane: a selection belongs to one lane's points.
      const top = d.row.top - scrollY;
      const bottom = top + d.row.height;
      d.box = { ...d.box, x1: x, y1: Math.min(bottom, Math.max(top, y)) };
      setBox(d.box);
      return true;
    }

    if (d.kind === 'bend') {
      const bent = bendSegmentToPointer(d.points, d.segment, mapping(d.row), x, y, d.grab);
      setPreview({ laneId: d.row.lane.id, points: bent.points });
      return true;
    }

    d.moved = true;
    const moved = movePoints(d.points, d.indices, d.grab, x, y, mapping(d.row), e.ctrlKey || e.metaKey);
    setPreview({ laneId: d.row.lane.id, points: moved.points });
    setSelection({ laneId: d.row.lane.id, indices: new Set(moved.indices) });
    return true;
  };

  const onPointerUp = () => {
    const d = drag.current;
    if (!d) return false;
    drag.current = null;

    if (d.kind === 'band') {
      const inside = pointsInRect(d.row.lane.points, mapping(d.row), d.box.x0, d.box.y0, d.box.x1, d.box.y1);
      const indices = new Set([...d.base, ...inside]);
      setSelection(indices.size > 0 ? { laneId: d.row.lane.id, indices } : null);
      setBox(null);
      setPreview(null);
      return true;
    }

    if (preview && preview.laneId === d.row.lane.id && (d.kind === 'bend' || d.moved)) commit(d.row, preview.points);
    else setPreview(null);
    return true;
  };

  /** Delete / Backspace removes the selected points; returns false when no points are selected. */
  const onKeyDown = (e: React.KeyboardEvent) => {
    if (!selection || (e.key !== 'Delete' && e.key !== 'Backspace')) return false;
    const row = lanes.flatMap((l) => l.automation).find((r) => r.lane.id === selection.laneId);
    if (!row) return false;
    e.preventDefault();
    commit(row, deletePoints(row.lane.points, [...selection.indices]));
    setSelection(null);
    return true;
  };

  /** The points to draw for a lane (the preview while editing). */
  const pointsOf = (row: AutomationRow) => (preview?.laneId === row.lane.id ? preview.points : row.lane.points);
  const selectedOf = (row: AutomationRow): ReadonlySet<number> =>
    selection?.laneId === row.lane.id ? selection.indices : new Set<number>();

  return {
    onPointerDown,
    onPointerMove,
    onPointerUp,
    onKeyDown,
    pointsOf,
    selectedOf,
    mapping,
    box,
    clearSelection: () => {
      setSelection(null);
      setBox(null);
    },
  };
}
