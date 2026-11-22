import { describe, expect, it } from 'vitest';
import type { ParamDescriptor, ParamLink, Project } from '../bridge/protocol';
import { groupByNodes, linkDomain, linkDriving, linksFrom, linksOfTrack, mapLinkValue } from './links';

// The UI's copy of the link arithmetic (docs/design/parameter-linking.md, L2) agrees with engine/src/ParamLinks.cpp,
// as LinkTests checks the engine's.

const param = (fields: Partial<ParamDescriptor>): ParamDescriptor => ({
  id: 'p',
  name: 'P',
  min: 0,
  max: 1,
  default: 0,
  kind: 'continuous',
  unit: '',
  mid: 0,
  choices: [],
  modulationSpan: 0,
  modulationPolarity: 'bipolar',
  modulatable: true,
  automatable: true,
  ...fields,
});

const gain = param({ id: 'mid1Gain', min: -24, max: 24, unit: 'dB' });
const freq = param({ id: 'mid1Freq', min: 20, max: 20000, default: 1000, unit: 'Hz', mid: 1000 });
const q = param({ id: 'mid1Q', min: 0.1, max: 10, default: 0.7, mid: 1 });

describe('link mappings', () => {
  it('invert a gain around 0 dB and keep a frequency', () => {
    expect(mapLinkValue({ kind: 'inverted' }, gain, gain, 6)).toBeCloseTo(-6);
    expect(mapLinkValue({ kind: 'identity' }, freq, freq, 3000)).toBeCloseTo(3000);
  });

  it('move a frequency in octaves and scale around the middle', () => {
    expect(linkDomain(freq, freq)).toBe('octaves');
    expect(mapLinkValue({ kind: 'offset', amount: 1 }, freq, freq, 1000)).toBeCloseTo(2000);
    expect(mapLinkValue({ kind: 'scaled', amount: 0.5 }, gain, gain, 12)).toBeCloseTo(6);
  });

  it('clamp to the target and work in normalised terms between other units', () => {
    expect(mapLinkValue({ kind: 'offset', amount: 30 }, gain, gain, 6)).toBeCloseTo(24);
    expect(linkDomain(gain, q)).toBe('normalised');
    expect(linkDomain(gain, undefined)).toBe('normalised');
    expect(mapLinkValue({ kind: 'inverted' }, undefined, undefined, 0.8)).toBeCloseTo(0.2);
  });

  it('map a falling range', () => {
    const range = { kind: 'range' as const, inMin: 0.5, inMax: 1, outMin: 1, outMax: 0 };
    expect(mapLinkValue(range, gain, undefined, 0)).toBeCloseTo(1);
    expect(mapLinkValue(range, gain, undefined, 12)).toBeCloseTo(0.5);
  });
});

describe('finding links', () => {
  const link = (id: number, sourceNode: number, targetNode: number, targetTrack = 1): ParamLink => ({
    id,
    source: { trackId: 1, nodeId: sourceNode, param: 'mid1Gain' },
    target: { trackId: targetTrack, nodeId: targetNode, param: 'mid1Gain' },
    mapping: { kind: 'inverted' },
    active: true,
  });
  const project = { links: [link(1, 10, 20), link(2, 10, 30, 2), link(3, 10, 20)] } as unknown as Project;

  it('finds what drives a parameter and what it drives', () => {
    expect(linkDriving(project, { trackId: 2, nodeId: 30, param: 'mid1Gain' })?.id).toBe(2);
    expect(linkDriving(project, { trackId: 1, nodeId: 10, param: 'mid1Gain' })).toBeUndefined();
    expect(linksFrom(project, { trackId: 1, nodeId: 10, param: 'mid1Gain' })).toHaveLength(3);
    expect(linksOfTrack(project, 2).map((l) => l.id)).toEqual([2]);
  });

  it('groups links by the devices they join', () => {
    expect(groupByNodes(project.links).map((g) => g.map((l) => l.id))).toEqual([[1, 3], [2]]);
  });
});
