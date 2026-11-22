import { describe, expect, it } from 'vitest';
import { TICKS_PER_BEAT, type Clip, type Command, type Track } from '../bridge/protocol';
import { TempoMap } from '../time/tempoMap';
import { addNotesCommand, combineCommands, removeNotesCommand, splitNotesCommand, updateNotesCommand, type EditNote } from './edit';
import type { PlacedNote } from './geometry';

const BEAT = TICKS_PER_BEAT;
const BAR = 4 * BEAT;

function clip(id: number, start: number, length: number): Clip {
  return { id, start, length, offset: 0, loopStart: 0, loopLength: 0, name: '', colour: '', linkId: 0, fileId: 0, gainDb: 0, fadeIn: 0, fadeOut: 0, fadeInShape: 0, fadeOutShape: 0, transpose: 0, lane: 0, order: 0, sequencerPattern: -1, sequencerStartStep: 0, controllers: {}, notes: {} };
}

function track(...clips: Clip[]): Track {
  return {
    id: 1,
    name: 'T',
    colour: '#7c8cff',
    gainDb: 0,
    pan: 0,
    offsetMs: 0,
    drumMode: 'auto',
    automation: {},
    automationOrder: [],
    automationMode: 1,
    automationExpanded: false,
  lanes: 1,
  lanesExpanded: false,
    height: 64,
    kind: 'instrument',
    parent: 0,
    folded: false,
    sends: {},
    mute: false,
    solo: false,
    inputChannel: -1,
    inputStereo: false,
    monitor: 1,
    clips: Object.fromEntries(clips.map((c) => [String(c.id), c])),
    graph: { nodes: {}, connections: {} },
  };
}

const note = (start: number, length = BEAT, pitch = 60): EditNote => ({ start, length, pitch, velocity: 0.8 });

/** The commands inside the batch the builders return. */
function commandsOf(command: Command | null): Command[] {
  expect(command).not.toBeNull();
  expect(command!.command).toBe('batch');
  return (command as Extract<Command, { command: 'batch' }>).args.commands;
}

const FOUR_FOUR = new TempoMap();

describe('addNotesCommand', () => {
  it('keeps links between copies, and drops links to notes that were not copied', () => {
    const a = { ...note(0), copiedFrom: 1, link: { to: 2, glideMs: 30 } };
    const b = { ...note(BEAT), copiedFrom: 2, link: { to: 9, glideMs: 30 } };
    const [add] = commandsOf(addNotesCommand(track(clip(10, 0, BAR)), [a, b], FOUR_FOUR));
    expect(add).toMatchObject({ args: { notes: [{ link: { toIndex: 1, glideMs: 30 } }, {}] } });
    expect((add as { args: { notes: Array<{ link?: unknown }> } }).args.notes[1].link).toBeUndefined();
  });

  it('adds a note inside a clip at a clip-relative position', () => {
    const [add] = commandsOf(addNotesCommand(track(clip(10, BAR, BAR)), [note(BAR + BEAT)], FOUR_FOUR));
    expect(add).toMatchObject({ command: 'addNotes', args: { clipId: 10, notes: [{ start: BEAT }] } });
  });

  it('extends the previous clip to the end of the note bar', () => {
    const commands = commandsOf(addNotesCommand(track(clip(10, 0, BAR)), [note(2 * BAR + BEAT)], FOUR_FOUR));
    expect(commands[0]).toEqual({ command: 'resizeClip', args: { id: 10, length: 3 * BAR } });
    expect(commands[1]).toMatchObject({ command: 'addNotes', args: { clipId: 10, notes: [{ start: 2 * BAR + BEAT }] } });
  });

  it('creates a whole-bar clip when no clip precedes the note', () => {
    const [create] = commandsOf(addNotesCommand(track(clip(10, 4 * BAR, BAR)), [note(BAR + BEAT, 4 * BEAT)], FOUR_FOUR));
    expect(create).toMatchObject({
      command: 'addClip',
      args: { trackId: 1, start: BAR, length: 2 * BAR, notes: [{ start: BEAT }] },
    });
  });

  it('extends a clip when a note inside it reaches past its end', () => {
    const commands = commandsOf(addNotesCommand(track(clip(10, 0, BAR)), [note(3 * BEAT, 2 * BEAT)], FOUR_FOUR));
    expect(commands[0]).toEqual({ command: 'resizeClip', args: { id: 10, length: 2 * BAR } });
  });
});

