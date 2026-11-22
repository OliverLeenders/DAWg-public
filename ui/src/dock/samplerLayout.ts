import type { SampleZone } from '../bridge/protocol';

// The geometry of the Sampler's panel, apart from its drawing.

const PAD_COLUMNS = 4;
const PAD_ROWS = 4;
const FIRST_PAD_KEY = 36;

/** The pads' keys in reading order: the lowest key at the bottom left, rising to the right and up, as on a drum machine. */
export function padKeys() {
  const keys: number[] = [];
  for (let row = PAD_ROWS - 1; row >= 0; row--) {
    for (let column = 0; column < PAD_COLUMNS; column++) keys.push(FIRST_PAD_KEY + row * PAD_COLUMNS + column);
  }
  return keys;
}

export type WaveMarker = 'start' | 'end' | 'loopStart' | 'loopEnd';

/** A zone's markers in frames, with an end of 0 as the file's end. Loop markers only while it loops. */
export function waveMarkers(zone: SampleZone, frames: number): Array<{ marker: WaveMarker; frame: number }> {
  const end = zone.end > 0 ? Math.min(zone.end, frames) : frames;
  const markers: Array<{ marker: WaveMarker; frame: number }> = [
    { marker: 'start', frame: zone.start },
    { marker: 'end', frame: end },
  ];
  if (zone.mode === 'loop') {
    markers.push({ marker: 'loopStart', frame: zone.loopStart }, { marker: 'loopEnd', frame: zone.loopEnd > 0 ? Math.min(zone.loopEnd, end) : end });
  }
  return markers;
}

/** Moves a marker, kept in order with its neighbours: start < loop start < loop end <= end. */
export function moveMarker(zone: SampleZone, frames: number, marker: WaveMarker, frame: number): Partial<SampleZone> {
  const end = zone.end > 0 ? Math.min(zone.end, frames) : frames;
  const loopEnd = zone.loopEnd > 0 ? Math.min(zone.loopEnd, end) : end;
  const clamp = (value: number, low: number, high: number) => Math.round(Math.min(Math.max(low, high), Math.max(low, value)));
  switch (marker) {
    case 'start':
      return { start: clamp(frame, 0, (zone.mode === 'loop' ? zone.loopStart : end) - 1) };
    case 'end':
      return { end: clamp(frame, Math.max(zone.start, zone.mode === 'loop' ? loopEnd : zone.start) + 1, frames) };
    case 'loopStart':
      return { loopStart: clamp(frame, zone.start, loopEnd - 1) };
    case 'loopEnd':
      return { loopEnd: clamp(frame, zone.loopStart + 1, end) };
  }
}

export type EnvelopePoint = 'attack' | 'decay' | 'release';

/** Where the envelope's points are, in fractions of its box with y up. Stages share the width by their times; a one-shot
    has no release, and a hold in the middle keeps the sustain in reach. */
export function envelopeLayout(zone: Pick<SampleZone, 'attackMs' | 'decayMs' | 'sustain' | 'releaseMs' | 'mode'>) {
  const release = zone.mode === 'oneShot' ? 0 : zone.releaseMs;
  const hold = Math.max(100, (zone.attackMs + zone.decayMs + release) / 3);
  // A decay of no time still gets some width, or its point would hide the attack's and neither could be told apart.
  const shownDecay = Math.max(zone.decayMs, 0.15 * hold);
  const totalMs = zone.attackMs + shownDecay + hold + release;
  const attack = zone.attackMs / totalMs;
  const decay = attack + shownDecay / totalMs;
  const held = decay + hold / totalMs;
  return {
    totalMs,
    points: { attack: [attack, 1], decay: [decay, zone.sustain], release: [held, zone.sustain] } as Record<EnvelopePoint, [number, number]>,
    curve: [[0, zone.attackMs > 0 ? 0 : 1], [attack, 1], [decay, zone.sustain], [held, zone.sustain], [1, release > 0 ? 0 : zone.sustain]] as Array<[number, number]>,
  };
}

export type EnvelopeSegment = 'attack' | 'decay' | 'release';

type HandleFields = 'attackHandleTime' | 'attackHandleLevel' | 'decayHandleTime' | 'decayHandleLevel' | 'releaseHandleTime' | 'releaseHandleLevel';

const clamp01 = (value: number) => Math.min(1, Math.max(0, value));

/** The envelope's sloped segments, in fractions of its box with y up: where each runs, from and to which level, and its
    tension point as fractions of its rectangle (0.5, 0.5 is straight). A one-shot's release isn't shown: it has none. */
export function envelopeSegments(
  zone: Pick<SampleZone, 'sustain' | 'mode' | HandleFields>,
  layout: ReturnType<typeof envelopeLayout>,
): Record<EnvelopeSegment, { x0: number; x1: number; from: number; to: number; handle: { time: number; level: number }; shown: boolean }> {
  const handle = (time: number | undefined, level: number | undefined) => ({ time: clamp01(time ?? 0.5), level: clamp01(level ?? 0.5) });
  const [attack] = layout.points.attack;
  const [decay] = layout.points.decay;
  const [held] = layout.points.release;
  const end = layout.curve[layout.curve.length - 1][1];
  return {
    attack: { x0: 0, x1: attack, from: layout.curve[0][1], to: 1, handle: handle(zone.attackHandleTime, zone.attackHandleLevel), shown: true },
    decay: { x0: attack, x1: decay, from: 1, to: zone.sustain, handle: handle(zone.decayHandleTime, zone.decayHandleLevel), shown: true },
    release: {
      x0: held,
      x1: 1,
      from: zone.sustain,
      to: end,
      handle: handle(zone.releaseHandleTime, zone.releaseHandleLevel),
      shown: zone.mode !== 'oneShot',
    },
  };
}

/** The fields that put a segment's tension point at (time, level), fractions of its rectangle. */
export function handleFields(segment: EnvelopeSegment, time: number, level: number): Partial<SampleZone> {
  return { [`${segment}HandleTime`]: clamp01(time), [`${segment}HandleLevel`]: clamp01(level) };
}

/** What dragging an envelope point changes, for a drag by fractions of the box (y up) from where the drag began. */
export function dragEnvelope(
  zone: Pick<SampleZone, 'attackMs' | 'decayMs' | 'sustain' | 'releaseMs'>,
  point: EnvelopePoint,
  totalMs: number,
  dx: number,
  dy: number,
): Partial<SampleZone> {
  const ms = (value: number) => Math.round(Math.min(10000, Math.max(0, value)));
  switch (point) {
    case 'attack':
      return { attackMs: ms(zone.attackMs + dx * totalMs) };
    case 'decay':
      return { decayMs: ms(zone.decayMs + dx * totalMs), sustain: Math.min(1, Math.max(0, zone.sustain + dy)) };
    case 'release':
      return { releaseMs: ms(zone.releaseMs - dx * totalMs), sustain: Math.min(1, Math.max(0, zone.sustain + dy)) };
  }
}
