import * as bridge from '../bridge/bridge';
import type { AutomationTarget } from '../bridge/protocol';

// Tells the engine when a control is held, for automation writing in Touch and Latch modes
// (docs/design/phase10-automation.md, A7).

export function touch(trackId: number, target: AutomationTarget, touching: boolean) {
  bridge.call('automation.touch', { trackId, target, touching }).catch(() => {});
}

/** Props for a control that writes automation while it is dragged. */
export const touchProps = (trackId: number, target: AutomationTarget) => ({
  onGestureStart: () => touch(trackId, target, true),
  onGestureEnd: () => touch(trackId, target, false),
});

export const trackTarget = (kind: 'gain' | 'pan'): AutomationTarget => ({ kind, nodeId: 0, param: kind, name: '' });
export const nodeTarget = (nodeId: number, param: string): AutomationTarget => ({ kind: 'node', nodeId, param, name: '' });