describe('updateNotesCommand', () => {
  it('writes clip-relative positions and never moves a note before its clip', () => {
    const edited: EditNote = { ...note(BAR - BEAT), id: 5, clipId: 10 };
    const [update] = commandsOf(updateNotesCommand(track(clip(10, BAR, BAR)), [edited], FOUR_FOUR, 'Move notes'));
    expect(update).toMatchObject({ command: 'updateNotes', args: { clipId: 10, notes: [{ id: 5, start: 0 }] } });
  });

  it('uses the given label for the undo history', () => {
    const edited: EditNote = { ...note(0), id: 5, clipId: 10 };
    const command = updateNotesCommand(track(clip(10, 0, BAR)), [edited], FOUR_FOUR, 'Quantise');
    expect(command).toMatchObject({ command: 'batch', args: { label: 'Quantise' } });
  });
});

describe('removeNotesCommand', () => {
  it('groups deletions by clip', () => {
    const commands = commandsOf(
      removeNotesCommand([
        { ...note(0), id: 1, clipId: 10 },
        { ...note(0), id: 2, clipId: 11 },
        { ...note(0), id: 3, clipId: 10 },
      ]),
    );
    expect(commands).toEqual([
      { command: 'removeNotes', args: { clipId: 10, ids: [1, 3] } },
      { command: 'removeNotes', args: { clipId: 11, ids: [2] } },
    ]);
  });

  it('returns nothing for an empty selection', () => {
    expect(removeNotesCommand([])).toBeNull();
  });
});

describe('splitNotesCommand', () => {
  const placed = (c: Clip, id: number, start: number, length: number): PlacedNote => ({
    note: { id, start: start - c.start, length, pitch: 60, velocity: 0.8, releaseVelocity: 0.5, expression: {}, vibrato: {} } as never,
    clip: c,
    start,
    end: start + length,
  });

  it('splits the notes it is inside, per clip, at clip-relative positions', () => {
    const a = clip(1, 0, BAR);
    const b = clip(2, BAR, BAR);
    const commands = commandsOf(
      splitNotesCommand(
        [placed(a, 10, 0, 2 * BAR), placed(a, 11, BEAT, BAR + BEAT), placed(b, 20, BAR, 2 * BEAT), placed(a, 12, 3 * BAR, BEAT)],
        BAR + BEAT,
      ),
    );
    expect(commands).toEqual([
      { command: 'splitNotes', args: { clipId: 1, ids: [10, 11], at: BAR + BEAT } },
      { command: 'splitNotes', args: { clipId: 2, ids: [20], at: BEAT } },
    ]);
  });

  it('does nothing at a note edge', () => {
    const a = clip(1, 0, BAR);
    expect(splitNotesCommand([placed(a, 10, 0, BEAT)], BEAT)).toBeNull();
    expect(splitNotesCommand([placed(a, 10, 0, BEAT)], 0)).toBeNull();
  });
});

describe('combineCommands', () => {
  it('joins batches into one flat batch (the engine refuses nested ones)', () => {
    const inner: Command = { command: 'batch', args: { label: 'Inner', commands: [{ command: 'removeNotes', args: { clipId: 1, ids: [2] } }] } };
    const single: Command = { command: 'removeNotes', args: { clipId: 3, ids: [4] } };
    expect(combineCommands('Both', [inner, null, single])).toEqual({
      command: 'batch',
      args: { label: 'Both', commands: [{ command: 'removeNotes', args: { clipId: 1, ids: [2] } }, single] },
    });
    expect(combineCommands('None', [null])).toBeNull();
  });
});
