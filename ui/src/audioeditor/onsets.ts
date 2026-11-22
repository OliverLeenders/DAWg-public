import { create } from 'zustand';
import * as bridge from '../bridge/bridge';

// A pool file's onsets (docs/design/time-stretching.md, TS11), fetched once per file and cached here. The app finds
// them in the background and says when with dawg_onsets.

export type OnsetList = ReadonlyArray<readonly [number, number]>;

const cache = new Map<number, OnsetList>();
const pending = new Set<number>();
const waiting = new Map<string, Set<number>>();
let subscribed = false;

export const useOnsetsVersion = create<{ version: number }>(() => ({ version: 0 }));
const bump = () => useOnsetsVersion.setState((s) => ({ version: s.version + 1 }));

/** The file's onsets as [frame, strength], or undefined while they are found (a redraw follows). */
export function getOnsets(fileId: number): OnsetList | undefined {
  const cached = cache.get(fileId);
  if (cached || pending.has(fileId)) return cached;

  if (!subscribed) {
    subscribed = true;
    bridge.on('dawg_onsets', ({ path }) => {
      for (const id of waiting.get(path) ?? []) pending.delete(id);
      waiting.delete(path);
      bump();
    });
  }

  pending.add(fileId);
  bridge
    .call('audio.onsets', { fileId })
    .then((reply) => {
      if (!reply.ready || !reply.onsets) {
        waiting.set(reply.path, (waiting.get(reply.path) ?? new Set()).add(fileId));
        return;
      }
      cache.set(fileId, reply.onsets);
      pending.delete(fileId);
      bump();
    })
    .catch(() => pending.delete(fileId));
  return undefined;
}
