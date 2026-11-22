// The compressor panel's level history (docs/design/phase11-faust-panels.md, P4): the engine's 10 ms slots of
// [input, output, reduction, key] in dB (key: the detector's level; docs/design/sidechain.md, K8), kept for the longest span the graph shows. When no slots come in (the node is
// bypassed or the engine stopped), silent slots keep the graph scrolling at the wall-clock rate.

export const SLOT_MS = 10;
export const SPANS = [2, 4, 8] as const;
export type Span = (typeof SPANS)[number];

/** How long without slots before silence is added. */
const STALE_MS = 100;
const SILENCE: readonly [number, number, number, number] = [-180, -180, 0, -180];
const STRIDE = 4;

export class LevelHistory {
  readonly capacity: number;
  private readonly data: Float32Array;
  private next = 0;
  private count = 0;
  /** When the newest slot ended (ms), or null before the first. */
  private clock: number | null = null;

  constructor(seconds: number = SPANS[SPANS.length - 1]) {
    this.capacity = Math.round((seconds * 1000) / SLOT_MS);
    this.data = new Float32Array(this.capacity * STRIDE);
  }

  get length() {
    return this.count;
  }

  /** Appends slots given as [in, out, gr, key, in, out, gr, key, ...], received at `now`. */
  push(flat: readonly number[], now: number) {
    for (let i = 0; i + STRIDE - 1 < flat.length; i += STRIDE) this.append(flat[i], flat[i + 1], flat[i + 2], flat[i + 3]);
    if (flat.length >= STRIDE) this.clock = now;
  }

  /** Adds silence for the time since the last slot once that is over STALE_MS. Returns whether any was added. */
  pad(now: number) {
    if (this.clock === null) {
      this.clock = now;
      return false;
    }
    if (now - this.clock < STALE_MS) return false;
    const slots = Math.min(this.capacity, Math.floor((now - this.clock) / SLOT_MS));
    for (let i = 0; i < slots; i++) this.append(...SILENCE);
    this.clock = slots === this.capacity ? now : this.clock + slots * SLOT_MS;
    return slots > 0;
  }

  /** The slot `age` slots before the newest (0 is the newest) as [in, out, gr, key]. */
  at(age: number): [number, number, number, number] {
    const i = ((((this.next - 1 - age) % this.capacity) + this.capacity) % this.capacity) * STRIDE;
    return [this.data[i], this.data[i + 1], this.data[i + 2], this.data[i + 3]];
  }

  private append(input: number, output: number, reduction: number, key: number) {
    const i = this.next * STRIDE;
    this.data[i] = input;
    this.data[i + 1] = output;
    this.data[i + 2] = reduction;
    this.data[i + 3] = key;
    this.next = (this.next + 1) % this.capacity;
    this.count = Math.min(this.capacity, this.count + 1);
  }
}
