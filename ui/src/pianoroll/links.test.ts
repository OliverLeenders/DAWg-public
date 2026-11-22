import { describe, expect, it } from 'vitest';
import type { Clip, Note } from '../bridge/protocol';
import type { PlacedNote } from './geometry';
import { pitchAtTick } from './geometry';
import { linkCommand, linkInRowCommand, linkLines, unlinkCommand, withGlides } from './links';
import { TempoMap } from '../time/tempoMap';

const CONSTANT_TEMPO = new TempoMap();

const placed = (id: number, start: number, length: number, pitch: number, link?: Note['link'], clipId = 1): PlacedNote => ({
  note: { id, start, length, pitch, link } as Note,
  clip: { id: clipId } as Clip,
  start,
  end: start + length,
});

describe('linking notes', () => {
  it('links the earlier note to the later, whichever way they were joined', () => {
    const [a, b] = [placed(1, 0, 10, 60), placed(2, 20, 10, 62)];
    const forward = { command: 'setNoteLinks', args: { clipId: 1, links: [{ from: 1, to: 2, glideMs: 100 }] } };
    expect(linkCommand(a, b)).toEqual(forward);
    expect(linkCommand(b, a)).toEqual(forward);
  });

  it('keeps the glide of a link it moves', () => {
    const command = linkCommand(placed(1, 0, 10, 60, { to: 9, glideMs: 30 }), placed(2, 20, 10, 62));
    expect(command).toMatchObject({ args: { links: [{ glideMs: 30 }] } });
  });

  it('refuses notes of two clips, and notes that start together', () => {
    expect(linkCommand(placed(1, 0, 10, 60), placed(2, 20, 10, 62, undefined, 2))).toBeNull();
    expect(linkCommand(placed(1, 0, 10, 60), placed(2, 0, 10, 62))).toBeNull();
  });

  it('links from the menu: one note to the next start, nearest in pitch', () => {
    const notes = [placed(1, 0, 10, 60), placed(2, 20, 10, 72), placed(3, 20, 10, 61), placed(4, 40, 10, 60), placed(5, 30, 10, 60, undefined, 2)];
    expect(linkInRowCommand([notes[0]], notes)).toMatchObject({ args: { links: [{ from: 1, to: 3 }] } });
    expect(linkInRowCommand([notes[3]], notes)).toBeNull();
  });

  it('links selected notes in a row, by start, in each clip', () => {
    const notes = [placed(4, 40, 10, 60), placed(1, 0, 10, 60), placed(2, 20, 10, 62), placed(3, 20, 10, 64)];
    const command = linkInRowCommand(notes, notes);
    // 1 -> 2 (or 3, which starts with it: those two aren't linked to each other), then on to 4.
    expect(command).toMatchObject({ command: 'batch', args: { label: 'Link notes' } });
    const links = (command as { args: { commands: Array<{ args: { links: Array<{ from: number; to: number }> } }> } }).args.commands.map((c) => c.args.links[0]);
    expect(links.map((l) => l.from)).toEqual([1, 3]);
    expect(links.at(-1)).toMatchObject({ to: 4 });
  });

  it('unlinks in one step', () => {
    expect(unlinkCommand([placed(1, 0, 10, 60, { to: 2, glideMs: 1 })])).toMatchObject({
      command: 'batch',
      args: { commands: [{ command: 'setNoteLinks', args: { links: [{ from: 1 }] } }] },
    });
  });
});

describe('drawing a glide', () => {
  const flat = (id: number, start: number, length: number, pitch: number, link?: Note['link']) => {
    const n = placed(id, start, length, pitch, link);
    n.note.expression = { pitch: [], pressure: [], timbre: [], vibrato: [] };
    return n;
  };

  it('starts at the earlier note and reaches the later one after the glide time', () => {
    const second = CONSTANT_TEMPO.tickAt(1);
    const notes = withGlides([flat(1, 0, second, 60, { to: 2, glideMs: 500 }), flat(2, second, second, 72)], CONSTANT_TEMPO);
    const later = notes[1];
    expect(pitchAtTick(later, second, CONSTANT_TEMPO)).toBeCloseTo(60);
    expect(pitchAtTick(later, CONSTANT_TEMPO.tickAt(1.25), CONSTANT_TEMPO)).toBeCloseTo(66);
    expect(pitchAtTick(later, CONSTANT_TEMPO.tickAt(1.5), CONSTANT_TEMPO)).toBeCloseTo(72);
    expect(pitchAtTick(later, CONSTANT_TEMPO.tickAt(1.9), CONSTANT_TEMPO)).toBeCloseTo(72);
  });

  it('leaves notes alone without a link, or with no glide', () => {
    const plain = [flat(1, 0, 10, 60)];
    expect(withGlides(plain, CONSTANT_TEMPO)).toBe(plain);
    const none = withGlides([flat(1, 0, 10, 60, { to: 2, glideMs: 0 }), flat(2, 20, 10, 72)], CONSTANT_TEMPO);
    expect(pitchAtTick(none[1], 20, CONSTANT_TEMPO)).toBe(72);
  });
});

describe('link lines', () => {
  it('run from the end of the note to the start of the next, or from the hand-over', () => {
    const gap = [placed(1, 0, 10, 60, { to: 2, glideMs: 1 }), placed(2, 20, 10, 62)];
    expect(linkLines(gap)).toEqual([{ fromTick: 10, fromPitch: 60, toTick: 20, toPitch: 60 }]);
    const jump = [placed(1, 0, 10, 60, { to: 2, glideMs: 0 }), placed(2, 20, 10, 62)];
    expect(linkLines(jump)[0].toPitch).toBe(62);
    const overlap = [placed(1, 0, 30, 60, { to: 2, glideMs: 1 }), placed(2, 20, 10, 62)];
    expect(linkLines(overlap)[0].fromTick).toBe(20);
  });

  it('leave out a link that does not play', () => {
    expect(linkLines([placed(1, 30, 10, 60, { to: 2, glideMs: 1 }), placed(2, 20, 10, 62)])).toEqual([]);
    expect(linkLines([placed(1, 0, 10, 60, { to: 2, glideMs: 1 })])).toEqual([]);
  });
});
