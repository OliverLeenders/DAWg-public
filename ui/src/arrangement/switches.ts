import type { Command, Project } from '../bridge/protocol';

// The global mute and solo switches, Cubase-style (docs/design/phase6-arrangement.md, section 7): they light while any
// track is muted (or soloed), and a click clears them all as one undo step, remembering which tracks they were. A
// click while none is on restores the remembered ones.

export type SwitchKey = 'mute' | 'solo';

/** The ids of the tracks that have the switch on, in track order (the master has no switches). */
export function tracksWith(project: Project | null, key: SwitchKey): number[] {
  if (!project) return [];
  return project.trackOrder.filter((id) => project.tracks[String(id)]?.[key] === true);
}

/** The remembered tracks that can still be restored: those that exist and have the switch off. */
export function restorable(project: Project | null, key: SwitchKey, remembered: readonly number[]): number[] {
  if (!project) return [];
  return remembered.filter((id) => project.tracks[String(id)] !== undefined && !project.tracks[String(id)][key]);
}

/** What a click on a global switch does: the command to send (or null), and the tracks to remember afterwards. */
export interface SwitchClick {
  command: Command | null;
  remember: number[];
}

const batch = (label: string, ids: number[], key: SwitchKey, on: boolean): Command => ({
  command: 'batch',
  args: {
    label,
    commands: ids.map((id) => ({ command: 'setTrackSwitches' as const, args: { id, [key]: on } })),
  },
});

/** Clears the switch on every track that has it on and remembers them; with none on, restores the remembered tracks
    that still exist (and then forgets them). */
export function switchClick(project: Project | null, key: SwitchKey, remembered: readonly number[]): SwitchClick {
  const on = tracksWith(project, key);
  const noun = key === 'mute' ? 'mute' : 'solo';
  if (on.length > 0) return { command: batch(`Un${noun} all tracks`, on, key, false), remember: on };
  const back = restorable(project, key, remembered);
  if (back.length > 0) return { command: batch(`Restore ${noun}s`, back, key, true), remember: [] };
  return { command: null, remember: [] };
}
