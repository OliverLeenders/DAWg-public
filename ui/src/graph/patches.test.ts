import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type Connection } from '../bridge/protocol';
import { PATCHES, patchCommand, patchComponent } from './patches';
import { wouldCycle } from './validate';

describe('factory patches', () => {
  it('close their loops only through Feedback nodes', () => {
    for (const patch of PATCHES) {
      const feedback = new Set(patch.nodes.flatMap((n, i) => (n.type === NODE_TYPES.feedback ? [i] : [])));
      const cables: Connection[] = [];
      for (const c of patch.connections) {
        const cable = { fromNode: c.from, fromPort: c.fromPort, toNode: c.to, toPort: c.toPort };
        expect(wouldCycle(cables, c.from, c.to, feedback)).toBe(false);
        cables.push(cable);
      }
    }
  });

  it('are placed where they are asked for', () => {
    const command = patchCommand(PATCHES[0], 1, {}, 100, 200);
    if (command.command !== 'addNodes') throw new Error('expected addNodes');
    expect(command.args.nodes[0]).toMatchObject({ x: 100, y: 200 });
  });

  it('arrive as components, with their ports and controls', () => {
    for (const patch of PATCHES.filter((p) => p.box)) {
      const inside = patchComponent(patch)!;
      expect(inside.name).toBe(patch.name);
      expect(inside.inputs).toHaveLength(1);
      expect(inside.outputs).toHaveLength(1);

      const nodes = Object.values(inside.graph.nodes);
      expect(nodes.filter((n) => n.type === NODE_TYPES.componentInput)).toHaveLength(1);
      expect(nodes.filter((n) => n.type === NODE_TYPES.componentOutput)).toHaveLength(1);

      // Every control aims at a node that's there, and every cable joins two.
      for (const control of inside.exposed) expect(inside.graph.nodes[String(control.nodeId)]).toBeDefined();
      for (const c of Object.values(inside.graph.connections)) {
        expect(inside.graph.nodes[String(c.fromNode)]).toBeDefined();
        expect(inside.graph.nodes[String(c.toNode)]).toBeDefined();
      }

      const command = patchCommand(patch, 1, {}, 100, 200);
      if (command.command !== 'addNodes') throw new Error('expected addNodes');
      expect(command.args.nodes).toHaveLength(1);
      expect(command.args.nodes[0]).toMatchObject({ type: NODE_TYPES.component, x: 100, y: 200 });
    }
  });

  it('give a voice graph its fixed nodes', () => {
    const string = PATCHES.find((p) => p.key === 'plucked-string')!;
    const voice = string.nodes[0].voice as unknown as { nodes: Record<string, { type: string }> };
    const types = Object.values(voice.nodes).map((n) => n.type);
    expect(types).toContain(NODE_TYPES.voiceInput);
    expect(types).toContain(NODE_TYPES.voiceOutput);
  });
});
