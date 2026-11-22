import { describe, expect, it } from 'vitest';
import type { SampleZone } from '../bridge/protocol';
import { dragEnvelope, envelopeLayout, envelopeSegments, handleFields, moveMarker, padKeys, waveMarkers } from './samplerLayout';

const zone = (fields: Partial<SampleZone> = {}) =>
  ({ start: 0, end: 0, loopStart: 0, loopEnd: 0, mode: 'oneShot', attackMs: 0, decayMs: 0, sustain: 1, releaseMs: 50, ...fields }) as SampleZone;

describe('the pads', () => {
  it('rise from the bottom left, as on a drum machine', () => {
    const keys = padKeys();
    expect(keys.slice(0, 4)).toEqual([48, 49, 50, 51]);
    expect(keys.slice(12)).toEqual([36, 37, 38, 39]);
  });
});

describe('the waveform markers', () => {
  it('show the file end for an end of 0, and loop markers only for a loop', () => {
    expect(waveMarkers(zone(), 100).map((m) => [m.marker, m.frame])).toEqual([['start', 0], ['end', 100]]);
    expect(waveMarkers(zone({ mode: 'loop', loopStart: 10 }), 100).map((m) => m.frame)).toEqual([0, 100, 10, 100]);
  });

  it('stay in order', () => {
    const looped = zone({ mode: 'loop', start: 10, end: 90, loopStart: 30, loopEnd: 60 });
    expect(moveMarker(looped, 100, 'start', 50)).toEqual({ start: 29 });
    expect(moveMarker(looped, 100, 'end', 20)).toEqual({ end: 61 });
    expect(moveMarker(looped, 100, 'loopStart', 5)).toEqual({ loopStart: 10 });
    expect(moveMarker(looped, 100, 'loopEnd', 95)).toEqual({ loopEnd: 90 });
    expect(moveMarker(zone(), 100, 'end', 500)).toEqual({ end: 100 });
  });
});

describe('the envelope', () => {
  it('has no release on a one-shot, and starts high with no attack', () => {
    expect(envelopeLayout(zone()).curve[0]).toEqual([0, 1]);
    expect(envelopeLayout(zone({ sustain: 0.5 })).curve[4]).toEqual([1, 0.5]);
    expect(envelopeLayout(zone({ sustain: 0.5, mode: 'gate' })).curve[4]).toEqual([1, 0]);
  });

  it('keeps its points apart, even with no attack or decay', () => {
    const { points } = envelopeLayout(zone());
    expect(points.decay[0] - points.attack[0]).toBeGreaterThan(0.05);
    expect(points.release[0] - points.decay[0]).toBeGreaterThan(0.05);
  });

  it('turns drags into times and a level', () => {
    expect(dragEnvelope(zone(), 'attack', 1000, 0.1, 0.5)).toEqual({ attackMs: 100 });
    expect(dragEnvelope(zone({ sustain: 0.5 }), 'decay', 1000, 0.2, 0.25)).toEqual({ decayMs: 200, sustain: 0.75 });
    expect(dragEnvelope(zone(), 'release', 1000, -0.1, -0.5)).toEqual({ releaseMs: 150, sustain: 0.5 });
    expect(dragEnvelope(zone(), 'attack', 1000, -1, 0)).toEqual({ attackMs: 0 });
  });
});

describe('the envelope tension points', () => {
  it('sit on each sloped segment, straight by default, with no release on a one-shot', () => {
    const gated = zone({ mode: 'gate', attackMs: 100, decayMs: 200, sustain: 0.5, releaseMs: 300 });
    const segments = envelopeSegments(gated, envelopeLayout(gated));
    expect(segments.attack).toMatchObject({ x0: 0, from: 0, to: 1, handle: { time: 0.5, level: 0.5 }, shown: true });
    expect(segments.decay).toMatchObject({ from: 1, to: 0.5, shown: true });
    expect(segments.release).toMatchObject({ from: 0.5, to: 0, x1: 1, shown: true });
    expect(segments.attack.x1).toBe(segments.decay.x0);

    const shot = zone({ attackMs: 0, sustain: 0.5 });
    const flat = envelopeSegments(shot, envelopeLayout(shot));
    expect(flat.release.shown).toBe(false);
    // No attack: the curve starts at full level, so the attack is flat too.
    expect(flat.attack.from).toBe(1);
  });

  it("read the zone's handles and write them back clamped", () => {
    const bent = zone({ attackHandleTime: 0.2, attackHandleLevel: 0.9 });
    expect(envelopeSegments(bent, envelopeLayout(bent)).attack.handle).toEqual({ time: 0.2, level: 0.9 });
    expect(handleFields('decay', 1.4, -0.2)).toEqual({ decayHandleTime: 1, decayHandleLevel: 0 });
    expect(handleFields('release', 0.5, 0.5)).toEqual({ releaseHandleTime: 0.5, releaseHandleLevel: 0.5 });
  });
});
