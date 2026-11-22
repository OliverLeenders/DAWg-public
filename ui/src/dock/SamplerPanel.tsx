import { Readout, Readouts } from '../components/Readouts';
import { Ellipsis, Plus } from 'lucide-react';
import { ValueText } from '../components/ValueText';
import { parseNumber } from '../graph/params';
import { useEffect, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { NODE_TYPES, type Command, type SampleZone } from '../bridge/protocol';
import { FOLDER_MIME, PATHS_MIME } from '../arrangement/importAudio';
import { DragNumber } from '../components/DragNumber';
import { parsePitch } from '../pianoroll/geometry';
import { Auditioner } from '../pianoroll/audition';
import { dispatch, newGestureId, reportError, useDawg } from '../store/store';
import { DEFAULT_DRUM_MAP, getDrumPiece } from '../store/drumMap';
import { DragKnob, Knob } from './Knob';
import { useParam } from './useParam';
import { SampleWaveform } from './SampleWaveform';
import { padKeys } from './samplerLayout';
import { Section, SectionHeader } from './Section';
import { ZoneEnvelope } from './ZoneEnvelope';
import { useZoneEdit } from './useZoneEdit';
import type { PanelProps } from './panels';
import { powerTaper } from '../components/dragScale';
import { Button, IconButton, Segmented, Select } from '../ui/controls';
import { Menu, Popover } from '../ui/overlays';
import { Tab, Tabs } from '../ui/Tabs';
import { MenuItem } from '../components/MenuItem';

// A crossfade drags finer at its short end (dragScale.ts).
const CROSSFADE_TAPER = powerTaper(0, 480000, 3);

/** How long a zone's envelope stages may be, as dragging them allows (samplerLayout.ts). */
const MAX_ENVELOPE_MS = 10000;
/** Envelope times on knobs: a cubic curve, so half the sweep covers the first 1.25 s. */
const ENVELOPE_TAPER = 3;

// The Sampler's panel (docs/design/sampler-midi-delay-limiter.md, section 4): its zones as drum pads or a list, the selected
// zone's sample with its envelope under it to edit by dragging, and its sound beside them. The layout follows the panel's
// size (docs/ui-review-2026-11-10.md, 5.2): the pads are square and as tall as the panel, the envelope is a short strip
// with knobs under it where there is room (values to type where there isn't), and the controls move under the sample
// when it's narrow. A zone's keys are behind its mapping button.
// Sounds dragged from the sound explorer become zones, on a pad's key when dropped on a pad; clicking a pad plays it.
// From the keyboard: Enter or Space plays the focused pad, the arrows move through the pads or the zones.

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const PAD_KEYS = padKeys();
const MODES: Array<{ mode: SampleZone['mode']; name: string; title: string }> = [
  { mode: 'oneShot', name: 'One-shot', title: 'Plays to its end whatever the length of the note' },
  { mode: 'gate', name: 'Gate', title: 'Plays while the note is held, then releases' },
  { mode: 'loop', name: 'Loop', title: 'Loops while the note is held, then releases' },
];

// The whole sampler's choices (docs/design/sampler-mapping.md, PM1 and TL2), each a row of its own under the knobs, as
// the zone's play modes are. Pitch: Tape plays higher notes faster and shorter; Keep length lasts as long as the sample.
// Takes: zones with the same keys and velocities in a layer play in turn, or at random.
const PITCH_MODES = [
  { name: 'Tape', title: 'Pitch: Tape. A higher note plays the sample faster, so it is shorter' },
  { name: 'Keep length', title: 'Pitch: Keep length. Every note lasts as long as the sample, whatever its pitch' },
];
const TAKE_MODES = [
  { name: 'Takes in turn', title: 'Takes: zones with the same keys and velocities play one after the other' },
  { name: 'At random', title: 'Takes: zones with the same keys and velocities play at random, never one twice in a row' },
];

function SamplerChoice({
  track,
  node,
  param,
  label,
  choices,
}: Pick<PanelProps, 'track' | 'node'> & { param: PanelProps['info']['params'][number]; label: string; choices: typeof PITCH_MODES }) {
  const edit = useParam(track, node, param);
  return (
    <div className="sampler-choice">
      <span className="sampler-choice-label">{label}</span>
      <Segmented
        className="sampler-modes"
        label={label}
        value={Math.round(edit.value)}
        options={choices.map(({ name, title }, i) => ({ value: i, label: name, title }))}
        onChange={(i) => edit.set(i)}
      />
    </div>
  );
}

/** A pan from -1 to 1 as the track's pan reads: C, L30, R30. */
const panText = (pan: number) => {
  const amount = Math.round(Math.abs(pan) * 100);
  return amount === 0 ? 'C' : `${pan < 0 ? 'L' : 'R'}${amount}`;
};

/** A MIDI key's name, with C4 at key 60. */
export const keyName = (key: number) => {
  const whole = Math.round(key);
  return `${NOTE_NAMES[whole % 12]}${Math.floor(whole / 12) - 1}`;
};

/** The zone a key plays: the first that holds it, as in the engine. */
export const zoneOnKey = (zones: SampleZone[], key: number) => zones.find((z) => key >= z.lowKey && key <= z.highKey);

const keysOf = (zone: SampleZone) => (zone.lowKey === zone.highKey ? keyName(zone.lowKey) : `${keyName(zone.lowKey)}–${keyName(zone.highKey)}`);

export function SamplerPanel({ track, node, info }: PanelProps) {
  const files = useDawg((s) => s.project?.audioFiles);
  const zones = node.zones ?? [];
  const [selectedId, setSelectedId] = useState<number | null>(null);
  const [pads, setPads] = useState(true);
  // A zone's context menu, an empty pad's (its key), or (with neither) the menu for all of them.
  const [menu, setMenu] = useState<{ zone?: SampleZone; key?: number; x: number; y: number } | null>(null);
  const auditioner = useRef(new Auditioner());
  // The pad the keyboard is on: one tab stop in the pads, moved by the arrows.
  const [focusPad, setFocusPad] = useState<number | null>(null);
  const padButtons = useRef<Array<HTMLButtonElement | null>>([]);
  const zoneRows = useRef<Array<HTMLDivElement | null>>([]);
  const selected = zones.find((z) => z.id === selectedId) ?? zones[0];
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const nameOf = (zone: SampleZone) => files?.[String(zone.fileId)]?.name ?? 'Missing file';
  const target = { trackId: track.id, nodeId: node.id };
  // Keep length renders each zone at its keys' pitches in the background (docs/design/sampler-mapping.md, PM2, PM3).
  const rendering = useDawg((s) => s.stretchPending.has(node.id));

  useEffect(() => {
    const player = auditioner.current;
    return () => player.stop();
  }, []);

  const send = (zone: SampleZone, fields: Partial<SampleZone>, gesture?: string) =>
    dispatch({ command: 'setSampleZone', args: { ...target, zoneId: zone.id, ...fields } } as Command, gesture);
  // The selected zone as edited: drags show at once and go out once per frame (useZoneEdit).
  const { shown, edit } = useZoneEdit(selected, send);

  // Files, or a folder from the sound explorer: on a pad they start at its key, elsewhere they're mapped by their
  // notes (docs/design/sampler-mapping.md, SM1).
  const dropProps = (key?: number) => ({
    onDragOver: (e: React.DragEvent) => {
      if (!e.dataTransfer.types.includes(PATHS_MIME) && !e.dataTransfer.types.includes(FOLDER_MIME)) return;
      e.preventDefault();
      e.stopPropagation();
    },
    onDrop: (e: React.DragEvent) => {
      const paths = e.dataTransfer.getData(PATHS_MIME) || e.dataTransfer.getData(FOLDER_MIME);
      if (!paths) return;
      e.preventDefault();
      e.stopPropagation();
      // A pad that holds this key alone gets the new sound in place of its old one.
      const onPad = key !== undefined ? zones.find((z) => z.lowKey === key && z.highKey === key) : undefined;
      bridge.call('sampler.import', { ...target, paths: JSON.parse(paths) as string[], key, replaceZone: onPad?.id }).catch(reportError);
    },
  });

  const zoneMenu = (zone: SampleZone) => (e: React.MouseEvent) => {
    e.preventDefault();
    setMenu({ zone, x: e.clientX, y: e.clientY });
  };

  // Every zone at once, as one step to undo.
  const allZones = (label: string, command: (zone: SampleZone) => Command) =>
    dispatch({ command: 'batch', args: { label, commands: zones.map(command) } } as Command);
  const setAllModes = (mode: SampleZone['mode'], name: string) =>
    allZones(`All samples ${name}`, (zone) => ({ command: 'setSampleZone', args: { ...target, zoneId: zone.id, mode } }) as Command);
  const removeAll = () => allZones('Remove all samples', (zone) => ({ command: 'removeSampleZone', args: { ...target, zoneId: zone.id } }) as Command);

  // The zone's root key and tune from the pitch of its sound (docs/design/sampler-mapping.md, SM4).
  const detectPitch = (zone: SampleZone) =>
    bridge
      .call('sampler.detectPitch', { fileId: zone.fileId, start: zone.start, end: zone.end })
      .then((pitch) => {
        if (!pitch) {
          useDawg.setState({ lastHint: `No clear pitch in ${nameOf(zone)}` });
          return;
        }
        const rootKey = Math.min(127, Math.max(0, Math.round(pitch)));
        // The tune plays the sample at the root key's pitch: a sound 30 cents sharp is tuned 30 cents down.
        const tune = Math.round((rootKey - pitch) * 100) / 100;
        send(zone, { rootKey, tune });
        useDawg.setState({ lastHint: `${nameOf(zone)}: ${keyName(rootKey)}${tune ? ` ${tune > 0 ? '+' : ''}${Math.round(tune * 100)} cents` : ''}` });
      })
      .catch(reportError);
  const layered = zones.some((z) => z.layer > 1);

  // Outputs (docs/design/multi-output-instruments.md, MO1, MO5): each one's name, the output track it leads to where it
  // is wired to one. Only a Sampler on a track has them, not a Sample Voice.
  const tracks = useDawg((s) => s.project?.tracks);
  const outputNames =
    node.type === NODE_TYPES.sampler
      ? Array.from({ length: 16 }, (_, port) => {
          if (port === 0) return 'Main';
          const cable = Object.values(track.graph.connections).find((c) => c.fromNode === node.id && c.fromPort === port);
          const to = cable ? track.graph.nodes[String(cable.toNode)] : undefined;
          const out = to?.type === NODE_TYPES.busOutput && to.target ? tracks?.[String(to.target)] : undefined;
          return out ? out.name : cable ? `Out ${port + 1}` : `Out ${port + 1} (plays in Main)`;
        })
      : undefined;
  // Groups of zones still on Main, each to an output track of its own, in one step (MO5).
  const split = (groups: Array<{ name: string; zoneIds: number[] }>) => {
    if (groups.length === 0) {
      useDawg.setState({ lastHint: 'Every sample already plays from an output of its own' });
      return;
    }
    dispatch({ command: 'splitInstrumentOutputs', args: { ...target, outputs: groups } });
  };
  const onMain = zones.filter((z) => (z.output ?? 1) === 1);
  const eachPad = () => {
    const pads = new Map<string, SampleZone[]>();
    for (const zone of [...onMain].sort((a, b) => a.lowKey - b.lowKey || a.highKey - b.highKey)) pads.set(`${zone.lowKey}:${zone.highKey}`, [...(pads.get(`${zone.lowKey}:${zone.highKey}`) ?? []), zone]);
    split(
      [...pads.values()].map((pad) => ({
        name: (pad[0].lowKey === pad[0].highKey ? getDrumPiece(DEFAULT_DRUM_MAP, pad[0].lowKey)?.name : undefined) ?? nameOf(pad[0]),
        zoneIds: pad.map((z) => z.id),
      })),
    );
  };
  const eachZone = () => split(onMain.map((zone) => ({ name: nameOf(zone), zoneIds: [zone.id] })));

  return (
    <div className="device-panel sampler-panel" {...dropProps()}>
      <div className="sampler-layout">
        <div className="sampler-list">
          <div className="sampler-bar">
            <Tabs variant="view" label="Show the samples as">
              <Tab selected={pads} onClick={() => setPads(true)}>
                Pads
              </Tab>
              <Tab selected={!pads} onClick={() => setPads(false)}>
                Zones
              </Tab>
            </Tabs>
            <IconButton title="Add samples" onClick={() => bridge.call('sampler.importDialog', target).catch(reportError)}>
              <Plus size={14} />
            </IconButton>
            <IconButton
              title="All samples: play mode, remove"
              disabled={zones.length === 0}
              onClick={(e) => {
                const r = e.currentTarget.getBoundingClientRect();
                setMenu({ x: r.left, y: r.bottom + 2 });
              }}
            >
              <Ellipsis size={14} />
            </IconButton>
          </div>
          {pads ? (
            <div className="sampler-pads" role="group" aria-label="Pads: Enter or Space plays, the arrows move">
              {PAD_KEYS.map((key, position) => {
                const zone = zoneOnKey(zones, key);
                const piece = getDrumPiece(DEFAULT_DRUM_MAP, key)?.name;
                const tabStop = focusPad ?? Math.max(0, PAD_KEYS.findIndex((k) => !!selected && zoneOnKey(zones, k) === selected));
                // The arrows move through the grid as drawn, four pads a row; a pad with a sound is selected as it's reached.
                const move = (step: number) => {
                  const next = position + step;
                  if (next < 0 || next >= PAD_KEYS.length) return;
                  const nextZone = zoneOnKey(zones, PAD_KEYS[next]);
                  if (nextZone) setSelectedId(nextZone.id);
                  setFocusPad(next);
                  padButtons.current[next]?.focus();
                };
                return (
                  <button
                    key={key}
                    ref={(b) => {
                      padButtons.current[position] = b;
                    }}
                    className={`sampler-pad ${zone ? 'filled' : ''} ${zone && zone === selected ? 'selected' : ''}`}
                    title={zone ? `${nameOf(zone)}, key ${keyName(key)}` : `Key ${keyName(key)}: drop a sound here, or right-click to add one`}
                    aria-current={!!zone && zone === selected}
                    tabIndex={position === tabStop ? 0 : -1}
                    onFocus={() => setFocusPad(position)}
                    onPointerDown={(e) => {
                      if (e.button !== 0 || !zone) return;
                      setSelectedId(zone.id);
                      auditioner.current.play(track.id, key);
                    }}
                    onPointerUp={() => auditioner.current.stop()}
                    onPointerLeave={() => auditioner.current.stop()}
                    onKeyDown={(e) => {
                      if (e.key === 'Enter' || e.key === ' ') {
                        e.preventDefault();
                        if (e.repeat || !zone) return;
                        setSelectedId(zone.id);
                        auditioner.current.play(track.id, key);
                      } else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight' || e.key === 'ArrowUp' || e.key === 'ArrowDown') {
                        e.preventDefault();
                        move({ ArrowLeft: -1, ArrowRight: 1, ArrowUp: -4, ArrowDown: 4 }[e.key]);
                      }
                    }}
                    onKeyUp={(e) => {
                      if (e.key !== 'Enter' && e.key !== ' ') return;
                      e.preventDefault();
                      auditioner.current.stop();
                    }}
                    onContextMenu={
                      zone
                        ? zoneMenu(zone)
                        : (e) => {
                            e.preventDefault();
                            setMenu({ key, x: e.clientX, y: e.clientY });
                          }
                    }
                    {...dropProps(key)}
                  >
                    <span className="sampler-pad-top">
                      <span className="sampler-pad-key">{piece ?? keyName(key)}</span>
                      {piece && (
                        <span className="sampler-pad-note" aria-hidden>
                          {keyName(key)}
                        </span>
                      )}
                    </span>
                    <span className="sampler-pad-name">{zone ? nameOf(zone) : ''}</span>
                  </button>
                );
              })}
            </div>
          ) : (
            <div className="sampler-zones" role="listbox" aria-label="Zones: the arrows move, Enter plays">
              {zones.map((zone, row) => {
                // The arrows move the selection, and the keyboard with it.
                const moveTo = (next: number) => {
                  const nextZone = zones[next];
                  if (!nextZone) return;
                  setSelectedId(nextZone.id);
                  zoneRows.current[next]?.focus();
                };
                return (
                  <div
                    key={zone.id}
                    ref={(r) => {
                      zoneRows.current[row] = r;
                    }}
                    role="option"
                    aria-selected={zone === selected}
                    tabIndex={zone === selected ? 0 : -1}
                    className={`sampler-zone ${zone === selected ? 'selected' : ''}`}
                    onClick={() => setSelectedId(zone.id)}
                    onKeyDown={(e) => {
                      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
                        e.preventDefault();
                        moveTo(row + (e.key === 'ArrowDown' ? 1 : -1));
                      } else if (e.key === 'Enter' || e.key === ' ') {
                        e.preventDefault();
                        if (!e.repeat) auditioner.current.play(track.id, zone.rootKey);
                      }
                    }}
                    onKeyUp={(e) => (e.key === 'Enter' || e.key === ' ') && auditioner.current.stop()}
                    onContextMenu={zoneMenu(zone)}
                  >
                    <span className="sampler-zone-name">{nameOf(zone)}</span>
                    {layered && (
                      <span className="sampler-zone-layer" title="A note plays a zone from each layer that holds it">
                        Layer {zone.layer}
                      </span>
                    )}
                    <span className="sampler-zone-keys">{keysOf(zone)}</span>
                  </div>
                );
              })}
              {zones.length === 0 && <p className="dock-empty">No zones yet. Drop sounds here from the sound explorer, or add them with +.</p>}
            </div>
          )}
        </div>
        <div className="sampler-editors">
          {shown ? (
            <>
              <SampleWaveform zone={shown} file={files?.[String(shown.fileId)]} onChange={edit} />
              <div className="sampler-envelope-box">
                <ZoneEnvelope zone={shown} onChange={edit} />
                <ZoneEnvelopeKnobs zone={shown} send={edit} />
                <ZoneEnvelopeValues zone={shown} send={edit} />
              </div>
            </>
          ) : (
            <p className="dock-empty sampler-empty">No sound selected. Drop one on a pad, or add one with +.</p>
          )}
        </div>
        <div className="sampler-side">
          {shown && (
            <ZoneSound
              zone={shown}
              send={edit}
              sampleRate={files?.[String(shown.fileId)]?.sampleRate ?? 48000}
              outputNames={outputNames}
              onOwnOutput={(zone) => split([{ name: nameOf(zone), zoneIds: [zone.id] }])}
            />
          )}
          <div className="panel-sections sampler-whole">
            <Section label="Whole sampler">
              {['level', 'velocity'].map((id) => (
                <Knob key={id} track={track} node={node} param={param(id)} />
              ))}
              <SamplerChoice track={track} node={node} param={param('pitchMode')} label="Pitch" choices={PITCH_MODES} />
              {rendering && (
                <p className="sampler-rendering" title="Until the renders are ready, notes keep their length with grains, which sound rougher">
                  Rendering pitches…
                </p>
              )}
              <SamplerChoice track={track} node={node} param={param('takes')} label="Takes" choices={TAKE_MODES} />
            </Section>
          </div>
        </div>
      </div>
      {menu && (
        <Menu onClose={() => setMenu(null)} at={{ x: menu.x, y: menu.y }} onClick={(e) => (e.target as HTMLElement).closest('.menu-item') && setMenu(null)}>
          {menu.zone ? (
            <ZoneMenu
              zone={menu.zone}
              name={nameOf(menu.zone)}
              onReplace={(zone) => bridge.call('sampler.importDialog', { ...target, replaceZone: zone.id }).catch(reportError)}
              onDetectPitch={detectPitch}
              onRemove={(zone) => dispatch({ command: 'removeSampleZone', args: { ...target, zoneId: zone.id } })}
            />
          ) : menu.key !== undefined ? (
            <>
              <div className="menu-title">{getDrumPiece(DEFAULT_DRUM_MAP, menu.key)?.name ?? keyName(menu.key)}</div>
              <MenuItem icon={Plus} label="Add sample here…" onClick={() => bridge.call('sampler.importDialog', { ...target, key: menu.key }).catch(reportError)} />
            </>
          ) : (
            <>
              <div className="menu-title">All {zones.length} samples</div>
              <div className="menu-heading">Play mode</div>
              {MODES.map(({ mode, name, title }) => (
                <MenuItem key={mode} label={name} title={title} onClick={() => setAllModes(mode, name)} />
              ))}
              {outputNames && (
                <>
                  <div className="menu-separator" />
                  <MenuItem
                    label={pads ? 'Each pad to its own track' : 'Each zone to its own track'}
                    title="An output track for each pad (or key range) whose samples play from Main, wired in one step"
                    onClick={pads ? eachPad : eachZone}
                  />
                </>
              )}
              <div className="menu-separator" />
              <MenuItem label="Remove all samples" danger onClick={removeAll} />
            </>
          )}
        </Menu>
      )}
    </div>
  );
}

