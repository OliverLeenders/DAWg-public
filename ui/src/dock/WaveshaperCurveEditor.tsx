import { useEffect, useRef, useState, type MouseEvent as ReactMouseEvent, type PointerEvent as ReactPointerEvent } from 'react';
import { TICKS_PER_BEAT, type CurvePoint, type GraphNode, type Track } from '../bridge/protocol';
import { dispatch } from '../store/store';
import { evaluateCurve } from '../pianoroll/geometry';
import {
  addPoint,
  bendSegmentToPointer,
  deletePoints,
  handlePosition,
  handleTimeFor,
  hitCurve,
  movePoints,
  type CurveMapping,
  type CurveTarget,
  straightenSegment,
} from '../pianoroll/curveEditing';
import { drawnPoints, mirrored, pinOrigin, PLOT_PAD, positiveHalf } from './waveshaper';

// The waveshaper's drawn curve (docs/design/waveshaper.md, S6, S12): an SVG over the panel's plot square, edited with
// the LFO shape editor's gestures. The curve is f(u) for the shaper's input u from -1
// to 1 (times 0..TICKS_PER_BEAT) and values -1..1; a commit dispatches setModulationShape, as the LFO's editor does.
// Symmetric, the editor shows only the positive half (u and values 0..1) filling the square, with its first point
// pinned at the origin, and a commit stores that half with its mirror image, so the whole curve always plays as drawn.

const cycle = TICKS_PER_BEAT;

const HINT =
  'Alt-click adds; drag moves; double-click deletes; drag a handle to bend; double-click a handle to straighten';

