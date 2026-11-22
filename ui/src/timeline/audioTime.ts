import { TICKS_PER_BEAT, type Clip } from '../bridge/protocol';
import { useDawg } from '../store/store';
import type { TempoMap } from '../time/tempoMap';

// An audio clip's content ticks (offset, loop, fades) run at the tempo where the clip starts, at a constant rate in
// real time (docs/design/tempo-map.md, TM8). These convert between timeline ticks and the clip's content ticks
// from its start ("local" ticks); without a map, or at one tempo, the two are the same. Clips of musical files are
// in musical ticks (docs/design/time-stretching.md, TS2): there the two are always the same.

export interface AudioTime {
  /** Seconds per content tick. */
  secondsPerTick: number;
  /** Content ticks from the clip start at a timeline tick. */
  localAt: (tick: number) => number;
  /** The timeline tick at content ticks from the clip start. */
  tickAt: (local: number) => number;
  /** The clip's duration in content ticks. */
  localLength: number;
}

/** Whether a clip plays a musical file (in the current project). */
export function isMusicalClip(clip: Clip) {
  return useDawg.getState().project?.audioFiles[String(clip.fileId)]?.timing?.mode === 'musical';
}

export function audioTime(clip: Clip, map?: TempoMap): AudioTime {
  if (!map || isMusicalClip(clip)) {
    return {
      secondsPerTick: 60 / (120 * TICKS_PER_BEAT),
      localAt: (tick) => tick - clip.start,
      tickAt: (local) => clip.start + local,
      localLength: clip.length,
    };
  }
  const startSeconds = map.secondsAt(clip.start);
  const secondsPerTick = 60 / (map.bpmAt(clip.start) * TICKS_PER_BEAT);
  const localAt = (tick: number) => (map.secondsAt(tick) - startSeconds) / secondsPerTick;
  return {
    secondsPerTick,
    localAt,
    tickAt: (local) => map.tickAt(startSeconds + local * secondsPerTick),
    localLength: localAt(clip.start + clip.length),
  };
}
