import { useRef, useState } from 'react';
import type { ParamDescriptor } from '../bridge/protocol';
import { formatParam, noteHint, parseParam, takesNoteNames } from '../graph/params';
import { dispatch, newGestureId } from '../store/store';

// A shown value you can click and type into (docs/ui-review-2026-11-10.md, 1.10): a knob's value, a graph's readout,
// a sample zone's envelope time. Enter or leaving the field sets what was typed; Escape keeps the value. What can't
// be read as a number is ignored.

export function ValueText({
  text,
  parse,
  onSet,
  className = '',
  title,
  hint,
}: {
  /** The value as shown, with its unit. The field starts with it. */
  text: string;
  /** What was typed as a value, or null if it isn't one. */
  parse: (typed: string) => number | null;
  /** Sets the value, as one undo step. */
  onSet: (value: number, gesture: string) => void;
  className?: string;
  title?: string;
  /** A note beside the value, for its tooltip and under the field while typing: a frequency's nearest note (DS17).
      Given what was typed so far, read as a value, or null where it isn't one yet. */
  hint?: (typed: number | null) => string | undefined;
}) {
  const [editing, setEditing] = useState(false);
  const [typed, setTyped] = useState<string | null>(null);
  const cancelled = useRef(false);

  if (editing) {
    const commit = (typed: string) => {
      setEditing(false);
      if (cancelled.current) return;
      const value = parse(typed);
      if (value !== null) onSet(value, newGestureId());
    };
    const note = hint?.(typed === null ? null : parse(typed));
    const field = (
      <input
        className={`value-text editing ${className}`}
        autoFocus
        defaultValue={text}
        aria-label={title ?? 'Value'}
        aria-description={note}
        onChange={(e) => setTyped(e.currentTarget.value)}
        onFocus={(e) => e.currentTarget.select()}
        onBlur={(e) => commit(e.currentTarget.value)}
        onKeyDown={(e) => {
          e.stopPropagation(); // keys typed here aren't shortcuts
          if (e.key === 'Enter') e.currentTarget.blur();
          else if (e.key === 'Escape') {
            cancelled.current = true;
            e.currentTarget.blur();
          }
        }}
        onPointerDown={(e) => e.stopPropagation()}
      />
    );
    if (!hint) return field;
    return (
      <span className="value-text-field">
        {field}
        {note && <span className="value-hint">{note}</span>}
      </span>
    );
  }

  const shownNote = hint?.(null);

  return (
    <span
      className={`value-text ${className}`}
      role="button"
      tabIndex={0}
      title={`${title ? `${title}: ` : ''}${shownNote ? `${shownNote}, ` : ''}click to type a value${shownNote ? ' or a note (A4, C#3+20)' : ''}`}
      onClick={(e) => {
        e.stopPropagation();
        cancelled.current = false;
        setTyped(null);
        setEditing(true);
      }}
      onKeyDown={(e) => {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          cancelled.current = false;
          setTyped(null);
          setEditing(true);
        }
      }}
      onPointerDown={(e) => e.stopPropagation()}
    >
      {text}
    </span>
  );
}

/** A node parameter's value, shown as its readout and typed into: for readouts beside a graph (a threshold, a cutoff). */
export function ParamValueText({
  trackId,
  nodeId,
  param,
  value,
  text,
  className,
}: {
  trackId: number;
  nodeId: number;
  param: ParamDescriptor;
  value: number;
  /** Shown instead of the formatted value, such as "−∞ dB" at a range's bottom. */
  text?: string;
  className?: string;
}) {
  return (
    <ValueText
      text={text ?? formatParam(param, value)}
      title={param.name}
      className={className}
      parse={(typed) => parseParam(param, typed)}
      hint={paramHint(param, value)}
      onSet={(next, gesture) => dispatch({ command: 'setNodeParam', args: { trackId, nodeId, param: param.id, value: next } }, gesture)}
    />
  );
}

/** The hint for a parameter's ValueText: the nearest note of what was typed, or of the value as it is; none where the
    parameter takes no note names (DS17). */
export function paramHint(param: ParamDescriptor, value: number) {
  return takesNoteNames(param) ? (typed: number | null) => noteHint(param, typed ?? value) : undefined;
}

