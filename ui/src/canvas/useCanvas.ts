import { useEffect, useRef, useState } from 'react';
import type { Size } from './theme';

/**
 * A canvas that tracks its CSS size and device pixel ratio. Returns the ref to attach, the CSS size, and a
 * function that returns a context scaled so drawing code works in CSS pixels.
 */
export function useCanvas() {
  const ref = useRef<HTMLCanvasElement>(null);
  const [size, setSize] = useState<Size>({ width: 0, height: 0 });

  const watching = useRef<{ canvas: HTMLCanvasElement; observer: ResizeObserver } | null>(null);

  // After every render: a component can render its canvas later, or replace it (the piano roll shows a placeholder
  // while there is no track), and the new element must be watched too.
  useEffect(() => {
    const canvas = ref.current;
    if (watching.current?.canvas === canvas) return;
    watching.current?.observer.disconnect();
    watching.current = null;
    if (!canvas) return;

    const observer = new ResizeObserver(([entry]) => {
      const { width, height } = entry.contentRect;
      const dpr = window.devicePixelRatio || 1;
      canvas.width = Math.max(1, Math.round(width * dpr));
      canvas.height = Math.max(1, Math.round(height * dpr));
      setSize({ width, height });
    });

    observer.observe(canvas);
    watching.current = { canvas, observer };
  });

  useEffect(
    () => () => {
      watching.current?.observer.disconnect();
      watching.current = null;
    },
    [],
  );

  const context = () => {
    const canvas = ref.current;
    const ctx = canvas?.getContext('2d');
    if (!canvas || !ctx) return null;
    const dpr = canvas.width / Math.max(1, size.width);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return ctx;
  };

  return { ref, size, context };
}
