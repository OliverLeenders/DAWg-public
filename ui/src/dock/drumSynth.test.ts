import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type DrumPad, type GraphNode, type ParamDescriptor, type Track } from '../bridge/protocol';
import { paramApplies } from '../graph/lfo';
import { drumSynthMap, explicitDrumMode } from '../store/drumMap';
import {
  DRUM_PAD_KEYS,
  drumOutputsShown,
  drumParamId,
  drumParamLabel,
  freeOutputs,
  padEnvelopes,
  padOfKey,
  padParams,
  padsHit,
  padsOf,
  parseDrumParam,
  shortParamName,
} from './drumSynth';
import { padKeys } from './samplerLayout';

// The Drum Synth's panel helpers (docs/design/drum-synth.md, DR12, step 5).

const drumNode = (pads?: DrumPad[], params: Record<string, number> = {}) =>
  ({ id: 9, type: NODE_TYPES.drumSynth, params, pads }) as unknown as GraphNode;

const param = (id: string, name: string, def = 0): ParamDescriptor =>
  ({ id, name, min: 0, max: 100, default: def, kind: 'continuous', unit: '', mid: 0, choices: [] }) as unknown as ParamDescriptor;

const trackWith = (node: GraphNode, drumMode: Track['drumMode'] = 'auto') =>
  ({ id: 1, name: 'Track 1', drumMode, graph: { nodes: { [String(node.id)]: node }, connections: {} } }) as unknown as Track;

