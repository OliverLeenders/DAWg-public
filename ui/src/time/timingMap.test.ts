import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { TimingMap } from './timingMap';

// The same cases as tests/TimeStretchTests.cpp, so the UI and the engine agree.
const beat = TICKS_PER_BEAT;

describe('TimingMap', () => {
  it('runs through its markers and on at the source tempo', () => {
    const plain = new TimingMap({ sourceBpm: 120, markers: [] }, 48000);
    expect(plain.frameAt(beat)).toBeCloseTo(24000, 9);
    expect(plain.tickAt(24000)).toBeCloseTo(beat, 9);
    expect(plain.frameAt(-beat)).toBeCloseTo(-24000, 9);
    expect(plain.rateAt(0)).toBeCloseTo(1.5625, 12);

    const marked = new TimingMap(
      {
        sourceBpm: 120,
        markers: [
          { tick: beat, frame: 12000 },
          { tick: 2 * beat, frame: 48000 },
        ],
      },
      48000,
    );
    expect(marked.frameAt(0.5 * beat)).toBeCloseTo(6000, 9);
    expect(marked.frameAt(1.5 * beat)).toBeCloseTo(30000, 9);
    expect(marked.frameAt(3 * beat)).toBeCloseTo(72000, 9);
    expect(marked.rateAt(0.5 * beat)).toBeCloseTo(12000 / beat, 12);
    expect(marked.rateAt(3 * beat)).toBeCloseTo(1.5625, 12);
    for (const tick of [0, 100, 0.99 * beat, beat, 1.7 * beat, 2 * beat, 5.5 * beat]) expect(marked.tickAt(marked.frameAt(tick))).toBeCloseTo(tick, 6);
  });
});
