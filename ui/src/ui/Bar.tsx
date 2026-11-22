import { MoreHorizontal } from 'lucide-react';
import { useLayoutEffect, useRef, useState, type HTMLAttributes, type ReactNode } from 'react';
import { IconButton } from './controls';
import { Popover } from './overlays';

// A bar of groups that never wraps (docs/design/design-system.md, DS14, step 6): when the groups don't fit, whole
// groups fold into a "⋯" menu at the bar's end, the ones with the highest `fold` first and, among equals, the
// rightmost. Groups without `fold` always stay.

export interface BarItem {
  key: string;
  node: ReactNode;
  /** How readily it folds: higher first. Leave out for groups that always stay (the title, the window's buttons). */
  fold?: number;
  /** Takes the room that is left (a spacer). */
  grow?: boolean;
  /** After the "⋯" button (the window's buttons). */
  end?: boolean;
}

/** The room the "⋯" button and its gap take. */
const MORE_WIDTH = 40;

export function Bar({ items, className = '', ...rest }: HTMLAttributes<HTMLElement> & { items: Array<BarItem | false | null | undefined> }) {
  const shown = items.filter((i): i is BarItem => !!i);
  const ref = useRef<HTMLElement>(null);
  const widths = useRef(new Map<string, number>());
  const [hidden, setHidden] = useState<string[]>([]);
  const [open, setOpen] = useState<{ x: number; y: number } | null>(null);
  const [, setSize] = useState(0);

  useLayoutEffect(() => {
    const bar = ref.current;
    if (!bar) return;
    const observer = new ResizeObserver(() => setSize(bar.clientWidth));
    observer.observe(bar);
    return () => observer.disconnect();
  }, []);

  // After every render: measure what shows, then work out what has to fold.
  useLayoutEffect(() => {
    const bar = ref.current;
    if (!bar) return;
    for (const child of bar.children) {
      const key = (child as HTMLElement).dataset.barKey;
      if (key) widths.current.set(key, (child as HTMLElement).offsetWidth);
    }
    const style = getComputedStyle(bar);
    const gap = parseFloat(style.columnGap) || 0;
    const available = bar.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
    const widthOf = (item: BarItem) => (item.grow ? 24 : widths.current.get(item.key) ?? 0);
    let total = shown.reduce((sum, item) => sum + widthOf(item) + gap, -gap);
    const folding = shown
      .map((item, index) => ({ item, index }))
      .filter(({ item }) => item.fold !== undefined)
      .sort((a, b) => b.item.fold! - a.item.fold! || b.index - a.index);
    const next: string[] = [];
    for (const { item } of folding) {
      if (total + (next.length > 0 ? MORE_WIDTH : 0) <= available) break;
      next.push(item.key);
      total -= widthOf(item) + gap;
    }
    if (next.join() !== hidden.join()) setHidden(next);
  });

  const folded = shown.filter((item) => hidden.includes(item.key));
  const render = (item: BarItem) => (
    <div key={item.key} data-bar-key={item.key} className={`bar-item ${item.grow ? 'bar-grow' : ''}`}>
      {item.node}
    </div>
  );
  return (
    <header ref={ref} className={`bar ${className}`.trim()} {...rest}>
      {shown.filter((item) => !item.end && !hidden.includes(item.key)).map(render)}
      {folded.length > 0 && (
        <IconButton
          size="l"
          className={`bar-more ${open ? 'active' : ''}`}
          title={`More: ${folded.length} group${folded.length > 1 ? 's' : ''} that don't fit`}
          onClick={(e) => {
            const r = e.currentTarget.getBoundingClientRect();
            setOpen(open ? null : { x: r.right - 8, y: r.bottom + 6 });
          }}
        >
          <MoreHorizontal size={16} />
        </IconButton>
      )}
      {shown.filter((item) => item.end).map(render)}
      {open && folded.length > 0 && (
        <Popover className="context-menu bar-overflow" at={open} onClose={() => setOpen(null)}>
          {folded.map((item) => (
            <div key={item.key} className="bar-overflow-group">
              {item.node}
            </div>
          ))}
        </Popover>
      )}
    </header>
  );
}
