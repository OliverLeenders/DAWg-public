import { useContext, useLayoutEffect, useRef, useState } from 'react';
import { PanelMinWidth } from './panelMinWidth';

// The dynamics panels' layout (the Compressor, Gate and Expander; docs/design/gate-expander.md, N14): a square
// transfer curve, a level history beside it, a gain-reduction meter, the timing knobs and the Sidechain column.

// The layout is measured, since the curve's size follows the dock's height (useCurveLayout). From wide to narrow: the
// curve at full height with the level history beside it (when it has MIN_HISTORY to itself); a gain-reduction meter
// in the history's place; the curve getting smaller, down to MIN_CURVE. The card can't get narrower than that, so
// the controls never cover the curve. Only where nothing limits the width (no card around the panel) does the
// Sidechain column go at last.
export const MIN_HISTORY = 160;
export const MIN_CURVE = 120;
export const GR_WIDTH = 24;
export const PANEL_GAP = 16;
/** The gap between the curve and the readout under it. */
export const READOUT_GAP = 4;

export interface Fit {
  mode: 'full' | 'meter' | 'compact';
  /** The curve's side, in px. */
  curve: number;
}

export function useCurveLayout(panel: React.RefObject<HTMLDivElement | null>): Fit {
  const [fit, setFit] = useState<Fit>({ mode: 'full', curve: MIN_CURVE });
  const reportMin = useContext(PanelMinWidth);
  // The Sidechain column's width when last shown: hidden, it measures 0.
  const keyWidth = useRef(0);

  useLayoutEffect(() => {
    const root = panel.current;
    if (!root) return;
    const update = () => {
      const timing = root.querySelector<HTMLElement>('.panel-sections:not(.key-sections)')?.offsetWidth ?? 0;
      const key = root.querySelector<HTMLElement>('.key-sections')?.offsetWidth ?? 0;
      if (key > 0) keyWidth.current = key;
      const others = timing + keyWidth.current + 3 * PANEL_GAP;
      const width = root.clientWidth;
      const readout = root.querySelector<HTMLElement>('.readouts, .panel-readout')?.offsetHeight ?? 16;
      const tall = Math.max(MIN_CURVE, root.clientHeight - readout - READOUT_GAP);
      reportMin(MIN_CURVE + GR_WIDTH + others);

      const next: Fit =
        width >= tall + MIN_HISTORY + others
          ? { mode: 'full', curve: tall }
          : width >= MIN_CURVE + GR_WIDTH + others
            ? { mode: 'meter', curve: Math.min(tall, width - GR_WIDTH - others) }
            : { mode: 'compact', curve: Math.max(MIN_CURVE, Math.min(tall, width - GR_WIDTH - timing - 2 * PANEL_GAP)) };
      setFit((current) => (current.mode === next.mode && current.curve === next.curve ? current : next));
    };
    const observer = new ResizeObserver(update);
    observer.observe(root);
    root.querySelectorAll('.panel-sections, .readouts, .panel-readout').forEach((element) => observer.observe(element));
    update();
    return () => observer.disconnect();
  }, [panel, reportMin]);

  return fit;
}

