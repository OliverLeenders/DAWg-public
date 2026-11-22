import type { WindowName } from './bridge/protocol';

// Which window this UI runs in (docs/design/mixer.md, X11): the app loads the same bundle in the mixer window with
// ?window=mixer, in the MIDI editor window with ?window=roll and in the audio editor with ?window=audio.

const params = typeof window !== 'undefined' ? new URLSearchParams(window.location.search) : null;
const requested = params?.get('window');

export const WINDOW: WindowName =
  requested === 'mixer'
    ? 'mixer'
    : requested === 'roll'
      ? 'roll'
      : requested === 'audio'
        ? 'audio'
        : requested === 'instrument'
          ? 'instrument'
          : 'main';

export const isMixerWindow = WINDOW === 'mixer';
export const isRollWindow = WINDOW === 'roll';
/** The audio editor (docs/design/time-stretching.md, TS13), opened with ?window=audio&clip=<id>. */
export const isAudioWindow = WINDOW === 'audio';
/** An instrument's editor (docs/design/macros-and-modulation-column.md, MC14), opened with ?window=instrument&track=<id>&node=<id>. */
export const isInstrumentWindow = WINDOW === 'instrument';

function positiveId(value: string | null) {
  const id = value === null ? Number.NaN : Number(value);
  return Number.isInteger(id) && id > 0 ? id : null;
}

/** The track and clip a new MIDI editor window opens on (later changes arrive through the selection relay). */
export const initialRollTrack = isRollWindow ? positiveId(params?.get('track') ?? null) : null;
export const initialRollClip = isRollWindow ? positiveId(params?.get('clip') ?? null) : null;

/** The clip a new audio editor window opens on (later ones arrive as dawg_audioEditorClip). */
export const initialAudioClip = isAudioWindow ? positiveId(params?.get('clip') ?? null) : null;

/** The instrument a new instrument window opens on (later ones arrive as dawg_instrumentEditor). */
export const initialInstrument = isInstrumentWindow
  ? { trackId: positiveId(params?.get('track') ?? null), nodeId: positiveId(params?.get('node') ?? null) }
  : null;