/** A zone's context menu. */
function ZoneMenu({
  zone,
  name,
  onReplace,
  onDetectPitch,
  onRemove,
}: {
  zone: SampleZone;
  name: string;
  onReplace: (zone: SampleZone) => void;
  onDetectPitch: (zone: SampleZone) => void;
  onRemove: (zone: SampleZone) => void;
}) {
  return (
    <>
      <div className="menu-title">{name}</div>
      <MenuItem label="Replace sample…" onClick={() => onReplace(zone)} />
      <MenuItem label="Detect pitch" title="Sets the root key and tune from the pitch of the sound" onClick={() => onDetectPitch(zone)} />
      <div className="menu-separator" />
      <MenuItem label="Remove sample" danger onClick={() => onRemove(zone)} />
    </>
  );
}

/** The zone's envelope as knobs under its graph, where the panel has room for them. */
function ZoneEnvelopeKnobs({ zone, send }: { zone: SampleZone; send: (fields: Partial<SampleZone>, gesture?: string) => void }) {
  const gesture = useRef(newGestureId());
  const ms = (v: number) => (v < 1000 ? `${v.toFixed(0)} ms` : `${(v / 1000).toFixed(2)} s`);
  const knob = (label: string, value: number, max: number, text: (v: number) => string, set: (v: number) => Partial<SampleZone>, taper = 1) => (
    <DragKnob
      label={label}
      value={value}
      min={0}
      max={max}
      resetValue={0}
      taper={taper}
      text={text}
      onBegin={() => (gesture.current = newGestureId())}
      onChange={(v) => send(set(v), gesture.current)}
    />
  );
  return (
    <div className="sampler-envelope-knobs">
      {knob('Attack', zone.attackMs, MAX_ENVELOPE_MS, ms, (v) => ({ attackMs: Math.round(v) }), ENVELOPE_TAPER)}
      {knob('Decay', zone.decayMs, MAX_ENVELOPE_MS, ms, (v) => ({ decayMs: Math.round(v) }), ENVELOPE_TAPER)}
      {knob('Sustain', zone.sustain * 100, 100, (v) => `${v.toFixed(0)}%`, (v) => ({ sustain: v / 100 }))}
      {zone.mode !== 'oneShot' && knob('Release', zone.releaseMs, MAX_ENVELOPE_MS, ms, (v) => ({ releaseMs: Math.round(v) }), ENVELOPE_TAPER)}
    </div>
  );
}

