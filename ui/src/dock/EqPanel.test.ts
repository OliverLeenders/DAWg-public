import { describe, expect, it } from 'vitest';
import { bandTypeForDoubleClick, bandTypeForDrag, edgeAt } from './EqPanel';

describe('EQ band creation', () => {
  it('puts the low and high edges under 100 Hz and over 5 kHz', () => {
    expect(edgeAt(20)).toBe('low');
    expect(edgeAt(99.9)).toBe('low');
    expect(edgeAt(100)).toBeNull();
    expect(edgeAt(5000)).toBeNull();
    expect(edgeAt(5000.1)).toBe('high');
    expect(edgeAt(20000)).toBe('high');
  });

  it('adds a bell in the middle, and a filter below 0 dB or a shelf above it at the edges', () => {
    expect(bandTypeForDoubleClick(1000, 6)).toBe('bell');
    expect(bandTypeForDoubleClick(1000, -6)).toBe('bell');
    expect(bandTypeForDoubleClick(50, -6)).toBe('hp');
    expect(bandTypeForDoubleClick(50, 6)).toBe('lowShelf');
    expect(bandTypeForDoubleClick(10000, -6)).toBe('lp');
    expect(bandTypeForDoubleClick(10000, 6)).toBe('highShelf');
  });

  it('always adds a shelf when dragging from 0 dB at the edges, and a bell in the middle', () => {
    expect(bandTypeForDrag(1000)).toBe('bell');
    expect(bandTypeForDrag(50)).toBe('lowShelf');
    expect(bandTypeForDrag(10000)).toBe('highShelf');
  });
});
