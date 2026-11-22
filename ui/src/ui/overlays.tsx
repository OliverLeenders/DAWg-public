import { ChevronRight } from 'lucide-react';
import { createContext, useContext, useEffect, useId, useLayoutEffect, useRef, useState, type CSSProperties, type HTMLAttributes, type ReactNode } from 'react';
import { createPortal } from 'react-dom';

// The design system's overlays (docs/design/design-system.md, DS7, DS10, DS15): menus, popovers and dialogs, on the
// floating level, with one way to close each (Escape, a click outside) and the keyboard kept inside a dialog.

/** The open overlays, innermost last: Escape closes only the innermost one. */
const stack: string[] = [];

function useOverlay(onClose: (() => void) | undefined) {
  const id = useId();
  const close = useRef(onClose);
  close.current = onClose;
  useEffect(() => {
    stack.push(id);
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || stack[stack.length - 1] !== id || !close.current) return;
      e.preventDefault();
      e.stopPropagation();
      close.current();
    };
    // Capture, so the window's shortcuts and the views under it never see the Escape.
    window.addEventListener('keydown', onKey, true);
    return () => {
      window.removeEventListener('keydown', onKey, true);
      const at = stack.lastIndexOf(id);
      if (at >= 0) stack.splice(at, 1);
    };
  }, [id]);
}

/** Gives the focus back to whatever had it when the overlay opened. */
function useReturnFocus() {
  useEffect(() => {
    const before = document.activeElement as HTMLElement | null;
    return () => {
      if (before && document.contains(before)) before.focus({ preventScroll: true });
    };
  }, []);
}

const FOCUSABLE = 'button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), [href], [tabindex]:not([tabindex="-1"])';

/** Keeps a box inside the window: shifts it left or up by what would hang over the edge. With `end`, the box first
    goes up and to the left of its point, so that point is its bottom right corner. */
function useInsideWindow(ref: React.RefObject<HTMLElement | null>, active: boolean, end = false) {
  useLayoutEffect(() => {
    const element = ref.current;
    if (!active || !element) return;
    const margin = 8;
    // Once: the layout effect runs on every render, and the box has moved by then.
    if (end && !element.dataset.anchored) {
      element.dataset.anchored = 'end';
      const box = element.getBoundingClientRect();
      element.style.left = `${box.left - box.width}px`;
      element.style.top = `${box.top - box.height}px`;
    }
    const rect = element.getBoundingClientRect();
    const overRight = rect.right - (window.innerWidth - margin);
    const overBottom = rect.bottom - (window.innerHeight - margin);
    if (overRight > 0) element.style.left = `${Math.max(margin, rect.left - overRight)}px`;
    if (overBottom > 0) element.style.top = `${Math.max(margin, rect.top - overBottom)}px`;
    if (rect.height > window.innerHeight - 2 * margin) {
      element.style.maxHeight = `${window.innerHeight - 2 * margin}px`;
      element.style.overflowY = 'auto';
    }
  });
}

/** A point in the window, where a popover or menu opens. */
export type Point = { x: number; y: number };

/** The point under an element's bottom left corner: where a menu opened from a button goes. A menu in a bar must open
    at a point like this, drawn into the body: the top bar clips what hangs out of it (`.topbar { overflow: hidden }`),
    so a menu placed by its class inside it would never show. */
export function below(element: Element): Point {
  const box = element.getBoundingClientRect();
  return { x: box.left, y: box.bottom + 6 };
}

type PopoverProps = Omit<HTMLAttributes<HTMLElement>, 'onSubmit'> & {
  onClose: () => void;
  /** At a point in the window (a right-click, a button's corner): drawn into the document's body, so no view clips it,
      and kept inside the window. Without it the popover is placed by its class, next to what opened it. */
  at?: Point | null;
  style?: CSSProperties;
  /** Where the box goes from `at`: `start` (below and to the right, the default) or `end` (above and to the left, so a
      button at the foot of a panel opens it upwards). */
  anchor?: 'start' | 'end';
  /** A form instead of a box (a popover that saves something on Enter). */
  onSubmit?: React.FormEventHandler<HTMLFormElement>;
  children: ReactNode;
};

