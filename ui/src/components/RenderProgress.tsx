import * as bridge from '../bridge/bridge';
import { useDawg } from '../store/store';
import { Button } from '../ui/controls';

// A running track render (freeze, bounce, render selection; docs/design/audio-features.md, AU14): playback stops while
// it runs, so it shows what it is doing and can be cancelled.
export function RenderProgress() {
  const render = useDawg((s) => s.render);
  if (!render) return null;

  return (
    <div className="render-progress" role="status">
      <span>{render.label ?? 'Rendering'}…</span>
      <progress max={1} value={render.progress ?? 0} />
      <Button onClick={() => bridge.call('export.cancel').catch(() => {})}>
        Cancel
      </Button>
    </div>
  );
}