export function WaveshaperCurveEditor({
  track,
  node,
  size,
  symmetric,
}: {
  track: Track;
  node: GraphNode;
  size: number;
  symmetric: boolean;
}) {
  const shown = (n: GraphNode) => (symmetric ? positiveHalf(drawnPoints(n)) : drawnPoints(n));
  const [points, setPoints] = useState<CurvePoint[]>(() => shown(node));
  const [selected, setSelected] = useState<ReadonlySet<number>>(new Set());
  const svg = useRef<SVGSVGElement>(null);
  const drag = useRef<
    | { kind: 'points'; target: Extract<CurveTarget, { kind: 'point' }>; indices: number[]; moved: boolean }
    | { kind: 'bend'; target: Extract<CurveTarget, { kind: 'handle' }>; grab: { x: number; y: number } }
    | null
  >(null);
  const preview = useRef(points);

  useEffect(() => {
    const next = shown(node);
    setPoints(next);
    preview.current = next;
    setSelected(new Set());
  }, [node.id, node.modulationShape, symmetric]);

  const commit = (next: CurvePoint[]) => {
    setPoints(next);
    preview.current = next;
    dispatch({
      command: 'setModulationShape',
      args: { trackId: track.id, nodeId: node.id, points: symmetric ? mirrored(next) : next },
    });
  };

  // Keeps the symmetric half's first point at the origin while it's dragged.
  const pinned = (next: CurvePoint[]) => (symmetric ? pinOrigin(next) : next);

  // Pixels within the square, the same mapping the panel's canvas uses.
  const inner = size - 2 * PLOT_PAD;
  const low = symmetric ? 0 : -1;
  const mapping: CurveMapping = {
    toX: (time) => PLOT_PAD + (time / cycle) * inner,
    toY: (value) => PLOT_PAD + ((1 - value) / (1 - low)) * inner,
    fromX: (x) => ((x - PLOT_PAD) / inner) * cycle,
    fromY: (y) => 1 - ((y - PLOT_PAD) / inner) * (1 - low),
    min: low,
    max: 1,
    maxTime: cycle,
    neutral: 0,
    anchorStart: symmetric,
  };

  const position = (e: ReactPointerEvent<SVGElement> | ReactMouseEvent<SVGElement>) => {
    const rect = svg.current?.getBoundingClientRect();
    if (!rect) return { x: 0, y: 0 };
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  };

  const finish = (e?: ReactPointerEvent<SVGSVGElement>) => {
    const current = drag.current;
    if (!current) return;
    drag.current = null;
    if (e && svg.current?.hasPointerCapture(e.pointerId)) svg.current.releasePointerCapture(e.pointerId);
    if (current.kind === 'points' && current.moved) commit(preview.current);
    if (current.kind === 'bend') commit(preview.current);
  };

  // Sampled, with each bent segment's control point included, so the path doesn't skip the bend.
  const times = new Set<number>();
  for (let i = 0; i <= 200; i++) times.add((i / 200) * cycle);
  for (let i = 0; i + 1 < points.length; i++) {
    times.add(points[i][0]);
    if (points[i][1] !== points[i + 1][1]) times.add(handleTimeFor(points, i));
  }
  const path = [...times]
    .sort((a, b) => a - b)
    .map((time, index) => `${index === 0 ? 'M' : 'L'} ${mapping.toX(time)} ${mapping.toY(evaluateCurve(points, time, 0))}`)
    .join(' ');

  return (
    <svg
        ref={svg}
        className="waveshaper-editor"
        width={size}
        height={size}
        viewBox={`0 0 ${size} ${size}`}
        onPointerDown={(e) => {
          const at = position(e);
          const target = hitCurve(points, at.x, at.y, mapping);
          if (!target) {
            if (e.altKey) {
              const added = addPoint(points, at.x, at.y, mapping, e.ctrlKey || e.metaKey);
              setSelected(new Set([added.index]));
              commit(pinned(added.points));
            } else {
              setSelected(new Set());
            }
            return;
          }
          if (target.kind === 'handle') {
            const handle = handlePosition(points, target.segment, mapping);
            drag.current = { kind: 'bend', target, grab: { x: at.x - handle.x, y: at.y - handle.y } };
          } else {
            const indices = e.shiftKey ? [...selected, target.index] : selected.has(target.index) ? [...selected] : [target.index];
            setSelected(new Set(indices));
            drag.current = { kind: 'points', target, indices, moved: false };
          }
          svg.current?.setPointerCapture(e.pointerId);
        }}
        onPointerMove={(e) => {
          const current = drag.current;
          if (!current) return;
          const at = position(e);
          if (current.kind === 'bend') {
            preview.current = pinned(bendSegmentToPointer(points, current.target.segment, mapping, at.x, at.y, current.grab).points);
          } else {
            const moved = movePoints(points, current.indices, current.target.index, at.x, at.y, mapping, e.ctrlKey || e.metaKey);
            preview.current = pinned(moved.points);
            setSelected(new Set(moved.indices));
            current.moved = true;
          }
          setPoints(preview.current);
        }}
        onDoubleClick={(e) => {
          const at = position(e);
          const target = hitCurve(points, at.x, at.y, mapping);
          if (target?.kind === 'point') {
            setSelected(new Set());
            commit(pinned(deletePoints(points, [target.index], mapping.anchorStart)));
          } else if (target?.kind === 'handle') {
            commit(straightenSegment(points, target.segment));
          }
        }}
        onPointerUp={finish}
        onPointerCancel={() => finish()}
        onLostPointerCapture={() => finish()}
      >
        <title>{HINT}</title>
        <path d={path} className="waveshaper-curve" />
        {points.map(([time, value], index) => (
          <circle
            key={`${index}-${time}`}
            cx={mapping.toX(time)}
            cy={mapping.toY(value)}
            r={selected.has(index) ? 4.5 : 3.5}
            className={selected.has(index) ? 'waveshaper-point selected' : 'waveshaper-point'}
          />
        ))}
        {points.slice(0, -1).map((point, index) => {
          if (point[1] === points[index + 1][1]) return null;
          const handle = handlePosition(points, index, mapping);
          return <circle key={`handle-${index}`} cx={handle.x} cy={handle.y} r={2} className="waveshaper-handle" />;
        })}
      </svg>
  );
}
