import * as bridge from '../bridge/bridge';
import type { Command, MidiFileContents } from '../bridge/protocol';
import { importMidi, startMidiImport } from '../components/MidiImportDialog';
import { dispatch, useDawg } from '../store/store';

// Getting audio files into the project (docs/design/phase7-audio-tracks.md, section 9). Files dragged from the
// sound explorer carry their paths; files dropped from the OS reach the web UI without one, so their bytes go
// to the app in chunks.

/** Drag data type of files dragged from the sound explorer: a JSON array of absolute paths. */
export const PATHS_MIME = 'application/x-dawg-paths';

/** Drag data type of a folder or an SFZ patch dragged from the sound explorer: a JSON array of its absolute path. Only
    the Sampler takes it, mapping the folder's files to keys or the patch's regions to zones (docs/design/
    sampler-mapping.md, SM1, SF1). */
export const FOLDER_MIME = 'application/x-dawg-folder';

/** Whether a path is an SFZ patch, which goes to a Sampler, not to the arrangement or the preview. */
export const isSfz = (path: string) => path.toLowerCase().endsWith('.sfz');

const CHUNK_BYTES = 3 * 1024 * 1024;

function toBase64(bytes: Uint8Array) {
  let binary = '';
  const step = 0x8000;
  for (let i = 0; i < bytes.length; i += step) binary += String.fromCharCode(...bytes.subarray(i, i + step));
  return btoa(binary);
}

const reportError = (error: unknown) =>
  useDawg.setState({ lastError: error instanceof Error ? error.message : String(error) });

async function upload(file: File, trackId: number | undefined, start: number) {
  const id = `${Date.now()}-${Math.random().toString(36).slice(2)}`;

  for (let offset = 0; offset < file.size || offset === 0; offset += CHUNK_BYTES) {
    const bytes = new Uint8Array(await file.slice(offset, offset + CHUNK_BYTES).arrayBuffer());
    const last = offset + CHUNK_BYTES >= file.size;
    await bridge.call('audio.upload', { id, name: file.name, data: toBase64(bytes), last, trackId, start });
    if (last) break;
  }
}

/** The MIDI files browser's drags carry a file's path. */
export const MIDI_PATH_MIME = 'application/x-dawg-midi-path';

/** A file's notes as one clip on a track: what it holds in several parts is merged, for dropping it on one track. */
export function clipFromFile(file: MidiFileContents, trackId: number, start: number): Command {
  return {
    command: 'addClip',
    args: {
      trackId,
      start,
      length: Math.max(...file.tracks.map((track) => track.length)),
      name: file.name,
      notes: file.tracks.flatMap((track) => track.notes),
      controllers: Object.assign({}, ...file.tracks.map((track) => track.controllers)),
    },
  };
}

// A MIDI file goes in whole: onto an instrument track if it was dropped on one, else on new tracks. The browser's files
// have no tempo of their own worth asking about.
function placeMidi(file: MidiFileContents, instrumentTrackId: number | undefined, start: number, ask: boolean) {
  if (file.tracks.length === 0) useDawg.setState({ lastError: `“${file.name}” has no notes to import` });
  else if (instrumentTrackId !== undefined) dispatch(clipFromFile(file, instrumentTrackId, start));
  else if (ask) startMidiImport(file, start);
  else importMidi(file, start, false);
}

async function importMidiFile(file: File, instrumentTrackId: number | undefined, start: number) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  placeMidi(await bridge.call('midi.parse', { name: file.name, data: toBase64(bytes) }), instrumentTrackId, start, true);
}

/**
 * Imports what was dropped: the MIDI files browser's file, explorer paths or OS files. Without an audio track as the
 * target, each audio file gets a new audio track.
 */
export function importDrop(data: DataTransfer, trackId: number | undefined, start: number, instrumentTrackId?: number) {
  const midiPath = data.getData(MIDI_PATH_MIME);

  if (midiPath) {
    bridge
      .call('midi.parse', { path: midiPath })
      .then((file) => placeMidi(file, instrumentTrackId, start, false))
      .catch(reportError);
    return;
  }

  const paths = data.getData(PATHS_MIME);

  if (paths) {
    bridge.call('audio.import', { paths: JSON.parse(paths) as string[], trackId, start }).catch(reportError);
    return;
  }

  for (const file of Array.from(data.files)) {
    if (/\.midi?$/i.test(file.name)) importMidiFile(file, instrumentTrackId, start).catch(reportError);
    else upload(file, trackId, start).catch(reportError);
  }
}

export const isAudioDrag = (data: DataTransfer) =>
  data.types.includes(PATHS_MIME) || data.types.includes(MIDI_PATH_MIME) || data.types.includes('Files');
