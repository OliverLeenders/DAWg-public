import { Info, TriangleAlert, X } from 'lucide-react';
import { useEffect } from 'react';
import { create } from 'zustand';
import { useDawg } from '../store/store';
import { IconButton } from '../ui/controls';

// A new error or hint, where it's seen: a toast at the bottom right that fades after a few seconds. An error then stays
// in the status bar until it's dismissed there (docs/design/ux-review.md, U28); it isn't shown in both at once
// (docs/ui-review-2026-11-10.md, 3.3). A hint (lastHint: how something works, nothing failed) has an info style and
// isn't kept.

const SHOWN_MS = 6000;

/** What the toast shows now. */
const useToast = create<{ error: string | null; hint: string | null }>(() => ({ error: null, hint: null }));

/** Whether the toast shows the latest error, so the status bar leaves it out meanwhile. */
export const useErrorToastShowing = () => useToast((s) => s.error !== null);

export function ErrorToast() {
  const lastError = useDawg((s) => s.lastError);
  const lastHint = useDawg((s) => s.lastHint);
  const { error, hint } = useToast();

  useEffect(() => {
    useToast.setState({ error: lastError });
    if (!lastError) return;
    const timer = window.setTimeout(() => useToast.setState({ error: null }), SHOWN_MS);
    return () => window.clearTimeout(timer);
  }, [lastError]);

  useEffect(() => {
    useToast.setState({ hint: lastHint });
    if (!lastHint) return;
    const timer = window.setTimeout(() => {
      useToast.setState({ hint: null });
      useDawg.setState({ lastHint: null });
    }, SHOWN_MS);
    return () => window.clearTimeout(timer);
  }, [lastHint]);

  if (error) {
    return (
      <div className="error-toast" role="alert">
        <TriangleAlert size={15} />
        <span>{error}</span>
        <IconButton title="Hide (the status bar keeps it)" onClick={() => useToast.setState({ error: null })}>
          <X size={12} />
        </IconButton>
      </div>
    );
  }
  if (hint) {
    return (
      <div className="error-toast hint" role="status">
        <Info size={15} />
        <span>{hint}</span>
        <IconButton title="Hide" onClick={() => useDawg.setState({ lastHint: null })}>
          <X size={12} />
        </IconButton>
      </div>
    );
  }
  return null;
}
