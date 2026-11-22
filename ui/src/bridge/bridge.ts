import type { Events, Methods } from './protocol';

// Typed wrapper around JUCE's WebBrowserComponent native integration (app/src/Bridge.h). A call goes out as a
// "dawg_call" event with an id; the answer comes back through window.__dawgReply(id, json), which every web view gets
// and only the one waiting for that id parses. The app encodes answers itself: JUCE's native functions escape them in
// a way that took 40 seconds for a large folder's listing.

interface JuceBackend {
  addEventListener(event: string, fn: (payload: unknown) => void): unknown;
  removeEventListener(token: never): void;
  emitEvent(event: string, payload: unknown): void;
}

// @juce-framework/webview declares window.__JUCE__ as always present; outside the
// app (plain browser tab) it is not, so read it through this helper.
const juceBackend = (): JuceBackend | undefined =>
  (window as unknown as { __JUCE__?: { backend: JuceBackend } }).__JUCE__?.backend;

export class BridgeError extends Error {}

type Reply<T> = { ok: true; result: T } | { ok: false; error: string };

// Calls waiting for their answers, by id; ids start at a random number, so two web views' don't meet.
const waiting = new Map<number, (reply: Reply<unknown>) => void>();
let nextId = Math.floor(Math.random() * 1e9) * 1000;
let connected = false;

declare global {
  interface Window {
    __dawgReply?: (id: number, json: string) => void;
  }
}

/** True when running inside the DAWg app (as opposed to a plain browser tab). */
export const isConnected = (): boolean => juceBackend() !== undefined;

/** Must be awaited once before call(). The JUCE helper touches window.__JUCE__
    at import time, so it is only loaded when running inside the app. */
export async function connect(): Promise<boolean> {
  if (!isConnected()) return false;
  if (!connected) {
    // The JUCE helper sets up window.__JUCE__'s event plumbing when it loads.
    await import('@juce-framework/webview');
    window.__dawgReply = (id, json) => {
      const resolve = waiting.get(id);
      if (!resolve) return; // another web view's call
      waiting.delete(id);
      resolve(JSON.parse(json) as Reply<unknown>);
    };
    connected = true;
  }
  return true;
}

export async function call<M extends keyof Methods>(
  method: M,
  ...params: Methods[M][0] extends void ? [] : [Methods[M][0]]
): Promise<Methods[M][1]> {
  const backend = juceBackend();
  if (!connected || !backend) throw new BridgeError('Not connected to the DAWg engine');
  const id = nextId++;
  const reply = await new Promise<Reply<unknown>>((resolve) => {
    waiting.set(id, resolve);
    backend.emitEvent('dawg_call', { id, method, params: params[0] ?? null });
  });
  if (!reply.ok) throw new BridgeError(reply.error);
  return reply.result as Methods[M][1];
}

/** Subscribes to an event pushed from C++. Returns an unsubscribe function. */
export function on<E extends keyof Events>(event: E, fn: (payload: Events[E]) => void): () => void {
  const backend = juceBackend();
  if (!backend) return () => {};
  const token = backend.addEventListener(event, (payload) => fn(payload as Events[E]));
  return () => backend.removeEventListener(token as never);
}
