import type { Command } from '../bridge/protocol';
import { pitchAtTick, type PlacedNote } from './geometry';
import type { TempoMap } from '../time/tempoMap';

// Linked notes in the roll (docs/design/linked-notes.md): the commands and where the link lines go.

const DEFAULT_GLIDE_MS = 100;

/** The command that links two notes, the earlier to the later, whichever way they were joined. Null if they can't be:
    they must be two notes of one clip that don't start together. */
export function linkCommand(a: PlacedNote, b: PlacedNote): Command | null {
  if (a.clip.id !== b.clip.id || a.note.start === b.note.start) return null;
  const [from, to] = a.note.start < b.note.start ? [a, b] : [b, a];
  return {
    command: 'setNoteLinks',
    args: { clipId: from.clip.id, links: [{ from: from.note.id, to: to.note.id, glideMs: from.note.link?.glideMs ?? DEFAULT_GLIDE_MS }] },
  };
}

/** Links from the roll's menu, without Ctrl+Alt+drag (docs/design/ux-review.md, U12): two or more notes each to the
    next of them by start, in their clip; one note to the note after it, the next start in its clip and the nearest
    in pitch of the notes starting then. Null when there's nothing to link. */
export function linkInRowCommand(chosen: PlacedNote[], all: PlacedNote[]): Command | null {
  const pairs: Array<[PlacedNote, PlacedNote]> = [];
  if (chosen.length === 1) {
    const [from] = chosen;
    const later = all.filter((n) => n.clip.id === from.clip.id && n.note.start > from.note.start);
    if (later.length === 0) return null;
    const next = Math.min(...later.map((n) => n.note.start));
    const to = later
      .filter((n) => n.note.start === next)
      .sort((a, b) => Math.abs(a.note.pitch - from.note.pitch) - Math.abs(b.note.pitch - from.note.pitch))[0];
    pairs.push([from, to]);
  } else {
    for (const clipId of new Set(chosen.map((n) => n.clip.id))) {
      const row = chosen.filter((n) => n.clip.id === clipId).sort((a, b) => a.note.start - b.note.start);
      for (let i = 1; i < row.length; i++) if (row[i].note.start !== row[i - 1].note.start) pairs.push([row[i - 1], row[i]]);
    }
  }
  const commands = pairs.map(([a, b]) => linkCommand(a, b)).filter((c): c is Command => c !== null);
  if (commands.length === 0) return null;
  return commands.length === 1 ? commands[0] : { command: 'batch', args: { label: 'Link notes', commands } };
}

/** Sets the selected notes' glide time, one undo step for one gesture. */
export function glideCommand(linked: PlacedNote[], glideMs: number): Command {
  return {
    command: 'batch',
    args: {
      label: 'Set glide time',
      commands: linked.map((n) => ({
        command: 'setNoteLinks' as const,
        args: { clipId: n.clip.id, links: [{ from: n.note.id, to: n.note.link!.to, glideMs }] },
      })),
    },
  };
}

/** Takes the notes' links away, one undo step. */
export function unlinkCommand(linked: PlacedNote[]): Command {
  return {
    command: 'batch',
    args: {
      label: 'Unlink notes',
      commands: linked.map((n) => ({ command: 'setNoteLinks' as const, args: { clipId: n.clip.id, links: [{ from: n.note.id }] } })),
    },
  };
}

/** The notes with their glides worked out, in timeline order so a chain's earlier glide counts, to draw what is heard. */
export function withGlides(notes: PlacedNote[], map: TempoMap): PlacedNote[] {
  if (!notes.some((n) => n.note.link)) return notes;
  const result = new Map(notes.map((n) => [n, n]));
  const ordered = [...notes].sort((a, b) => a.start - b.start);
  const glided = new Map<number, PlacedNote>();

  for (const original of ordered) {
    const placed = glided.get(original.note.id) ?? original;
    result.set(original, placed);
    const link = original.note.link;
    if (!link) continue;
    const target = notes.find((n) => n.clip.id === original.clip.id && n.note.id === link.to);
    if (!target || target.start <= original.start) continue;
    const offset = pitchAtTick(placed, target.start, map) - pitchAtTick(target, target.start, map);
    glided.set(target.note.id, { ...target, glide: { offset, ms: link.glideMs } });
  }

  return notes.map((n) => result.get(n)!);
}

export interface LinkLine {
  fromTick: number;
  fromPitch: number;
  toTick: number;
  toPitch: number;
}

/** A line per link that plays, for what is heard: the voice holds its pitch from the note's end, or the hand-over if the
    notes overlap, to the later note's start, whose ribbon then glides. With no glide time the line goes to its pitch. */
export function linkLines(notes: PlacedNote[]): LinkLine[] {
  const lines: LinkLine[] = [];
  for (const placed of notes) {
    const link = placed.note.link;
    if (!link) continue;
    const target = notes.find((n) => n.clip.id === placed.clip.id && n.note.id === link.to);
    if (!target || target.start <= placed.start) continue;
    lines.push({
      fromTick: Math.min(placed.end, target.start),
      fromPitch: placed.note.pitch,
      toTick: target.start,
      toPitch: link.glideMs > 0 ? placed.note.pitch : target.note.pitch,
    });
  }
  return lines;
}
