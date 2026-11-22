import { describe, expect, it } from 'vitest';
import type { ParamDescriptor } from '../bridge/protocol';
import { depthStep, formatDepth, formatParam, fromPosition, noteHint, noteOf, parseNote, parseNumber, parseParam, toPosition } from './params';
import { closesLoop, growingLoop, isLoopBack, loopNodePosition, wouldCycle } from './validate';
import { NODE_TYPES, type GraphNode, type Project, type TrackGraph } from '../bridge/protocol';

const param = (fields: Partial<ParamDescriptor>): ParamDescriptor => ({
  modulationSpan: (fields.max ?? 1) - (fields.min ?? 0),
  id: 'p',
  name: 'P',
  min: 0,
  max: 1,
  default: 0,
  kind: 'continuous',
  unit: '',
  mid: 0,
  choices: [],
  modulationPolarity: 'bipolar',
  modulatable: true,
  automatable: true,
  ...fields,
});

describe('parameter controls', () => {
  it('puts the mid value at the middle of a skewed range', () => {
    const cutoff = param({ min: 20, max: 20000, mid: 1000, unit: 'Hz' });
    expect(fromPosition(cutoff, 0.5)).toBeCloseTo(1000, 3);
    expect(toPosition(cutoff, 1000)).toBeCloseTo(0.5, 6);
    expect(fromPosition(cutoff, 0)).toBe(20);
    expect(fromPosition(cutoff, 1)).toBeCloseTo(20000, 6);
    // Low positions still reach useful frequencies (close to a logarithmic scale).
    expect(fromPosition(cutoff, 0.25)).toBeGreaterThan(150);
    expect(fromPosition(cutoff, 0.25)).toBeLessThan(400);
    expect(toPosition(cutoff, fromPosition(cutoff, 0.3))).toBeCloseTo(0.3, 6);
  });

  it('is linear without a mid value and rounds choices', () => {
    expect(fromPosition(param({ min: -24, max: 24 }), 0.5)).toBeCloseTo(0, 6);
    expect(fromPosition(param({ kind: 'choice', max: 3, choices: ['a', 'b', 'c', 'd'] }), 0.4)).toBe(1);
  });

  it('formats values with units and labels', () => {
    expect(formatParam(param({ unit: 'Hz', min: 20, max: 20000 }), 2500)).toBe('2.50 kHz');
    expect(formatParam(param({ unit: 'dB', min: -24, max: 24 }), -3)).toBe('-3.00 dB');
    expect(formatParam(param({ unit: '%', max: 100 }), 50)).toBe('50%');
    expect(formatParam(param({ unit: '%', max: 100 }), 4.25)).toBe('4.3%');
    expect(formatParam(param({ unit: 'deg', max: 360 }), 90)).toBe('90°');
    expect(formatParam(param({ unit: 'ms', max: 200 }), 12.34)).toBe('12.3 ms');
    expect(formatParam(param({ kind: 'toggle' }), 1)).toBe('On');
    expect(formatParam(param({ kind: 'choice', max: 1, choices: ['Low', 'High'] }), 1)).toBe('High');
  });
});

describe('connection rules', () => {
  it('finds feedback loops', () => {
    const chain = [
      { fromNode: 1, fromPort: 0, toNode: 2, toPort: 0 },
      { fromNode: 2, fromPort: 0, toNode: 3, toPort: 0 },
    ];
    expect(wouldCycle(chain, 3, 1)).toBe(true);
    expect(wouldCycle(chain, 1, 3)).toBe(false);
    expect(wouldCycle(chain, 2, 2)).toBe(true);
  });
});

describe('modulation depth', () => {
  it("shows a depth as the amount it moves the parameter, in the parameter's units", () => {
    const amplitude = param({ min: 0, max: 200, unit: '%' });
    expect(formatDepth(amplitude, 0.5)).toBe('+100%');
    expect(formatDepth(amplitude, -0.25)).toBe('-50%');
    const pitch = param({ min: -120, max: 120, unit: 'st' });
    expect(formatDepth(pitch, 0.01)).toBe('+2.40 st');
    expect(formatDepth(pitch, 0)).toBe('0.00 st');
  });

  it('edits a depth in steps of about a thousandth of the range, whole for integers', () => {
    expect(depthStep(param({ min: 0, max: 200 }))).toBe(0.1);
    expect(depthStep(param({ min: 20, max: 20000 }))).toBe(10);
    expect(depthStep(param({ min: -48, max: 48, kind: 'integer' }))).toBe(1);
  });
});

