import type { ButtonHTMLAttributes, HTMLAttributes, ReactNode, Ref } from 'react';

// Tabs, two kinds and no others (docs/design/design-system.md, DS10, step 7): `view` tabs switch what a panel shows and
// are words with an accent underline (the browser's Sounds, MIDI files, Plugins); `lane` tabs pick a lane under an
// editor and are raised into it (the roll's Velocity, Pressure…). The arrow keys move between a row's tabs.

export function Tabs({
  variant,
  label,
  className = '',
  children,
  ref,
  ...rest
}: HTMLAttributes<HTMLDivElement> & { variant: 'view' | 'lane'; label: string; children: ReactNode; ref?: Ref<HTMLDivElement> }) {
  const move = (e: React.KeyboardEvent<HTMLDivElement>) => {
    rest.onKeyDown?.(e);
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    const tabs = [...e.currentTarget.querySelectorAll<HTMLElement>('[role="tab"]')];
    const at = tabs.indexOf(document.activeElement as HTMLElement);
    if (at < 0) return;
    e.preventDefault();
    tabs[(at + (e.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length].focus();
  };
  return (
    <div ref={ref} role="tablist" aria-label={label} className={`tabs ${variant} ${className}`.trim()} {...rest} onKeyDown={move}>
      {children}
    </div>
  );
}

export function Tab({ selected, className = '', ...rest }: ButtonHTMLAttributes<HTMLButtonElement> & { selected: boolean }) {
  return <button type="button" role="tab" aria-selected={selected} className={`tab ${selected ? 'active' : ''} ${className}`.trim()} {...rest} />;
}
