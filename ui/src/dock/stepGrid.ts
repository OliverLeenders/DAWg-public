import { NODE_TYPES, STEP_RATE_PER_BEAT, TICKS_PER_BEAT, type Clip, type GraphNode, type StepCell, type StepSequence, type Track } from '../bridge/protocol';
import { swungLine } from '../time/swing';

// The step sequencer's grid (docs/design/step-sequencer.md, SS9): what a cell holds and where it is on the panel.

/** A step as the panel uses it. */
export interface Cell {
  on: boolean;
  velocity: number;
  length: number;
  chance: number;
  ratchet: number;
}

export const EMPTY_CELL: Cell = { on: false, velocity: 0.8, length: 1, chance: 1, ratchet: 1 };

/** A cell of a pattern's row; steps past the row's stored ones are empty. */
export function cellAt(steps: StepSequence, pattern: number, row: number, step: number): Cell {
  const stored: StepCell | undefined = steps.patterns[pattern]?.rows[row]?.cells[step];
  if (!stored) return EMPTY_CELL;
  const [on, velocity, length, chance, ratchet] = stored;
  return { on, velocity, length, chance, ratchet };
}

/** How many steps the grid shows for a pattern: its longest row, at least 16 (and at most 64). */
export function shownSteps(steps: StepSequence, pattern: number) {
  const lengths = steps.patterns[pattern]?.rows.map((r) => r.length) ?? [];
  return Math.min(64, Math.max(16, ...lengths));
}

/** The grid's layout in the panel's canvas: a column of row names, then the steps. */
export interface GridLayout {
  labelWidth: number;
  cellWidth: number;
  rowHeight: number;
  rows: number;
  steps: number;
}

export function gridLayout(width: number, height: number, rows: number, steps: number): GridLayout {
  const labelWidth = Math.min(72, Math.max(40, width * 0.14));
  return {
    labelWidth,
    cellWidth: Math.max(1, (width - labelWidth) / Math.max(1, steps)),
    rowHeight: Math.max(1, height / Math.max(1, rows)),
    rows,
    steps,
  };
}

/** What is under a point: a step of a row, a row's name, or nothing. */
export type GridHit = { kind: 'cell'; row: number; step: number } | { kind: 'label'; row: number } | null;

export function hitGrid(layout: GridLayout, x: number, y: number): GridHit {
  const row = Math.floor(y / layout.rowHeight);
  if (row < 0 || row >= layout.rows) return null;
  if (x < layout.labelWidth) return x >= 0 ? { kind: 'label', row } : null;
  const step = Math.floor((x - layout.labelWidth) / layout.cellWidth);
  return step >= 0 && step < layout.steps ? { kind: 'cell', row, step } : null;
}

/** A velocity from a pointer's height in a cell: the top is full, the bottom the softest. */
export const velocityAt = (layout: GridLayout, row: number, y: number) =>
  Math.min(1, Math.max(0.05, 1 - (y - row * layout.rowHeight) / layout.rowHeight));

/**
 * The step a pattern is on at a tick, counted from `origin` (a clip's content origin, or where a preview started), with
 * the pattern's swing: the k whose step line is at or before the tick.
 */
export function stepIndexAt(steps: StepSequence, pattern: number, origin: number, tick: number) {
  const p = steps.patterns[pattern];
  if (!p || tick < origin) return -1;
  const stepTicks = TICKS_PER_BEAT / (STEP_RATE_PER_BEAT[p.rate] ?? 4);
  let k = Math.floor((tick - origin) / stepTicks);
  while (k > 0 && origin + swungLine(k, stepTicks, p.swing) > tick) k--;
  return k;
}

/** Where the playhead is in a sequencer clip of the track: the clip's pattern and step count, or null outside them. */
export function playingStep(track: Track, steps: StepSequence, tick: number): { pattern: number; step: number } | null {
  const clip = Object.values(track.clips)
    .filter((c) => c.sequencerPattern >= 0 && tick >= c.start && tick < c.start + c.length)
    .sort((a, b) => b.order - a.order)[0];
  if (!clip) return null;
  const k = stepIndexAt(steps, clip.sequencerPattern, clip.start - clip.offset, tick);
  return k < 0 ? null : { pattern: clip.sequencerPattern, step: k + clip.sequencerStartStep };
}

/** The pattern names: A to P. */
export const patternName = (index: number) => String.fromCharCode(65 + index);

/** The track's step sequencer, if it has one (one per track, SS1). */
export const sequencerOf = (track: Track): GraphNode | undefined =>
  Object.values(track.graph.nodes).find((n) => n.type === NODE_TYPES.stepSequencer);

/** A hit as the arrangement draws it: where, which row, and whether it only plays by chance. */
export interface StepHit {
  start: number;
  end: number;
  row: number;
  chancy: boolean;
}

/**
 * The hits a sequencer clip plays between two ticks, as engine/include/dawg/StepSequencer.h works them out, for drawing:
 * steps with a chance below 100 % are all included, marked `chancy`.
 */
export function stepHits(steps: StepSequence, clip: Clip, from: number, to: number): StepHit[] {
  const pattern = steps.patterns[clip.sequencerPattern];
  if (!pattern) return [];
  const stepTicks = TICKS_PER_BEAT / (STEP_RATE_PER_BEAT[pattern.rate] ?? 4);
  const origin = clip.start - clip.offset;
  const end = clip.start + clip.length;
  from = Math.max(from, clip.start);
  to = Math.min(to, end);
  const hits: StepHit[] = [];
  if (to <= from) return hits;
  const line = (k: number) => origin + swungLine(k, stepTicks, pattern.swing);

  for (let k = Math.max(0, Math.floor((from - origin) / stepTicks) - 1); line(k) < to; k++) {
    const at = line(k);
    const next = line(k + 1);
    pattern.rows.forEach((row, r) => {
      if (row.length <= 0) return;
      const cell = cellAt(steps, clip.sequencerPattern, r, (k + clip.sequencerStartStep) % row.length);
      if (!cell.on) return;
      const each = (next - at) / cell.ratchet;
      for (let h = 0; h < cell.ratchet; h++) {
        const start = at + each * h;
        if (start < from || start >= to) continue;
        const length = cell.ratchet > 1 ? each * Math.min(1, cell.length) : stepTicks * cell.length;
        hits.push({ start, end: Math.min(start + Math.max(1, length), end), row: r, chancy: cell.chance < 1 });
      }
    });
  }
  return hits;
}
