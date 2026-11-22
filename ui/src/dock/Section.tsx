import type { ReactNode } from 'react';

// A labelled group of controls in a device panel: a small letter-spaced header followed by a rule across the group,
// then the controls in knob-sized slots. Shared by every dock panel so groups look the same everywhere.

export function Section({
  label,
  children,
  column = false,
  actions,
}: {
  label: string;
  children: ReactNode;
  column?: boolean;
  /** Small buttons at the header's end, after its rule (the step sequencer's preview and clear). */
  actions?: ReactNode;
}) {
  return (
    <section className="panel-section">
      <SectionHeader label={label} actions={actions} />
      <div className={`panel-section-items ${column ? 'column' : ''}`}>{children}</div>
    </section>
  );
}

export function SectionHeader({ label, actions }: { label: string; actions?: ReactNode }) {
  return (
    <header className="panel-section-header">
      <span>{label}</span>
      {actions && <span className="panel-section-actions">{actions}</span>}
    </header>
  );
}
