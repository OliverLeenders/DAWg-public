import { useEffect, useRef, useState } from 'react';
import type { SampleZone } from '../bridge/protocol';

/** How long an edit keeps showing its own value if the engine never confirms it (it may have clamped it). */
const HOLD_MS = 1000;

type Fields = Partial<SampleZone>;

const caughtUp = (zone: SampleZone, fields: Fields) =>
  Object.entries(fields).every(([key, value]) => {
    const current = zone[key as keyof SampleZone];
    return typeof value === 'number' && typeof current === 'number' ? Math.abs(current - value) < 1e-6 : current === value;
  });

/** Editing a Sampler zone from its panel, as useParam does for a node parameter: the zone shows an edit at once
    instead of waiting for the engine's round trip, and the edits go out as setSampleZone commands at most once per
    animation frame (the newest fields), so a fast drag never queues up commands. After the drag the edited fields stay
    shown until the model has caught up. `send` dispatches one command; `shown` is the zone with the edits on top. */
export function useZoneEdit(
  zone: SampleZone | undefined,
  send: (zone: SampleZone, fields: Fields, gesture?: string) => void,
): { shown: SampleZone | undefined; edit: (fields: Fields, gesture?: string) => void } {
  const [local, setLocal] = useState<{ zoneId: number; fields: Fields } | null>(null);
  const pending = useRef<{ zone: SampleZone; fields: Fields; gesture?: string } | null>(null);
  const frame = useRef(0);
  const sendRef = useRef(send);
  sendRef.current = send;

  const flush = () => {
    frame.current = 0;
    const waiting = pending.current;
    if (!waiting) return;
    pending.current = null;
    sendRef.current(waiting.zone, waiting.fields, waiting.gesture);
  };

  const edit = (fields: Fields, gesture?: string) => {
    if (!zone) return;
    setLocal((previous) => ({ zoneId: zone.id, fields: { ...(previous?.zoneId === zone.id ? previous.fields : {}), ...fields } }));
    // Another zone or another gesture goes out on its own.
    if (pending.current && (pending.current.zone.id !== zone.id || pending.current.gesture !== gesture)) flush();
    pending.current = { zone, fields: { ...(pending.current?.fields ?? {}), ...fields }, gesture };
    if (!frame.current) frame.current = requestAnimationFrame(flush);
  };

  // Drop the shown edit once the model has it (or after a while, in case it was clamped).
  useEffect(() => {
    if (!local || frame.current) return;
    if (!zone || local.zoneId !== zone.id || caughtUp(zone, local.fields)) {
      setLocal(null);
      return;
    }
    const timer = window.setTimeout(() => setLocal(null), HOLD_MS);
    return () => window.clearTimeout(timer);
  }, [zone, local]);

  // The panel can go away mid-drag: what is waiting still goes out.
  useEffect(
    () => () => {
      if (frame.current) {
        cancelAnimationFrame(frame.current);
        flush();
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );

  const shown = zone && local?.zoneId === zone.id ? { ...zone, ...local.fields } : zone;
  return { shown, edit };
}