/** A box over the view that closes on Escape or a press outside it. */
export function Popover({ onClose, at, anchor = 'start', className = '', style, onSubmit, children, ...rest }: PopoverProps) {
  const ref = useRef<HTMLElement>(null);
  useOverlay(onClose);
  useReturnFocus();
  useInsideWindow(ref, !!at, anchor === 'end');
  useEffect(() => {
    // Focus moves in unless something inside took it (a search field).
    if (ref.current && !ref.current.contains(document.activeElement)) ref.current.focus({ preventScroll: true });
  }, []);
  const placed = at ? { left: at.x, top: at.y, ...style } : style;
  // Drawn into the body, its presses still pass up through the views that opened it (React's tree): they stop here, so
  // a top bar doesn't take a press in its "⋯" menu for one on its empty parts.
  const stop = (e: React.PointerEvent<HTMLElement>) => {
    rest.onPointerDown?.(e);
    if (at) e.stopPropagation();
  };
  const props = { ref: ref as never, tabIndex: -1, className: `${className} ${at ? 'fixed' : ''}`.trim(), style: placed, ...rest, onPointerDown: stop };
  const body = (
    <>
      <div
        className="menu-backdrop fixed"
        onPointerDown={(e) => {
          if (at) e.stopPropagation();
          onClose();
        }}
        onContextMenu={(e) => e.preventDefault()}
      />
      {onSubmit ? <form {...props} onSubmit={onSubmit}>{children}</form> : <div {...props}>{children}</div>}
    </>
  );
  return at ? createPortal(body, document.body) : body;
}

/** Which of a menu's submenus is open: one at a time, per level. */
const OpenSubmenu = createContext<{ open: string | null; setOpen: (id: string | null) => void } | null>(null);

/** A menu level's submenus: the one open closes when the pointer moves onto another item of the level. */
function useSubmenuLevel() {
  const [open, setOpen] = useState<string | null>(null);
  const onPointerMove = (e: React.PointerEvent<HTMLElement>) => {
    const target = e.target as HTMLElement;
    if (open !== null && target.closest('[role="menu"]') === e.currentTarget && !target.closest('.submenu')) setOpen(null);
  };
  return { value: { open, setOpen }, onPointerMove };
}

