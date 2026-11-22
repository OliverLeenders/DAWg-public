import * as bridge from '../bridge/bridge';

// Stretched audio is rendered once a drag that changes it ends, not on every step of it
// (docs/design/time-stretching.md, TS9); the repitch preview plays meanwhile.

/** Holds renders back while a drag runs (true when it starts, false when it ends). */
export function holdRenders(held: boolean) {
  if (bridge.isConnected()) bridge.call('stretch.hold', { held }).catch(() => {});
}
