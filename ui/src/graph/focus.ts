import type { Connection } from '../bridge/protocol';

// What stays bright in the graph view while nodes are hovered or selected: those nodes, what they connect to, and those cables.

export interface Focus {
  nodes: Set<number>;
  cables: Set<string>;
}

/** The focus around some nodes, or null if there are none. */
export function focusOf(connections: Connection[], around: ReadonlySet<number>): Focus | null {
  if (around.size === 0) return null;
  const nodes = new Set(around);
  const cables = new Set<string>();

  for (const c of connections) {
    if (!around.has(c.fromNode) && !around.has(c.toNode)) continue;
    nodes.add(c.fromNode);
    nodes.add(c.toNode);
    cables.add(`${c.fromNode}:${c.fromPort}>${c.toNode}:${c.toPort}`);
  }

  return { nodes, cables };
}
