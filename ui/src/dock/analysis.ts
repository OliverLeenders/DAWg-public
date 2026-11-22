import { useEffect, useRef, useState } from 'react';
import { create } from 'zustand';
import * as bridge from '../bridge/bridge';
import type { Command, NodeAnalysis, ResponseCurve } from '../bridge/protocol';
import { touch, nodeTarget } from '../automation/touch';
import { dispatch, newGestureId } from '../store/store';
import { mapMoves } from './assign';
import { useModulationUi } from './modulationStore';
import { WINDOW } from '../window';

// Data for the device panels (docs/design/phase11-faust-panels.md, P1-P3): the analysis of the watched node, response
// curves from the engine, and multi-parameter drags.

/** The latest analysis of each watched node, keyed "<trackId>:<nodeId>". */
export const useAnalysis = create<{ byKey: Record<string, NodeAnalysis> }>()(() => ({ byKey: {} }));

let listening = false;

/** Runs the analysers of some nodes while the caller is mounted (the device strip), and none otherwise
    (docs/design/device-strip.md, D8). A node that stays watched keeps its analysis when others come and go, so a
    spectrum doesn't blank while the strip scrolls. The app keeps a list per window and runs all of them, so the
    instrument window's panel keeps its analysis while the main window's dock shows other devices. */
export function useWatchNodes(nodes: Array<{ trackId: number; nodeId: number }>) {
  const key = nodes.map((n) => `${n.trackId}:${n.nodeId}`).join(',');
  useEffect(() => {
    if (!bridge.isConnected()) return;
    if (!listening) {
      listening = true;
      bridge.on('dawg_analysis', (analysis) => useAnalysis.setState((s) => ({ byKey: { ...s.byKey, [analysis.key]: analysis } })));
    }
    bridge.call('nodes.watch', { window: WINDOW, nodes }).catch(() => {});
    const watched = new Set(key.split(','));
    useAnalysis.setState((s) => ({ byKey: Object.fromEntries(Object.entries(s.byKey).filter(([k]) => watched.has(k))) }));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key]);
  useEffect(
    () => () => {
      if (bridge.isConnected()) bridge.call('nodes.watch', { window: WINDOW, nodes: [] }).catch(() => {});
      useAnalysis.setState({ byKey: {} });
    },
    [],
  );
}

/** The latest analysis of a node, or null. */
export function useNodeAnalysis(trackId: number, nodeId: number) {
  return useAnalysis((s) => s.byKey[`${trackId}:${nodeId}`] ?? null);
}

/** A built-in effect's response curve for parameter values, from the engine (P1). While a request is out, only the
    newest values are asked for next, so dragging never queues up requests. */
export function useResponse(type: string, params: Record<string, number>) {
  const [curve, setCurve] = useState<ResponseCurve | null>(null);
  const key = JSON.stringify(params);
  const state = useRef<{ busy: boolean; next: string | null }>({ busy: false, next: null });

  useEffect(() => {
    if (!bridge.isConnected()) return;
    const request = (wanted: string) => {
      state.current.busy = true;
      bridge
        .call('nodes.response', { type, params: JSON.parse(wanted) as Record<string, number> })
        .then(setCurve)
        .catch(() => {})
        .finally(() => {
          state.current.busy = false;
          const next = state.current.next;
          state.current.next = null;
          if (next !== null && next !== wanted) request(next);
        });
    };
    if (state.current.busy) state.current.next = key;
    else request(key);
  }, [type, key]);

  return curve;
}

/** How long a panel keeps showing a dragged value if the engine never confirms it. */
const DRAG_HOLD_MS = 1000;

/** Edits several parameters of a node in one drag: one undo step, with the automation lanes touched (P3).

    While dragging, the values are shown at once instead of waiting for the engine's round trip, and at most one
    batch of setNodeParam commands goes out per animation frame (the newest values), so a handle follows the pointer
    and a fast drag never queues up commands. After the drag it keeps showing the values until the model has caught
    up, like useParam() does for a single control. */
