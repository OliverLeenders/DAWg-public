import { Minus, Plus } from 'lucide-react';
import { useEffect, useRef, useState, type MouseEvent as ReactMouseEvent, type PointerEvent as ReactPointerEvent } from 'react';
import { TICKS_PER_BEAT, type CurvePoint, type GraphNode, type Track } from '../bridge/protocol';
import { dispatch } from '../store/store';
import { evaluateCurve } from '../pianoroll/geometry';
import {
  addPoint,
  bendSegmentToPointer,
  deletePoints,
  handlePosition,
  handleTimeFor as freeHandleTime,
  hitCurve,
  movePoints,
  type CurveMapping,
  type CurveTarget,
  straightenSegment,
} from '../pianoroll/curveEditing';
import { Button } from '../ui/controls';

const cycle = TICKS_PER_BEAT;

/**
 * Sine: four free segments through 0, 1, 0, -1 and 0, each bent as close to a quarter sine as one shared control
 * point allows (within 1.3 %). The same curve as lfoSineShape in engine/include/dawg/Modulation.h, which an LFO with
 * no shape plays.
 */
const SINE: CurvePoint[] = [
  [0, 0, 3, 0, 0.6005, 0.984],
  [cycle / 4, 1, 3, 0, 0.3995, 0.016],
  [cycle / 2, 0, 3, 0, 0.6005, 0.984],
  [(cycle * 3) / 4, -1, 3, 0, 0.3995, 0.016],
  [cycle, 0, 0, 0],
];

const HINT =
  'Alt-click adds; click selects; Shift selects more; drag moves; double-click deletes; drag a curve handle freely ' +
  'inside its segment; double-click a handle to straighten the segment';

const presets: Record<string, CurvePoint[]> = {
  Sine: SINE,
  Triangle: [[0, 0, 0, 0], [Math.round(cycle * 0.25), 1, 0, 0], [Math.round(cycle * 0.75), -1, 0, 0], [cycle, 0, 0, 0]],
  Saw: [[0, -1, 0, 0], [cycle, 1, 0, 0]],
  Ramp: [[0, 1, 0, 0], [cycle, -1, 0, 0]],
  Square: [[0, 1, 1, 0], [Math.round(cycle * 0.5), -1, 1, 0], [cycle, 1, 0, 0]],
};

const pointsOf = (node: GraphNode): CurvePoint[] =>
  node.modulationShape && node.modulationShape.length > 0 ? node.modulationShape : presets.Sine;

/** Keep the curve and its point markers clear of the canvas edges. */
const PAD = 5;

/** Polarity positions: negative (top), bipolar (middle), positive (bottom). */
const zeroTitles = ['Negative polarity (zero at the top)', 'Bipolar polarity (zero in the middle)', 'Positive polarity (zero at the bottom)'];
function BipolarIcon() {
  return (
    <span className="lfo-bipolar-icon">
      <Plus size={9} />
      <Minus size={9} />
    </span>
  );
}
const zeroIcons = [Minus, BipolarIcon, Plus];

const zeroOf = (node: GraphNode) =>
  Math.max(0, Math.min(2, Math.round(node.params.zero ?? 1)));