/** The arrow keys move through the buttons of the menu level that has the focus, not those of its submenus. */
function moveFocus(e: React.KeyboardEvent<HTMLElement>) {
  if (e.defaultPrevented || !['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(e.key)) return;
  const list = (document.activeElement as HTMLElement | null)?.closest<HTMLElement>('[role="menu"]') ?? e.currentTarget;
  const items = [...list.querySelectorAll<HTMLElement>('button:not(:disabled)')].filter((b) => b.closest('[role="menu"]') === list);
  if (items.length === 0) return;
  e.preventDefault();
  const at = items.indexOf(document.activeElement as HTMLElement);
  const next =
    e.key === 'Home' ? 0 : e.key === 'End' ? items.length - 1 : e.key === 'ArrowDown' ? (at + 1) % items.length : (at - 1 + items.length) % items.length;
  items[next].focus();
}

/** A list of actions (a right-click menu, the top bar's menus): a Popover the arrow keys move through. Its items may
    open submenus (Submenu). */
export function Menu({ className = '', onKeyDown, onPointerMove, ...props }: PopoverProps) {
  const level = useSubmenuLevel();
  const move = (e: React.KeyboardEvent<HTMLElement>) => {
    onKeyDown?.(e);
    moveFocus(e);
  };
  return (
    <OpenSubmenu.Provider value={level.value}>
      <Popover
        role="menu"
        className={`context-menu ${className}`.trim()}
        onKeyDown={move}
        onPointerMove={(e) => {
          onPointerMove?.(e);
          level.onPointerMove(e);
        }}
        {...props}
      />
    </OpenSubmenu.Provider>
  );
}

/** An item of a Menu that opens a menu of its own beside it: on hover, a click, or the right arrow key (the left one
    goes back). The submenu is placed at a point in the window, so a scrolling menu doesn't clip it, and kept inside
    the window; a long one scrolls. */
export function Submenu({ label, detail, className = '', children }: { label: ReactNode; detail?: ReactNode; className?: string; children: ReactNode }) {
  const id = useId();
  const level = useContext(OpenSubmenu);
  const [ownOpen, setOwnOpen] = useState(false);
  const open = level ? level.open === id : ownOpen;
  const setOpen = (on: boolean) => (level ? level.setOpen(on ? id : null) : setOwnOpen(on));
  const row = useRef<HTMLButtonElement>(null);
  const popup = useRef<HTMLDivElement>(null);
  const focusFirst = useRef(false);
  const inner = useSubmenuLevel();

  useLayoutEffect(() => {
    const box = popup.current;
    const rowBox = row.current?.getBoundingClientRect();
    // Beside the menu the row is in, not over its edge.
    const menuBox = row.current?.closest('[role="menu"]')?.getBoundingClientRect();
    if (!open || !box || !rowBox || !menuBox) return;
    const anchor = { left: menuBox.left, right: menuBox.right - 1, top: rowBox.top };
    const margin = 8;
    // Measured at the window's corner, where nothing squeezes it.
    box.style.left = '0px';
    box.style.top = '0px';
    box.style.maxHeight = `${window.innerHeight - 2 * margin}px`;
    const { width, height } = box.getBoundingClientRect();
    // To the right of the row, or to its left where the window ends; level with it, or higher where it would hang out.
    const left = anchor.right + width <= window.innerWidth - margin ? anchor.right : Math.max(margin, anchor.left - width);
    box.style.left = `${left}px`;
    box.style.top = `${Math.max(margin, Math.min(anchor.top - 4, window.innerHeight - margin - height))}px`;
    if (focusFirst.current) {
      focusFirst.current = false;
      box.querySelector<HTMLElement>('button:not(:disabled)')?.focus({ preventScroll: true });
    }
  }, [open]);

  const openWithKeys = () => {
    focusFirst.current = true;
    setOpen(true);
  };

  return (
    <div className={`submenu ${className}`.trim()}>
      <button
        ref={row}
        className={`menu-item submenu-row ${open ? 'open' : ''}`}
        aria-haspopup="menu"
        aria-expanded={open}
        onPointerEnter={() => setOpen(true)}
        onClick={() => (open ? setOpen(false) : openWithKeys())}
        onKeyDown={(e) => {
          if (e.key !== 'ArrowRight') return;
          e.preventDefault();
          openWithKeys();
        }}
      >
        <span className="submenu-label">{label}</span>
        {detail !== undefined && <span className="submenu-detail">{detail}</span>}
        <ChevronRight size={12} />
      </button>
      {open && (
        <OpenSubmenu.Provider value={inner.value}>
          <div
            ref={popup}
            role="menu"
            className="context-menu submenu-popup fixed"
            onPointerMove={inner.onPointerMove}
            onKeyDown={(e) => {
              if (e.key !== 'ArrowLeft') return;
              e.preventDefault();
              e.stopPropagation();
              setOpen(false);
              row.current?.focus({ preventScroll: true });
            }}
          >
            {children}
          </div>
        </OpenSubmenu.Provider>
      )}
    </div>
  );
}

/** A question or a form over the whole window. Escape cancels (`onClose`), Tab stays inside, and the focus goes back
    where it was when it closes. Its title is the first heading inside, or `title`. */
export function Dialog({
  onClose,
  title,
  className = '',
  closeOnBackdrop = false,
  onSubmit,
  children,
  ...rest
}: Omit<HTMLAttributes<HTMLElement>, 'onSubmit' | 'title'> & {
  /** Escape (and a press outside, with `closeOnBackdrop`); without it the dialog can only be closed from inside. */
  onClose?: () => void;
  title?: ReactNode;
  closeOnBackdrop?: boolean;
  onSubmit?: React.FormEventHandler<HTMLFormElement>;
  children: ReactNode;
}) {
  const ref = useRef<HTMLElement>(null);
  const titleId = useId();
  useOverlay(onClose);
  useReturnFocus();
  useEffect(() => {
    const box = ref.current;
    if (!box || box.contains(document.activeElement)) return;
    const first = box.querySelector<HTMLElement>('input:not(:disabled), select:not(:disabled), textarea:not(:disabled)') ?? box.querySelector<HTMLElement>(FOCUSABLE);
    (first ?? box).focus({ preventScroll: true });
  }, []);
  const trap = (e: React.KeyboardEvent<HTMLElement>) => {
    rest.onKeyDown?.(e);
    if (e.key !== 'Tab' || !ref.current) return;
    const items = [...ref.current.querySelectorAll<HTMLElement>(FOCUSABLE)].filter((el) => el.offsetParent !== null);
    if (items.length === 0) return;
    const first = items[0];
    const last = items[items.length - 1];
    if (e.shiftKey && (document.activeElement === first || !ref.current.contains(document.activeElement))) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && (document.activeElement === last || !ref.current.contains(document.activeElement))) {
      e.preventDefault();
      first.focus();
    }
  };
  const labelled = title ? titleId : rest['aria-labelledby'];
  const props = {
    ref: ref as never,
    tabIndex: -1,
    role: 'dialog',
    'aria-modal': true,
    ...rest,
    'aria-labelledby': labelled,
    className: `modal ${className}`.trim(),
    onKeyDown: trap,
  };
  const content = (
    <>
      {title && <h3 id={titleId}>{title}</h3>}
      {children}
    </>
  );
  return createPortal(
    // Its presses stay here: drawn into the body, but React still passes them up to the views that opened it (the top
    // bar would start dragging the window).
    <div
      className="modal-backdrop"
      onPointerDown={(e) => {
        e.stopPropagation();
        if (closeOnBackdrop && e.target === e.currentTarget) onClose?.();
      }}
    >
      {onSubmit ? <form {...props} onSubmit={onSubmit}>{content}</form> : <div {...props}>{content}</div>}
    </div>,
    document.body,
  );
}
