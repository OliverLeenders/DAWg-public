import { describe, expect, it } from 'vitest';
import {
  NODE_TYPES,
  type GraphNode,
  type NodeTypeInfo,
  type PortDescriptor,
  type Track,
} from '../bridge/protocol';
import { inspectorGroups, pluginEditorAction, reorderIndex, trackChain } from './chain';

const node = (id: number, type: string, name?: string): GraphNode => ({
  id,
  type,
  params: {},
  x: 0,
  y: 0,
  bypassed: false,
  ...(name ? { plugin: { identifier: name, name, manufacturer: '', format: 'VST3' } } : {}),
});

function track(nodes: GraphNode[], links: Array<[number, number]>): Track {
  return {
    id: 1,
    name: 'T',
    colour: '#fff',
    gainDb: 0,
    pan: 0,
    offsetMs: 0,
    drumMode: 'auto',
    automation: {},
    automationOrder: [],
    automationMode: 1,
    automationExpanded: false,
  lanes: 1,
  lanesExpanded: false,
    height: 64,
    kind: 'instrument',
    parent: 0,
    folded: false,
    sends: {},
    mute: false,
    solo: false,
    inputChannel: -1,
    inputStereo: false,
    monitor: 1,
    clips: {},
    graph: {
      nodes: Object.fromEntries(nodes.map((n) => [String(n.id), n])),
      connections: Object.fromEntries(
        links.map(([from, to]) => [`${from}:0>${to}:0`, { fromNode: from, fromPort: 0, toNode: to, toPort: 0 }]),
      ),
    },
  };
}

const base = [node(1, NODE_TYPES.sequencer), node(3, NODE_TYPES.gain), node(4, NODE_TYPES.trackOutput)];

const port = (type: PortDescriptor['type']): PortDescriptor => ({ type, name: type });
const info = (
  type: string,
  category: NodeTypeInfo['category'],
  inputs: PortDescriptor[] = [],
  outputs: PortDescriptor[] = [],
  userEditable = true,
): NodeTypeInfo => ({ type, name: type, category, userEditable, inputs, outputs, params: [] });

const types: Record<string, NodeTypeInfo> = {
  [NODE_TYPES.sequencer]: info(NODE_TYPES.sequencer, 'io', [], [port('events')], false),
  [NODE_TYPES.gain]: info(NODE_TYPES.gain, 'utility', [port('audio')], [port('audio')], false),
  [NODE_TYPES.trackOutput]: info(NODE_TYPES.trackOutput, 'io', [port('audio')], [], false),
  [NODE_TYPES.mpeSynth]: info(NODE_TYPES.mpeSynth, 'instrument', [port('events')], [port('audio')]),
  [NODE_TYPES.pluginInstrument]: info(NODE_TYPES.pluginInstrument, 'instrument', [port('events')], [port('audio')]),
  [NODE_TYPES.pluginEffect]: info(
    NODE_TYPES.pluginEffect,
    'effect',
    [port('audio'), port('modulation')],
    [port('audio')],
  ),
  [NODE_TYPES.lfo]: info(NODE_TYPES.lfo, 'utility', [], [port('modulation')]),
  [NODE_TYPES.adsr]: info(NODE_TYPES.adsr, 'utility', [port('events')], [port('modulation')]),
};

