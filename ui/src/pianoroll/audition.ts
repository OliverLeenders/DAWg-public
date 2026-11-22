import * as bridge from '../bridge/bridge';

// Plays notes through a track's instrument while the user presses keys or drags notes.

export class Auditioner {
  private pending: Promise<number> | null = null;
  private trackId = 0;
  private pitch: number | null = null;

  /** Starts (or changes to) a pitch; does nothing if it is already sounding. */
  play(trackId: number, pitch: number, velocity = 0.8) {
    if (this.pitch === pitch && this.trackId === trackId) return;
    this.stop();
    this.trackId = trackId;
    this.pitch = pitch;
    this.pending = bridge
      .call('audition.noteOn', { trackId, pitch, velocity })
      .then(({ id }) => id)
      .catch(() => 0);
  }

  /** Releases the sounding note, if any. */
  stop() {
    const pending = this.pending;
    const trackId = this.trackId;
    this.pending = null;
    this.pitch = null;
    pending?.then((id) => {
      if (id !== 0) bridge.call('audition.noteOff', { trackId, id }).catch(() => {});
    });
  }

  get sounding() {
    return this.pitch;
  }
}
