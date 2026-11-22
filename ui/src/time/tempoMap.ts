import { TICKS_PER_BEAT, type TempoPoint, type TimeSignature } from '../bridge/protocol';

// The song's musical time (docs/design/tempo-map.md, TM3): the TypeScript twin of dawg::TempoMap
// (engine/include/dawg/Time.h). Real time from tempo points with steps and ramps, bars from time-signature changes.
// The engine normalises both lists, so this takes them as they come from the model.

export interface BarPosition {
  /** 0-based. */
  bar: number;
  /** 0-based, in the signature's beat unit (eighths in 7/8). */
  beat: number;
  tickInBeat: number;
}

export const beatTicks = (s: TimeSignature) => (TICKS_PER_BEAT * 4) / s.denominator;
export const barTicks = (s: TimeSignature) => beatTicks(s) * s.numerator;

/** Seconds over dt ticks from a segment's start, at start tempo b0 changing by k BPM per tick. */
function segmentSeconds(b0: number, k: number, dt: number) {
  if (Math.abs(k * dt) < 1e-9 * b0) return (dt * 60) / (b0 * TICKS_PER_BEAT);
  return (60 / (k * TICKS_PER_BEAT)) * Math.log1p((k * dt) / b0);
}

/** The inverse: ticks a segment covers in ds seconds from its start. */
function segmentTicks(b0: number, k: number, ds: number) {
  const x = (ds * k * TICKS_PER_BEAT) / 60;
  if (Math.abs(x) < 1e-12) return (ds * b0 * TICKS_PER_BEAT) / 60;
  return (b0 * Math.expm1(x)) / k;
}

