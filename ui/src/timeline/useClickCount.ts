import { useRef } from 'react';

/** Counts a press as a double-click when it comes soon after one at the same place: PointerEvent.detail is always 0 in
    Chromium, so canvases that act on double-clicks within a press count them here. */
export function useClickCount() {
  const last = useRef({ time: 0, x: 0, y: 0 });
  return (x: number, y: number) => {
    const now = performance.now();
    const previous = last.current;
    const isDouble = now - previous.time < 400 && Math.abs(x - previous.x) < 5 && Math.abs(y - previous.y) < 5;
    last.current = isDouble ? { time: 0, x, y } : { time: now, x, y };
    return isDouble ? 2 : 1;
  };
}
