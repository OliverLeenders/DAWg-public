import { BaseEdge, getBezierPath, useReactFlow, type EdgeProps } from '@xyflow/react';
import { useState, type PointerEvent as ReactPointerEvent } from 'react';
import { useLayout } from '../store/layoutStore';
import { routedPath, type Point, type Side } from './layout';
import { GRID, snapPoint, type End } from './grid';
import { acrossFlow, detourAt, flat, movable, moveStretch, removeBend, routeOf, splitAt, stepAt } from './cables';

// A cable (docs/design/graph-grid-and-cables.md, G5–G8): through its bends in straight stretches with rounded corners when it has any,
// otherwise around what it would cross, or the usual curve. Dragging a stretch moves it sideways (a cable without bends gets a step where
// it's grabbed, or pulled across the flow a detour); double-clicking a stretch adds a bend there, double-clicking a bend takes it out.

export interface CableData {
  /** The bends, on the canvas, of a cable that has them. */
  bends?: Point[];
  /** A way around what the cable would cross, for one without bends. */
  route?: Point[];
  /** Stores new bends, or none for the usual curve. */
  onBends?: (bends: Point[] | null) => void;
  [key: string]: unknown;
}

/** How far the pointer moves before a press on a cable is a drag. */
const DRAG_START = 3;

export function RoutedEdge({ sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition, data, markerEnd, style, interactionWidth }: EdgeProps) {
  const cable = (data ?? {}) as CableData;
  const flow = useReactFlow();
  // The bends while a stretch is dragged, before they are stored.
  const [dragged, setDragged] = useState<Point[] | null>(null);

  const from: End = { at: { x: sourceX, y: sourceY }, side: sourcePosition as Side };
  const to: End = { at: { x: targetX, y: targetY }, side: targetPosition as Side };
  const bends = dragged ?? cable.bends;
  const route = bends && bends.length > 0 ? routeOf(bends, from, to) : null;
  const path = route
    ? routedPath(from.at, route.slice(1, -1), to.at, 4)
    : cable.route
      ? routedPath(from.at, cable.route, to.at)
      : getBezierPath({ sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition })[0];

  const snapped = (p: Point, e: { shiftKey: boolean }) => (useLayout.getState().graphSnap && !e.shiftKey ? snapPoint(p) : { x: Math.round(p.x), y: Math.round(p.y) });
  const at = (e: { clientX: number; clientY: number }) => flow.screenToFlowPosition({ x: e.clientX, y: e.clientY });

  /** A press on a stretch, or anywhere on a cable without bends (index -1), which a drag turns into a step. The drag follows the pointer
      on the window, as what is under it changes once the cable has bends. */
  const press = (e: ReactPointerEvent<SVGPathElement>, index: number) => {
    if (e.button !== 0 || !cable.onBends) return;
    e.stopPropagation();
    const onBends = cable.onBends;
    const d = { start: at(e), bends: route ? route.slice(1, -1) : [], index, moved: false, latest: null as Point[] | null };

    const move = (ev: PointerEvent) => {
      const now = at(ev);
      if (!d.moved && Math.hypot(now.x - d.start.x, now.y - d.start.y) < DRAG_START / flow.getZoom()) return;
      if (!d.moved) {
        d.moved = true;
        // A cable without bends gets a step where it was grabbed, whose middle stretch is then the one dragged; pulled across the flow,
        // a detour whose stretch aside is.
        if (d.index < 0) {
          const grabbed = snapped(d.start, ev);
          [d.bends, d.index] = acrossFlow(from, d.start, now) ? [detourAt(from, to, grabbed, 2 * GRID), 2] : [stepAt(from, to, grabbed), 1];
        }
      }
      const full = [from.at, ...d.bends, to.at];
      const p = snapped(now, ev);
      d.latest = moveStretch(d.bends, full, d.index, flat(full[d.index], full[d.index + 1]) ? p.y : p.x);
      setDragged(d.latest);
    };
    const release = () => {
      window.removeEventListener('pointermove', move);
      window.removeEventListener('pointerup', release);
      window.removeEventListener('pointercancel', release);
      if (d.moved && d.latest) onBends(d.latest);
      setDragged(null);
    };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', release);
    window.addEventListener('pointercancel', release);
  };

  const stretches = route ? movable(route) : [];

  return (
    <>
      <BaseEdge path={path} markerEnd={markerEnd} style={style} interactionWidth={interactionWidth} />
      {cable.onBends && (
        <g className="graph-cable-edit">
          {route ? (
            route.slice(0, -1).map((p, i) => {
              const q = route[i + 1];
              const draggable = stretches.includes(i);
              return (
                <path
                  key={i}
                  className={`graph-cable-grab nopan ${draggable ? (flat(p, q) ? 'flat' : 'upright') : ''}`}
                  d={`M ${p.x} ${p.y} L ${q.x} ${q.y}`}
                  onPointerDown={draggable ? (e) => press(e, i) : undefined}
                  onDoubleClick={(e) => {
                    e.stopPropagation();
                    cable.onBends?.(splitAt(route.slice(1, -1), route, i, snapped(at(e), e)));
                  }}
                />
              );
            })
          ) : (
            <path
              className="graph-cable-grab nopan"
              d={path}
              onPointerDown={(e) => press(e, -1)}
              onDoubleClick={(e) => {
                e.stopPropagation();
                cable.onBends?.(stepAt(from, to, snapped(at(e), e)));
              }}
            />
          )}
          {route?.slice(1, -1).map((p, k) => (
            <rect
              key={k}
              className="graph-bend nopan"
              x={p.x - 3}
              y={p.y - 3}
              width={6}
              height={6}
              onDoubleClick={(e) => {
                e.stopPropagation();
                const rest = removeBend(route.slice(1, -1), k);
                cable.onBends?.(rest.length > 0 ? rest : null);
              }}
            >
              <title>Double-click to take this bend out</title>
            </rect>
          ))}
        </g>
      )}
    </>
  );
}
