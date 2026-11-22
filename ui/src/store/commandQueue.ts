import type { Command } from '../bridge/protocol';

// Model commands go to the engine one at a time: the next is sent when the previous one's reply is in. A drag
// produces values faster than a slow round trip can take them, so while a command is out, a newer value for the
// same control in the same gesture replaces the one still waiting. The engine then only ever lags by one value
// instead of working through a backlog after the drag.

/** The arguments holding the new values, for commands that set values (the rest of the args name the target). */
const VALUE_ARG: Partial<Record<Command['command'], string | string[]>> = {
  setTempo: 'bpm',
  setTrackGain: 'gainDb',
  setTrackPan: 'pan',
  setTrackOffset: 'offsetMs',
  setTrackHeight: 'height',
  setNodeParam: 'value',
  setModulation: 'depth',
  setSend: 'levelDb',
  setTempoPoint: ['tick', 'bpm'],
};

interface Entry {
  send: () => Promise<unknown>;
  /** Set for a value command within a gesture: entries with the same key set the same thing. */
  key?: string;
  gesture?: string;
}

/** The arguments of a zone edit that name what is edited; every other argument is a new value. */
const ZONE_TARGET = ['trackId', 'nodeId', 'zoneId', 'scope'];

/** What a command sets, or undefined when it can't be replaced by a newer one. */
export function coalesceKey(command: Command, gesture?: string): string | undefined {
  // A zone edit sets whichever fields it carries: a newer edit of the same fields of the same zone replaces it.
  if (command.command === 'setSampleZone') {
    if (!gesture) return undefined;
    const args = command.args as Record<string, unknown>;
    const target = ZONE_TARGET.map((name) => args[name]);
    const fields = Object.keys(args).filter((name) => !ZONE_TARGET.includes(name)).sort();
    return `${gesture}|setSampleZone|${JSON.stringify(target)}|${fields.join(',')}`;
  }
  const valueArg = VALUE_ARG[command.command];
  if (!gesture || valueArg === undefined) return undefined;
  const values = Array.isArray(valueArg) ? valueArg : [valueArg];
  const target = Object.entries(command.args as Record<string, unknown>).filter(([name]) => !values.includes(name));
  return `${gesture}|${command.command}|${JSON.stringify(target)}`;
}

export function createCommandQueue() {
  const waiting: Entry[] = [];
  let busy = false;

  const next = () => {
    const entry = waiting.shift();
    busy = entry !== undefined;
    if (entry) entry.send().then(next, next);
  };

  return {
    /** Queues a send. With a key, replaces a waiting entry with the same key, looking back only over value
        commands of the same gesture (which set other things, so the order among them doesn't matter). */
    push(send: () => Promise<unknown>, key?: string, gesture?: string) {
      if (key !== undefined) {
        for (let i = waiting.length - 1; i >= 0 && waiting[i].key !== undefined && waiting[i].gesture === gesture; i--) {
          if (waiting[i].key === key) {
            waiting[i] = { send, key, gesture };
            return;
          }
        }
      }
      waiting.push({ send, key, gesture });
      if (!busy) next();
    },
    /** Entries not sent yet (for tests). */
    get pending() {
      return waiting.length;
    },
  };
}
