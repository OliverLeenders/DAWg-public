import { Info } from 'lucide-react';
import { useDawg } from '../store/store';
import { useLayout } from '../store/layoutStore';
import { Button } from '../ui/controls';

// On a Linux desktop whose window manager moves windows on Alt+drag (KDE Plasma 5, Xfce, Cinnamon, MATE by default),
// DAWg's Alt gestures never arrive. Said once, with what works instead and where the desktop's key is changed
// (docs/design/ux-review.md, U12; app/src/DesktopCheck.h).

export function AltDragNotice() {
  const grab = useDawg((s) => s.app?.altDrag ?? null);
  const seen = useLayout((s) => s.altDragNoticeSeen);
  if (!grab || seen) return null;

  return (
    <div className="alt-drag-notice" role="status">
      <Info size={16} />
      <span>
        <strong>{grab.desktop} moves windows with Alt+drag</strong>, so DAWg's Alt gestures don't reach it. Instead:
        double-click empty space for a new clip or note, S or the right-click menu to split, Ctrl+D to duplicate, and the
        piano roll's right-click menu for pitch points and links. Or set another key in {grab.setting}.
      </span>
      <Button onClick={() => useLayout.setState({ altDragNoticeSeen: true })}>
        Got it
      </Button>
    </div>
  );
}
