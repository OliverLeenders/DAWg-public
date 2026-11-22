import { NODE_TYPES, type Command, type ComponentGraph, type TrackGraph } from '../bridge/protocol';

// Factory patches built around the Feedback node (docs/design/feedback-routing.md, section 2) and the split nodes
// (docs/design/split-nodes.md, SP8): several nodes that only make sense together, which arrive as one component to cable
// in (docs/design/component-nodes.md, N9), with its main controls on it.

type AddNodes = Extract<Command, { command: 'addNodes' }>['args'];
type NodeSpec = AddNodes['nodes'][number];

export interface Patch {
  key: string;
  name: string;
  /** Where to cable it, for the picker. */
  hint: string;
  nodes: NodeSpec[];
  connections: NonNullable<AddNodes['connections']>;
  /** For a patch that's a component: where its input goes, which node is its output, and the controls on it. */
  box?: {
    input: Array<{ to: number; toPort: number }>;
    output: number;
    controls: Array<{ node: number; param: string; name: string }>;
  };
}

const SPACING = 220;

const row = (specs: Array<Omit<NodeSpec, 'x' | 'y'> & { at: [number, number] }>): NodeSpec[] =>
  specs.map(({ at, ...spec }) => ({ ...spec, x: at[0] * SPACING, y: at[1] * 130 }));

const cable = (from: number, to: number, toPort = 0, fromPort = 0) => ({ from, fromPort, to, toPort });

/** A note's length in the Feedback node's Sync choices. */
const DOTTED_EIGHTH = 12;

const dubDelay: Patch = {
  key: 'dub-delay',
  name: 'Dub Delay',
  hint: 'Echoes that darken and break up as they repeat',
  nodes: row([
    { type: NODE_TYPES.mix, at: [0, 1] },
    { type: NODE_TYPES.feedback, params: { mode: 1, note: DOTTED_EIGHTH, amount: 65 }, at: [1, 1] },
    { type: NODE_TYPES.filter, params: { mode: 0, cutoff: 2200, resonance: 15 }, at: [2, 1] },
    { type: NODE_TYPES.saturator, params: { drive: 6, oversampling: 0 }, at: [3, 1] },
    { type: NODE_TYPES.mix, params: { gain2: -6 }, at: [4, 0] },
  ]),
  connections: [cable(0, 1), cable(1, 2), cable(2, 3), cable(3, 0, 1), cable(3, 4, 1)],
  box: {
    input: [
      { to: 0, toPort: 0 },
      { to: 4, toPort: 0 },
    ],
    output: 4,
    controls: [
      { node: 1, param: 'note', name: 'Time' },
      { node: 1, param: 'amount', name: 'Repeats' },
      { node: 2, param: 'cutoff', name: 'Tone' },
    ],
  },
};

const resonator: Patch = {
  key: 'resonator',
  name: 'Resonator',
  hint: 'Tunes what it hears to a fifth on C',
  nodes: row([
    { type: NODE_TYPES.gain, params: { gainDb: -24 }, at: [0, 1] },
    { type: NODE_TYPES.mix, at: [1, 1] },
    { type: NODE_TYPES.feedback, params: { mode: 2, pitch: -12, amount: 97 }, at: [2, 1] },
    { type: NODE_TYPES.mix, at: [1, 2] },
    { type: NODE_TYPES.feedback, params: { mode: 2, pitch: -5, amount: 97 }, at: [2, 2] },
    { type: NODE_TYPES.mix, at: [3, 0] },
  ]),
  connections: [cable(0, 1), cable(1, 2), cable(2, 1, 1), cable(0, 3), cable(3, 4), cable(4, 3, 1), cable(2, 5, 1), cable(4, 5, 2)],
  box: {
    input: [
      { to: 0, toPort: 0 },
      { to: 5, toPort: 0 },
    ],
    output: 5,
    controls: [
      { node: 2, param: 'pitch', name: 'Root' },
      { node: 4, param: 'pitch', name: 'Fifth' },
      { node: 2, param: 'amount', name: 'Ring' },
    ],
  },
};

