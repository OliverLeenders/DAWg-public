import type { PatchOp } from '../bridge/protocol';

// Immutable application of the JSON-Patch subset produced by engine/src/JsonPatch.cpp.
// Only the containers along each path are copied, so unchanged subtrees keep their
// identity and React/zustand selectors skip re-rendering them.

type Container = Record<string, unknown> | unknown[];

const unescape = (segment: string) => segment.replace(/~1/g, '/').replace(/~0/g, '~');

function setIn(node: unknown, keys: string[], op: PatchOp): unknown {
  if (keys.length === 0) {
    if (op.op === 'remove') throw new Error('Cannot remove the document root');
    return op.value;
  }

  const [head, ...rest] = keys;
  const container = node as Container;

  if (Array.isArray(container)) {
    const index = Number(head);
    const copy = container.slice();
    if (rest.length === 0 && op.op === 'remove') copy.splice(index, 1);
    else if (rest.length === 0 && op.op === 'add') copy.splice(index, 0, op.value);
    else copy[index] = setIn(container[index], rest, op);
    return copy;
  }

  const copy = { ...container };
  if (rest.length === 0 && op.op === 'remove') delete copy[head];
  else copy[head] = setIn(container[head], rest, op);
  return copy;
}

export function applyPatch<T>(doc: T, ops: PatchOp[]): T {
  let result: unknown = doc;
  for (const op of ops) {
    const keys = op.path === '' ? [] : op.path.slice(1).split('/').map(unescape);
    result = setIn(result, keys, op);
  }
  return result as T;
}
