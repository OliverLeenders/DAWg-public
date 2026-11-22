import { CornerDownRight } from 'lucide-react';
import { useEffect, useMemo, useRef } from 'react';
import type { Track } from '../bridge/protocol';
import { dispatch, useDawg } from '../store/store';
import { ui, useUi } from '../store/uiStore';
import { mixerLayout, stripOrder } from './order';
import { MixerStrip } from './MixerStrip';
import { keyBelongsToFocus } from '../keys';
import { useScrollFade } from '../ui/useScrollFade';

// The mixer (docs/design/mixer.md): a strip per track in signal-flow order, a gap and the returns, and the master
// pinned at the right edge while the strips scroll sideways (X2). Clicking a strip selects its track in both windows,
// Ctrl and Shift extend the selection (X6); arrows move it, M and S mute and solo the selected strips (X10).

export function Mixer() {
  const project = useDawg((s) => s.project);
  const layout = useMemo(() => (project ? mixerLayout(project) : null), [project]);
  const scroller = useRef<HTMLDivElement>(null);
  useScrollFade(scroller);
  const primary = useUi((s) => s.selectedTrackId);

  // Keeps the selected strip in view.
  useEffect(() => {
    const strip = scroller.current?.querySelector<HTMLElement>(`[data-track="${primary}"]`);
    strip?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }, [primary]);

  useEffect(() => {
    if (!layout) return;
    const onKey = (e: KeyboardEvent) => {
      if (keyBelongsToFocus(e)) return;
      if (e.ctrlKey || e.metaKey || e.altKey) return;
      const order = stripOrder(layout);
      const { selectedTrackId, selectedTracks } = useUi.getState();
      const tracks = useDawg.getState().project?.tracks ?? {};

      if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        e.preventDefault();
        const at = selectedTrackId === null ? -1 : order.indexOf(selectedTrackId);
        const next = order[Math.min(order.length - 1, Math.max(0, at < 0 ? 0 : at + (e.key === 'ArrowLeft' ? -1 : 1)))];
        if (next !== undefined) ui.selectTrack(next);
      } else if (e.key.toLowerCase() === 'm' || e.key.toLowerCase() === 's') {
        // The master has no switches; the first selected strip decides whether they all go on or off.
        const ids = order.filter((id) => selectedTracks.has(id) && tracks[String(id)]);
        if (ids.length === 0) return;
        e.preventDefault();
        const key = e.key.toLowerCase() === 'm' ? 'mute' : 'solo';
        const on = !tracks[String(ids[0])][key];
        dispatch({
          command: 'batch',
          args: {
            label: `${on ? '' : 'Un'}${key === 'mute' ? 'mute' : 'solo'} tracks`,
            commands: ids.map((id) => ({ command: 'setTrackSwitches' as const, args: { id, [key]: on } })),
          },
        });
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [layout]);

  if (!layout) return null;

  const select = (track: Track, e: React.PointerEvent) => {
    const { selectedTrackId } = useUi.getState();
    if (e.ctrlKey || e.metaKey) ui.toggleTrack(track.id);
    else if (e.shiftKey && selectedTrackId !== null) {
      const order = stripOrder(layout);
      const [a, b] = [order.indexOf(selectedTrackId), order.indexOf(track.id)].sort((x, y) => x - y);
      if (a >= 0) ui.selectTracks(order.slice(a, b + 1), track.id);
      else ui.selectTrack(track.id);
    } else ui.selectTrack(track.id);
  };

  // Every strip keeps room for the deepest nesting's brackets, so the faders line up.
  const bracketRows = Math.max(1, ...layout.strips.map((s) => s.brackets.length));

  return (
    <div className="mixer" style={{ '--bracket-rows': bracketRows } as React.CSSProperties}>
      <div className="mixer-strips-area working-area">
        <div className="mixer-strips scroll-fade" ref={scroller}>
          {layout.strips.map(({ track, brackets }) => (
            <MixerStrip key={track.id} track={track} brackets={brackets} onSelect={select} />
          ))}
          {layout.strips.length === 0 && <div className="mixer-empty">No tracks yet</div>}
          {layout.returns.length > 0 && (
            <div className="mixer-gap" title="Returns" aria-label="Returns">
              <CornerDownRight size={12} />
            </div>
          )}
          {layout.returns.map(({ track, brackets }) => (
            <MixerStrip key={track.id} track={track} brackets={brackets} onSelect={select} />
          ))}
        </div>
      </div>
      {layout.master && (
        <div className="mixer-master">
          <MixerStrip track={layout.master} brackets={[]} onSelect={select} />
        </div>
      )}
    </div>
  );
}