describe('trackChain', () => {
  it('finds the instrument and the effects in order', () => {
    const chain = trackChain(
      track(
        [...base, node(2, NODE_TYPES.pluginInstrument, 'Synth'), node(5, NODE_TYPES.pluginEffect, 'A'), node(6, NODE_TYPES.pluginEffect, 'B')],
        [[1, 2], [2, 6], [6, 5], [5, 3], [3, 4]],
      ),
    );
    expect(chain?.instrument?.plugin?.name).toBe('Synth');
    expect(chain?.effects.map((e) => e.plugin?.name)).toEqual(['B', 'A']);
  });

  it("takes a Gain before the chain's own Gain as an effect", () => {
    const chain = trackChain(track([...base, node(2, NODE_TYPES.mpeSynth), node(7, NODE_TYPES.gain)], [[1, 2], [2, 7], [7, 3], [3, 4]]));
    expect(chain?.effects.map((n) => n.id)).toEqual([7]);
    expect(chain?.gain?.id).toBe(3);
  });

  it('handles a track without instrument', () => {
    const chain = trackChain(track([...base, node(5, NODE_TYPES.pluginEffect, 'A')], [[5, 3], [3, 4]]));
    expect(chain?.instrument).toBeNull();
    expect(chain?.effects).toHaveLength(1);
  });

  it('finds a chain whose gain was removed', () => {
    const withoutGain = base.filter((n) => n.type !== NODE_TYPES.gain);
    const synth = node(2, NODE_TYPES.mpeSynth);
    const chain = trackChain(track([...withoutGain, synth, node(5, NODE_TYPES.eq)], [[1, 2], [2, 5], [5, 4]]));
    expect(chain?.instrument?.id).toBe(2);
    expect(chain?.effects.map((n) => n.id)).toEqual([5]);
    // Nothing left but the fixed nodes is an empty chain.
    expect(trackChain(track(withoutGain, []))).toEqual({ midiEffects: [], instrument: null, effects: [], gain: null });
  });

  it('rejects graphs that are not chains', () => {
    expect(trackChain(track(base, [[1, 3]]))).toBeNull();
    expect(trackChain(track(base, []))).toBeNull();
  });

  it('rejects chains with extra nodes or a sequencer feeding more than the instrument', () => {
    const synth = node(2, NODE_TYPES.mpeSynth);
    const links: Array<[number, number]> = [[1, 2], [2, 3], [3, 4]];
    expect(trackChain(track([...base, synth], links))).not.toBeNull();
    expect(trackChain(track([...base, synth, node(7, NODE_TYPES.eq)], links))).toBeNull();
    expect(trackChain(track([...base, synth, node(8, NODE_TYPES.mpeSynth)], [...links, [1, 8]]))).toBeNull();
  });
});

describe('trackChain with MIDI effects', () => {
  const synth = node(2, NODE_TYPES.mpeSynth);
  const arp = node(10, NODE_TYPES.arpeggiator);
  const chord = node(11, NODE_TYPES.chord);

  it('finds them between the sequencer and the instrument, in order', () => {
    const chain = trackChain(track([...base, synth, arp, chord], [[1, 11], [11, 10], [10, 2], [2, 3], [3, 4]]));
    expect(chain?.midiEffects.map((n) => n.id)).toEqual([11, 10]);
    expect(chain?.instrument?.id).toBe(2);
  });

  it('finds them hanging off the sequencer without an instrument', () => {
    const chain = trackChain(track([...base, arp, chord], [[1, 10], [10, 11], [3, 4]]));
    expect(chain?.midiEffects.map((n) => n.id)).toEqual([10, 11]);
    expect(chain?.instrument).toBeNull();
  });

  it('rejects a MIDI effect the sequencer does not feed', () => {
    expect(trackChain(track([...base, synth, arp], [[10, 2], [2, 3], [3, 4]]))).toBeNull();
    // The sequencer feeding the instrument past a MIDI effect.
    expect(trackChain(track([...base, synth, arp], [[1, 10], [1, 2], [2, 3], [3, 4]]))).toBeNull();
  });
});

describe('trackChain with modulation', () => {
  const synth = node(2, NODE_TYPES.mpeSynth);
  const links: Array<[number, number]> = [[1, 2], [2, 5], [5, 3], [3, 4]];

  it('keeps a chain when sources feed the modulation pool of an effect and take the notes', () => {
    const t = track([...base, synth, node(5, NODE_TYPES.eq), node(8, NODE_TYPES.lfo), node(9, NODE_TYPES.adsr)], links);
    t.graph.connections['8:0>5:1'] = { fromNode: 8, fromPort: 0, toNode: 5, toPort: 1 };
    t.graph.connections['1:0>9:0'] = { fromNode: 1, fromPort: 0, toNode: 9, toPort: 0 };

    const chain = trackChain(t);
    expect(chain?.instrument?.id).toBe(2);
    expect(chain?.effects.map((n) => n.id)).toEqual([5]);
  });

  it('is still not a chain when an effect has two audio inputs', () => {
    const t = track([...base, synth, node(5, NODE_TYPES.eq), node(6, NODE_TYPES.eq)], [...links, [6, 5]]);
    expect(trackChain(t)).toBeNull();
  });
});

