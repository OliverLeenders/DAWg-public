import { type Clip, type Command, type CurvePoint, type LaneId, type Track, type Vibrato } from '../bridge/protocol';
import { contentOrigin, isLooped } from '../timeline/clips';
import { sortedClips, type PlacedNote } from './geometry';
import type { TempoMap } from '../time/tempoMap';

// Turns piano roll edits (in absolute timeline ticks) into model commands. Notes live in clips, so edits
// that reach past a clip extend it, and new notes outside every clip extend the previous clip or create
// one (see docs/design/phase3-piano-roll.md).

/** A note as the roll edits it: absolute position, no id (new) or an id (existing). */
export interface EditNote {
  id?: number;
  clipId?: number;
  start: number;
  length: number;
  pitch: number;
  velocity: number;
  expression?: Partial<Record<LaneId, CurvePoint[]>>;
  vibrato?: Vibrato;
  /** The note's link: copies keep it to the copy of its target (docs/design/linked-notes.md). */
  link?: { to: number; glideMs: number };
  /** On a copy: the id of the note it was copied from. */
  copiedFrom?: number;
}

export const toEditNote = (placed: PlacedNote): EditNote => ({
  id: placed.note.id,
  clipId: placed.clip.id,
  start: placed.start,
  length: placed.note.length,
  pitch: placed.note.pitch,
  velocity: placed.note.velocity,
  expression: placed.note.expression,
  vibrato: placed.note.vibrato,
  link: placed.note.link,
});

// Clips extend to whole bars of the tempo map (docs/design/tempo-map.md, TM14).
const barFloor = (tick: number, map: TempoMap) => map.barFloor(tick);
const barCeil = (tick: number, map: TempoMap) => map.barCeil(tick);

/** The clip a new note at `tick` belongs in: the one containing it, else the nearest one ending before it. */
function homeClip(clips: Clip[], tick: number): Clip | undefined {
  const containing = clips.find((c) => tick >= c.start && tick < c.start + c.length);
  if (containing) return containing;
  return clips.filter((c) => c.start + c.length <= tick).sort((a, b) => b.start + b.length - (a.start + a.length))[0];
}

// Always a batch, even for one command, so the undo history shows the roll's own label ("Move notes").
function batch(label: string, commands: Command[]): Command | null {
  return commands.length === 0 ? null : { command: 'batch', args: { label, commands } };
}

/** Joins edits (note changes and curve changes, say) into one undo step. The engine refuses nested batches, so
    batches are unpacked. */
export const combineCommands = (label: string, commands: Array<Command | null>) =>
  batch(
    label,
    commands.flatMap((c): Command[] =>
      c === null ? [] : c.command === 'batch' ? (c.args as { commands: Command[] }).commands : [c],
    ),
  );

/** Clip resizes needed so each clip reaches `requiredEnd` (rounded up to whole bars). */
function extensions(clipEnds: Map<Clip, number>, map: TempoMap): Command[] {
  const commands: Command[] = [];
  for (const [clip, end] of clipEnds) {
    // A looped clip's length sets how often it repeats; notes never stretch it.
    if (end > clip.start + clip.length && !isLooped(clip))
      commands.push({ command: 'resizeClip', args: { id: clip.id, length: barCeil(end, map) - clip.start } });
  }
  return commands;
}

const requireEnd = (ends: Map<Clip, number>, clip: Clip, end: number) =>
  ends.set(clip, Math.max(ends.get(clip) ?? 0, end));

/**
 * Adds new notes (absolute positions), extending or creating clips as needed. With an edited clip, every note
 * goes into it (docs/design/phase6-arrangement.md, section 5).
 */
