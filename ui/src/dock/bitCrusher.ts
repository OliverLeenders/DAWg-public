// The Bit Crusher's curves for its panel (docs/design/bit-crusher.md, section 4): the quantiser, the Note rate and the
// sample-and-hold, the same formulas as the engine's (engine/src/EffectNodes.cpp, BitCrusherNode), so the graph
// follows the knobs without a round trip. The DC blocker, the filters, dither and jitter aren't drawn.

/** At this depth the quantiser is off. */
export const MAX_BITS = 24;

/** The Note rate's 0 semitones: C4. */
const C4 = 261.6255653005986;

/** The test tone the scope draws, in Hz, and its level before Drive. */
export const SCOPE_HZ = 220;
const SCOPE_LEVEL = 0.8;

/** Quantises x to a depth: mid-tread levels k / M (0 among them, -1 to 1) or mid-rise (k + 1/2) / M, M = 2^(bits - 1). */
export function quantise(x: number, bits: number, midRise: boolean): number {
  if (bits >= MAX_BITS) return x;
  const m = 2 ** (Math.max(bits, 1) - 1);
  if (midRise) {
    const top = Math.floor(m - 0.5);
    return (Math.min(top, Math.max(-top - 1, Math.floor(x * m))) + 0.5) / m;
  }
  const top = Math.floor(m);
  // The engine rounds halves away from zero (std::round).
  const k = Math.sign(x * m) * Math.round(Math.abs(x * m));
  return Math.min(top, Math.max(-top, k)) / m;
}

/** How many levels the quantiser has at a depth; Infinity when it's off. */
export function levelCount(bits: number, midRise: boolean): number {
  if (bits >= MAX_BITS) return Infinity;
  const m = 2 ** (Math.max(bits, 1) - 1);
  return midRise ? 2 * (Math.floor(m - 0.5) + 1) : 2 * Math.floor(m) + 1;
}

/** The Note mode's rate for semitones from C4. */
export function noteRate(semitones: number): number {
  return C4 * 2 ** (semitones / 12);
}

/** The hold's rate in Hz for the node's parameters. */
export function holdRate(params: Record<string, number>): number {
  return params.rateMode >= 0.5 ? noteRate(params.note) : params.rate;
}

/** What the quantiser sees for an input: Drive, the soft clip if chosen, then the bias in half-steps. */
function beforeQuantiser(x: number, params: Record<string, number>): number {
  let v = x * 10 ** (params.drive / 20);
  if (params.clip >= 0.5) v = Math.tanh(v);
  if (params.bits < MAX_BITS) v += ((params.bias / 100) * 0.5) / 2 ** (params.bits - 1);
  return v;
}

/** The static transfer curve: output for an input from -1 to 1, through Drive, clip, bias and the quantiser. */
export function transfer(x: number, params: Record<string, number>): number {
  return quantise(beforeQuantiser(x, params), params.bits, params.quantiser >= 0.5);
}

/** How far below 1 the phase may be and still take a sample, so that whole ratios stay whole (the engine's). */
const PHASE_TOLERANCE = 1e-9;

/**
 * The hold's sample instants over a run of samples, as the engine takes them: a phase that starts at 1 and gains
 * rate / sampleRate per sample; a sample is taken whenever it has reached 1, which then comes off it.
 */
export function holdInstants(rate: number, sampleRate: number, length: number): boolean[] {
  const increment = rate / sampleRate;
  const result: boolean[] = [];
  let phase = 1;
  for (let i = 0; i < length; i++) {
    const take = phase >= 1 - PHASE_TOLERANCE;
    if (take) phase = Math.min(phase - 1, 1);
    phase += increment;
    result.push(take);
  }
  return result;
}

/** One cycle of the scope's test tone, dry and crushed (held and quantised), one value per sample. */
export function scope(params: Record<string, number>, sampleRate: number): { dry: number[]; wet: number[] } {
  const length = Math.max(2, Math.round(sampleRate / SCOPE_HZ));
  const takes = holdInstants(holdRate(params), sampleRate, length);
  const dry: number[] = [];
  const wet: number[] = [];
  let held = 0;
  for (let i = 0; i < length; i++) {
    const x = SCOPE_LEVEL * Math.sin((2 * Math.PI * i) / length);
    if (takes[i]) held = transfer(x, params);
    dry.push(x);
    wet.push(held);
  }
  return { dry, wet };
}

/** The rate as a readout: "11.0 kHz · ÷4.36", or "Full rate" at or above the sample rate. */
export function rateText(rate: number, sampleRate: number): string {
  if (rate >= sampleRate) return 'Full rate';
  const hz = rate >= 1000 ? `${(rate / 1000).toFixed(rate >= 10000 ? 1 : 2)} kHz` : `${rate.toFixed(rate >= 100 ? 0 : 1)} Hz`;
  return `${hz} · ÷${(sampleRate / rate).toFixed(2)}`;
}

/** The depth as a readout: "8.0 bits · 257 levels", or "Bits off" at the top. */
export function bitsText(bits: number, midRise: boolean): string {
  if (bits >= MAX_BITS) return 'Bits off';
  return `${bits.toFixed(1)} bits · ${levelCount(bits, midRise)} levels`;
}