describe('the Drum Synth panel', () => {
  it('lays its pads out as the Sampler does, keys 36 to 51', () => {
    expect(DRUM_PAD_KEYS).toEqual(padKeys());
    expect(Math.min(...DRUM_PAD_KEYS)).toBe(36);
    expect(Math.max(...DRUM_PAD_KEYS)).toBe(51);
    expect(padOfKey(38)).toBe(2);
    expect(padOfKey(35)).toBe(-1);
    expect(padOfKey(52)).toBe(-1);
  });

  it('plays the default kit until it has pads of its own', () => {
    const pads = padsOf(drumNode());
    expect(pads).toHaveLength(16);
    expect(pads[0]).toMatchObject({ name: 'Kick', model: 'kick', mono: true });
    expect(pads[6]).toMatchObject({ name: 'Closed hat', model: 'hat', choke: 1 });
    expect(pads[15].model).toBe('cymbal');
  });

  it('reads and writes pad parameter ids', () => {
    expect(drumParamId(2, 'snappy', 'snare')).toBe('p3.snare.snappy');
    expect(drumParamId(0, 'level')).toBe('p1.level');
    expect(parseDrumParam('p3.snare.snappy')).toEqual({ pad: 2, model: 'snare', id: 'snappy' });
    expect(parseDrumParam('p16.decay')).toEqual({ pad: 15, model: undefined, id: 'decay' });
    expect(parseDrumParam('p17.decay')).toBeNull();
    expect(parseDrumParam('gain')).toBeNull();
  });

  it("shows a pad's common parameters and its model's, nothing of a pad that is off", () => {
    const pads = padsOf(drumNode()).map((p, i) => (i === 1 ? { ...p, model: 'off' as const } : p));
    const node = drumNode(pads);
    expect(paramApplies(node, 'p3.level')).toBe(true);
    expect(paramApplies(node, 'p3.snare.snappy')).toBe(true);
    expect(paramApplies(node, 'p3.tom.pitch')).toBe(false);
    expect(paramApplies(node, 'p2.level')).toBe(false);
    expect(paramApplies(node, 'p2.rim.pitch')).toBe(false);
  });

  it('names a parameter after its pad, without the descriptor prefix', () => {
    const pads = padsOf(drumNode()).map((p, i) => (i === 2 ? { ...p, name: 'Crack' } : p));
    const snappy = param('p3.snare.snappy', 'Pad 3 Snare Snappy');
    const level = param('p3.level', 'Pad 3 Level');
    expect(shortParamName(snappy)).toBe('Snappy');
    expect(shortParamName(level)).toBe('Level');
    expect(drumParamLabel(drumNode(pads), snappy)).toBe('Crack Snappy');
    expect(shortParamName(param('gain', 'Gain'))).toBe('Gain');
  });

  it("lists a pad's parameters in order: the common ones, then its model's", () => {
    const params = [
      param('p1.level', 'Pad 1 Level'),
      param('p1.decay', 'Pad 1 Decay'),
      param('p1.kick.pitch', 'Pad 1 Kick Pitch'),
      param('p1.snare.pitch', 'Pad 1 Snare Pitch'),
      param('p2.level', 'Pad 2 Level'),
      param('p1.kick.sweep', 'Pad 1 Kick Sweep'),
    ];
    const { common, own } = padParams(params, 0, 'kick');
    expect(common.map((p) => p.id)).toEqual(['p1.level', 'p1.decay']);
    expect(own.map((p) => p.id)).toEqual(['p1.kick.pitch', 'p1.kick.sweep']);
  });

  it("draws the envelope from the pad's sweep, hold and decay", () => {
    const values: Record<string, number> = { decay: 200, hold: 100, sweep: 24, sweepTime: 40 };
    const { level, pitch, decay, sweep } = padEnvelopes('kick', (id) => values[id] ?? 0, 2, 2001);
    expect(sweep).toBe(24);
    expect(decay).toBeCloseTo(0.1 + 0.9, 6); // hold, then 0.45 s at 200 %
    const at = (points: Array<{ t: number; v: number }>, t: number) => points[Math.round(t * 1000)].v;
    expect(at(level, 0.05)).toBe(1); // in the hold
    expect(at(level, 1.0)).toBeCloseTo(1e-3, 6); // -60 dB at hold + decay
    expect(at(pitch, 0)).toBe(24);
    expect(at(pitch, 0.04)).toBeCloseTo(24 / Math.E, 4);

    // A hat has no sweep, and its attack ramps up.
    const hat = padEnvelopes('hat', (id) => ({ decay: 100, attack: 10 })[id] ?? 0, 0.2, 201);
    expect(hat.sweep).toBe(0);
    expect(hat.level[5].v).toBeCloseTo(0.5, 6);
    expect(hat.wash).toEqual([]);

    // A cymbal's wash rises over 10 ms plus half the Swell, to the Wash's height.
    const crash = padEnvelopes('cymbal', (id) => ({ decay: 100, ride: 0, wash: 40, swell: 20 })[id] ?? 0, 1, 1001);
    expect(crash.decay).toBeCloseTo(2.5, 6);
    expect(crash.wash[0].v).toBe(0);
    expect(crash.wash[20].v).toBeGreaterThan(0.3);
    expect(crash.wash[20].v).toBeLessThan(0.4);
    expect(crash.wash[500].v).toBeLessThan(0.1 * crash.wash[20].v + 0.1);
  });

  it('flashes the pads whose hit counts went up', () => {
    expect([...padsHit([0, 2, 0], [1, 2, 0])]).toEqual([0]);
    expect(padsHit(undefined, [3, 3]).size).toBe(0); // the first counts are no hit
    expect(padsHit([1], undefined).size).toBe(0);
  });

  it('counts the outputs still free for pads of their own', () => {
    const pads = padsOf(drumNode()).map((p, i) => (i === 4 ? { ...p, output: 3 } : p));
    expect(freeOutputs(padsOf(drumNode()), [])).toBe(15);
    expect(freeOutputs(pads, [])).toBe(14);
    expect(freeOutputs(pads, [2, 5, 0])).toBe(13); // port 2 is Out 3, already counted; Main isn't an extra output
  });

  it('shows outputs up to the highest a pad plays from or a cable leaves', () => {
    const pads = padsOf(drumNode()).map((p, i) => (i === 4 ? { ...p, output: 3 } : p));
    expect(drumOutputsShown(drumNode(), [])).toBe(1);
    expect(drumOutputsShown(drumNode(pads), [])).toBe(3);
    expect(drumOutputsShown(drumNode(pads), [5])).toBe(6);
  });

  it("makes a Drum Synth's track a drum track named by its pads", () => {
    const pads = padsOf(drumNode()).map((p, i) => (i === 2 ? { ...p, name: 'Crack' } : i === 1 ? { ...p, model: 'off' as const } : p));
    const track = trackWith(drumNode(pads));
    expect(explicitDrumMode(track)).toBe(true);
    expect(explicitDrumMode(trackWith(drumNode(pads), 'melodic'))).toBe(false);

    const map = drumSynthMap(track)!;
    expect(map.pieces[38]).toMatchObject({ name: 'Crack', role: 'snare', limb: 'left_hand' });
    expect(map.pieces[37]).toBeUndefined();
    expect(map.pieces[46].role).toBe('hihat_open');
    // A pad playing another kind of drum than its key's takes its model's role.
    const kickOnRide = padsOf(drumNode()).map((p, i) => (i === 15 ? { ...p, model: 'kick' as const } : p));
    expect(drumSynthMap(trackWith(drumNode(kickOnRide)))!.pieces[51].role).toBe('kick');

    const plain = { ...track, graph: { nodes: {}, connections: {} } } as unknown as Track;
    expect(drumSynthMap(plain)).toBeUndefined();
    expect(explicitDrumMode(plain)).toBeUndefined();
  });
});
