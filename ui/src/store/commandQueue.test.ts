import { describe, expect, it } from 'vitest';
import type { Command } from '../bridge/protocol';
import { coalesceKey, createCommandQueue } from './commandQueue';

/** A queue whose sends record their label and stay out until released. */
function harness() {
  const queue = createCommandQueue();
  const sent: string[] = [];
  const replies: Array<() => void> = [];
  const push = (label: string, key?: string, gesture?: string) =>
    queue.push(
      () => {
        sent.push(label);
        return new Promise<void>((resolve) => replies.push(resolve));
      },
      key,
      gesture,
    );
  const reply = async () => {
    replies.shift()?.();
    await new Promise((r) => setTimeout(r, 0));
  };
  return { queue, sent, push, reply };
}

const gain = (gainDb: number): Command => ({ command: 'setTrackGain', args: { id: 1, gainDb } });
const param = (param: string, value: number): Command => ({
  command: 'setNodeParam',
  args: { trackId: 1, nodeId: 2, param, value },
});
const modulation = (depth: number): Command => ({
  command: 'setModulation',
  args: { trackId: 1, nodeId: 2, port: 8, param: 'amplitude', depth },
});

describe('coalesceKey', () => {
  it('names the target of a value command in a gesture', () => {
    expect(coalesceKey(gain(-1), 'g')).toBe(coalesceKey(gain(-6), 'g'));
    expect(coalesceKey(param('a', 0), 'g')).not.toBe(coalesceKey(param('b', 0), 'g'));
    expect(coalesceKey(modulation(-0.2), 'g')).toBe(coalesceKey(modulation(-0.8), 'g'));
    expect(coalesceKey(gain(-1), 'g')).not.toBe(coalesceKey(gain(-1), 'h'));
  });

  it('names the zone and fields of a zone edit, so a drag only ever waits on its newest value', () => {
    const zone = (fields: Record<string, number>, zoneId = 3) =>
      ({ command: 'setSampleZone', args: { trackId: 1, nodeId: 2, zoneId, ...fields } }) as unknown as Command;
    expect(coalesceKey(zone({ attackMs: 10 }), 'g')).toBe(coalesceKey(zone({ attackMs: 90 }), 'g'));
    expect(coalesceKey(zone({ decayMs: 1, sustain: 0.2 }), 'g')).toBe(coalesceKey(zone({ sustain: 0.5, decayMs: 4 }), 'g'));
    expect(coalesceKey(zone({ attackMs: 10 }), 'g')).not.toBe(coalesceKey(zone({ decayMs: 10 }), 'g'));
    expect(coalesceKey(zone({ attackMs: 10 }), 'g')).not.toBe(coalesceKey(zone({ attackMs: 10 }, 4), 'g'));
    expect(coalesceKey(zone({ attackMs: 10 }))).toBeUndefined();
  });

  it('leaves other commands and edits outside a gesture alone', () => {
    expect(coalesceKey(gain(-1))).toBeUndefined();
    expect(coalesceKey({ command: 'renameTrack', args: { id: 1, name: 'x' } } as Command, 'g')).toBeUndefined();
  });
});

describe('createCommandQueue', () => {
  it('sends one at a time, in order', async () => {
    const h = harness();
    h.push('a');
    h.push('b');
    expect(h.sent).toEqual(['a']);
    await h.reply();
    expect(h.sent).toEqual(['a', 'b']);
    await h.reply();
    h.push('c');
    expect(h.sent).toEqual(['a', 'b', 'c']);
  });

  it('keeps only the newest waiting value for a target', async () => {
    const h = harness();
    h.push('gain 1', 'k', 'g');
    h.push('gain 2', 'k', 'g');
    h.push('gain 3', 'k', 'g');
    expect(h.queue.pending).toBe(1);
    await h.reply();
    expect(h.sent).toEqual(['gain 1', 'gain 3']);
  });

  it('replaces across other values of the same gesture, not across other commands', async () => {
    const h = harness();
    h.push('first');
    h.push('a 1', 'a', 'g');
    h.push('b 1', 'b', 'g');
    h.push('a 2', 'a', 'g');
    expect(h.queue.pending).toBe(2);
    h.push('undo');
    h.push('a 3', 'a', 'g');
    expect(h.queue.pending).toBe(4);
    for (let i = 0; i < 5; i++) await h.reply();
    expect(h.sent).toEqual(['first', 'a 2', 'b 1', 'undo', 'a 3']);
  });
});

describe('coalesceKey with several value arguments', () => {
  it('replaces a tempo point drag by point, whatever its tick and BPM', () => {
    const a = coalesceKey({ command: 'setTempoPoint', args: { index: 2, tick: 100, bpm: 120 } }, 'g');
    const b = coalesceKey({ command: 'setTempoPoint', args: { index: 2, tick: 900, bpm: 96 } }, 'g');
    const other = coalesceKey({ command: 'setTempoPoint', args: { index: 3, tick: 900, bpm: 96 } }, 'g');
    expect(a).toBe(b);
    expect(other).not.toBe(a);
  });
});
