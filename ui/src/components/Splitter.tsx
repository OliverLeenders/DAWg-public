import { useRef } from 'react';

/**
 * A drag handle that resizes something: horizontally by default (a vertical bar), vertically with axis "y" (a
 * horizontal bar). `direction` is +1 when dragging right (down) makes it bigger, -1 when dragging right (down) makes it
 * smaller.
 */
export function Splitter({
  width,
  direction,
  onResize,
  className = 'splitter',
  title = 'Drag to resize',
  axis = 'x',
}: {
  width: number;
  direction: 1 | -1;
  onResize: (width: number) => void;
  className?: string;
  title?: string;
  axis?: 'x' | 'y';
}) {
  const drag = useRef<{ startX: number; startWidth: number } | null>(null);
  const position = (e: React.PointerEvent) => (axis === 'x' ? e.clientX : e.clientY);

  return (
    <div
      className={className}
      title={title}
      onPointerDown={(e) => {
        e.preventDefault();
        e.stopPropagation();
        e.currentTarget.setPointerCapture(e.pointerId);
        drag.current = { startX: position(e), startWidth: width };
      }}
      onPointerMove={(e) => {
        const d = drag.current;
        if (d) onResize(d.startWidth + direction * (position(e) - d.startX));
      }}
      onPointerUp={() => (drag.current = null)}
      onPointerCancel={() => (drag.current = null)}
    />
  );
}