export function addNotesCommand(
  track: Track,
  notes: EditNote[],
  map: TempoMap,
  label = 'Add notes',
  editedClipId: number | null = null,
): Command | null {
  const clips = sortedClips(track);
  const edited = editedClipId !== null ? track.clips[String(editedClipId)] : undefined;
  const perClip = new Map<Clip, EditNote[]>();
  const orphans: EditNote[] = [];

  for (const note of notes) {
    const clip = edited ?? homeClip(clips, note.start);
    if (clip) perClip.set(clip, [...(perClip.get(clip) ?? []), note]);
    else orphans.push(note);
  }

  const ends = new Map<Clip, number>();
  const commands: Command[] = [];

  for (const [clip, clipNotes] of perClip) {
    for (const note of clipNotes) requireEnd(ends, clip, note.start + note.length);
  }

  commands.push(...extensions(ends, map));

  for (const [clip, clipNotes] of perClip) {
    commands.push({
      command: 'addNotes',
      args: {
        clipId: clip.id,
        notes: clipNotes.map((n) => {
          const toIndex = n.link ? clipNotes.findIndex((other) => other.copiedFrom !== undefined && other.copiedFrom === n.link!.to) : -1;
          return {
            start: Math.max(0, n.start - contentOrigin(clip)),
            length: n.length,
            pitch: n.pitch,
            velocity: n.velocity,
            expression: n.expression,
            vibrato: n.vibrato,
            ...(n.link && toIndex >= 0 ? { link: { toIndex, glideMs: n.link.glideMs } } : {}),
          };
        }),
      },
    });
  }

  if (orphans.length > 0) {
    // No clip before these notes (they all precede the first clip): one new clip covering their bars.
    const start = barFloor(Math.min(...orphans.map((n) => n.start)), map);
    const end = barCeil(Math.max(...orphans.map((n) => n.start + n.length)), map);
    commands.push({
      command: 'addClip',
      args: {
        trackId: track.id,
        start,
        length: end - start,
        notes: orphans.map((n) => ({
          start: n.start - start,
          length: n.length,
          pitch: n.pitch,
          velocity: n.velocity,
          expression: n.expression,
          vibrato: n.vibrato,
        })),
      },
    });
  }

  return batch(label, commands);
}

/** Writes changed existing notes back, extending their clips where they now reach further. */
export function updateNotesCommand(track: Track, notes: EditNote[], map: TempoMap, label: string): Command | null {
  const clipsById = new Map(Object.values(track.clips).map((c) => [c.id, c]));
  const perClip = new Map<Clip, EditNote[]>();

  for (const note of notes) {
    const clip = note.clipId !== undefined ? clipsById.get(note.clipId) : undefined;
    if (clip && note.id !== undefined) perClip.set(clip, [...(perClip.get(clip) ?? []), note]);
  }

  const ends = new Map<Clip, number>();
  for (const [clip, clipNotes] of perClip)
    for (const note of clipNotes) requireEnd(ends, clip, note.start + note.length);

  const commands = extensions(ends, map);

  for (const [clip, clipNotes] of perClip) {
    commands.push({
      command: 'updateNotes',
      args: {
        clipId: clip.id,
        notes: clipNotes.map((n) => ({
          id: n.id!,
          start: Math.max(0, n.start - contentOrigin(clip)),
          length: n.length,
          pitch: n.pitch,
          velocity: n.velocity,
          ...(n.vibrato ? { vibrato: n.vibrato } : {}),
        })),
      },
    });
  }

  return batch(label, commands);
}

/** Deletes notes by id. */
export function removeNotesCommand(notes: EditNote[]): Command | null {
  const perClip = new Map<number, number[]>();
  for (const note of notes)
    if (note.clipId !== undefined && note.id !== undefined)
      perClip.set(note.clipId, [...(perClip.get(note.clipId) ?? []), note.id]);

  return batch(
    'Delete notes',
    [...perClip].map(([clipId, ids]) => ({ command: 'removeNotes', args: { clipId, ids } }) as Command),
  );
}

/** Splits notes in two at an absolute tick, as one undo step. Notes it isn't strictly inside are left out. */
export function splitNotesCommand(notes: PlacedNote[], at: number): Command | null {
  const perClip = new Map<Clip, number[]>();
  for (const placed of notes)
    if (at > placed.start && at < placed.end) perClip.set(placed.clip, [...(perClip.get(placed.clip) ?? []), placed.note.id]);

  return batch(
    'Split notes',
    [...perClip].map(
      ([clip, ids]) => ({ command: 'splitNotes', args: { clipId: clip.id, ids, at: Math.round(at - contentOrigin(clip)) } }) as Command,
    ),
  );
}

/** Clamps edited notes so they don't start before their clip's content (content positions stay ≥ 0). */
export function clampToClips(track: Track, notes: EditNote[]): EditNote[] {
  return notes.map((n) => {
    const clip = n.clipId !== undefined ? track.clips[String(n.clipId)] : undefined;
    return clip && n.start < contentOrigin(clip) ? { ...n, start: contentOrigin(clip) } : n;
  });
}

/** Replaces expression lanes of one or more notes, as one undo step. */
export function setCurvesCommand(
  edits: Array<{ placed: PlacedNote; lane: LaneId; points: CurvePoint[] }>,
  label: string,
): Command | null {
  return batch(
    label,
    edits.map(
      ({ placed, lane, points }): Command => ({
        command: 'setCurve',
        args: { clipId: placed.clip.id, noteId: placed.note.id, lane, points },
      }),
    ),
  );
}
