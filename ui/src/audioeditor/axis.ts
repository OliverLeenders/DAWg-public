import { TICKS_PER_BEAT, type AudioFile, type Clip, type Project } from '../bridge/protocol';
import type { TempoMap } from '../time/tempoMap';
import { TimingMap } from '../time/timingMap';

// Where a pool file's frames sit on the timeline as one clip plays them (docs/design/time-stretching.md, TS13): the
// audio editor draws the whole file around that clip, in the arrangement's bars.

export interface EditorAxis {
  musical: boolean;
  /** The file's timing map: content ticks (musical) or content seconds (one-shot) to file frames. */
  timing: TimingMap;
  /** The timeline tick where the file's frame `frame` sounds (on the clip's first pass). */
  frameToTick: (frame: number) => number;
  /** The file frame that sounds at a timeline tick. */
  tickToFrame: (tick: number) => number;
  /** The file's content position at a timeline tick, in its markers' unit (ticks or seconds), and back. */
  contentAt: (tick: number) => number;
  tickOfContent: (content: number) => number;
}

/** One-shot clips: seconds per content tick, the tick length at the tempo where the clip starts (TM8). */
const oneShotSecondsPerTick = (clip: Clip, map: TempoMap) => 60 / (map.bpmAt(clip.start) * TICKS_PER_BEAT);

export function editorAxis(clip: Clip, file: AudioFile, map: TempoMap): EditorAxis {
  const timing = new TimingMap(file.timing, file.sampleRate);
  if (file.timing?.mode === 'musical') {
    const origin = clip.start - clip.offset;
    return {
      musical: true,
      timing,
      frameToTick: (frame) => origin + timing.tickAt(frame),
      tickToFrame: (tick) => timing.frameAt(tick - origin),
      contentAt: (tick) => tick - origin,
      tickOfContent: (content) => origin + content,
    };
  }
  // The timeline second at which content second 0 (frame 0) sounds: one-shot content positions are ticks at the
  // tempo where the clip starts, and the markers pin frames to content seconds.
  const zero = map.secondsAt(clip.start) - clip.offset * oneShotSecondsPerTick(clip, map);
  return {
    musical: false,
    timing,
    frameToTick: (frame) => map.tickAt(zero + timing.tickAt(frame)),
    tickToFrame: (tick) => timing.frameAt(map.secondsAt(tick) - zero),
    contentAt: (tick) => map.secondsAt(tick) - zero,
    tickOfContent: (content) => map.tickAt(zero + content),
  };
}

/** Where another clip of the same file plays its content, on this axis: [first tick, last tick). */
export function clipRangeOn(axis: EditorAxis, other: Clip, map: TempoMap): [number, number] {
  if (axis.musical) return [axis.tickOfContent(other.offset), axis.tickOfContent(other.offset + other.length)];
  const from = other.offset * oneShotSecondsPerTick(other, map);
  const seconds = map.secondsAt(other.start + other.length) - map.secondsAt(other.start);
  return [axis.frameToTick(axis.timing.frameAt(from)), axis.frameToTick(axis.timing.frameAt(from + seconds))];
}

/** The clips that play a pool file, with their tracks. */
export function clipsOfFile(project: Project | null, fileId: number) {
  const result: Array<{ clip: Clip; trackId: number }> = [];
  for (const track of Object.values(project?.tracks ?? {}))
    for (const clip of Object.values(track.clips)) if (clip.fileId === fileId) result.push({ clip, trackId: track.id });
  return result;
}

/** The onset nearest a frame within maxFrames, or null. */
export function snapToOnset(frame: number, onsets: ReadonlyArray<readonly [number, number]>, maxFrames: number) {
  let best: number | null = null;
  for (const [at] of onsets) if (Math.abs(at - frame) <= maxFrames && (best === null || Math.abs(at - frame) < Math.abs(best - frame))) best = at;
  return best;
}
