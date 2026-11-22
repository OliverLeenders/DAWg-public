import { describe, expect, it } from 'vitest';
import type { PortDescriptor } from '../bridge/protocol';
import { isFolded, isMono, portChannels, portTitle, PORT_WIDTHS } from './widths';

const audio = (channels?: number): PortDescriptor => ({ type: 'audio', name: 'Audio', channels });

describe('port widths', () => {
  it('takes the compiled width when the node was compiled with a mono port', () => {
    const widths = { inputs: [1, 0], outputs: [2], folded: [] };
    expect(isMono(audio(0), 'in', 0, widths)).toBe(true);
    expect(isMono(audio(0), 'out', 0, widths)).toBe(false);
  });

  it('shows a port the compilation knows nothing of as declared', () => {
    // A component's entry lists only its mono inputs; its outputs show as declared.
    const component = { inputs: [1, 0], outputs: [], folded: [0] };
    expect(isMono(audio(2), 'in', 1, component)).toBe(false);
    expect(isMono(audio(1), 'out', 0, component)).toBe(true);
  });

  it('steps a component port through stereo, mono and adaptive', () => {
    expect(PORT_WIDTHS.map((w) => w.channels)).toEqual([2, 1, 0]);
    expect(portChannels(audio())).toBe(2);
    expect(portChannels(audio(0))).toBe(0);
  });

  it('shows a node the compilation left out as all stereo, and one it never compiled as declared', () => {
    expect(isMono(audio(0), 'in', 0, undefined)).toBe(false);
    expect(isMono(audio(1), 'in', 0, undefined)).toBe(true);
    expect(isMono(audio(2), 'in', 0, undefined)).toBe(false);
  });

  it('never calls a port that is not audio mono', () => {
    expect(isMono({ type: 'modulation', name: 'Mod' }, 'in', 1, { inputs: [1, 1], outputs: [], folded: [] })).toBe(false);
  });

  it('marks folding inputs and names widths in tooltips', () => {
    expect(isFolded(0, { inputs: [1], outputs: [1], folded: [0] })).toBe(true);
    expect(isFolded(0, undefined)).toBe(false);
    expect(portTitle(audio(), true)).toBe('Audio: Mono');
    expect(portTitle({ type: 'events', name: 'Notes' }, false)).toBe('Notes');
  });
});
