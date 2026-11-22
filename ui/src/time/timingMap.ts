import { TICKS_PER_BEAT, type FileTiming, type TimingMarker } from '../bridge/protocol';

// A pool file's content-to-file mapping (docs/design/time-stretching.md, TS3): the TypeScript twin of
// dawg::TimingMap (engine/include/dawg/TimingMap.h). Piecewise linear through the markers from the implicit point
// (0, frame 0), and on at the file's natural rate past the last marker and before 0. A musical file's content is in
// ticks at its source tempo; a one-shot file's is in seconds at its sample rate (the methods say "tick" for either).
// The engine normalises the markers, so this takes them as they come from the model.

/** File frames per content tick at a tempo. */
export const framesPerTick = (fileRate: number, bpm: number) => (fileRate * 60) / (bpm * TICKS_PER_BEAT);

export class TimingMap {
  readonly points: TimingMarker[];
  readonly sourceRate: number;

  constructor(timing: Pick<FileTiming, 'sourceBpm' | 'markers'> & Partial<Pick<FileTiming, 'mode'>>, fileRate: number) {
    this.points = [{ tick: 0, frame: 0 }, ...timing.markers];
    this.sourceRate = timing.mode === 'oneShot' ? fileRate : framesPerTick(fileRate, timing.sourceBpm);
  }

  /** The index of the segment a tick lies in: the last point at or before it (0 before the second point). */
  segmentAt(tick: number) {
    let lo = 0;
    let hi = this.points.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (this.points[mid].tick <= tick) lo = mid;
      else hi = mid - 1;
    }
    return lo;
  }

  private slope(segment: number) {
    if (segment + 1 >= this.points.length) return this.sourceRate;
    const a = this.points[segment];
    const b = this.points[segment + 1];
    return (b.frame - a.frame) / (b.tick - a.tick);
  }

  /** The file frame at a content tick. */
  frameAt(tick: number) {
    if (tick < 0) return tick * this.sourceRate;
    const i = this.segmentAt(tick);
    return this.points[i].frame + (tick - this.points[i].tick) * this.slope(i);
  }

  /** The content tick at a file frame, the inverse of frameAt. */
  tickAt(frame: number) {
    if (frame < 0) return frame / this.sourceRate;
    let lo = 0;
    let hi = this.points.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (this.points[mid].frame <= frame) lo = mid;
      else hi = mid - 1;
    }
    return this.points[lo].tick + (frame - this.points[lo].frame) / this.slope(lo);
  }

  /** File frames per content tick at a tick. */
  rateAt(tick: number) {
    return tick < 0 ? this.sourceRate : this.slope(this.segmentAt(tick));
  }
}