/** The last index whose key is at or before value (0 when none is). */
function lastAtOrBefore(keys: number[], value: number) {
  let lo = 0;
  let hi = keys.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (keys[mid] <= value) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

export class TempoMap {
  readonly points: TempoPoint[];
  readonly signatures: TimeSignature[];
  private readonly startSeconds: number[] = [0];
  private readonly pointTicks: number[];
  private readonly signatureTicks: number[] = [0];
  private readonly signatureBars: number[];

  constructor(points: TempoPoint[] = [], signatures: TimeSignature[] = []) {
    this.points = points.length > 0 ? points : [{ tick: 0, bpm: 120, ramp: false }];
    this.signatures = signatures.length > 0 ? signatures : [{ bar: 0, numerator: 4, denominator: 4 }];
    this.pointTicks = this.points.map((p) => p.tick);
    this.signatureBars = this.signatures.map((s) => s.bar);
    for (let i = 1; i < this.points.length; i++) {
      const dt = this.points[i].tick - this.points[i - 1].tick;
      this.startSeconds.push(this.startSeconds[i - 1] + segmentSeconds(this.points[i - 1].bpm, this.slope(i - 1), dt));
    }
    for (let i = 1; i < this.signatures.length; i++) {
      const previous = this.signatures[i - 1];
      this.signatureTicks.push(this.signatureTicks[i - 1] + (this.signatures[i].bar - previous.bar) * barTicks(previous));
    }
  }

  get hasTempoChanges() {
    return this.points.length > 1;
  }

  /** Seconds from tick 0 to a tick position (negative before it). */
  secondsAt(tick: number) {
    if (tick < 0) return (tick * 60) / (this.points[0].bpm * TICKS_PER_BEAT);
    const i = lastAtOrBefore(this.pointTicks, tick);
    return this.startSeconds[i] + segmentSeconds(this.points[i].bpm, this.slope(i), tick - this.points[i].tick);
  }

  /** The tick at a time in seconds, the inverse of secondsAt. */
  tickAt(seconds: number) {
    if (seconds < 0) return (seconds * this.points[0].bpm * TICKS_PER_BEAT) / 60;
    const i = lastAtOrBefore(this.startSeconds, seconds);
    return this.points[i].tick + segmentTicks(this.points[i].bpm, this.slope(i), seconds - this.startSeconds[i]);
  }

  bpmAt(tick: number) {
    if (tick < 0) return this.points[0].bpm;
    const i = lastAtOrBefore(this.pointTicks, tick);
    return this.points[i].bpm + this.slope(i) * (tick - this.points[i].tick);
  }

  /** The index of the tempo point in force at a tick (the last at or before it). */
  pointIndexAt(tick: number) {
    return lastAtOrBefore(this.pointTicks, tick);
  }

  signatureAt(tick: number) {
    return this.signatures[lastAtOrBefore(this.signatureTicks, tick)];
  }

  /** The tick where a 0-based bar starts. */
  tickOfBar(bar: number) {
    const i = lastAtOrBefore(this.signatureBars, bar);
    return this.signatureTicks[i] + (bar - this.signatures[i].bar) * barTicks(this.signatures[i]);
  }

  barAt(tick: number): BarPosition {
    const i = lastAtOrBefore(this.signatureTicks, tick);
    const signature = this.signatures[i];
    const into = tick - this.signatureTicks[i];
    const bars = Math.floor(into / barTicks(signature));
    const intoBar = into - bars * barTicks(signature);
    const beat = Math.min(Math.floor(intoBar / beatTicks(signature)), signature.numerator - 1);
    return { bar: signature.bar + bars, beat, tickInBeat: intoBar - beat * beatTicks(signature) };
  }

  /** The position in bars, with the fraction into the bar (bar 0 starts at 0). */
  barsAt(tick: number) {
    const signature = this.signatureAt(tick);
    const position = this.barAt(tick);
    return position.bar + (position.beat * beatTicks(signature) + position.tickInBeat) / barTicks(signature);
  }

  /** The start of the bar a tick is in. */
  barFloor(tick: number) {
    return this.tickOfBar(this.barAt(tick).bar);
  }

  /** The first bar line at or after a tick. */
  barCeil(tick: number) {
    const start = this.barFloor(tick);
    return start >= tick ? start : this.tickOfBar(this.barAt(tick).bar + 1);
  }

  /** The length of the bar a tick is in. */
  barLengthAt(tick: number) {
    return barTicks(this.signatureAt(tick));
  }

  /**
   * Snaps a position to a grid that restarts at every bar line (docs/design/tempo-map.md, TM14), so after a 7/8 bar
   * a quarter-note grid still starts on the bar. A grid at least a bar long snaps to bar lines. round picks the
   * nearest line; otherwise the line at or before the tick.
   */
  snap(tick: number, gridTicks: number, round = false) {
    if (gridTicks <= 0) return Math.round(tick);
    const bar = this.barAt(tick).bar;
    const start = this.tickOfBar(bar);
    const next = this.tickOfBar(bar + 1);
    const step = Math.min(gridTicks, next - start);
    const snapped = start + (round ? Math.round : Math.floor)((tick - start) / step) * step;
    if (!round) return snapped;
    return Math.abs(next - tick) < Math.abs(snapped - tick) ? next : Math.min(snapped, next);
  }

  private slope(segment: number) {
    if (segment + 1 >= this.points.length || !this.points[segment].ramp) return 0;
    const a = this.points[segment];
    const b = this.points[segment + 1];
    return (b.bpm - a.bpm) / (b.tick - a.tick);
  }
}

let cached: { points?: TempoPoint[]; signatures?: TimeSignature[]; map: TempoMap } = { map: new TempoMap() };

/** The map of a project's tempo points and signatures, rebuilt only when either list changes. */
export function tempoMapOf(project: { tempoMap: TempoPoint[]; signatures: TimeSignature[] } | null | undefined) {
  if (!project) return cached.map;
  if (cached.points !== project.tempoMap || cached.signatures !== project.signatures)
    cached = { points: project.tempoMap, signatures: project.signatures, map: new TempoMap(project.tempoMap, project.signatures) };
  return cached.map;
}
