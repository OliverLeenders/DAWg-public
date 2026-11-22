import { describe, expect, it } from 'vitest';
import { LevelHistory } from './levelHistory';

describe('LevelHistory', () => {
  it('keeps the newest slots up to its capacity, newest first by age', () => {
    const history = new LevelHistory(0.03);
    history.push([-10, -11, -1, -12, -20, -21, -2, -22], 0);
    expect(history.length).toBe(2);
    expect(history.at(0)).toEqual([-20, -21, -2, -22]);
    history.push([-30, -31, -3, -32, -40, -41, -4, -42], 10);
    expect(history.length).toBe(3);
    expect(history.at(0)).toEqual([-40, -41, -4, -42]);
    expect(history.at(2)).toEqual([-20, -21, -2, -22]);
  });

  it('adds silence at the slot rate once slots stop coming', () => {
    const history = new LevelHistory();
    history.push([-10, -10, -3, -14], 1000);
    expect(history.pad(1050)).toBe(false);
    expect(history.pad(1200)).toBe(true);
    expect(history.length).toBe(21);
    expect(history.at(0)).toEqual([-180, -180, 0, -180]);
    expect(history.at(20)).toEqual([-10, -10, -3, -14]);

    // The clock moves on by whole slots, so the next pad adds only the time since.
    history.pad(1305);
    expect(history.length).toBe(31);
  });
});
