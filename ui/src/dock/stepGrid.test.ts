import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT, type Clip, type StepCell, type StepSequence } from '../bridge/protocol';
import { cellAt, gridLayout, hitGrid, patternName, playingStep, shownSteps, stepHits, stepIndexAt, velocityAt } from './stepGrid';
import type { Track } from '../bridge/protocol';

// The step sequencer's grid and what its clips draw (docs/design/step-sequencer.md); stepHits mirrors
// engine/include/dawg/StepSequencer.h, which tests/MidiEffectTests.cpp checks the same way.

const sixteenth = TICKS_PER_BEAT / 4;
const on = (patch: Partial<{ velocity: number; length: number; chance: number; ratchet: number }> = {}): StepCell => [
  true,
  patch.velocity ?? 0.8,
  patch.length ?? 1,
  patch.chance ?? 1,
  patch.ratchet ?? 1,
];
const off: StepCell = [false, 0.8, 1, 1, 1];

const steps: StepSequence = {
  rows: [
    { pitch: 38, name: 'Snare' },
    { pitch: 36, name: 'Kick' },
  ],
  patterns: [
    {
      rate: 2,
      swing: 0.5,
      rows: [
        { length: 8, cells: [off, off, off, off, on()] },
        { length: 3, cells: [on(), off, on({ ratchet: 2, chance: 0.5 })] },
      ],
    },
  ],
};

const clip = (patch: Partial<Clip>): Clip =>
  ({ id: 1, start: 0, length: 16 * sixteenth, offset: 0, sequencerPattern: 0, sequencerStartStep: 0, ...patch }) as Clip;

describe('the step grid', () => {
  it('reads cells, empty past what is stored', () => {
    expect(cellAt(steps, 0, 0, 4).on).toBe(true);
    expect(cellAt(steps, 0, 0, 40).on).toBe(false);
    expect(cellAt(steps, 3, 0, 0).on).toBe(false);
    expect(shownSteps(steps, 0)).toBe(16);
    expect(patternName(2)).toBe('C');
  });

  it('finds steps and row names under the pointer, and velocity by height', () => {
    const layout = gridLayout(500, 40, 2, 16);
    expect(layout.labelWidth).toBe(70);
    expect(hitGrid(layout, 10, 5)).toEqual({ kind: 'label', row: 0 });
    expect(hitGrid(layout, 70 + 2.5 * layout.cellWidth, 25)).toEqual({ kind: 'cell', row: 1, step: 2 });
    expect(hitGrid(layout, 10, 45)).toBeNull();
    expect(velocityAt(layout, 1, 20)).toBe(1);
    expect(velocityAt(layout, 1, 30)).toBeCloseTo(0.5, 9);
  });
});

describe('what a sequencer clip plays', () => {
  it('counts steps from the clip, rows looping on their own lengths, ratchets split in the step', () => {
    const hits = stepHits(steps, clip({}), 0, 16 * sixteenth);
    const at = (row: number) => hits.filter((h) => h.row === row).map((h) => h.start / sixteenth);
    expect(at(0)).toEqual([4, 12]);
    // Kick: step 0 every 3 steps, step 2 twice in its step (by chance).
    expect(at(1)).toEqual([0, 2, 2.5, 3, 5, 5.5, 6, 8, 8.5, 9, 11, 11.5, 12, 14, 14.5, 15]);
    expect(hits.find((h) => h.start === 2 * sixteenth)?.chancy).toBe(true);
  });

  it('starts on the clip start step, and a trimmed edge keeps the steps in place', () => {
    const from4 = stepHits(steps, clip({ sequencerStartStep: 4 }), 0, 16 * sixteenth).filter((h) => h.row === 0);
    expect(from4[0].start).toBe(0);
    const trimmed = stepHits(steps, clip({ start: sixteenth, offset: sixteenth, length: 15 * sixteenth }), 0, 16 * sixteenth);
    expect(trimmed.filter((h) => h.row === 0)[0].start).toBe(4 * sixteenth);
    expect(trimmed.some((h) => h.start < sixteenth)).toBe(false);
  });
});

describe('the playhead in the grid', () => {
  it('finds the step at a tick, with swing', () => {
    expect(stepIndexAt(steps, 0, 0, 0)).toBe(0);
    expect(stepIndexAt(steps, 0, 0, 2.5 * sixteenth)).toBe(2);
    const swung: StepSequence = { ...steps, patterns: [{ ...steps.patterns[0], swing: 0.75 }] };
    // Step 1 starts at 1.5 sixteenths with 75 % swing.
    expect(stepIndexAt(swung, 0, 0, 1.4 * sixteenth)).toBe(0);
    expect(stepIndexAt(swung, 0, 0, 1.6 * sixteenth)).toBe(1);
    expect(stepIndexAt(steps, 0, 100, 50)).toBe(-1);
  });

  it('follows the sequencer clip under the playhead, from its start step', () => {
    const track = {
      clips: {
        '1': clip({ id: 1, start: 0, length: 16 * sixteenth, sequencerStartStep: 2, order: 1 }),
        '2': clip({ id: 2, start: 16 * sixteenth, length: 16 * sixteenth, sequencerPattern: 0, order: 2 }),
      },
    } as unknown as Track;
    expect(playingStep(track, steps, 3 * sixteenth)).toEqual({ pattern: 0, step: 5 });
    expect(playingStep(track, steps, 17 * sixteenth)).toEqual({ pattern: 0, step: 1 });
    expect(playingStep(track, steps, 40 * sixteenth)).toBeNull();
  });
});