describe('inspectorGroups', () => {
  it('puts instruments above effects even when the graph stores them later', () => {
    const groups = inspectorGroups(
      track(
        [
          node(5, NODE_TYPES.pluginEffect, 'A'),
          node(2, NODE_TYPES.pluginInstrument, 'Synth'),
          node(6, NODE_TYPES.pluginEffect, 'B'),
          ...base,
        ],
        [[1, 2], [2, 6], [6, 5], [5, 3], [3, 4]],
      ),
      types,
    );

    expect(groups.instruments.map((n) => n.id)).toEqual([2]);
    expect(groups.effects.map((n) => n.id)).toEqual([6, 5]);
    expect(groups.modulation).toEqual([]);
  });

  it('keeps effects in signal-flow order across parallel branches', () => {
    const groups = inspectorGroups(
      track(
        [
          node(6, NODE_TYPES.pluginEffect, 'B'),
          node(5, NODE_TYPES.pluginEffect, 'A'),
          node(7, NODE_TYPES.pluginEffect, 'C'),
          node(2, NODE_TYPES.mpeSynth),
          ...base,
        ],
        [[1, 2], [2, 6], [2, 5], [6, 7], [5, 7], [7, 3], [3, 4]],
      ),
      types,
    );

    const effects = groups.effects.map((n) => n.id);
    expect(effects).toHaveLength(3);
    expect(effects.indexOf(7)).toBeGreaterThan(effects.indexOf(6));
    expect(effects.indexOf(7)).toBeGreaterThan(effects.indexOf(5));
  });

  it('lists LFOs and ADSRs separately from the inserts', () => {
    const t = track(
      [
        ...base,
        node(2, NODE_TYPES.mpeSynth),
        node(5, NODE_TYPES.pluginEffect, 'A'),
        node(8, NODE_TYPES.lfo),
        node(9, NODE_TYPES.adsr),
      ],
      [[1, 2], [2, 5], [5, 3], [3, 4]],
    );
    t.graph.connections['8:0>5:1'] = { fromNode: 8, fromPort: 0, toNode: 5, toPort: 1 };
    t.graph.connections['1:0>9:0'] = { fromNode: 1, fromPort: 0, toNode: 9, toPort: 0 };

    const groups = inspectorGroups(t, types);
    expect(groups.instruments.map((n) => n.id)).toEqual([2]);
    expect(groups.effects.map((n) => n.id)).toEqual([5]);
    expect(groups.modulation.map((n) => n.id)).toEqual([8, 9]);
  });
});

describe('reorderIndex', () => {
  it('accounts for the effect being taken out before it is put back', () => {
    // A -> B -> C, dragged A (0) onto C (2): after removing A, C is at 1, so A goes back at 1.
    expect(reorderIndex(0, 2)).toBe(1);
    // Dragged C (2) before A (0): nothing above it moved.
    expect(reorderIndex(2, 0)).toBe(0);
    // Dropped on its own top half, or on the lower half of the slot above: no move.
    expect(reorderIndex(1, 1)).toBe(1);
    expect(reorderIndex(1, 2)).toBe(1);
    // Dropped past the last effect.
    expect(reorderIndex(0, 3)).toBe(2);
  });
});

describe('pluginEditorAction', () => {
  const plugin = node(4, NODE_TYPES.pluginEffect, 'Valhalla');
  const ready = { status: 'ready' as const, error: '' };

  it('opens a ready plugin and closes an open one', () => {
    expect(pluginEditorAction(plugin, { ...ready, editorOpen: false })).toBe('open');
    expect(pluginEditorAction(plugin, { ...ready, editorOpen: true })).toBe('close');
  });

  it('leaves plugins that are not ready and built-in nodes alone', () => {
    expect(pluginEditorAction(plugin, undefined)).toBeNull();
    expect(pluginEditorAction(plugin, { status: 'loading', error: '' })).toBeNull();
    expect(pluginEditorAction(node(5, NODE_TYPES.faust), ready)).toBeNull();
  });
});
