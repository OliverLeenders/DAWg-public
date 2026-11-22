import { useEffect, useRef, useState } from 'react';
import { newGestureId } from '../store/store';
import { dragTo, normOf, settle, wheelTo, type DragSpec, type Taper } from './dragScale';

interface Props {
  value: number;
  min: number;
  max: number;
  /** Whole steps only (a key, a count): steps of `quantum`, 1 by default. How far a drag goes comes from the range
      (dragScale.ts); Shift is finer. */
  integer?: boolean;
  /** What values round to; none for a continuous value unless given. */
  quantum?: number;
  /** The value's curve, as its knob's: the low end of a skewed range gets more travel. */
  taper?: Taper;
  /** Pixels for the whole range instead of 200, for a range much wider than what's used. */
  rangePixels?: number;
  /** Value per wheel notch, instead of a step of a whole number or a hundredth of a continuous value's travel. */
  wheelStep?: number;
  format?: (v: number) => string;
  /** Reads a typed value (a note name, say); when given, typing starts from the formatted value. Plain numbers otherwise. */
  parse?: (text: string) => number | null;
  /** Called continuously while dragging (same gesture id) and once on typed entry. */
  onChange: (value: number, gesture: string) => void;
  title?: string;
  className?: string;
  /** Drag direction: 'y' (up increases, the default) or 'x' (right increases). */
  axis?: 'x' | 'y';
  /** When set, double-click resets to this value instead of typing a number. */
  resetValue?: number;
  /** Called when a drag starts and ends (automation touch; docs/design/phase10-automation.md, A7). */
  onGestureStart?: () => void;
  onGestureEnd?: () => void;
}

/** A numeric field you drag vertically to change, click to type into, or click and then scroll to nudge. */
export function DragNumber({
  value,
  min,
  max,
  integer = false,
  quantum,
  taper,
  rangePixels,
  wheelStep,
  format = String,
  parse,
  onChange,
  title,
  className,
  axis = 'y',
  resetValue,
  onGestureStart,
  onGestureEnd,
}: Props) {
  const [editing, setEditing] = useState(false);
  const field = useRef<HTMLSpanElement>(null);
  // A drag counts from where it started, along the value's travel; pressing or letting go of Shift starts it over from
  // where it is, so the value doesn't jump.
  const drag = useRef<{ start: number; startNorm: number; fine: boolean; gesture: string; moved: boolean } | null>(null);
  const last = useRef(value);
  last.current = value;
  const wheel = useRef<{ gesture: string; last: number; value: number } | null>(null);
  // The wheel only nudges a field you have clicked, until the pointer leaves it: otherwise scrolling a panel past
  // its fields would change them.
  const wheelArmed = useRef(false);
  const spec: DragSpec = { min, max, integer, quantum, taper, rangePixels };
  const position = (e: React.PointerEvent) => (axis === 'x' ? -e.clientX : e.clientY);

  // Wheel events need a non-passive listener to keep the page from scrolling behind the field.
  useEffect(() => {
    const element = field.current;
    if (element === null || editing) return;

    const onWheel = (e: WheelEvent) => {
      if (!wheelArmed.current) return;
      e.preventDefault();
      const now = performance.now();
      const recent = wheel.current !== null && now - wheel.current.last < 250;
      const gesture = recent && wheel.current !== null ? wheel.current.gesture : newGestureId();
      const direction = e.deltaY < 0 ? 1 : -1;
      const base = recent && wheel.current !== null ? wheel.current.value : value;
      const next = wheelTo(spec, base, direction, wheelStep);
      wheel.current = { gesture, last: now, value: next };
      if (next !== base) onChange(next, gesture);
    };

    element.addEventListener('wheel', onWheel, { passive: false });
    return () => element.removeEventListener('wheel', onWheel);
  }, [editing, value, wheelStep, integer, quantum, taper, rangePixels, min, max, onChange]); // eslint-disable-line react-hooks/exhaustive-deps

  if (editing) {
    return (
      <input
        className={`drag-number editing ${className ?? ''}`}
        autoFocus
        defaultValue={parse && format ? format(value) : value}
        onFocus={(e) => e.currentTarget.select()}
        onBlur={() => setEditing(false)}
        onKeyDown={(e) => {
          if (e.key === 'Enter') {
            const text = (e.target as HTMLInputElement).value;
            const parsed = parse ? parse(text) : Number(text);
            if (parsed !== null && Number.isFinite(parsed)) onChange(settle(spec, parsed), newGestureId());
            setEditing(false);
          } else if (e.key === 'Escape') {
            setEditing(false);
          }
        }}
      />
    );
  }

  return (
    <span
      ref={field}
      className={`drag-number ${className ?? ''}`}
      title={title ?? (resetValue === undefined ? 'Drag to change (Shift for fine), click to type, click and scroll to nudge' : 'Drag to change, double-click to reset')}
      onDoubleClick={() => {
        if (resetValue !== undefined && value !== resetValue) onChange(resetValue, newGestureId());
      }}
      onPointerDown={(e) => {
        if (e.button !== 0) return;
        wheel.current = null;
        wheelArmed.current = true;
        // The device strip leaves the wheel to an armed field (device-strip.md, D9).
        if (field.current) field.current.dataset.takesWheel = '';
        (e.target as HTMLElement).setPointerCapture(e.pointerId);
        drag.current = { start: position(e), startNorm: normOf(spec, value), fine: e.shiftKey, gesture: newGestureId(), moved: false };
        onGestureStart?.();
      }}
      onPointerMove={(e) => {
        const d = drag.current;
        if (!d) return;

        // A click without movement opens the field for typing; only a real drag changes the value.
        if (!d.moved) {
          if (Math.abs(position(e) - d.start) < 3) return;
          d.moved = true;
        }

        if (e.shiftKey !== d.fine) {
          d.start = position(e);
          d.startNorm = normOf(spec, last.current);
          d.fine = e.shiftKey;
        }

        const next = dragTo(spec, d.startNorm, d.start - position(e), d.fine);
        if (next !== last.current) {
          last.current = next;
          onChange(next, d.gesture);
        }
      }}
      onPointerUp={() => {
        const d = drag.current;
        drag.current = null;
        if (!d) return;
        onGestureEnd?.();

        if (!d.moved && resetValue === undefined) setEditing(true);
      }}
      onPointerLeave={() => {
        wheelArmed.current = false;
        if (field.current) delete field.current.dataset.takesWheel;
      }}
      onPointerCancel={() => {
        if (drag.current) onGestureEnd?.();
        drag.current = null;
      }}
    >
      {format(value)}
    </span>
  );
}
