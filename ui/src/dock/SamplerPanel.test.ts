import { describe, expect, it } from 'vitest';
import type { SampleZone } from '../bridge/protocol';
import { parsePitch } from '../pianoroll/geometry';
import { keyName, zoneOnKey } from './SamplerPanel';

const zone = (id: number, lowKey: number, highKey: number) => ({ id, lowKey, highKey }) as SampleZone;

describe('the Sampler panel', () => {
  it('names keys with C4 at 60', () => {
    expect(keyName(60)).toBe('C4');
    expect(keyName(36)).toBe('C2');
    expect(keyName(61)).toBe('C#4');
  });

  it('finds the first zone holding a key, as the engine does', () => {
    const zones = [zone(1, 36, 36), zone(2, 0, 127)];
    expect(zoneOnKey(zones, 36)?.id).toBe(1);
    expect(zoneOnKey(zones, 40)?.id).toBe(2);
    expect(zoneOnKey([zone(1, 36, 36)], 40)).toBeUndefined();
  });
  it('reads back every key name it shows, and names a key by the whole key', () => {
    for (let key = 0; key <= 127; key++) expect(parsePitch(keyName(key))).toBe(key);
    expect(parsePitch('c#4')).toBe(61);
    expect(parsePitch('64')).toBe(64);
    expect(keyName(60.25)).toBe('C4');
  });
});
