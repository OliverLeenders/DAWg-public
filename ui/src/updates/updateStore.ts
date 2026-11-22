import { create } from 'zustand';
import * as bridge from '../bridge/bridge';
import { useDawg } from '../store/store';
import { checkDue, failureText, formatVersion, parseReleases, pickUpdate, type Release } from './updates';

// The update check's state (docs/design/update-checks.md): settings kept in the user's settings (UC7), the release on
// offer, and what a manual check found.

const KEYS = {
  auto: 'updates.auto',
  beta: 'updates.beta',
  skipped: 'updates.skipped',
  lastCheck: 'updates.lastCheck',
  asked: 'updates.asked',
} as const;

/** The automatic check waits this long after startup, so it never competes with opening a project (UC3). */
export const STARTUP_DELAY_MS = 5000;

interface UpdateState {
  /** Settings, once read. */
  loaded: boolean;
  auto: boolean;
  beta: boolean;
  skipped: string | null;
  /** Whether the first-start notice was answered (UC4). */
  asked: boolean;
  checking: boolean;
  /** The release on offer: the notice shows while it is set and not put off. */
  offer: Release | null;
  dialogOpen: boolean;
  /** What a manual check found when there is nothing to offer: shown in the dialog. */
  manualResult: { kind: 'upToDate' } | { kind: 'failed'; message: string } | null;
  init: () => Promise<void>;
  check: (manual: boolean) => Promise<void>;
  answerFirstRun: (auto: boolean) => void;
  setAuto: (auto: boolean) => void;
  setBeta: (beta: boolean) => void;
  skip: () => void;
  later: () => void;
  download: () => void;
  closeDialog: () => void;
}

const read = (key: string) => bridge.call('settings.get', { key }).catch(() => null);
const write = (key: string, value: string) => bridge.call('settings.set', { key, value }).catch(() => {});

let manualPending = false;

export const useUpdates = create<UpdateState>((set, get) => {
  const finish = (result: { status: number; body: string; error: string }) => {
    const manual = manualPending;
    manualPending = false;
    set({ checking: false });
    const ok = result.error === '' && result.status === 200;
    if (!ok) {
      // An automatic check that fails stays quiet (UC5).
      if (manual) set({ manualResult: { kind: 'failed', message: failureText(result.status, result.error) }, dialogOpen: true });
      return;
    }
    write(KEYS.lastCheck, String(Date.now()));
    const { beta, skipped } = get();
    const current = useDawg.getState().app?.version ?? '0.0.0';
    // A manual check offers a skipped version again: the user asked.
    const offer = pickUpdate(parseReleases(result.body), current, { beta, skipped: manual ? null : skipped });
    if (offer) set({ offer, manualResult: null, dialogOpen: manual });
    else if (manual) set({ manualResult: { kind: 'upToDate' }, dialogOpen: true });
  };

  return {
    loaded: false,
    auto: true,
    beta: false,
    skipped: null,
    asked: true,
    checking: false,
    offer: null,
    dialogOpen: false,
    manualResult: null,

    init: async () => {
      if (get().loaded) return;
      bridge.on('dawg_updates', finish);
      const [auto, beta, skipped, lastCheck, asked] = await Promise.all([
        read(KEYS.auto),
        read(KEYS.beta),
        read(KEYS.skipped),
        read(KEYS.lastCheck),
        read(KEYS.asked),
      ]);
      set({ loaded: true, auto: auto !== '0', beta: beta === '1', skipped: skipped || null, asked: asked === '1' });
      // Nothing is checked before the first-start notice is answered (UC4).
      if (asked !== '1' || auto === '0') return;
      if (checkDue(lastCheck ? Number(lastCheck) : null, Date.now())) setTimeout(() => void get().check(false), STARTUP_DELAY_MS);
    },

    check: async (manual) => {
      if (manual) {
        manualPending = true;
        set({ manualResult: null });
      }
      if (get().checking) return;
      set({ checking: true });
      // False means a fetch is already running, and its answer will come; an error means none will.
      await bridge.call('updates.fetch').catch((e: unknown) => finish({ status: 0, body: '', error: e instanceof Error ? e.message : String(e) }));
    },

    answerFirstRun: (auto) => {
      write(KEYS.asked, '1');
      write(KEYS.auto, auto ? '1' : '0');
      set({ asked: true, auto });
      if (auto) void get().check(false);
    },

    setAuto: (auto) => {
      write(KEYS.auto, auto ? '1' : '0');
      write(KEYS.asked, '1');
      set({ auto, asked: true });
    },

    setBeta: (beta) => {
      write(KEYS.beta, beta ? '1' : '0');
      set({ beta });
    },

    skip: () => {
      const offer = get().offer;
      if (offer) {
        const version = formatVersion(offer.version);
        write(KEYS.skipped, version);
        set({ skipped: version });
      }
      set({ offer: null, dialogOpen: false });
    },

    later: () => set({ offer: null, dialogOpen: false }),

    download: () => {
      const offer = get().offer;
      if (offer) bridge.call('app.openUrl', { url: offer.url }).catch((e) => useDawg.setState({ lastError: String(e) }));
      set({ offer: null, dialogOpen: false });
    },

    closeDialog: () => set({ dialogOpen: false, manualResult: null }),
  };
});
