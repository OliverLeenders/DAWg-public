import type { EditNote } from './edit';

// How note drags meet the grid. The edge the pointer holds (the grabbed note's start for a move or a start resize,
// its end for an end resize) snaps to absolute grid lines, as in other DAWs: an off-grid note dragged by its end lands
// on a grid line, not on its old length plus whole grid steps. The other selected notes travel by the same amount, so
// the selection keeps its shape.

/**
 * The roll's grid: the line at or before a tick, or the nearest with `round`. Null drags freely (Ctrl/Cmd, or no
 * grid), by whole ticks.
 */
export type Grid = ((tick: number, round?: boolean) => number) | null;

/** The travel that puts `edge`, dragged by `travel` ticks, on the grid. */
export function snappedTravel(edge: number, travel: number, grid: Grid) {
  return grid ? grid(edge + travel, true) - edge : Math.round(travel);
}

/**
 * Moves the end of each note by the grabbed note's travel. Snapped, the grabbed note's end goes to a grid line, the
 * first one after its start at the shortest. `minLength` is the shortest a note gets otherwise (a grid step, or the
 * note's own length if shorter).
 */
export function resizeEnds(originals: EditNote[], grabbed: EditNote, travel: number, minLength: number, grid: Grid) {
  const end = grabbed.start + grabbed.length;
  let delta = snappedTravel(end, travel, grid);
  const firstLine = grid ? grid(grabbed.start + minLength) : 0;
  if (grid && firstLine > grabbed.start) delta = Math.max(delta, firstLine - end);
  return originals.map((n) => {
    const shortest = grid && n.id === grabbed.id && firstLine > n.start ? firstLine - n.start : Math.min(minLength, n.length);
    return { ...n, length: Math.max(1, shortest, n.length + delta) };
  });
}

/**
 * Moves the start of each note by the grabbed note's travel, keeping its end. Snapped, the grabbed note's start goes to
 * a grid line, the last one before its end at the latest.
 */
export function resizeStarts(originals: EditNote[], grabbed: EditNote, travel: number, minLength: number, grid: Grid) {
  const grabbedEnd = grabbed.start + grabbed.length;
  let delta = snappedTravel(grabbed.start, travel, grid);
  const lastLine = grid ? grid(grabbedEnd - 1) : -1;
  if (grid && lastLine >= 0 && lastLine < grabbedEnd) delta = Math.min(delta, lastLine - grabbed.start);
  return originals.map((n) => {
    const end = n.start + n.length;
    const latest = grid && n.id === grabbed.id && lastLine >= 0 ? lastLine : end - Math.max(1, Math.min(minLength, n.length));
    const start = Math.min(Math.max(0, n.start + delta), latest);
    return { ...n, start, length: end - start };
  });
}
