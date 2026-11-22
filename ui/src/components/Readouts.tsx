import type { ReactNode } from 'react';

// Values under a graph (a compressor's threshold and ratio, a filter's cutoff) as a grid of labels and boxed values, so
// the values form a column, nothing wraps mid-value, and what can be typed into looks like a field
// (docs/ui-review-2026-11-10.md, section 3.11 and the follow-up on inline values).

/** The grid: `pairs` label-and-value pairs to a row. */
export function Readouts({ children, pairs = 1, className = '' }: { children: ReactNode; pairs?: number; className?: string }) {
  return (
    <div className={`readouts ${className}`} style={{ gridTemplateColumns: `repeat(${pairs}, max-content max-content)` }}>
      {children}
    </div>
  );
}

/** One label and its value. The value is usually a ParamValueText or ValueText, which the grid draws as a box. */
export function Readout({ label, title, children }: { label: string; title?: string; children: ReactNode }) {
  return (
    <>
      <span className="readout-label" title={title}>
        {label}
      </span>
      <span className="readout-value">{children}</span>
    </>
  );
}
