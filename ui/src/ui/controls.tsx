import { Power as PowerIcon } from 'lucide-react';
import type { ButtonHTMLAttributes, ReactNode, SelectHTMLAttributes } from 'react';

// The design system's controls (docs/design/design-system.md, DS10–DS12): one component per job, each drawn by
// ui/controls.css with the same states everywhere. Sizes are the three control heights (DS5): `s` 18 px (card and
// track headers, chips), `m` 24 px (panels, fields, most buttons), `l` 28 px (the top bar, dialog buttons).

export type ControlSize = 's' | 'm' | 'l';

const sizeClass = (size: ControlSize) => (size === 's' ? 'small' : size === 'l' ? 'large' : '');
const classes = (...names: Array<string | false | undefined>) => names.filter(Boolean).join(' ');

type ButtonProps = ButtonHTMLAttributes<HTMLButtonElement> & {
  size?: ControlSize;
  /** `primary`: the whole accent, one per dialog; `danger`: removes something; `ghost`: no border or fill, in dense
      rows; `plain`: the default. */
  variant?: 'plain' | 'primary' | 'danger' | 'ghost';
  ref?: React.Ref<HTMLButtonElement>;
};

/** A button with words (and an icon before them, if any). */
export function Button({ size = 'm', variant = 'plain', className, type = 'button', ...props }: ButtonProps) {
  return <button type={type} className={classes('text-button', sizeClass(size), variant !== 'plain' && variant, className)} {...props} />;
}

/** A button that is only an icon: square, at the control heights. It needs a `title` (and gets it as its label). */
export function IconButton({ size = 'm', variant = 'plain', className, type = 'button', ...props }: ButtonProps) {
  return (
    <button
      type={type}
      aria-label={props['aria-label'] ?? props.title}
      className={classes('icon-button', sizeClass(size), variant !== 'plain' && variant, className)}
      {...props}
    />
  );
}

type ToggleProps = Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'onChange'> & {
  on: boolean;
  onChange: (on: boolean) => void;
  size?: ControlSize;
  /** Only an icon: square, like an IconButton. */
  icon?: boolean;
};

/** Something that is on or off (Sync, Listen, a section shown): lit in the accent's tint with an accent border while on
    (DS11). Its words or icon say what it does, never "On" or "Off". */
export function Toggle({ on, onChange, size = 'm', icon = false, className, type = 'button', ...props }: ToggleProps) {
  return (
    <button
      type={type}
      aria-pressed={on}
      aria-label={props['aria-label'] ?? (icon ? props.title : undefined)}
      className={classes('toggle', icon ? 'icon-button' : 'text-button', sizeClass(size), on && 'on', className)}
      onClick={() => onChange(!on)}
      {...props}
    />
  );
}

/** A device's or a send's on/off: the same power switch everywhere (DS2, 3.9). */
export function Power({
  on,
  onChange,
  name,
  size = 'm',
  className,
}: {
  on: boolean;
  onChange: (on: boolean) => void;
  /** What it switches, for its tooltip. */
  name?: string;
  size?: ControlSize;
  className?: string;
}) {
  const title = `${name ? `${name}: ` : ''}${on ? 'on (click to bypass)' : 'bypassed (click to switch on)'}`;
  return (
    <button
      type="button"
      aria-pressed={on}
      aria-label={name ? `${name} on` : 'On'}
      title={title}
      className={classes('power', sizeClass(size), on && 'on', className)}
      onClick={(event) => {
        event.stopPropagation();
        onChange(!on);
      }}
    >
      <PowerIcon size={size === 's' ? 11 : 13} />
    </button>
  );
}

/** A track's switch: mute, solo, arm, monitor, automation. A letter or icon on the switch's own colour when on, so
    the state never rests on colour alone (DS15). */
export function TrackSwitch({
  kind,
  on,
  className,
  children,
  ...props
}: ButtonHTMLAttributes<HTMLButtonElement> & { kind: string; on: boolean; children: ReactNode }) {
  return (
    <button type="button" aria-pressed={on} className={classes('switch', kind, on && 'on', className)} {...props}>
      {children}
    </button>
  );
}

/** One of a few choices side by side: the chosen one in the whole accent (DS11). */
export function Segmented<T extends string | number>({
  options,
  value,
  onChange,
  label,
  className,
}: {
  options: ReadonlyArray<{ value: T; label: ReactNode; title?: string }>;
  value: T;
  onChange: (value: T) => void;
  /** What is chosen, for assistive technology and the tooltip. */
  label: string;
  className?: string;
}) {
  // One tab stop, the chosen option; the arrow keys choose the next or previous one, as a radio group does.
  const chosen = Math.max(0, options.findIndex((o) => o.value === value));
  const onKeyDown = (e: React.KeyboardEvent<HTMLDivElement>) => {
    const step = { ArrowLeft: -1, ArrowUp: -1, ArrowRight: 1, ArrowDown: 1 }[e.key];
    if (step === undefined || options.length === 0) return;
    e.preventDefault();
    const next = (chosen + step + options.length) % options.length;
    onChange(options[next].value);
    e.currentTarget.querySelectorAll<HTMLButtonElement>('[role="radio"]')[next]?.focus();
  };
  return (
    <div className={classes('segmented', className)} role="radiogroup" aria-label={label} title={label} onKeyDown={onKeyDown}>
      {options.map((option, i) => (
        <button
          key={String(option.value)}
          type="button"
          role="radio"
          aria-checked={option.value === value}
          tabIndex={i === chosen ? 0 : -1}
          className={option.value === value ? 'active' : ''}
          title={option.title}
          onClick={() => onChange(option.value)}
        >
          {option.label}
        </button>
      ))}
    </div>
  );
}

/** A menu of choices: the native select, at the control heights. */
export function Select({ size = 'm', className, ...props }: Omit<SelectHTMLAttributes<HTMLSelectElement>, 'size'> & { size?: ControlSize; ref?: React.Ref<HTMLSelectElement> }) {
  return <select className={classes('select', sizeClass(size), className)} {...props} />;
}