// A short loop whose length an LFO moves by a couple of milliseconds. Port 1 is the Feedback node's first modulation input.
const flanger: Patch = {
  key: 'flanger',
  name: 'Flanger',
  hint: 'A jet sweep from a short, moving loop',
  nodes: row([
    { type: NODE_TYPES.lfo, params: { rateMs: 4000 }, at: [0, 2] },
    { type: NODE_TYPES.mix, at: [0, 1] },
    {
      type: NODE_TYPES.feedback,
      params: { time: 4, amount: 70 },
      modulations: { 1: { param: 'time', depth: 2.5 / 50, active: true } },
      at: [1, 1],
    },
    { type: NODE_TYPES.mix, at: [2, 0] },
  ]),
  connections: [cable(1, 2), cable(2, 1, 1), cable(0, 2, 1), cable(2, 3, 1)],
  box: {
    input: [
      { to: 1, toPort: 0 },
      { to: 3, toPort: 0 },
    ],
    output: 3,
    controls: [
      { node: 0, param: 'rateMs', name: 'Rate' },
      { node: 2, param: 'time', name: 'Delay' },
      { node: 2, param: 'amount', name: 'Feedback' },
    ],
  },
};

// The voice graph of a plucked string: a short burst of noise goes round a loop as long as the note's period, losing its highs.
const INPUT = 1;
const EXPRESSION = 2;
const NOISE = 3;
const ENVELOPE = 4;
const BURST = 5;
const MIX = 6;
const LOOP = 7;
const DAMPING = 8;
const OUTPUT = 9;

const voiceNode = (id: number, type: string, x: number, y: number, params: Record<string, number> = {}, modulations = {}) => ({
  id,
  type,
  params,
  x: x * SPACING,
  y: y * 130,
  bypassed: false,
  modulations,
  modulationShape: [],
});

const voiceCable = (fromNode: number, toNode: number, toPort = 0) => [
  `${fromNode}:0>${toNode}:${toPort}`,
  { fromNode, fromPort: 0, toNode, toPort },
];

const pluckedString: Patch = {
  key: 'plucked-string',
  name: 'Plucked String',
  hint: 'An instrument. Cable the Sequencer into it, and it into the chain',
  nodes: row([
    {
      type: NODE_TYPES.voiceInstrument,
      at: [0, 0],
      voice: {
        nodes: Object.fromEntries(
          [
            voiceNode(INPUT, NODE_TYPES.voiceInput, 0, 0),
            voiceNode(EXPRESSION, NODE_TYPES.noteExpression, 1, -1),
            voiceNode(NOISE, NODE_TYPES.oscillator, 1, 0, { waveform: 4, level: 0 }),
            voiceNode(ENVELOPE, NODE_TYPES.adsr, 1, 1, { attackMs: 0, decayMs: 6, sustain: 0, releaseMs: 0 }),
            voiceNode(BURST, NODE_TYPES.gain, 2, 0, { amplitude: 0 }, { 1: { param: 'amplitude', depth: 0.5, active: true } }),
            voiceNode(MIX, NODE_TYPES.mix, 3, 0),
            voiceNode(LOOP, NODE_TYPES.feedback, 4, 0, { mode: 2, amount: 99, trim: 1.6 }, { 1: { param: 'pitch', depth: 1, active: true } }),
            voiceNode(DAMPING, NODE_TYPES.filter, 5, 0, { mode: 0, cutoff: 7000, resonance: 0 }),
            voiceNode(OUTPUT, NODE_TYPES.voiceOutput, 6, 0),
          ].map((n) => [String(n.id), n]),
        ),
        connections: Object.fromEntries([
          voiceCable(INPUT, EXPRESSION),
          voiceCable(INPUT, ENVELOPE),
          voiceCable(EXPRESSION, LOOP, 1),
          voiceCable(NOISE, BURST),
          voiceCable(ENVELOPE, BURST, 1),
          voiceCable(BURST, MIX),
          voiceCable(MIX, LOOP),
          voiceCable(LOOP, DAMPING),
          voiceCable(DAMPING, MIX, 1),
          voiceCable(DAMPING, OUTPUT),
        ]),
      },
    },
  ] as never),
  connections: [],
};

