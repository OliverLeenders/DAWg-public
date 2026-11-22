import { useEffect, useRef } from 'react';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { drawPlayhead, drawRulerPlayhead } from '../pianoroll/draw';
import { type Size, type Theme } from '../canvas/theme';
import type { View } from '../pianoroll/geometry';
import { useCanvas } from '../canvas/useCanvas';
import { useDawg } from '../store/store';

interface Props {
  view: View;
  theme: Theme;
  /** Draw the ruler's triangle instead of the full-height line. */
  ruler?: boolean;
  /** Called with the playhead tick after each redraw (follow mode). */
  onTick?: (tick: number) => void;
  /** Extra marks that move with the playhead (the played automation values). */
  onPaint?: (ctx: CanvasRenderingContext2D, size: Size, tick: number) => void;
}

/**
 * The playhead on its own transparent canvas, stacked over a view's main canvas. The view subscribes to the
 * playhead only through this component, so during playback the telemetry rate repaints this thin layer instead of
 * every clip, waveform and note (docs/design/phase3-piano-roll.md and phase6-arrangement.md, "playhead overlay").
 */
export function PlayheadOverlay({ view, theme, ruler = false, onTick, onPaint }: Props) {
  const canvas = useCanvas();
  const frame = useRef(0);
  const tick = useDawg((s) => s.telemetry.positionBeats) * TICKS_PER_BEAT;
  const callbacks = useRef({ onTick, onPaint });
  callbacks.current = { onTick, onPaint };

  // No dependency array: any render of the overlay (a tick, a scroll or a resize) repaints it. It is only a few
  // rectangles, so the cost is bounded by the telemetry rate rather than by the size of the view.
  useEffect(() => {
    cancelAnimationFrame(frame.current);
    frame.current = requestAnimationFrame(() => {
      const ctx = canvas.context();
      if (!ctx) return;
      ctx.clearRect(0, 0, canvas.size.width, canvas.size.height);
      // The played values sit under the line, as they did when both were drawn into the lanes canvas.
      callbacks.current.onPaint?.(ctx, canvas.size, tick);
      if (ruler) drawRulerPlayhead(ctx, canvas.size, view, theme, tick);
      else drawPlayhead(ctx, canvas.size, view, theme, tick);
      callbacks.current.onTick?.(tick);
    });
  });

  return <canvas ref={canvas.ref} className="playhead-overlay" aria-hidden />;
}
