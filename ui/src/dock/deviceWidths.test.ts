import { describe, expect, it } from 'vitest';
import { NODE_TYPES, type GraphNode } from '../bridge/protocol';
import { clampDeviceWidth, deviceWidth, widthKey } from './deviceWidths';

describe('device widths', () => {
  it('start at least as wide as their minimum', () => {
    for (const type of Object.values(NODE_TYPES)) {
      const width = deviceWidth(type, 6);
      expect(width.initial).toBeGreaterThanOrEqual(width.min);
      expect(width.min).toBeGreaterThan(0);
    }
  });

  it('give what has its own window a narrow card', () => {
    expect(deviceWidth(NODE_TYPES.pluginEffect, 0).initial).toBe(deviceWidth(NODE_TYPES.pluginInstrument, 0).initial);
    expect(deviceWidth(NODE_TYPES.pluginEffect, 0).initial).toBeLessThan(deviceWidth(NODE_TYPES.eq, 0).initial);
  });

  it('give a panel of knobs room for them in two rows', () => {
    expect(deviceWidth(NODE_TYPES.mix, 8).min).toBeGreaterThan(deviceWidth(NODE_TYPES.mix, 2).min);
    expect(deviceWidth(NODE_TYPES.mix, 3).min).toBe(deviceWidth(NODE_TYPES.mix, 4).min);
  });

  it('keep a remembered width to what the device can use, however wide', () => {
    const eq = deviceWidth(NODE_TYPES.eq, 0);
    expect(clampDeviceWidth(10, eq)).toBe(eq.min);
    expect(clampDeviceWidth(1234.4, eq)).toBe(1234);
  });

  it('are remembered per component, Faust script and plugin, and per type for built-in devices', () => {
    const node = (fields: Partial<GraphNode>) => ({ id: 1, type: NODE_TYPES.eq, params: {}, ...fields }) as GraphNode;
    const component = (name: string) => node({ type: NODE_TYPES.component, component: { name, graph: { nodes: {}, connections: {} }, inputs: [], outputs: [], exposed: [] } as never });
    expect(widthKey(component('Resonator'))).not.toBe(widthKey(component('Big bus')));
    expect(widthKey(component('Resonator'))).toBe(widthKey({ ...component('Resonator'), id: 7 }));
    expect(widthKey(node({ type: NODE_TYPES.faust, code: 'declare name "Fold";' }))).toBe(`${NODE_TYPES.faust}:Fold`);
    expect(widthKey(node({ type: NODE_TYPES.pluginEffect, plugin: { identifier: 'vst3:abc', name: 'X', manufacturer: '', format: 'VST3' } }))).toBe('plugin:vst3:abc');
    expect(widthKey(node({ type: NODE_TYPES.eq }))).toBe(NODE_TYPES.eq);
  });

  it('give a component room for its header and grow with its pinned controls', () => {
    expect(deviceWidth(NODE_TYPES.component, 3).initial).toBe(340);
    expect(deviceWidth(NODE_TYPES.component, 12).initial).toBeGreaterThan(340);
  });
});
