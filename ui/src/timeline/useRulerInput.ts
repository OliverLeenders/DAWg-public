import { useRef, useState } from 'react';
import { TICKS_PER_BEAT, type LoopRange } from '../bridge/protocol';
import { LOOP_ZONE } from '../pianoroll/draw';
import { tickToX, xToTick, type View } from '../pianoroll/geometry';
import type { TempoMap } from '../time/tempoMap';
import { dispatch } from '../store/store';

// The ruler's loop zone (docs/design/phase6-arrangement.md, section 7): dragging in the upper part sets the
// loop range (snapped to the grid, Ctrl/Cmd for free), dragging an end moves that end, dragging between the ends moves
// the whole range, and clicking there toggles the loop. With Alt, the same gestures edit the punch range instead
// (docs/design/audio-features.md, AU10). The lower part is left to the caller (seeking).

const CLICK_PX = 3;
const MIN_LOOP = TICKS_PER_BEAT / 4;

type Target = 'loop' | 'punch';

export function useRulerInput({
  view,
  gridTicks,
  map,
  loop,
  punch,
}: {
  view: View;
  gridTicks: number;
  map: TempoMap;
  loop?: LoopRange;
  punch?: LoopRange;
}) {
  const [preview, setPreview] = useState<{ target: Target; range: LoopRange } | null>(null);
  const drag = useRef<
    | (
        | { kind: 'range'; anchor: number; startX: number; moved: boolean; range: LoopRange | null }
        | { kind: 'handle'; edge: 'start' | 'end'; startX: number; range: LoopRange }
        | { kind: 'body'; anchor: number; startX: number; moved: boolean; origin: LoopRange; range: LoopRange }
      ) & { target: Target }
    | null
  >(null);

  const tickAt = (x: number, free: boolean) => {
    const tick = Math.max(0, xToTick(view, x));
    return free ? Math.round(tick) : map.snap(tick, gridTicks, true);
  };

  return {
    /** The pointer over the ruler at (x, y), when not dragging: what pressing there would do. */
    cursorAt(x: number, y: number, height: number, alt = false) {
      const range = alt ? punch : loop;
      if (!range?.enabled || y > height * LOOP_ZONE) return 'default';
      const [startX, endX] = [tickToX(view, range.start), tickToX(view, range.end)];
      if (Math.abs(x - startX) <= 8 || Math.abs(x - endX) <= 8) return 'ew-resize';
      return x > startX && x < endX ? 'grab' : 'default';
    },

    /** The loop range to draw (the drag preview while dragging). */
    loop: (preview?.target === 'loop' ? preview.range : loop) ?? null,

    /** The punch range to draw. */
    punch: (preview?.target === 'punch' ? preview.range : punch) ?? null,

    /** Returns true if the press is in the loop zone (and handled). */
    down(e: React.PointerEvent, height: number) {
      if (e.button !== 0 || e.nativeEvent.offsetY > height * LOOP_ZONE) return false;
      const x = e.nativeEvent.offsetX;
      const target: Target = e.altKey ? 'punch' : 'loop';
      const current = target === 'punch' ? punch : loop;
      if (current?.enabled) {
        const startX = tickToX(view, current.start);
        const endX = tickToX(view, current.end);
        if (Math.abs(x - startX) <= 8) {
          drag.current = { kind: 'handle', edge: 'start', startX: x, range: current, target };
          return true;
        }
        if (Math.abs(x - endX) <= 8) {
          drag.current = { kind: 'handle', edge: 'end', startX: x, range: current, target };
          return true;
        }
        if (x > startX && x < endX) {
          drag.current = { kind: 'body', anchor: Math.max(0, xToTick(view, x)), startX: x, moved: false, origin: current, range: current, target };
          return true;
        }
      }
      drag.current = { kind: 'range', anchor: tickAt(x, e.ctrlKey || e.metaKey), startX: x, moved: false, range: null, target };
      return true;
    },

    /** Returns true while a loop drag is in progress. */
    move(e: React.PointerEvent) {
      const d = drag.current;
      if (!d) return false;
      const x = e.nativeEvent.offsetX;
      if (d.kind === 'handle') {
        const tick = tickAt(x, e.ctrlKey || e.metaKey);
        const range =
          d.edge === 'start'
            ? { ...d.range, start: Math.min(tick, d.range.end - MIN_LOOP) }
            : { ...d.range, end: Math.max(tick, d.range.start + MIN_LOOP) };
        d.range = range;
        setPreview({ target: d.target, range });
        return true;
      }
      if (Math.abs(x - d.startX) > CLICK_PX) d.moved = true;
      if (!d.moved) return true;
      if (d.kind === 'body') {
        // The start lands on the grid and the length stays; the range can't go before the song's start.
        const length = d.origin.end - d.origin.start;
        const moved = d.origin.start + Math.max(0, xToTick(view, x)) - d.anchor;
        const start = Math.max(0, e.ctrlKey || e.metaKey ? Math.round(moved) : map.snap(Math.max(0, moved), gridTicks, true));
        d.range = { ...d.origin, start, end: start + length };
        setPreview({ target: d.target, range: d.range });
        return true;
      }
      const tick = tickAt(x, e.ctrlKey || e.metaKey);
      d.range = { enabled: true, start: Math.min(d.anchor, tick), end: Math.max(d.anchor, tick) };
      setPreview({ target: d.target, range: d.range });
      return true;
    },

    up() {
      const d = drag.current;
      drag.current = null;
      setPreview(null);
      if (!d) return;

      const command = d.target === 'punch' ? 'setPunch' : 'setLoop';
      const current = d.target === 'punch' ? punch : loop;
      const set = (args: { enabled?: boolean; start?: number; end?: number }) => dispatch({ command, args });

      if (d.kind === 'body' && d.moved) set({ enabled: true, start: d.range.start, end: d.range.end });
      else if (d.kind === 'handle') set({ enabled: true, start: d.range.start, end: d.range.end });
      else if (!d.moved) set({ enabled: !(current?.enabled ?? false) });
      else if (d.range && d.range.end - d.range.start >= MIN_LOOP) set({ enabled: true, start: d.range.start, end: d.range.end });
    },
  };
}
