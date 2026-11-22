import { useEffect, useRef } from 'react';
import type { MidiFileContents } from '../bridge/protocol';
import { dispatch, useDawg } from '../store/store';
import { Button } from '../ui/controls';
import { Dialog } from '../ui/overlays';

// Importing a MIDI file (docs/design/sampler-midi-delay-limiter.md, section 3). Its tracks are added at once; only when
// its tempo or time signatures differ from the project's, and it goes in at the start, is there anything to ask.

/** Whether a file's tempo and signatures differ from those of a project, given as the same fields. */
export function differsInTime(
  file: Pick<MidiFileContents, 'tempo' | 'signatures'>,
  project: Pick<MidiFileContents, 'tempo' | 'signatures'>,
): boolean {
  const tempoSame =
    file.tempo.length === project.tempo.length &&
    file.tempo.every((p, i) => p.tick === project.tempo[i].tick && Math.abs(p.bpm - project.tempo[i].bpm) < 0.01);
  const signaturesSame =
    file.signatures.length === project.signatures.length &&
    file.signatures.every((s, i) => {
      const other = project.signatures[i];
      return s.bar === other.bar && s.numerator === other.numerator && s.denominator === other.denominator;
    });
  return !tempoSame || !signaturesSame;
}

/** Adds a file's parts as new tracks, as one undo step. */
export function importMidi(file: MidiFileContents, start: number, useFileTime: boolean) {
  dispatch({
    command: 'importMidi',
    args: {
      tracks: file.tracks,
      start,
      ...(useFileTime ? { tempo: file.tempo, signatures: file.signatures } : {}),
    },
  });
}

/** Imports a file that was read, asking first if its time differs from the project's. */
export function startMidiImport(file: MidiFileContents, start: number) {
  const { project } = useDawg.getState();
  if (file.tracks.length === 0) {
    useDawg.setState({ lastError: `“${file.name}” has no notes to import` });
    return;
  }

  const current = project ? { tempo: project.tempoMap, signatures: project.signatures } : null;

  if (start === 0 && current && differsInTime(file, current)) useDawg.setState({ midiImport: file });
  else importMidi(file, start, false);
}

/** Asks whether the project takes on a file's tempo and time signatures. */
export function MidiImportDialog() {
  const file = useDawg((s) => s.midiImport);
  const useRef_ = useRef<HTMLButtonElement>(null);

  useEffect(() => useRef_.current?.focus(), [file]);

  if (!file) return null;

  const close = () => useDawg.setState({ midiImport: null });
  const choose = (useFileTime: boolean) => {
    close();
    importMidi(file, 0, useFileTime);
  };
  const tracks = file.tracks.length;
  const first = file.signatures[0];

  return (
    <Dialog onClose={() => close()} aria-labelledby="midi-import-title">
      <h3 id="midi-import-title">Use the tempo of “{file.name}”?</h3>
      <p>
        It adds {tracks} track{tracks === 1 ? '' : 's'} and runs at {Math.round(file.tempo[0].bpm * 100) / 100} bpm in{' '}
        {first.numerator}/{first.denominator}
        {file.tempo.length > 1 || file.signatures.length > 1 ? ', with changes along the way' : ''}. Taking its tempo also
        changes this project&apos;s timing.
      </p>
      <div className="modal-buttons">
        <Button onClick={close}>
          Cancel
        </Button>
        <span className="spacer" />
        <Button onClick={() => choose(false)}>
          Keep the project&apos;s tempo
        </Button>
        <Button ref={useRef_} className="primary" onClick={() => choose(true)}>
          Use the file&apos;s tempo
        </Button>
      </div>
    </Dialog>
  );
}
