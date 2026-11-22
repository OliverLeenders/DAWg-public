import { useEffect, useRef, useState } from 'react';
import type { SampleZone } from '../bridge/protocol';
import { FREE_SHAPE } from '../pianoroll/curveEditing';
import { evaluateCurve } from '../pianoroll/geometry';
import { newGestureId } from '../store/store';
import { dragEnvelope, envelopeLayout, envelopeSegments, handleFields, type EnvelopePoint, type EnvelopeSegment } from './samplerLayout';

// A zone's envelope to edit by its points: attack across the top, decay and sustain at the knee, release after the hold. A
// one-shot has no release, so that point only sets the sustain. Each sloped segment bends at its tension point, as on the
// ADSR (dawg/EnvelopeHandle.h): drag it anywhere in the segment's rectangle, double-click it to straighten the segment.

const PAD = 8;
/** Samples drawn per segment. */
const SAMPLES = 32;

const clamp01 = (value: number) => Math.max(0, Math.min(1, value));
/** Two presses on the same tension point this close together straighten its segment. */
const DOUBLE_PRESS_MS = 400;

type Drag =
  | { kind: 'point'; point: EnvelopePoint; x: number; y: number; zone: SampleZone; totalMs: number; gesture: string }
  | { kind: 'handle'; segment: EnvelopeSegment; grabX: number; grabY: number; gesture: string };

export function ZoneEnvelope({ zone, onChange }: { zone: SampleZone; onChange: (fields: Partial<SampleZone>, gesture?: string) => void }) {
  const box = useRef<SVGSVGElement>(null);
  const drag = useRef<Drag | null>(null);
  // The last press on a tension point. A double-click is told from two presses rather than the browser's event, which
  // goes to the box once the first press has captured the pointer there.
  const lastPress = useRef<{ segment: EnvelopeSegment; at: number } | null>(null);
  const [size, setSize] = useState({ width: 300, height: 100 });
  const layout = envelopeLayout(zone);
  const segments = envelopeSegments(zone, layout);
  const innerWidth = Math.max(1, size.width - 2 * PAD);
  const innerHeight = Math.max(1, size.height - 2 * PAD);
  const toX = (x: number) => PAD + x * innerWidth;
  const toY = (y: number) => PAD + (1 - y) * innerHeight;

  // The curve: each segment sampled along its own bend, the hold flat between decay and release.
  const levelAt = (segment: EnvelopeSegment, progress: number) => {
    const { from, to, handle } = segments[segment];
    return evaluateCurve([[0, from, FREE_SHAPE, 0, handle.time, handle.level], [1, to, 0, 0]], progress, from);
  };
  const points: string[] = [`${toX(0)},${toY(layout.curve[0][1])}`];
  const sample = (segment: EnvelopeSegment) => {
    const { x0, x1 } = segments[segment];
    for (let i = 1; i <= SAMPLES; i++) points.push(`${toX(x0 + ((x1 - x0) * i) / SAMPLES)},${toY(levelAt(segment, i / SAMPLES))}`);
  };
  sample('attack');
  sample('decay');
  points.push(`${toX(layout.points.release[0])},${toY(zone.sustain)}`);
  sample('release');
  const line = points.join(' ');
  const handleAt = (segment: EnvelopeSegment) => {
    const { x0, x1, from, to, handle } = segments[segment];
    return { x: toX(x0 + (x1 - x0) * handle.time), y: toY(from + (to - from) * handle.level) };
  };

  // The graph follows its box as the panel resizes; its values are shown under it (SamplerPanel).
  useEffect(() => {
    const element = box.current;
    if (!element) return;
    const observer = new ResizeObserver(([entry]) => {
      const { width, height } = entry.contentRect;
      setSize((s) => (Math.abs(s.width - width) > 1 || Math.abs(s.height - height) > 1 ? { width, height } : s));
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const local = (e: React.PointerEvent) => {
    const rect = box.current?.getBoundingClientRect();
    return rect ? { x: e.clientX - rect.left, y: e.clientY - rect.top } : { x: 0, y: 0 };
  };

  return (
    <svg
      ref={box}
      className="sampler-envelope"
      onPointerMove={(e) => {
        const d = drag.current;
        if (!d) return;
        if (d.kind === 'point') {
          onChange(dragEnvelope(d.zone, d.point, d.totalMs, (e.clientX - d.x) / innerWidth, (d.y - e.clientY) / innerHeight), d.gesture);
          return;
        }
        // A tension point moves anywhere in its segment's rectangle.
        const at = local(e);
        const { x0, x1, from, to } = segments[d.segment];
        const time = x1 > x0 ? clamp01(((at.x - d.grabX - PAD) / innerWidth - x0) / (x1 - x0)) : 0.5;
        const level = Math.abs(to - from) > 1e-6 ? clamp01((1 - (at.y - d.grabY - PAD) / innerHeight - from) / (to - from)) : 0.5;
        onChange(handleFields(d.segment, time, level), d.gesture);
      }}
      onPointerUp={() => (drag.current = null)}
      onPointerCancel={() => (drag.current = null)}
    >
      <title>Drag a point to change its time or the sustain; drag a tension point to bend its segment, double-click it to straighten</title>
      <polygon points={`${toX(0)},${toY(0)} ${line} ${toX(1)},${toY(0)}`} className="sampler-envelope-fill" />
      <polyline points={line} className="sampler-envelope-line" />
      {(Object.keys(segments) as EnvelopeSegment[]).map((segment) => {
        const { x0, x1, from, to, shown } = segments[segment];
        // A segment of no width or no height has no visible bend.
        if (!shown || (x1 - x0) * innerWidth < 6 || Math.abs(to - from) * innerHeight < 6) return null;
        const at = handleAt(segment);
        return (
          <g
            key={segment}
            className="sampler-envelope-handle"
            onPointerDown={(e) => {
              e.stopPropagation();
              const now = e.timeStamp;
              const previous = lastPress.current;
              lastPress.current = { segment, at: now };
              if (previous?.segment === segment && now - previous.at < DOUBLE_PRESS_MS) {
                lastPress.current = null;
                drag.current = null;
                onChange(handleFields(segment, 0.5, 0.5), newGestureId());
                return;
              }
              e.currentTarget.ownerSVGElement?.setPointerCapture(e.pointerId);
              const pointer = local(e);
              drag.current = { kind: 'handle', segment, grabX: pointer.x - at.x, grabY: pointer.y - at.y, gesture: newGestureId() };
            }}
          >
            <circle cx={at.x} cy={at.y} r={7} className="sampler-envelope-hit" />
            <circle cx={at.x} cy={at.y} r={3} className="sampler-envelope-handle-ring" />
          </g>
        );
      })}
      {(Object.keys(layout.points) as EnvelopePoint[]).map((point) => {
        const [x, y] = layout.points[point];
        return (
          <g key={point}>
            <circle
              cx={toX(x)}
              cy={toY(y)}
              r={5}
              className="sampler-envelope-point"
              onPointerDown={(e) => {
                e.currentTarget.ownerSVGElement?.setPointerCapture(e.pointerId);
                drag.current = { kind: 'point', point, x: e.clientX, y: e.clientY, zone, totalMs: layout.totalMs, gesture: newGestureId() };
              }}
            />
          </g>
        );
      })}
    </svg>
  );
}
