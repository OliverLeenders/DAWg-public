import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { TempoMap } from './tempoMap';

// The same cases as tests/TempoMapTests.cpp, so the UI and the engine agree.
const beat = TICKS_PER_BEAT;

describe('TempoMap', () => {
  it('converts over one tempo, a step and a ramp', () => {
    expect(new TempoMap().secondsAt(4 * beat)).toBeCloseTo(2, 12);

    const step = new TempoMap([
      { tick: 0, bpm: 120, ramp: false },
      { tick: 4 * beat, bpm: 60, ramp: false },
    ]);
    expect(step.secondsAt(6 * beat)).toBeCloseTo(4, 12);
    expect(step.tickAt(4)).toBeCloseTo(6 * beat, 6);

    const ramp = new TempoMap([
      { tick: 0, bpm: 60, ramp: true },
      { tick: 4 * beat, bpm: 120, ramp: false },
    ]);
    expect(ramp.bpmAt(2 * beat)).toBeCloseTo(90, 9);
    expect(ramp.secondsAt(4 * beat)).toBeCloseTo(4 * Math.log(2), 9);
    expect(ramp.secondsAt(5 * beat)).toBeCloseTo(4 * Math.log(2) + 0.5, 9);
    for (const tick of [0, 1000, 1.5 * beat, 3.99 * beat, 4 * beat, 9 * beat]) expect(ramp.tickAt(ramp.secondsAt(tick))).toBeCloseTo(tick, 6);
  });

  it('places bars across signature changes', () => {
    const map = new TempoMap(
      [],
      [
        { bar: 0, numerator: 4, denominator: 4 },
        { bar: 2, numerator: 7, denominator: 8 },
        { bar: 4, numerator: 6, denominator: 8 },
      ],
    );
    expect(map.tickOfBar(2)).toBe(8 * beat);
    expect(map.tickOfBar(3)).toBe(8 * beat + (7 * beat) / 2);
    expect(map.tickOfBar(4)).toBe(15 * beat);
    expect(map.tickOfBar(5)).toBe(18 * beat);
    expect(map.tickOfBar(-1)).toBe(-4 * beat);
    expect(map.barAt(8 * beat + (7 * beat) / 2 + 2.5 * beat)).toEqual({ bar: 3, beat: 5, tickInBeat: 0 });
    expect(map.signatureAt(16 * beat).numerator).toBe(6);
    expect(map.barAt(-1).bar).toBe(-1);
  });
});

describe('TempoMap.snap', () => {
  const map = new TempoMap(
    [],
    [
      { bar: 0, numerator: 7, denominator: 8 },
      { bar: 1, numerator: 4, denominator: 4 },
    ],
  );

  it('restarts the grid at every bar line', () => {
    // Bar 1 starts at 3.5 beats: a quarter grid there runs 3.5, 4.5, 5.5 ...
    expect(map.snap(4.4 * beat, beat, true)).toBe(4.5 * beat);
    expect(map.snap(4.4 * beat, beat)).toBe(3.5 * beat);
    // Near the end of the 7/8 bar the bar line is closer than the grid's last line.
    expect(map.snap(3.4 * beat, beat, true)).toBe(3.5 * beat);
  });

  it('snaps to bar lines with a grid of a bar or more', () => {
    expect(map.snap(2 * beat, 1000 * beat, true)).toBe(3.5 * beat);
    expect(map.snap(1 * beat, 1000 * beat, true)).toBe(0);
    expect(map.barFloor(5 * beat)).toBe(3.5 * beat);
    expect(map.barCeil(5 * beat)).toBe(7.5 * beat);
    expect(map.barCeil(3.5 * beat)).toBe(3.5 * beat);
  });
});
