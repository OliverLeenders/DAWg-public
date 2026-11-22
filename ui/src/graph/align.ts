import type { Point } from './layout';
import { snapNode } from './grid';

// Align and distribute for a selection of nodes (docs/design/graph-grid-and-cables.md, G4). Everything here is in the canvas's own
// coordinates, where the signal runs along x left to right, or along y top to bottom.

export type AlignKind = 'inputs' | 'outputs' | 'flow' | 'distribute';

export interface AlignNode {
  id: number;
  /** The node's top left corner. */
  x: number;
  y: number;
  width: number;
  height: number;
  /** Its first input's and first output's centres, from its corner. */
  input: Point | null;
  output: Point | null;
}

/** Where each node that moves goes. The nodes line up with the one furthest upstream; with snapping on they end on the grid. */
export function align(nodes: AlignNode[], kind: AlignKind, vertical: boolean, snap: boolean): Map<number, Point> {
  const along = (p: Point) => (vertical ? p.y : p.x);
  const across = (p: Point) => (vertical ? p.x : p.y);
  const at = (alongValue: number, acrossValue: number): Point => (vertical ? { x: acrossValue, y: alongValue } : { x: alongValue, y: acrossValue });
  const upstream = (list: AlignNode[]) => list.reduce((a, b) => (along(b) < along(a) ? b : a));
  const moved = new Map<number, Point>();

  if (kind === 'inputs' || kind === 'outputs') {
    const port = (n: AlignNode) => (kind === 'inputs' ? n.input : n.output);
    const withPort = nodes.filter((n) => port(n));
    if (withPort.length < 2) return moved;
    const lead = upstream(withPort);
    const line = across(lead) + across(port(lead)!);
    for (const n of withPort) moved.set(n.id, at(along(n), line - across(port(n)!)));
  } else if (kind === 'flow') {
    if (nodes.length < 2) return moved;
    const line = along(upstream(nodes));
    for (const n of nodes) moved.set(n.id, at(line, across(n)));
  } else {
    // Even gaps across the flow, between the outermost two, which stay.
    if (nodes.length < 3) return moved;
    const size = (n: AlignNode) => (vertical ? n.width : n.height);
    const sorted = [...nodes].sort((a, b) => across(a) - across(b));
    const [first, last] = [sorted[0], sorted[sorted.length - 1]];
    const room = across(last) + size(last) - across(first) - sorted.reduce((sum, n) => sum + size(n), 0);
    const gap = room / (sorted.length - 1);
    let next = across(first);
    for (const n of sorted) {
      moved.set(n.id, at(along(n), next));
      next += size(n) + gap;
    }
  }

  for (const n of nodes) {
    const to = moved.get(n.id);
    if (!to) continue;
    const port = n.input ?? n.output;
    const placed = snap && port ? snapNode(to, port) : { x: Math.round(to.x), y: Math.round(to.y) };
    if (placed.x === n.x && placed.y === n.y) moved.delete(n.id);
    else moved.set(n.id, placed);
  }
  return moved;
}
