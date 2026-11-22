import { describe, expect, it } from 'vitest';
import type { ParamDescriptor } from '../bridge/protocol';
import { formatParam, fromPosition, toPosition } from '../graph/params';
import { computeFilterModulation } from './FilterPanel';

const filterSlopeParam: ParamDescriptor = {
  id: 'slope',
  name: 'Slope',
  min: 0,
  max: 3,
  default: 0,
  kind: 'choice',
  unit: '',
  mid: 0,
  choices: ['12 dB/oct', '24 dB/oct', '36 dB/oct', '48 dB/oct'],
  modulationSpan: 3,
  modulationPolarity: 'bipolar',
  modulatable: false,
  automatable: true,
};

describe('filter slopes', () => {
  it('formats slope choices by index', () => {
    expect(formatParam(filterSlopeParam, 0)).toBe('12 dB/oct');
    expect(formatParam(filterSlopeParam, 1)).toBe('24 dB/oct');
    expect(formatParam(filterSlopeParam, 2)).toBe('36 dB/oct');
    expect(formatParam(filterSlopeParam, 3)).toBe('48 dB/oct');
  });

  it('maps control positions to discrete slope choice indices', () => {
    expect(fromPosition(filterSlopeParam, 0.0)).toBe(0);
    expect(fromPosition(filterSlopeParam, 0.33)).toBe(1);
    expect(fromPosition(filterSlopeParam, 0.66)).toBe(2);
    expect(fromPosition(filterSlopeParam, 1.0)).toBe(3);

    expect(toPosition(filterSlopeParam, 0)).toBeCloseTo(0);
    expect(toPosition(filterSlopeParam, 1)).toBeCloseTo(1 / 3);
    expect(toPosition(filterSlopeParam, 2)).toBeCloseTo(2 / 3);
    expect(toPosition(filterSlopeParam, 3)).toBeCloseTo(1);
  });
});

describe('filter modulation detection', () => {
  const base = { cutoff: 1000, resonance: 20, drive: 0, slope: 0, mode: 0 };

  it('detects unmodulated state when no modulation or automation is active', () => {
    const result = computeFilterModulation({
      baseParams: base,
      meters: { cutoff: 1000, resonance: 20, drive: 0, slope: 0, mode: 0, hasModulationInput: false, isModulated: false },
    });
    expect(result.isModulated).toBe(false);
    expect(result.activeParams.cutoff).toBe(1000);
  });

  it('detects automation modulation when cutoff is automated', () => {
    const result = computeFilterModulation({
      baseParams: base,
      automatedParams: { cutoff: 3500 },
    });
    expect(result.isModulated).toBe(true);
    expect(result.activeParams.cutoff).toBe(3500);
  });

  it('detects real-time LFO or ADSR modulation from audio-thread meters', () => {
    const result = computeFilterModulation({
      baseParams: base,
      meters: {
        cutoff: 2400,
        resonance: 45,
        drive: 0,
        slope: 0,
        mode: 0,
        hasModulationInput: true,
        isModulated: true,
      },
    });
    expect(result.isModulated).toBe(true);
    expect(result.activeParams.cutoff).toBe(2400);
    expect(result.activeParams.resonance).toBe(45);
  });

  it('detects active modulation slot even if audio meters have not yet arrived', () => {
    const result = computeFilterModulation({
      baseParams: base,
      automatedParams: { resonance: 60 },
      hasModulationSlots: true,
    });
    expect(result.isModulated).toBe(true);
    expect(result.activeParams.resonance).toBe(60);
  });

  it('does not flag modulation if modulated parameter matches base setting', () => {
    const result = computeFilterModulation({
      baseParams: base,
      automatedParams: { cutoff: 1000 },
      hasModulationSlots: false,
    });
    expect(result.isModulated).toBe(false);
  });
});
