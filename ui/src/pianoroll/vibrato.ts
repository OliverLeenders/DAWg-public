import { TICKS_PER_BEAT, type Track, type Vibrato } from '../bridge/protocol';
import { dispatch } from '../store/store';
import { setCurvesCommand, toEditNote, updateNotesCommand } from './edit';
import { hasVibrato, NO_VIBRATO, type PlacedNote } from './geometry';
import type { TempoMap } from '../time/tempoMap';
export { cycleTicks } from './geometry';

// Vibrato helpers shared by the inspector, the roll handles and the context menu.

export const VIBRATO_SHAPES = ['Sine', 'Triangle', 'Square', 'Saw up', 'Saw down'] as const;

export const VIBRATO_SYNC_OPTIONS: Array<{ label: string; beats: number }> = [
  { label: 'Free (Hz)', beats: 0 },
  { label: '1/2', beats: 2 },
  { label: '1/4', beats: 1 },
  { label: '1/8', beats: 1 / 2 },
  { label: '1/8T', beats: 1 / 3 },
  { label: '1/16', beats: 1 / 4 },
  { label: '1/16T', beats: 1 / 6 },
  { label: '1/32', beats: 1 / 8 },
];

export const VIBRATO_LIMITS = { minDepth: 0.05, maxDepth: 2, minRate: 0.5, maxRate: 12, maxBeats: 4 };

export const VIBRATO_PRESETS: Array<{ label: string; vibrato: Vibrato | null }> = [
  { label: 'Light', vibrato: { depth: 0.2, rateHz: 5.5, syncBeats: 0, shape: 0, delay: TICKS_PER_BEAT / 4, fadeIn: TICKS_PER_BEAT / 2 } },
  { label: 'Wide', vibrato: { depth: 0.6, rateHz: 5, syncBeats: 0, shape: 0, delay: 0, fadeIn: TICKS_PER_BEAT / 4 } },
  { label: 'Slow', vibrato: { depth: 0.4, rateHz: 3.5, syncBeats: 0, shape: 0, delay: TICKS_PER_BEAT / 2, fadeIn: TICKS_PER_BEAT } },
  { label: 'Fast', vibrato: { depth: 0.25, rateHz: 7.5, syncBeats: 0, shape: 0, delay: 0, fadeIn: 0 } },
  { label: 'Trill (1/32, ±1 st)', vibrato: { depth: 1, rateHz: 5.5, syncBeats: 1 / 8, shape: 2, delay: 0, fadeIn: 0 } },
  { label: 'Remove vibrato', vibrato: null },
];

export const vibratoOf = (placed: PlacedNote): Vibrato => placed.note.vibrato ?? NO_VIBRATO;

/** Writes vibrato settings to notes (one undo step; pass a gesture id to merge a drag). */
export function applyVibrato(
  track: Track,
  notes: PlacedNote[],
  change: Partial<Vibrato>,
  map: TempoMap,
  gesture?: string,
) {
  if (notes.length === 0) return;
  const edited = notes.map((n) => ({ ...toEditNote(n), vibrato: { ...vibratoOf(n), ...change } }));
  const command = updateNotesCommand(track, edited, map, 'Edit vibrato');
  if (command) dispatch(command, gesture);
}

/** Turns vibrato on (full intensity over the whole note) or off (intensity cleared) for notes. */
export function setVibratoIntensity(notes: PlacedNote[], on: boolean) {
  const command = setCurvesCommand(
    notes.map((placed) => ({ placed, lane: 'vibrato' as const, points: on ? [[0, 1, 0, 0] as [number, number, number, number]] : [] })),
    on ? 'Vibrato on' : 'Vibrato off',
  );
  if (command) dispatch(command);
}

/** Applies a preset: its settings plus full intensity, or clears the intensity for "Remove vibrato". */
export function applyVibratoPreset(track: Track, notes: PlacedNote[], vibrato: Vibrato | null, map: TempoMap) {
  if (notes.length === 0) return;
  if (vibrato === null) {
    setVibratoIntensity(notes, false);
    return;
  }
  const settings = updateNotesCommand(
    track,
    notes.map((n) => ({ ...toEditNote(n), vibrato })),
    map,
    'Vibrato preset',
  );
  const intensity = setCurvesCommand(
    notes.map((placed) => ({ placed, lane: 'vibrato' as const, points: [[0, 1, 0, 0]] })),
    'Vibrato preset',
  );
  const commands = [settings, intensity].flatMap((c) => (c && c.command === 'batch' ? c.args.commands : []));
  dispatch({ command: 'batch', args: { label: 'Vibrato preset', commands } });
}

export { hasVibrato };
