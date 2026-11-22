import { useEffect, useRef, useState } from 'react';
import type { GraphNode, ParamDescriptor, Track } from '../bridge/protocol';
import { laneFor, playedPosition } from '../automation/automation';
import { nodeTarget, touch } from '../automation/touch';
import { fromPosition } from '../graph/params';
import { NODE_TYPES } from '../bridge/protocol';
import { dispatch, newGestureId, useDawg } from '../store/store';
import { addLink, drivenValue, endOf, sameAddress, trackOf } from './links';
import { macrosOf, mappedDepth, neutralOf } from './macros';
import { modulationUi, useModulationUi } from './modulationStore';

/** How long a control keeps showing its own value after a drag if the engine never confirms it. */
const HOLD_MS = 1000;

/** Editing one node parameter from a control: its shown value (following automation while it plays), and changes as
    setNodeParam commands where a drag is one undo step and touches the automation lane (docs/design/
    phase11-faust-panels.md, P3).

    While dragging, the control shows its own value at once instead of waiting for the engine's round trip, and sends
    at most one command per animation frame (the newest value), so a fast drag never queues up commands. After the
    drag it keeps that value until the model has caught up. */
export function useParam(track: Track, node: GraphNode, param: ParamDescriptor) {
  const target = nodeTarget(node.id, param.id);
  const played = useDawg((s) =>
    track.automationMode === 0 ? undefined : playedPosition(s.telemetry.automation, laneFor(track, target)?.id),
  );
  // A parameter that follows a link shows its source through the mapping, its automation included
  // (docs/design/parameter-linking.md, L6, L16).
  const driven = useDawg((s) =>
    drivenValue(s.project, { trackId: track.id, nodeId: node.id, param: param.id }, (address, sourceParam) => {
      const sourceTrack = trackOf(s.project, address.trackId);
      const source = endOf(s.project, address)?.node;
      const lane = sourceTrack && sourceTrack.automationMode !== 0 ? laneFor(sourceTrack, nodeTarget(address.nodeId, address.param)) : undefined;
      const position = lane ? playedPosition(s.telemetry.automation, lane.id) : undefined;
      return position !== undefined ? fromPosition(sourceParam, position) : (source?.params[address.param] ?? sourceParam.default);
    }),
  );
  const model = driven ?? node.params[param.id] ?? param.default;
  const [local, setLocal] = useState<number | null>(null);
  const [releases, setReleases] = useState(0);
  const gesture = useRef(newGestureId());
  const pending = useRef<number | null>(null);
  const frame = useRef(0);
  const dragging = useRef(false);
  const lastSent = useRef<number | null>(null);
  // Map mode (docs/design/macros-and-modulation-column.md, MC9): a move maps the parameter instead of changing it.
  const mapping = useModulationUi((s) =>
    s.mapping?.trackId === track.id && param.modulatable && node.type !== NODE_TYPES.macros ? s.mapping : null,
  );
  const mapStart = useRef<number | null>(null);
  // Link mode (L15): the next parameter moved follows the one it started on, and isn't changed.
  const linking = useModulationUi((s) => (s.linking && !sameAddress(s.linking, { trackId: track.id, nodeId: node.id, param: param.id }) ? s.linking : null));
  const linked = useRef(false);
  const link = () => {
    if (!linking) return false;
    modulationUi.stopLinking();
    addLink(linking, { trackId: track.id, nodeId: node.id, param: param.id });
    return true;
  };

  const send = () => {
    frame.current = 0;
    if (pending.current === null) return;
    lastSent.current = pending.current;
    dispatch(
      { command: 'setNodeParam', args: { trackId: track.id, nodeId: node.id, param: param.id, value: pending.current } },
      gesture.current,
    );
    pending.current = null;
  };

  // Drop the local value once the model has the last value sent (or after a while, in case it was clamped).
  useEffect(() => {
    if (local === null || dragging.current || frame.current) return;
    if (lastSent.current !== null && Math.abs(model - lastSent.current) < 1e-6) {
      setLocal(null);
      return;
    }
    const timer = window.setTimeout(() => setLocal(null), HOLD_MS);
    return () => window.clearTimeout(timer);
  }, [model, local, releases]);

  // The control can go away mid-drag (another node, another panel): send what is waiting and stop touching the lane,
  // so automation does not keep writing for a control nobody holds (A7).
  useEffect(
    () => () => {
      if (frame.current) {
        cancelAnimationFrame(frame.current);
        send();
      }

      if (dragging.current) {
        dragging.current = false;
        touch(track.id, nodeTarget(node.id, param.id), false);
      }
    },
    [track.id, node.id, param.id],
  );

  const value = local ?? (played !== undefined ? fromPosition(param, played) : model);

  return {
    value,
    /** Whether Map mode would map this parameter rather than change it. */
    mapping: mapping !== null,
    /** Whether Link mode would make this parameter follow another rather than change it. */
    linking: linking !== null,
    /** Starts a drag: one gesture until end(). */
    begin: () => {
      gesture.current = newGestureId();
      dragging.current = true;
      linked.current = link();
      if (linked.current) return;
      if (mapping) {
        mapStart.current = value;
        return;
      }
      touch(track.id, target, true);
    },
    end: () => {
      dragging.current = false;
      if (linked.current) {
        linked.current = false;
        setLocal(null);
        return;
      }
      if (mapStart.current !== null) {
        const from = mapStart.current;
        const to = local ?? from;
        mapStart.current = null;
        setLocal(null);
        if (mapping && Math.abs(to - from) > 1e-9) {
          const neutral = neutralOf(macrosOf(track), mapping.knob);
          dispatch({
            command: 'mapMacro',
            args: { trackId: track.id, nodeId: node.id, param: param.id, macro: mapping.knob, depth: mappedDepth(param, from, to, neutral) },
          });
        }
        return;
      }
      if (frame.current) {
        cancelAnimationFrame(frame.current);
        send();
      }
      touch(track.id, target, false);
      setReleases((r) => r + 1); // re-runs the catch-up check now that the drag is over
    },
    /** Sets the value within the current drag: shown at once, sent with the next frame. */
    drag: (next: number) => {
      if (linked.current) return;
      setLocal(next);
      if (mapStart.current !== null) return;
      pending.current = next;
      if (!frame.current) frame.current = requestAnimationFrame(send);
    },
    /** Sets the value as its own step. */
    set: (next: number) => {
      if (link()) return;
      if (mapping) {
        // A one-off change (a reset, a typed value) maps at the depth of that change too.
        const neutral = neutralOf(macrosOf(track), mapping.knob);
        const depth = mappedDepth(param, value, next, neutral);
        if (Math.abs(depth) > 1e-9)
          dispatch({ command: 'mapMacro', args: { trackId: track.id, nodeId: node.id, param: param.id, macro: mapping.knob, depth } });
        return;
      }
      setLocal(next);
      lastSent.current = next;
      dispatch({ command: 'setNodeParam', args: { trackId: track.id, nodeId: node.id, param: param.id, value: next } }, newGestureId());
    },
  };
}
