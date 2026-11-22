import { useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from 'react';
import type { GraphNode, NodeTypeInfo, Track } from '../bridge/protocol';
import { useParam } from '../dock/useParam';
import { FREE_SHAPE } from '../pianoroll/curveEditing';
import { evaluateCurve } from '../pianoroll/geometry';
import { fromPosition, toPosition } from './params';

// The ADSR's envelope as an editable graph (docs/design/modulation.md, M3). The corners set the times and the sustain
// level. Each sloped segment is a free curve segment, as in the LFO's shape: a cubic Bezier whose handles point at
// its tension point, which moves anywhere in the rectangle between the segment's ends. Each of attack, decay and release has
// a slot of the
// width that it fills along its knob's travel, so a corner moves the way the knob turns; the sustain plateau has a
// fixed width.

/** Keep the curve and its markers clear of the canvas edges, in px. */
const PAD = 8;
/** Share of the plot width for each of attack, decay and release at their longest. */
const SLOT = 0.27;
/** Share of the plot width for the sustain plateau. */
const HOLD = 1 - 3 * SLOT;
/** Samples drawn per segment. */
const SAMPLES = 48;

const HINT =
  'Drag a corner to change its time or the sustain level; drag a curve handle freely inside its segment; double-click ' +
  'a handle to straighten the segment';

const clamp01 = (value: number) => Math.max(0, Math.min(1, value));

type Corner = 'attack' | 'decay' | 'sustain' | 'release';
type Segment = 'attack' | 'decay' | 'release';

export function AdsrEditor({ track, node, info }: { track: Track; node: GraphNode; info: NodeTypeInfo }) {
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const attackParam = param('attackMs');
  const decayParam = param('decayMs');
  const releaseParam = param('releaseMs');
  const attack = useParam(track, node, attackParam);
  const decay = useParam(track, node, decayParam);
  const sustain = useParam(track, node, param('sustain'));
  const release = useParam(track, node, releaseParam);
  const handles = {
    attack: { time: useParam(track, node, param('attackHandleTime')), level: useParam(track, node, param('attackHandleLevel')) },
    decay: { time: useParam(track, node, param('decayHandleTime')), level: useParam(track, node, param('decayHandleLevel')) },
    release: {
      time: useParam(track, node, param('releaseHandleTime')),
      level: useParam(track, node, param('releaseHandleLevel')),
    },
  };

  const [size, setSize] = useState({ width: 0, height: 0 });
  const svg = useRef<SVGSVGElement>(null);
  const drag = useRef<
    | { kind: 'corner'; corner: Corner; grab: { x: number; y: number } }
    | { kind: 'handle'; segment: Segment; grab: { x: number; y: number } }
    | null
  >(null);

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

  const width = Math.max(1, size.width - 2 * PAD);
  const height = Math.max(1, size.height - 2 * PAD);
  const slot = SLOT * width;
  const toY = (level: number) => PAD + (1 - level) * height;
  const fromY = (y: number) => Math.max(0, Math.min(1, 1 - (y - PAD) / height));

  const level = Math.max(0, Math.min(1, sustain.value / 100));
  const attackX = PAD + slot * toPosition(attackParam, attack.value);
  const decayX = attackX + slot * toPosition(decayParam, decay.value);
  const holdX = decayX + HOLD * width;
  const releaseX = holdX + slot * toPosition(releaseParam, release.value);

  const segments: Record<Segment, { x0: number; x1: number; from: number; to: number }> = {
    attack: { x0: PAD, x1: attackX, from: 0, to: 1 },
    decay: { x0: attackX, x1: decayX, from: 1, to: level },
    release: { x0: holdX, x1: releaseX, from: level, to: 0 },
  };
  // A segment's handle in fractions of its rectangle, and on the canvas.
  const handleOf = (segment: Segment) => ({
    time: clamp01(handles[segment].time.value / 100),
    level: clamp01(handles[segment].level.value / 100),
  });
  const handleAt = (segment: Segment) => {
    const { x0, x1, from, to } = segments[segment];
    const handle = handleOf(segment);
    return { x: x0 + (x1 - x0) * handle.time, y: toY(from + (to - from) * handle.level) };
  };
  const levelAt = (segment: Segment, progress: number) => {
    const { from, to } = segments[segment];
    const handle = handleOf(segment);
    return evaluateCurve([[0, from, FREE_SHAPE, 0, handle.time, handle.level], [1, to, 0, 0]], progress, from);
  };
  const corners: Record<Corner, { x: number; y: number }> = {
    attack: { x: attackX, y: toY(1) },
    decay: { x: decayX, y: toY(level) },
    sustain: { x: holdX, y: toY(level) },
    release: { x: releaseX, y: toY(0) },
  };

  const points: string[] = [`${PAD} ${toY(0)}`];
  const sample = (segment: Segment) => {
    const { x0, x1 } = segments[segment];
    for (let i = 1; i <= SAMPLES; i++) points.push(`${x0 + ((x1 - x0) * i) / SAMPLES} ${toY(levelAt(segment, i / SAMPLES))}`);
  };
  sample('attack');
  sample('decay');
  points.push(`${holdX} ${toY(level)}`);
  sample('release');
  const path = `M ${points.join(' L ')}`;
  const fill = `${path} L ${releaseX} ${toY(0)} L ${PAD} ${toY(0)} Z`;

  const position = (e: ReactPointerEvent<SVGElement>) => {
    const rect = svg.current?.getBoundingClientRect();
    return rect ? { x: e.clientX - rect.left, y: e.clientY - rect.top } : { x: 0, y: 0 };
  };

  // The parameters each drag changes, so it can begin and end their gestures.
  const editsOf = (current: NonNullable<typeof drag.current>) => {
    if (current.kind === 'handle') return [handles[current.segment].time, handles[current.segment].level];
    if (current.corner === 'attack') return [attack];
    if (current.corner === 'decay') return [decay, sustain];
    if (current.corner === 'sustain') return [sustain];
    return [release];
  };

  const start = (e: ReactPointerEvent<SVGElement>, next: NonNullable<typeof drag.current>) => {
    e.stopPropagation();
    e.preventDefault();
    drag.current = next;
    editsOf(next).forEach((edit) => edit.begin());
    svg.current?.setPointerCapture(e.pointerId);
  };

  const finish = () => {
    const current = drag.current;
    if (!current) return;
    drag.current = null;
    editsOf(current).forEach((edit) => edit.end());
  };

  const move = (e: ReactPointerEvent<SVGSVGElement>) => {
    const current = drag.current;
    if (!current) return;
    const at = position(e);
    const x = at.x - current.grab.x;
    const y = at.y - current.grab.y;
    if (current.kind === 'handle') {
      const { segment } = current;
      const { x0, x1, from, to } = segments[segment];
      if (x1 > x0) handles[segment].time.drag(clamp01((x - x0) / (x1 - x0)) * 100);
      handles[segment].level.drag(clamp01((fromY(y) - from) / (to - from)) * 100);
      return;
    }
    if (current.corner === 'attack') attack.drag(fromPosition(attackParam, (x - PAD) / slot));
    if (current.corner === 'decay') decay.drag(fromPosition(decayParam, (x - attackX) / slot));
    if (current.corner === 'release') release.drag(fromPosition(releaseParam, (x - holdX) / slot));
    if (current.corner === 'decay' || current.corner === 'sustain') sustain.drag(fromY(y) * 100);
  };

  return (
    <div className="adsr-editor">
      <svg
        ref={svg}
        className="adsr-canvas"
        viewBox={`0 0 ${Math.max(1, size.width)} ${Math.max(1, size.height)}`}
        onPointerMove={move}
        onPointerUp={finish}
        onPointerCancel={finish}
        onLostPointerCapture={finish}
      >
        <title>{HINT}</title>
        <line x1={PAD} y1={toY(0)} x2={PAD + width} y2={toY(0)} className="adsr-grid" />
        {[attackX, decayX, holdX].map((x, index) => (
          <line key={index} x1={x} y1={PAD} x2={x} y2={toY(0)} className="adsr-grid" />
        ))}
        <path d={fill} className="adsr-fill" />
        <path d={path} className="adsr-curve" />
        {(Object.keys(segments) as Segment[]).map((segment) => {
          const { from, to } = segments[segment];
          if (Math.abs(to - from) < 1e-4) return null; // a flat segment has no visible bend
          const at = handleAt(segment);
          return (
            <g
              key={segment}
              className="adsr-handle"
              onPointerDown={(e) => {
                const pointer = position(e);
                start(e, { kind: 'handle', segment, grab: { x: pointer.x - at.x, y: pointer.y - at.y } });
              }}
              onDoubleClick={() => {
                handles[segment].time.set(50);
                handles[segment].level.set(50);
              }}
            >
              <circle cx={at.x} cy={at.y} r={7} className="adsr-hit" />
              <circle cx={at.x} cy={at.y} r={3} className="adsr-handle-ring" />
            </g>
          );
        })}
        <circle cx={PAD} cy={toY(0)} r={3} className="adsr-point fixed" />
        {(Object.keys(corners) as Corner[]).map((corner) => {
          const at = corners[corner];
          return (
            <g
              key={corner}
              className={`adsr-corner ${corner}`}
              onPointerDown={(e) => {
                const pointer = position(e);
                start(e, { kind: 'corner', corner, grab: { x: pointer.x - at.x, y: pointer.y - at.y } });
              }}
            >
              <circle cx={at.x} cy={at.y} r={8} className="adsr-hit" />
              <circle cx={at.x} cy={at.y} r={4} className="adsr-point" />
            </g>
          );
        })}
      </svg>
    </div>
  );
}
