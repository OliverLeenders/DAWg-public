import * as bridge from '../bridge/bridge';
import { bumpPeaksVersion, type PeakSource } from './peaks';

// A file's raw frames (the mono mid), a pool file's or one on disk (the sound browser's), fetched in blocks for drawing the wave when zoomed in past the finest
// peaks. Blocks arrive asynchronously; the peaks version counter makes canvases redraw when one is in.

export const SAMPLE_BLOCK = 32768;
const MAX_BLOCKS = 64; // about 8 MB of floats

const cache = new Map<string, Float32Array>();
const pending = new Set<string>();

function decode(base64: string) {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return new Float32Array(bytes.buffer);
}

/** One block of a file's frames (block b covers frames b * SAMPLE_BLOCK onwards), or undefined while it is
    fetched (a redraw follows). */
export function getSampleBlock(source: PeakSource, block: number): Float32Array | undefined {
  const key = `${'fileId' in source ? `f:${source.fileId}` : `p:${source.path}`}|${block}`;
  const cached = cache.get(key);
  if (cached) {
    // Most recently used last.
    cache.delete(key);
    cache.set(key, cached);
    return cached;
  }
  if (pending.has(key)) return undefined;

  pending.add(key);
  bridge
    .call('audio.samples', { ...source, start: block * SAMPLE_BLOCK, count: SAMPLE_BLOCK })
    .then((reply) => {
      pending.delete(key);
      if (!reply?.data) return;
      cache.set(key, decode(reply.data));
      while (cache.size > MAX_BLOCKS) cache.delete(cache.keys().next().value!);
      bumpPeaksVersion();
    })
    .catch(() => pending.delete(key));
  return undefined;
}

/** The frames [first, last] of a file, if all their blocks are in; also starts fetching missing ones. */
export function sampleReader(source: PeakSource, first: number, last: number) {
  const blocks = new Map<number, Float32Array>();
  let complete = true;
  for (let b = Math.floor(Math.max(0, first) / SAMPLE_BLOCK); b <= Math.floor(Math.max(0, last) / SAMPLE_BLOCK); b++) {
    const block = getSampleBlock(source, b);
    if (block) blocks.set(b, block);
    else complete = false;
  }
  if (!complete) return null;
  return (frame: number) => {
    if (frame < 0) return 0;
    const block = blocks.get(Math.floor(frame / SAMPLE_BLOCK));
    return block ? (block[frame % SAMPLE_BLOCK] ?? 0) : 0;
  };
}
