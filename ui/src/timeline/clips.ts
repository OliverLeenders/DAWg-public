import type { Clip, Note, Track } from '../bridge/protocol';

// Clip timing shared by the arrangement and the piano roll. Mirrors forEachAudibleNote() in
// engine/include/dawg/Model.h (docs/design/phase6-arrangement.md, A2 and A3).

export const clipEnd = (clip: Clip) => clip.start + clip.length;

/** Timeline position of content position 0 in the clip's first pass. */
export const contentOrigin = (clip: Clip) => clip.start - clip.offset;

export const isLooped = (clip: Clip) => clip.loopLength > 0;

export const clipColour = (clip: Clip, track: Track) => clip.colour || track.colour;

export const clipName = (clip: Clip, track: Track) => clip.name || track.name;

export interface AudibleNote {
  note: Note;
  clip: Clip;
  /** Timeline start and end (cut at the clip end and at each repeat's end). */
  start: number;
  end: number;
  /** 0 for the first pass. */
  repetition: number;
}

/** Every note a clip actually plays, in timeline order. */
export function audibleNotes(clip: Clip): AudibleNote[] {
  const result: AudibleNote[] = [];
  const end = clipEnd(clip);
  const notes = Object.values(clip.notes);

  if (!isLooped(clip)) {
    for (const note of notes) {
      if (note.start < clip.offset || note.start >= clip.offset + clip.length) continue;
      const start = clip.start + note.start - clip.offset;
      result.push({ note, clip, start, end: Math.min(start + note.length, end), repetition: 0 });
    }
  } else {
    const loopEnd = clip.loopStart + clip.loopLength;
    let contentFrom = clip.offset < loopEnd ? clip.offset : clip.loopStart;
    let passStart = clip.start;

    for (let repetition = 0; passStart < end; repetition++) {
      const passEnd = Math.min(end, passStart + (loopEnd - contentFrom));

      for (const note of notes) {
        if (note.start < contentFrom || note.start >= loopEnd) continue;
        const start = passStart + note.start - contentFrom;
        if (start < passEnd) result.push({ note, clip, start, end: Math.min(start + note.length, passEnd), repetition });
      }

      passStart = passEnd;
      contentFrom = clip.loopStart;
    }
  }

  return result.sort((a, b) => a.start - b.start || a.note.id - b.note.id);
}

/** Timeline positions where a looped clip's content starts over (excluding the clip start). */
export function loopBoundaries(clip: Clip): number[] {
  if (!isLooped(clip)) return [];
  const loopEnd = clip.loopStart + clip.loopLength;
  const first = clip.start + (loopEnd - (clip.offset < loopEnd ? clip.offset : clip.loopStart));
  const result: number[] = [];
  for (let t = first; t < clipEnd(clip); t += clip.loopLength) result.push(t);
  return result;
}

export const isAudio = (clip: Clip) => clip.fileId !== 0;

/** The content position (ticks of content time) a clip plays at a timeline tick inside it. */
export function contentAt(clip: Clip, tick: number): number {
  const fromStart = tick - clip.start;
  if (!isLooped(clip)) return clip.offset + fromStart;

  const loopEnd = clip.loopStart + clip.loopLength;
  const first = clip.offset < loopEnd ? clip.offset : clip.loopStart;
  const firstPass = loopEnd - first;
  if (fromStart < firstPass) return first + fromStart;
  return clip.loopStart + ((fromStart - firstPass) % clip.loopLength);
}

/** A clip with one edge dragged to `to`, as the engine's resizeClip leaves it: the start edge keeps the content in
    place (the arrangement and the piano roll preview with it). */
export function resizedClip(clip: Clip, edge: 'start' | 'end', to: number): Clip {
  if (edge === 'end') return { ...clip, length: to - clip.start };
  const delta = to - clip.start;
  // Same as trimStart() in engine/src/Commands.cpp: the content stays where it is.
  let offset = clip.offset + delta;
  if (isLooped(clip)) {
    const r = (offset - clip.loopStart) % clip.loopLength;
    offset = clip.loopStart + (r < 0 ? r + clip.loopLength : r);
  } else if (offset < 0) {
    return { ...clip, start: clip.start - clip.offset, length: clip.length + clip.offset, offset: 0 };
  }
  return { ...clip, start: to, length: clip.length - delta, offset };
}