export function LfoShapeEditor({ track, node }: { track: Track; node: GraphNode }) {
  const [points, setPoints] = useState<CurvePoint[]>(() => pointsOf(node));
  const [selected, setSelected] = useState<ReadonlySet<number>>(new Set());
  const [size, setSize] = useState({ width: 0, height: 0 });
  const svg = useRef<SVGSVGElement>(null);
  const drag = useRef<
    | { kind: 'points'; target: Extract<CurveTarget, { kind: 'point' }>; indices: number[]; moved: boolean }
    | { kind: 'bend'; target: Extract<CurveTarget, { kind: 'handle' }>; grab: { x: number; y: number } }
    | null
  >(null);
  const preview = useRef(points);

  useEffect(() => {
    const next = pointsOf(node);
    setPoints(next);
    preview.current = next;
    setSelected(new Set());
  }, [node.id, node.modulationShape]);

  useEffect(() => {
    const element = svg.current;
    if (!element) return;
    const update = () => {
      const bounds = element.getBoundingClientRect();
      setSize({ width: bounds.width, height: bounds.height });
    };
    update();
    const observer = new ResizeObserver(update);
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const commit = (next: CurvePoint[]) => {
    setPoints(next);
    preview.current = next;
    dispatch({ command: 'setModulationShape', args: { trackId: track.id, nodeId: node.id, points: next } });
  };

  const zero = zeroOf(node);
  const mapping: CurveMapping = {
    toX: (time) => PAD + (time / cycle) * (100 - 2 * PAD),
    toY: (value) => 50 - value * (50 - PAD),
    fromX: (x) => ((x - PAD) / (100 - 2 * PAD)) * cycle,
    fromY: (y) => (50 - y) / (50 - PAD),
    min: -1,
    max: 1,
    maxTime: cycle,
    neutral: 0,
  };
  const zeroY = mapping.toY(zero === 0 ? 1 : zero === 2 ? -1 : 0);

  const position = (e: ReactPointerEvent<SVGElement> | ReactMouseEvent<SVGElement>) => {
    const rect = svg.current?.getBoundingClientRect();
    if (!rect) return { x: 0, y: 50 };
    return { x: ((e.clientX - rect.left) / rect.width) * 100, y: ((e.clientY - rect.top) / rect.height) * 100 };
  };

  const finish = (e?: ReactPointerEvent<SVGSVGElement>) => {
    const current = drag.current;
    if (!current) return;
    drag.current = null;
    if (e && svg.current?.hasPointerCapture(e.pointerId)) svg.current.releasePointerCapture(e.pointerId);
    if (current.kind === 'points' && current.moved) commit(preview.current);
    if (current.kind === 'bend') commit(preview.current);
  };

  // Sample the curve and include each control point's x coordinate, so the rendered path does not skip over
  // the most visually important part of a bent segment between fixed samples.
  const pathTimes = new Set<number>();
  for (let i = 0; i <= 200; i++) pathTimes.add((i / 200) * cycle);
  for (let i = 0; i + 1 < points.length; i++) {
    if (points[i][1] !== points[i + 1][1]) pathTimes.add(freeHandleTime(points, i));
  }
  const path = [...pathTimes]
    .sort((a, b) => a - b)
    .map((time, index) => `${index === 0 ? 'M' : 'L'} ${mapping.toX(time)} ${mapping.toY(evaluateCurve(points, time, 0))}`)
    .join(' ');
  const xScale = size.width > 0 ? 100 / size.width : 1;
  const yScale = size.height > 0 ? 100 / size.height : 1;
  const pointRadius = (selectedPoint: boolean) => (selectedPoint ? 4.5 : 3.5);
  const tensionRadius = 1.7;

  return (
    <div className="lfo-shape">
      <div className="lfo-shape-header">
        <span>Shape</span>
        <div className="lfo-presets">
          {Object.entries(presets).map(([name, preset]) => (
            <Button
              key={name}
              onClick={() => {
                setSelected(new Set());
                commit(preset.map((p) => [...p] as CurvePoint));
              }}
            >
              {name}
            </Button>
          ))}
        </div>
      </div>
      <div className="lfo-zero-row">
        <span>Zero</span>
        <div className="lfo-zero-options" role="group" aria-label="Zero position">
          {zeroIcons.map((Icon, position) => (
            <button
              key={position}
              type="button"
              className={zero === position ? 'active' : ''}
              title={zeroTitles[position]}
              aria-label={zeroTitles[position]}
              onClick={() =>
                dispatch({
                  command: 'setNodeParam',
                  args: { trackId: track.id, nodeId: node.id, param: 'zero', value: position },
                })
              }
            >
              <Icon size={13} />
            </button>
          ))}
        </div>
      </div>
      <svg
        ref={svg}
        className="lfo-shape-canvas"
        viewBox="0 0 100 100"
        preserveAspectRatio="none"
        onPointerDown={(e) => {
          const at = position(e);
          const target = hitCurve(points, at.x, at.y, mapping);
          if (!target) {
            if (e.altKey) {
              const added = addPoint(points, at.x, at.y, mapping, e.ctrlKey || e.metaKey);
              preview.current = added.points;
              setSelected(new Set([added.index]));
              commit(added.points);
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
            const bent = bendSegmentToPointer(points, current.target.segment, mapping, at.x, at.y, current.grab);
            preview.current = bent.points;
          } else {
            const moved = movePoints(points, current.indices, current.target.index, at.x, at.y, mapping, e.ctrlKey || e.metaKey);
            preview.current = moved.points;
            setSelected(new Set(moved.indices));
            current.moved = true;
          }
          setPoints(preview.current);
        }}
        onDoubleClick={(e) => {
          const at = position(e);
          const target = hitCurve(points, at.x, at.y, mapping);
          if (target?.kind === 'point') {
            const next = deletePoints(points, [target.index]);
            setSelected(new Set());
            commit(next);
          }
        }}
        onPointerUp={finish}
        onPointerCancel={() => finish()}
        onLostPointerCapture={() => finish()}
      >
        <line x1={PAD} y1={zeroY} x2={100 - PAD} y2={zeroY} className="lfo-zero" />
        {[0.25, 0.5, 0.75].map((fraction) => (
          <line
            key={fraction}
            x1={mapping.toX(fraction * cycle)}
            y1={PAD}
            x2={mapping.toX(fraction * cycle)}
            y2={100 - PAD}
            className="lfo-grid"
          />
        ))}
        <path d={path} className="lfo-curve" />
        {points.map(([time, value], index) => (
          <ellipse
            key={`${index}-${time}`}
            cx={mapping.toX(time)}
            cy={mapping.toY(value)}
            rx={pointRadius(selected.has(index)) * xScale}
            ry={pointRadius(selected.has(index)) * yScale}
            className={selected.has(index) ? 'lfo-point selected' : 'lfo-point'}
            onPointerDown={(e) => {
              e.stopPropagation();
              const indices = e.shiftKey ? [...selected, index] : selected.has(index) ? [...selected] : [index];
              setSelected(new Set(indices));
              drag.current = { kind: 'points', target: { kind: 'point', index }, indices, moved: false };
              svg.current?.setPointerCapture(e.pointerId);
            }}
          />
        ))}
        {points.slice(0, -1).map((point, index) => {
          if (point[1] === points[index + 1][1]) return null;
          const handle = handlePosition(points, index, mapping);
          return (
            <g
              key={`handle-${index}`}
              className="lfo-tension"
              onDoubleClick={(e) => {
                e.stopPropagation();
                commit(straightenSegment(points, index));
              }}
              onPointerDown={(e) => {
                e.stopPropagation();
                e.preventDefault();
                const at = position(e);
                drag.current = {
                  kind: 'bend',
                  target: { kind: 'handle', segment: index },
                  grab: { x: at.x - handle.x, y: at.y - handle.y },
                };
                svg.current?.setPointerCapture(e.pointerId);
              }}
            >
              <ellipse
                cx={handle.x}
                cy={handle.y}
                rx={6 * xScale}
                ry={6 * yScale}
                className="lfo-tension-hit"
              />
              <ellipse
                cx={handle.x}
                cy={handle.y}
                rx={tensionRadius * xScale}
                ry={tensionRadius * yScale}
                className="lfo-tension-handle"
              />
            </g>
          );
        })}
      </svg>
      <div className="lfo-shape-hint" title={HINT}>
        {HINT}
      </div>
    </div>
  );
}