/** The zone's envelope as values to type under its graph, where knobs don't fit (docs/ui-review-2026-11-10.md, 1.10). */
function ZoneEnvelopeValues({ zone, send }: { zone: SampleZone; send: (fields: Partial<SampleZone>, gesture?: string) => void }) {
  const ms = (label: string, field: 'attackMs' | 'decayMs' | 'releaseMs') => (
    <Readout label={label}>
      <ValueText
        text={`${zone[field].toFixed(0)} ms`}
        title={label}
        parse={(t) => parseNumber(t, 0, MAX_ENVELOPE_MS)}
        onSet={(v, g) => send({ [field]: Math.round(v) }, g)}
      />
    </Readout>
  );
  return (
    <Readouts pairs={zone.mode === 'oneShot' ? 3 : 4} className="sampler-envelope-values">
      {ms('Attack', 'attackMs')}
      {ms('Decay', 'decayMs')}
      <Readout label="Sustain">
        <ValueText
          text={`${(zone.sustain * 100).toFixed(0)}%`}
          title="Sustain"
          parse={(t) => {
            const v = parseNumber(t, 0, 100);
            return v === null ? null : v / 100;
          }}
          onSet={(v, g) => send({ sustain: v }, g)}
        />
      </Readout>
      {zone.mode !== 'oneShot' && ms('Release', 'releaseMs')}
    </Readouts>
  );
}

