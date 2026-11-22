import { createContext, useLayoutEffect, useState, type RefObject } from 'react';

// How narrow a device card may get (docs/design/device-strip.md, D5). A card never gets narrower than what its panel
// needs at the dock's current height, by resizing or by a remembered width: the panel's controls then can't be pushed
// over its graphs or out of the card. Most panels are measured (useMeasuredMinWidth); a panel whose layout trades
// space itself, such as the compressor shrinking its curve, reports its own minimum instead (PanelMinWidth).

/** Lets a panel whose layout is measured tell its card how narrow it can go. Outside a card nothing listens. */
export const PanelMinWidth = createContext<(width: number) => void>(() => {});

/** The panel's min-content width: what it needs with every column at its own minimum and nothing overlapping. */
export function minContentWidth(panel: HTMLElement): number {
  const { width, flex, minWidth, maxWidth } = panel.style;
  panel.style.flex = 'none';
  panel.style.minWidth = '0';
  panel.style.maxWidth = 'none';
  panel.style.width = 'min-content';
  const measured = panel.getBoundingClientRect().width;
  panel.style.width = width;
  panel.style.flex = flex;
  panel.style.minWidth = minWidth;
  panel.style.maxWidth = maxWidth;
  return Math.ceil(measured);
}

// Content changes (a knob appearing, a longer readout) are checked at most this often; resizes at the next frame.
const CONTENT_INTERVAL_MS = 200;

/**
 * The min-content width of the panel (`.device-panel`) directly inside `container`, kept up to date as the container
 * resizes (the dock's height, the card's width) and as the panel's contents change. 0 until a panel has been
 * measured. While inactive (or while the panel is away, as for a card scrolled far out of view) the last measurement
 * is kept, so the card doesn't change width meanwhile.
 */
export function useMeasuredMinWidth(container: RefObject<HTMLElement | null>, active: boolean): number {
  const [min, setMin] = useState(0);

  useLayoutEffect(() => {
    const root = container.current;

    if (!root || !active) return;

    let frame = 0;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const panelOf = () => root.querySelector<HTMLElement>(':scope > .device-panel');
    const measure = () => {
      frame = 0;
      const panel = panelOf();
      if (panel) setMin(minContentWidth(panel));
    };
    const soon = () => {
      if (!frame) frame = requestAnimationFrame(measure);
    };
    const later = () => {
      if (timer === undefined)
        timer = setTimeout(() => {
          timer = undefined;
          soon();
        }, CONTENT_INTERVAL_MS);
    };

    // The container (its height follows the dock), the panel and its columns.
    const resize = new ResizeObserver(soon);
    const watch = () => {
      resize.disconnect();
      resize.observe(root);
      const panel = panelOf();

      if (panel) {
        resize.observe(panel);
        for (const column of panel.children) resize.observe(column);
      }
    };
    const content = new MutationObserver(() => {
      watch();
      later();
    });
    content.observe(root, { childList: true, subtree: true, characterData: true });
    watch();
    measure();

    return () => {
      cancelAnimationFrame(frame);
      clearTimeout(timer);
      resize.disconnect();
      content.disconnect();
    };
  }, [container, active]);

  return min;
}
