import { useEffect, useRef, type RefObject } from 'react';

// A scroll area's content fades out at an edge where more of it is hidden beyond (docs/design/design-system.md, DS7).
// The element gets the `scroll-fade` class; this marks it with `data-more-start` and `data-more-end` while there is
// more content before or after what shows, and the stylesheet fades that edge. Set directly on the element as it
// scrolls or its content changes size, so nothing re-renders.

/** How close to an end, in pixels, still counts as at the end (scroll positions are fractional at some scales). */
const SLACK = 1;

/** Marks `ref`'s element as it scrolls, along its main axis: horizontal unless `vertical`. */
export function useScrollFade(ref: RefObject<HTMLElement | null>, vertical = false) {
  const watching = useRef<{ element: HTMLElement; stop: () => void } | null>(null);

  // After every render, as the element can appear later or be replaced (the strip shows a placeholder without a track).
  useEffect(() => {
    const element = ref.current;
    if (watching.current?.element === element) return;
    watching.current?.stop();
    watching.current = null;
    if (!element) return;

    const update = () => {
      const position = vertical ? element.scrollTop : element.scrollLeft;
      const hidden = vertical ? element.scrollHeight - element.clientHeight : element.scrollWidth - element.clientWidth;
      element.toggleAttribute('data-more-start', position > SLACK);
      element.toggleAttribute('data-more-end', position < hidden - SLACK);
    };

    // The content's size changes as cards are added, resized or folded: watch the element and its children.
    const resized = new ResizeObserver(update);
    const observeChildren = () => {
      resized.disconnect();
      resized.observe(element);
      for (const child of element.children) resized.observe(child);
      update();
    };
    const added = new MutationObserver(observeChildren);
    added.observe(element, { childList: true });
    element.addEventListener('scroll', update, { passive: true });
    observeChildren();

    watching.current = {
      element,
      stop: () => {
        element.removeEventListener('scroll', update);
        added.disconnect();
        resized.disconnect();
      },
    };
  });

  useEffect(
    () => () => {
      watching.current?.stop();
      watching.current = null;
    },
    [],
  );
}
