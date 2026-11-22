import { create } from 'zustand';
import * as bridge from '../bridge/bridge';
import type { PitchAnalysisReply } from '../bridge/protocol';

// A pool file's pitch analysis (docs/design/pitch-correction.md, section 3.6), fetched once per file and cached here,
// as the onsets are. The app runs it in the background the first time the Pitch view asks, and says when with
// dawg_pitchAnalysis.

const cache = new Map<number, PitchAnalysisReply>();
const pending = new Set<number>();
const waiting = new Map<string, Set<number>>();
let subscribed = false;

export const usePitchVersion = create<{ version: number }>(() => ({ version: 0 }));
const bump = () => usePitchVersion.setState((s) => ({ version: s.version + 1 }));

/** The file's analysis (or its error), or undefined while it runs (a redraw follows). */
export function getPitchAnalysis(fileId: number): PitchAnalysisReply | undefined {
  const cached = cache.get(fileId);
  if (cached || pending.has(fileId)) return cached;

  if (!subscribed) {
    subscribed = true;
    bridge.on('dawg_pitchAnalysis', ({ path }) => {
      for (const id of waiting.get(path) ?? []) pending.delete(id);
      waiting.delete(path);
      bump();
    });
  }

  pending.add(fileId);
  bridge
    .call('audio.pitchAnalysis', { fileId })
    .then((reply) => {
      if (!reply.ready) {
        waiting.set(reply.path, (waiting.get(reply.path) ?? new Set()).add(fileId));
        return;
      }
      cache.set(fileId, reply);
      pending.delete(fileId);
      bump();
    })
    .catch(() => pending.delete(fileId));
  return undefined;
}