export function useNodeDrag(trackId: number, nodeId: number, model: Record<string, number>) {
  const gesture = useRef(newGestureId());
  const touched = useRef<string[]>([]);
  const [local, setLocal] = useState<Record<string, number>>({});
  const [releases, setReleases] = useState(0);
  const pending = useRef<Record<string, number>>({});
  const frame = useRef(0);
  const dragging = useRef(false);
  const lastSent = useRef<Record<string, number>>({});
  // Map mode (docs/design/macros-and-modulation-column.md, MC9): a drag maps what it moves instead of changing it.
  const mapping = useModulationUi((s) => (s.mapping?.trackId === trackId ? s.mapping.knob : null));
  const mapStart = useRef<Record<string, number | undefined> | null>(null);
  const values = { ...model, ...local };

  const flush = () => {
    frame.current = 0;
    const values = pending.current;
    pending.current = {};

    const entries = Object.entries(values);

    if (entries.length === 0) return;

    lastSent.current = { ...lastSent.current, ...values };

    const commands: Command[] = entries.map(([param, value]) => ({
      command: 'setNodeParam',
      args: { trackId, nodeId, param, value },
    }));
    const command: Command =
      commands.length === 1 ? commands[0] : { command: 'batch', args: { label: 'Change parameters', commands } };
    // Keyed so a batch still waiting for the engine is replaced by the newer one, not sent after it.
    dispatch(
      command,
      gesture.current,
      `node:${trackId}:${nodeId}:${entries.map(([param]) => param).sort().join(',')}`,
    );
  };

  // Drop a local value once the model has the last value sent (or after a while, in case it was clamped).
  useEffect(() => {
    if (Object.keys(local).length === 0 || dragging.current || frame.current) return;

    const remaining: Record<string, number> = {};
    let confirmed = false;

    for (const [param, value] of Object.entries(local)) {
      const sent = lastSent.current[param];
      if (sent !== undefined && Math.abs((model[param] ?? sent) - sent) < 1e-6) confirmed = true;
      else remaining[param] = value;
    }

    if (confirmed) setLocal(remaining);
    if (Object.keys(remaining).length === 0) return;

    const timer = window.setTimeout(() => setLocal({}), DRAG_HOLD_MS);
    return () => window.clearTimeout(timer);
  }, [model, local, releases]);

  useEffect(
    () => () => {
      if (frame.current) {
        cancelAnimationFrame(frame.current);
        frame.current = 0;
        flush(); // a drag that ended at the same moment as the panel does not lose its last values
      }

      // The panel is going away: a drag that never ended must not leave its lanes touched (A7).
      for (const param of touched.current) touch(trackId, nodeTarget(nodeId, param), false);
    },
    [trackId, nodeId],
  );

  return {
    /** The values to draw: the model's, with the values of a drag not yet confirmed on top. */
    values,
    /** Starts a drag: one gesture until end(). */
    begin: (params: string[]) => {
      if (mapping !== null) {
        mapStart.current = Object.fromEntries(params.map((param) => [param, values[param]]));
        dragging.current = true;
        return;
      }
      // A pointerup may have been missed: send anything still waiting under the old gesture, and stop touching it.
      for (const param of touched.current) touch(trackId, nodeTarget(nodeId, param), false);

      if (frame.current) {
        cancelAnimationFrame(frame.current);
        frame.current = 0;
        flush();
      }
      gesture.current = newGestureId();
      touched.current = params;
      dragging.current = true;
      for (const param of params) touch(trackId, nodeTarget(nodeId, param), true);
    },
    /** Sets values as their own step: shown at once, sent now. */
    set: (next: Record<string, number>) => {
      if (mapping !== null) {
        if (mapStart.current) setLocal((current) => ({ ...current, ...next }));
        else mapMoves(trackId, nodeId, Object.fromEntries(Object.entries(next).map(([param, to]) => [param, [values[param], to]])), mapping);
        return;
      }
      const values = next;
      // Between begin() and end() this is part of the drag's step. On its own (a double-click, a button) it is a
      // step of its own, so a new gesture keeps it from merging with the edit before it.
      if (!dragging.current) gesture.current = newGestureId();

      setLocal((current) => ({ ...current, ...values }));
      pending.current = { ...pending.current, ...values };
      if (frame.current) {
        cancelAnimationFrame(frame.current);
        frame.current = 0;
      }
      flush();
    },
    /** Sets values within the current drag: shown at once, sent with the next frame. */
    drag: (values: Record<string, number>) => {
      setLocal((current) => ({ ...current, ...values }));
      if (mapStart.current) return;
      pending.current = { ...pending.current, ...values };
      if (!frame.current) frame.current = requestAnimationFrame(flush);
    },
    end: () => {
      dragging.current = false;
      if (mapStart.current) {
        const start = mapStart.current;
        mapStart.current = null;
        if (mapping !== null)
          mapMoves(trackId, nodeId, Object.fromEntries(Object.entries(start).map(([param, from]) => [param, [from, local[param] ?? from]])), mapping);
        setLocal({});
        return;
      }
      if (frame.current) {
        cancelAnimationFrame(frame.current);
        frame.current = 0;
        flush();
      }
      for (const param of touched.current) touch(trackId, nodeTarget(nodeId, param), false);
      touched.current = [];
      setReleases((r) => r + 1); // re-runs the catch-up check now that the drag is over
    },
  };
}
