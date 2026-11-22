import { create } from 'zustand';
import * as bridge from '../bridge/bridge';

// Waveform peaks fetched from the app (docs/design/phase7-audio-tracks.md, T10) and cached per file and mip
// level. Peaks that are still being computed arrive later (dawg_peaks); the version counter makes canvases
// redraw when anything new is in.

export interface PeakLevel {
  samplesPerPoint: number;
  channels: number;
  points: number;
  frames: number;
  sampleRate: number;
  levels: number;
  /** (min, max) per point and channel, interleaved by channel. */
  data: Int8Array;
}

export type PeakSource = { fileId: number } | { path: string };

const BASE_SAMPLES = 256;
const LEVEL_FACTOR = 4;

const cache = new Map<string, PeakLevel>();
const pending = new Set<string>();
const waiting = new Map<string, Set<string>>(); // absolute path -> keys to refetch when its peaks are ready
let subscribed = false;

export const usePeaksVersion = create<{ version: number }>(() => ({ version: 0 }));
const bump = () => usePeaksVersion.setState((s) => ({ version: s.version + 1 }));
/** Makes waveform canvases redraw (raw sample blocks arriving, timeline/samples.ts). */
export const bumpPeaksVersion = bump;

const keyOf = (source: PeakSource, level: number) =>
  `${'fileId' in source ? `f:${source.fileId}` : `p:${source.path}`}|${level}`;

function decode(base64: string) {
  const binary = atob(base64);
  const bytes = new Int8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = (binary.charCodeAt(i) << 24) >> 24;
  return bytes;
}

/** The mip level whose points are no wider than `framesPerPixel`. */
export function levelFor(framesPerPixel: number) {
  if (framesPerPixel <= BASE_SAMPLES) return 0;
  return Math.floor(Math.log(framesPerPixel / BASE_SAMPLES) / Math.log(LEVEL_FACTOR));
}

/** Cached peaks, or undefined (then they are being fetched and a redraw follows). */
export function getPeaks(source: PeakSource, level: number): PeakLevel | undefined {
  const key = keyOf(source, level);
  const cached = cache.get(key);
  if (cached || pending.has(key)) return cached;

  if (!subscribed) {
    subscribed = true;
    bridge.on('dawg_peaks', ({ path }) => {
      for (const k of waiting.get(path) ?? []) pending.delete(k);
      waiting.delete(path);
      bump();
    });
  }

  pending.add(key);
  bridge
    .call('audio.peaks', { ...source, level })
    .then((reply) => {
      if (!reply.ready || !reply.data) {
        // Being computed: fetch again when the app says it's done.
        waiting.set(reply.path, (waiting.get(reply.path) ?? new Set()).add(key));
        return;
      }
      cache.set(key, {
        samplesPerPoint: reply.samplesPerPoint!,
        channels: reply.channels!,
        points: reply.points!,
        frames: reply.frames!,
        sampleRate: reply.sampleRate!,
        levels: reply.levels!,
        data: decode(reply.data),
      });
      pending.delete(key);
      bump();
    })
    .catch(() => pending.delete(key));

  return undefined;
}

/** Largest |sample| (0..127) in [frame, frame + span) of a level, over all channels; also the signed range. */
export function rangeAt(peaks: PeakLevel, frame: number, span: number): [number, number] {
  const p0 = Math.max(0, Math.floor(frame / peaks.samplesPerPoint));
  const p1 = Math.min(peaks.points, Math.max(p0 + 1, Math.ceil((frame + span) / peaks.samplesPerPoint)));
  let lo = 0;
  let hi = 0;

  for (let p = p0; p < p1; p++) {
    for (let ch = 0; ch < peaks.channels; ch++) {
      const i = (p * peaks.channels + ch) * 2;
      lo = Math.min(lo, peaks.data[i]);
      hi = Math.max(hi, peaks.data[i + 1]);
    }
  }

  return [lo, hi];
}
