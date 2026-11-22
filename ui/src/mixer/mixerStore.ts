import { create } from 'zustand';

// The mixer's view settings (docs/design/mixer.md, X4 and X5): which sections show and how wide the strips are. Per
// user (browser storage, a key of its own so the two windows never overwrite each other's layout).

export interface MixerSettings {
  io: boolean;
  inserts: boolean;
  sends: boolean;
  /** The track offsets: a power setting, hidden unless asked for (docs/ui-review-2026-11-10.md, 3.6). */
  offsets: boolean;
  narrow: boolean;
}

const DEFAULTS: MixerSettings = { io: true, inserts: true, sends: true, offsets: false, narrow: false };
const STORAGE_KEY = 'dawg.mixer';

function load(): MixerSettings {
  try {
    return { ...DEFAULTS, ...(JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '{}') as Partial<MixerSettings>) };
  } catch {
    return DEFAULTS;
  }
}

export const useMixer = create<MixerSettings>()(load);

useMixer.subscribe((state) => {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(state));
  } catch {
    // Storage unavailable: the settings just aren't remembered.
  }
});

export const toggleMixer = (key: keyof MixerSettings) => useMixer.setState((s) => ({ [key]: !s[key] }));