// Three bands, a Compressor on each, summed by a Mix; the Dry port, through the same all-passes as the bands, blends in
// after them, so a parallel blend doesn't comb (split-nodes.md, SP3, SP8). Dry starts silent.
const multibandCompressor: Patch = {
  key: 'multiband-compressor',
  name: 'Multiband Compressor',
  hint: 'Compresses lows, mids and highs apart',
  nodes: row([
    { type: NODE_TYPES.bandSplit, at: [0, 1] },
    { type: NODE_TYPES.compressor, params: { threshold: -24, ratio: 3 }, at: [1, 0] },
    { type: NODE_TYPES.compressor, params: { threshold: -20, ratio: 3 }, at: [1, 1] },
    { type: NODE_TYPES.compressor, params: { threshold: -18, ratio: 3 }, at: [1, 2] },
    { type: NODE_TYPES.mix, at: [2, 1] },
    { type: NODE_TYPES.mix, params: { gain1: -96 }, at: [3, 1] },
  ]),
  connections: [
    cable(0, 1, 0, 0),
    cable(0, 2, 0, 1),
    cable(0, 3, 0, 2),
    cable(1, 4, 0),
    cable(2, 4, 1),
    cable(3, 4, 2),
    cable(0, 5, 0, 4),
    cable(4, 5, 1),
  ],
  box: {
    input: [{ to: 0, toPort: 0 }],
    output: 5,
    controls: [
      { node: 0, param: 'crossover1', name: 'Low / mid' },
      { node: 0, param: 'crossover2', name: 'Mid / high' },
      ...[1, 2, 3].flatMap((node, i) => {
        const band = ['Low', 'Mid', 'High'][i];
        return [
          { node, param: 'threshold', name: `${band} threshold` },
          { node, param: 'ratio', name: `${band} ratio` },
          { node, param: 'makeup', name: `${band} makeup` },
        ];
      }),
      { node: 5, param: 'gain1', name: 'Dry' },
    ],
  },
};

// Mid and side apart, each on its own cable to drop effects onto, joined again with a gain each (split-nodes.md, SP8).
const midSide: Patch = {
  key: 'mid-side',
  name: 'Mid/Side',
  hint: 'The mid and the side on cables of their own',
  nodes: row([
    { type: NODE_TYPES.stereoSplit, at: [0, 1] },
    { type: NODE_TYPES.stereoMerge, at: [2, 1] },
  ]),
  connections: [cable(0, 1, 0, 0), cable(0, 1, 1, 1)],
  box: {
    input: [{ to: 0, toPort: 0 }],
    output: 1,
    controls: [
      { node: 1, param: 'gain1', name: 'Mid' },
      { node: 1, param: 'gain2', name: 'Side' },
    ],
  },
};

export const PATCHES: Patch[] = [dubDelay, resonator, flanger, multibandCompressor, midSide, pluckedString];

/** What's in a patch that's a component, in the form the engine reads. Its ids are only its own: the engine gives it new ones. */
export function patchComponent(patch: Patch): ComponentGraph | undefined {
  const box = patch.box;
  if (!box) return undefined;

  const idOf = (index: number) => index + 1;
  const input = patch.nodes.length + 1;
  const output = patch.nodes.length + 2;
  const fixed = (id: number, type: string, x: number) => ({ id, type, params: {}, x, y: 0, bypassed: false, modulations: {} });
  const right = Math.max(...patch.nodes.map((n) => n.x ?? 0)) + SPACING;
  const inner = patch.connections.map((c) => ({ fromNode: idOf(c.from), fromPort: c.fromPort, toNode: idOf(c.to), toPort: c.toPort }));
  const edge = [
    ...box.input.map((i) => ({ fromNode: input, fromPort: 0, toNode: idOf(i.to), toPort: i.toPort })),
    { fromNode: idOf(box.output), fromPort: 0, toNode: output, toPort: 0 },
  ];
  const nodes = [
    ...patch.nodes.map((n, i) => ({ ...n, id: idOf(i), params: n.params ?? {}, bypassed: false, modulations: n.modulations ?? {} })),
    fixed(input, NODE_TYPES.componentInput, -SPACING),
    fixed(output, NODE_TYPES.componentOutput, right),
  ];

  return {
    name: patch.name,
    graph: {
      nodes: Object.fromEntries(nodes.map((n) => [String(n.id), n])),
      connections: Object.fromEntries([...inner, ...edge].map((c) => [`${c.fromNode}:${c.fromPort}>${c.toNode}:${c.toPort}`, c])),
    } as unknown as TrackGraph,
    inputs: [{ type: 'audio', name: 'In' }],
    outputs: [{ type: 'audio', name: 'Out' }],
    exposed: box.controls.map((c) => ({ nodeId: idOf(c.node), param: c.param, name: c.name })),
  };
}

/** The command that adds a patch with its first node at a point. */
export function patchCommand(patch: Patch, trackId: number, scope: { voiceNodeId?: number }, x: number, y: number): Command {
  const component = patchComponent(patch);
  if (component) return { command: 'addNodes', args: { trackId, ...scope, nodes: [{ type: NODE_TYPES.component, component, x, y }] } };

  return {
    command: 'addNodes',
    args: {
      trackId,
      ...scope,
      nodes: patch.nodes.map((n) => ({ ...n, x: (n.x ?? 0) + x, y: (n.y ?? 0) + y })),
      connections: patch.connections,
    },
  };
}