describe('loops', () => {
  const node = (id: number, type: string) => ({ id, type, params: {}, x: id * 100, y: id * 10 }) as unknown as GraphNode;
  const cable = (fromNode: number, toNode: number) => ({ fromNode, fromPort: 0, toNode, toPort: 0 });
  const key = (c: ReturnType<typeof cable>) => `${c.fromNode}:0>${c.toNode}:0`;
  const graphOf = (nodes: GraphNode[], cables: ReturnType<typeof cable>[]) =>
    ({
      nodes: Object.fromEntries(nodes.map((n) => [String(n.id), n])),
      connections: Object.fromEntries(cables.map((c) => [key(c), c])),
    }) as unknown as TrackGraph;

  const plain = graphOf([node(1, 'a'), node(2, 'b')], [cable(1, 2)]);
  const looped = graphOf([node(1, 'a'), node(2, NODE_TYPES.feedback)], [cable(1, 2), cable(2, 1)]);

  it('needs a Feedback node to close', () => {
    expect(closesLoop(plain, cable(2, 1))).toBe(true);
    expect(closesLoop(plain, cable(1, 1))).toBe(true);
    expect(closesLoop(graphOf([node(1, 'a'), node(2, NODE_TYPES.feedback)], [cable(1, 2)]), cable(2, 1))).toBe(false);
  });

  it('finds the way back of a loop', () => {
    expect(isLoopBack(looped, cable(2, 1))).toBe(true);
    expect(isLoopBack(looped, cable(1, 2))).toBe(false);
    expect(isLoopBack(graphOf([node(1, NODE_TYPES.feedback), node(2, 'b')], [cable(1, 2)]), cable(1, 2))).toBe(false);
  });

  it('puts a new Feedback node under the cable', () => {
    expect(loopNodePosition(plain, cable(2, 1))).toEqual({ x: 150, y: 160 });
  });

  it('knows when a loop may ring for ever, in voice graphs too', () => {
    const above = { ...node(1, NODE_TYPES.feedback), params: { above: 1 } } as GraphNode;
    const inVoice = { ...node(2, NODE_TYPES.voiceInstrument), voice: graphOf([above], []) } as unknown as GraphNode;
    const project = (n: GraphNode) => ({ tracks: { 1: { graph: graphOf([n], []) } }, master: null }) as unknown as Project;
    expect(growingLoop(project(above))).toBe(true);
    expect(growingLoop(project(inVoice))).toBe(true);
    expect(growingLoop(project(node(3, NODE_TYPES.feedback)))).toBe(false);
  });
});

describe('parseParam', () => {
  const freq: ParamDescriptor = {
    id: 'f', name: 'Freq', min: 20, max: 20000, default: 1000, kind: 'continuous', unit: 'Hz', mid: 1000, choices: [],
    modulationSpan: 1, modulationPolarity: 'bipolar', modulatable: true, automatable: true,
  };
  const gain: ParamDescriptor = { ...freq, min: -24, max: 24, default: 0, unit: 'dB', mid: 0 };

  it('reads numbers with or without units, and k for thousands', () => {
    expect(parseParam(freq, '1500')).toBe(1500);
    expect(parseParam(freq, '1.5k')).toBe(1500);
    expect(parseParam(freq, '1.5 kHz')).toBe(1500);
    expect(parseParam(freq, ' 440 Hz')).toBe(440);
    expect(parseParam(gain, '-3,5 dB')).toBe(-3.5);
    expect(parseParam(gain, '+6')).toBe(6);
  });

  it('clamps to the range and rejects text that is not a number', () => {
    expect(parseParam(freq, '50k')).toBe(20000);
    expect(parseParam(gain, '-100')).toBe(-24);
    expect(parseParam(gain, 'loud')).toBeNull();
    expect(parseParam(gain, '')).toBeNull();
  });

  it('reads what the readouts show back: the minus sign and an infinite floor', () => {
    expect(parseParam(gain, '−12.0 dB')).toBe(-12);
    expect(parseParam(gain, '-inf')).toBe(-24);
    expect(parseParam(gain, '−∞ dB')).toBe(-24);
    expect(parseNumber('3.7', 0, 10, true)).toBe(4);
    expect(parseNumber('150 ms', 0, 100)).toBe(100);
  });
});

