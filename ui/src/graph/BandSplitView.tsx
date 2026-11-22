import { useRef } from 'react';
import type { GraphNode, NodeTypeInfo } from '../bridge/protocol';
import { dispatch, newGestureId } from '../store/store';
import { BAND_NAMES, bandCount, frequencyToPosition, orderCrossovers, positionToFrequency } from './splits';

// A Band Split's bands on a frequency strip, its crossovers as handles to drag (docs/design/split-nodes.md, section 6).
// Dragging one past the next pushes it along; the whole drag is one undo step.

const WIDTH = 150;
const HEIGHT = 22;

const formatHz = (hz: number) => (hz >= 1000 ? `${(hz / 1000).toFixed(hz >= 10000 ? 0 : 1)} kHz` : `${Math.round(hz)} Hz`);

export function BandSplitView({ trackId, node, info }: { trackId: number; node: GraphNode; info: NodeTypeInfo | undefined }) {
  const count = bandCount(node);
  const ids = Array.from({ length: count - 1 }, (_, i) => `crossover${i + 1}`);
  const values = ids.map((id) => node.params[id] ?? info?.params.find((p) => p.id === id)?.default ?? 1000);
  const names = BAND_NAMES[count] ?? BAND_NAMES[3];
  const svg = useRef<SVGSVGElement>(null);
  const edges = [0, ...values.map(frequencyToPosition), 1];

  const drag = (index: number) => (event: React.PointerEvent<SVGRectElement>) => {
    event.stopPropagation();
    const target = event.currentTarget;
    target.setPointerCapture(event.pointerId);
    const gesture = newGestureId();
    let current = values;

    const move = (e: PointerEvent) => {
      const box = svg.current?.getBoundingClientRect();
      if (!box) return;
      const next = orderCrossovers(current, index, positionToFrequency((e.clientX - box.left) / box.width));
      next.forEach((value, i) => {
        if (Math.abs(value - current[i]) > 1e-6)
          dispatch({ command: 'setNodeParam', args: { trackId, nodeId: node.id, param: ids[i], value } }, gesture, `${node.id}:${ids[i]}`);
      });
      current = next;
    };
    const up = () => {
      target.removeEventListener('pointermove', move);
      target.removeEventListener('pointerup', up);
    };
    target.addEventListener('pointermove', move);
    target.addEventListener('pointerup', up);
  };

  return (
    <svg ref={svg} className="band-split-view nodrag" viewBox={`0 0 ${WIDTH} ${HEIGHT}`} preserveAspectRatio="none" role="img" aria-label="Bands">
      {names.map((name, i) => (
        <rect key={name} className={`band-split-band tone-band-${i}`} x={edges[i] * WIDTH} y={0} width={(edges[i + 1] - edges[i]) * WIDTH} height={HEIGHT}>
          <title>{name}</title>
        </rect>
      ))}
      {values.map((hz, i) => (
        <g key={ids[i]}>
          <line className="band-split-crossover" x1={edges[i + 1] * WIDTH} x2={edges[i + 1] * WIDTH} y1={0} y2={HEIGHT} />
          <rect className="band-split-handle" x={edges[i + 1] * WIDTH - 4} y={0} width={8} height={HEIGHT} onPointerDown={drag(i)}>
            <title>{`Crossover ${i + 1}: ${formatHz(hz)} (drag)`}</title>
          </rect>
        </g>
      ))}
    </svg>
  );
}