/** The selected zone's sound: its knobs, how it follows its note, its choke group and, behind a button, its keys. */
function ZoneSound({
  zone,
  send,
  sampleRate,
  outputNames,
  onOwnOutput,
}: {
  zone: SampleZone;
  send: (fields: Partial<SampleZone>, gesture?: string) => void;
  /** The sample's rate, for its crossfade in milliseconds. */
  sampleRate: number;
  /** The Sampler's outputs by port, Main first; absent where there are none (a Sample Voice). */
  outputNames?: string[];
  onOwnOutput: (zone: SampleZone) => void;
}) {
  const [mapping, setMapping] = useState<{ x: number; y: number } | null>(null);
  const gesture = useRef(newGestureId());
  const knob = (label: string, field: 'gainDb' | 'pan' | 'tune', min: number, max: number, text: (v: number) => string) => (
    <DragKnob
      label={label}
      value={zone[field]}
      min={min}
      max={max}
      resetValue={0}
      text={text}
      onBegin={() => (gesture.current = newGestureId())}
      onChange={(value) => send({ [field]: value }, gesture.current)}
    />
  );
  const key = (label: string, field: 'lowKey' | 'highKey' | 'rootKey') => (
    <label className="sampler-field">
      <span>{label}</span>
      <DragNumber value={zone[field]} min={0} max={127} integer format={keyName} parse={parsePitch} onChange={(v, g) => send({ [field]: v }, g)} />
    </label>
  );
  const velocity = (label: string, field: 'lowVelocity' | 'highVelocity') => (
    <label className="sampler-field">
      <span>{label}</span>
      <DragNumber value={zone[field]} min={0} max={127} integer onChange={(v, g) => send({ [field]: v }, g)} />
    </label>
  );

  return (
    <div className="sampler-sound">
      <SectionHeader label="Sample" />
      <div className="sampler-knobs">
        {knob('Level', 'gainDb', -60, 24, (v) => `${v.toFixed(1)} dB`)}
        {knob('Pan', 'pan', -1, 1, panText)}
        {knob('Tune', 'tune', -48, 48, (v) => `${v.toFixed(2)} st`)}
      </div>
      <Segmented
        className="sampler-modes"
        label="Play mode"
        value={zone.mode}
        options={MODES.map(({ mode, name, title }) => ({ value: mode, label: name, title }))}
        onChange={(mode) => send({ mode })}
      />
      <label className="sampler-row" title="A hit in a choke group fades the group's other samples">
        <span>Choke</span>
        <Select value={zone.chokeGroup} onChange={(e) => send({ chokeGroup: Number(e.target.value) })}>
          {Array.from({ length: 9 }, (_, group) => (
            <option key={group} value={group}>
              {group === 0 ? 'No choke' : `Choke ${group}`}
            </option>
          ))}
        </Select>
      </label>
      {outputNames && (
        <label className="sampler-row" title="The output it plays from: Main, or an output wired to a track of its own (multi-output)">
          <span>Output</span>
          <Select value={zone.output ?? 1} onChange={(e) => (e.target.value === 'own' ? onOwnOutput(zone) : send({ output: Number(e.target.value) }))}>
            {outputNames.map((name, port) => (
              <option key={port} value={port + 1}>
                {name}
              </option>
            ))}
            <option value="own">Own output track</option>
          </Select>
        </label>
      )}
      {zone.mode === 'loop' && (
        <label className="sampler-row" title="How long the loop's end fades into its start">
          <span>Crossfade</span>
          <DragNumber
            value={zone.crossfade}
            min={0}
            max={480000}
            quantum={1}
            taper={CROSSFADE_TAPER}
            format={(v) => `${((v / sampleRate) * 1000).toFixed(1)} ms`}
            parse={(text) => {
              const ms = parseNumber(text, 0, (480000 / sampleRate) * 1000);
              return ms === null ? null : Math.round((ms / 1000) * sampleRate);
            }}
            onChange={(v, g) => send({ crossfade: Math.round(v) }, g)}
          />
        </label>
      )}
      <Button onClick={(e) => setMapping({ x: e.clientX, y: e.clientY })}>
        Mapping: {keysOf(zone)}
      </Button>
      {mapping && (
        <>
          <Popover onClose={() => setMapping(null)} className="sampler-mapping" aria-label="Mapping" at={{ x: mapping.x, y: mapping.y }} anchor="end">
            {key('Low key', 'lowKey')}
            {key('High key', 'highKey')}
            {key('Root key', 'rootKey')}
            {velocity('Low velocity', 'lowVelocity')}
            {velocity('High velocity', 'highVelocity')}
            <label className="sampler-field" title="A note plays a zone from each layer that holds it">
              <span>Layer</span>
              <DragNumber value={zone.layer} min={1} max={8} integer onChange={(v, g) => send({ layer: v }, g)} />
            </label>
          </Popover>
        </>
      )}
    </div>
  );
}
