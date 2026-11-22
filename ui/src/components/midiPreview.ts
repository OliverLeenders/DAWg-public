import * as bridge from '../bridge/bridge';
import { TICKS_PER_BEAT, type MidiFileContents } from '../bridge/protocol';

// Hears a MIDI file through a track's instrument, at the project's tempo, without touching the transport.

const RELEASE_TAIL_MS = 50;

export interface PreviewEvent {
  ms: number;
  on: boolean;
  pitch: number;
  velocity: number;
}

/** Every note on and off of a file at a tempo, in order, offs first where they meet. */
export function previewEvents(file: MidiFileContents, bpm: number): PreviewEvent[] {
  const msPerTick = 60000 / bpm / TICKS_PER_BEAT;
  const events = file.tracks.flatMap((track) =>
    track.notes.flatMap((note): PreviewEvent[] => [
      { ms: note.start * msPerTick, on: true, pitch: note.pitch, velocity: note.velocity ?? 0.8 },
      { ms: (note.start + note.length) * msPerTick, on: false, pitch: note.pitch, velocity: 0 },
    ]),
  );
  return events.sort((a, b) => a.ms - b.ms || Number(a.on) - Number(b.on));
}

export class MidiPreview {
  /** Called when a preview played to its end. */
  onEnd: (() => void) | null = null;

  private timers: number[] = [];
  private sounding = new Map<number, { trackId: number; id: Promise<number> }>();
  private requested: string | null = null;

  /** Notes which file is being read, so a slower earlier read doesn't start after a newer choice. */
  want(path: string) {
    this.requested = path;
  }

  wanted(path: string) {
    return this.requested === path;
  }

  start(file: MidiFileContents, trackId: number, bpm: number) {
    this.silence();
    const events = previewEvents(file, bpm);

    for (const event of events) {
      this.timers.push(
        window.setTimeout(() => {
          if (event.on) {
            const id = bridge
              .call('audition.noteOn', { trackId, pitch: event.pitch, velocity: event.velocity })
              .then((reply) => reply.id)
              .catch(() => 0);
            this.sounding.set(event.pitch, { trackId, id });
          } else this.release(event.pitch);
        }, event.ms),
      );
    }

    const end = events.at(-1)?.ms ?? 0;
    this.timers.push(window.setTimeout(() => this.onEnd?.(), end + RELEASE_TAIL_MS));
  }

  stop() {
    this.requested = null;
    this.silence();
  }

  private release(pitch: number) {
    const note = this.sounding.get(pitch);
    if (!note) return;
    this.sounding.delete(pitch);
    note.id.then((id) => {
      if (id !== 0) bridge.call('audition.noteOff', { trackId: note.trackId, id }).catch(() => {});
    });
  }

  private silence() {
    this.timers.forEach((timer) => window.clearTimeout(timer));
    this.timers = [];
    [...this.sounding.keys()].forEach((pitch) => this.release(pitch));
  }
}
