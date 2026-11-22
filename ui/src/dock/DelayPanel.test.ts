import { describe, expect, it } from 'vitest';
import { delayEchoes, noteBeats } from './DelayPanel';

describe('the delay panel', () => {
  it('reads note lengths from their names', () => {
    expect(noteBeats('1/4')).toBe(1);
    expect(noteBeats('1/8.')).toBeCloseTo(0.75);
    expect(noteBeats('1/4T')).toBeCloseTo(2 / 3);
    expect(noteBeats('4/1')).toBe(16);
  });

  it('draws each side repeating at its own time, each echo quieter', () => {
    const echoes = delayEchoes(0.1, 0.25, 0.5, false);
    const left = echoes.filter((e) => e.channel === 0);
    expect(left.slice(0, 3).map((e) => [+e.seconds.toFixed(3), e.level])).toEqual([[0.1, 1], [0.2, 0.5], [0.3, 0.25]]);
    expect(echoes.filter((e) => e.channel === 1)[1].seconds).toBeCloseTo(0.5);
  });

  it('crosses from side to side in ping-pong, starting left', () => {
    const echoes = delayEchoes(0.1, 0.2, 0.5, true);
    expect(echoes.slice(0, 4).map((e) => e.channel)).toEqual([0, 1, 0, 1]);
    expect(echoes[1].seconds).toBeCloseTo(0.3);
    expect(echoes[2].seconds).toBeCloseTo(0.4);
  });

  it('shows one echo without feedback, and ends at full feedback', () => {
    expect(delayEchoes(0.1, 0.1, 0, true)).toHaveLength(1);
    expect(delayEchoes(0.1, 0.1, 1, true).at(-1)!.seconds).toBeLessThanOrEqual(8);
  });
});