describe('note names in frequency fields (DS17)', () => {
  const cutoff = param({ min: 20, max: 20000, mid: 1000, unit: 'Hz' });
  const rate = param({ min: 0.01, max: 50, unit: 'Hz' });

  it('reads note names, sharps, flats and either case, C4 being MIDI 60 and A4 440 Hz', () => {
    expect(parseNote('A4')).toBeCloseTo(440, 6);
    expect(parseNote('a4')).toBeCloseTo(440, 6);
    expect(parseNote('C4')).toBeCloseTo(261.6256, 3);
    expect(parseNote('C#3')).toBeCloseTo(138.5913, 3);
    expect(parseNote('C♯3')).toBeCloseTo(138.5913, 3);
    expect(parseNote('Eb4')).toBeCloseTo(311.127, 3);
    expect(parseNote('E♭4')).toBeCloseTo(311.127, 3);
    expect(parseNote('bb3')).toBeCloseTo(233.0819, 3);
    expect(parseNote('B-1')).toBeCloseTo(15.4339, 3);
  });

  it('takes an offset in cents', () => {
    expect(parseNote('A4+1200')).toBeCloseTo(880, 6);
    expect(parseNote('A4 -100c')).toBeCloseTo(415.3047, 3);
    expect(parseNote('A4+20ct')).toBeCloseTo(440 * Math.pow(2, 20 / 1200), 6);
    expect(parseNote('A4 −50 cents')).toBeCloseTo(440 * Math.pow(2, -50 / 1200), 6);
  });

  it("isn't fooled by numbers and other words", () => {
    expect(parseNote('440')).toBeNull();
    expect(parseNote('1k')).toBeNull();
    expect(parseNote('H4')).toBeNull();
    expect(parseNote('A')).toBeNull();
  });

  it('parses note names only for audible frequencies, clamped to the range', () => {
    expect(parseParam(cutoff, 'A4')).toBeCloseTo(440, 6);
    expect(parseParam(cutoff, 'C-1')).toBe(20);
    expect(parseParam(cutoff, '1.5k')).toBe(1500);
    expect(parseParam(rate, 'A1')).toBeNull();
  });

  it('names the nearest note and how far off it is', () => {
    expect(noteOf(440)).toBe('A4 +0 ct');
    expect(noteOf(1000)).toBe('B5 +21 ct');
    expect(noteOf(430)).toBe('A4 −40 ct');
    expect(noteOf(261.6256)).toBe('C4 +0 ct');
    expect(noteOf(450)).toBe('A4 +39 ct');
    expect(noteHint(cutoff, 440)).toBe('A4 +0 ct');
    expect(noteHint(rate, 4)).toBeUndefined();
  });
});


describe('pan parameters', () => {
  const pan = { id: 'p3.pan', name: 'Pad 3 Pan', min: -100, max: 100, default: 0, kind: 'continuous', unit: '%' } as ParamDescriptor;
  it('read as the track pan does: C, L30, R30', () => {
    expect(formatParam(pan, 0)).toBe('C');
    expect(formatParam(pan, 0.3)).toBe('C');
    expect(formatParam(pan, -30)).toBe('L30');
    expect(formatParam(pan, 100)).toBe('R100');
    expect(formatParam({ ...pan, id: 'pan', unit: '' }, 25)).toBe('R25');
  });
  it('take C, L30 and R30 typed, as well as numbers', () => {
    expect(parseParam(pan, 'c')).toBe(0);
    expect(parseParam(pan, 'L30')).toBe(-30);
    expect(parseParam(pan, 'r 12.5')).toBe(12.5);
    expect(parseParam(pan, 'R200')).toBe(100);
    expect(parseParam(pan, '-40')).toBe(-40);
  });
  it('leave other parameters alone', () => {
    expect(formatParam({ ...pan, id: 'p3.span' }, 0)).toBe('0.0%');
  });
});
