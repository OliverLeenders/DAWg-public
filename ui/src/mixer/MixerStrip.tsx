import { ChevronDown, ChevronRight, Circle } from 'lucide-react';
import { useState } from 'react';
import type { Track } from '../bridge/protocol';
import { InputSelect, MonitorSelect } from '../components/AudioTrackInspector';
import { TrackOffset } from '../components/TrackOffset';
import { FaderWithMeter, GainReadout, PanSlider, TrackSwitches, useTrackValues } from '../dock/TrackFader';
import { actions, dispatch, useDawg } from '../store/store';
import { useUi } from '../store/uiStore';
import { useMixer } from './mixerStore';
import { MixerInserts } from './MixerInserts';
import { MixerSends } from './MixerSends';
import { MacroKnobs } from '../dock/ModulationColumn';
import { macrosOf } from '../dock/macros';
import { IconButton, TrackSwitch } from '../ui/controls';

// One strip of the mixer (docs/design/mixer.md, X3), top to bottom: I/O, inserts, sends, pan, the switches, the
// fader with its meter, and the name with the gain readout. Under it, the brackets of the groups it is in (X2).

/** Interactions with these don't select the strip (X6): only its background and name do. */
const CONTROLS = 'button, select, input, .pan-slider, .fader-track, .fader-readout, .drag-number, .meter, .send-knob, .insert-slot, .knob';

interface Props {
  track: Track;
  brackets: Track[];
  onSelect: (track: Track, e: React.PointerEvent) => void;
}

export function MixerStrip({ track, brackets, onSelect }: Props) {
  const { io, inserts, sends, offsets, narrow } = useMixer();
  const selected = useUi((s) => s.selectedTracks.has(track.id));
  const armed = useDawg((s) => s.armed.has(track.id));
  const { gainDb, pan } = useTrackValues(track);
  const isMaster = track.kind === 'master';
  const canArm = track.kind === 'instrument' || track.kind === 'audio';
  const macros = macrosOf(track);
  const anyMacros = useDawg(
    (s) =>
      (s.project?.master !== undefined && s.project.master !== null && macrosOf(s.project.master) !== undefined) ||
      Object.values(s.project?.tracks ?? {}).some((t) => macrosOf(t) !== undefined),
  );

  return (
    <div
      className={`mixer-strip ${narrow ? 'narrow' : ''} ${selected ? 'selected' : ''} kind-${track.kind}`}
      style={{ '--track': track.colour } as React.CSSProperties}
      data-track={track.id}
      onPointerDown={(e) => {
        if (!(e.target as HTMLElement).closest(CONTROLS)) onSelect(track, e);
      }}
    >
      {io && <StripIo track={track} />}
      {inserts && <MixerInserts track={track} narrow={narrow} />}
      {/* The master sends nowhere; an empty section keeps its fader level with the others. */}
      {sends && (isMaster ? <div className="strip-section strip-sends" /> : <MixerSends track={track} narrow={narrow} />)}
      {/* The track's macros (docs/design/macros-and-modulation-column.md, MC11); an empty section on the others while any
          track has some, so the faders stay level. */}
      {anyMacros && <div className="strip-section strip-macros">{macros && <MacroKnobs track={track} macros={macros} />}</div>}

      <div className="strip-pan">
        <PanSlider track={track} pan={pan} compact={narrow} />
      </div>
      {/* An offset shows when asked for, or when a track has one (docs/ui-review-2026-11-10.md, 3.6). */}
      {!isMaster && (offsets || track.offsetMs !== 0) && <TrackOffset track={track} compact />}
      <div className="strip-switches">
        <TrackSwitches track={track}>
          {canArm && (
            <TrackSwitch
              kind="arm"
              on={armed}
              title={track.kind === 'audio' ? 'Arm for recording' : 'Arm: record and play MIDI input'}
              onClick={() => actions.arm(track.id, !armed)}
            >
              {/* Filled when armed, hollow when not, as in the track headers (3.10). */}
              <Circle size={10} fill={armed ? 'currentColor' : 'none'} strokeWidth={armed ? 0 : 2.5} />
            </TrackSwitch>
          )}
        </TrackSwitches>
        {isMaster && <div className="strip-switches-spacer" />}
      </div>
      <FaderWithMeter track={track} gainDb={gainDb} scale={!narrow} />
      <GainReadout track={track} gainDb={gainDb} />
      <StripName track={track} />

      <div className="strip-brackets">
        {brackets.map((group) => (
          <span
            key={group.id}
            className={`strip-bracket ${group.id === track.id ? 'end' : ''}`}
            style={{ background: group.colour }}
            title={`In ${group.name}`}
          />
        ))}
      </div>
    </div>
  );
}

/** I/O: an audio track's input and monitoring; where the strip's signal goes, read-only (X3). */
function StripIo({ track }: { track: Track }) {
  const output = useDawg((s) => {
    if (track.kind === 'master') return 'Output';
    const parent = track.parent ? s.project?.tracks[String(track.parent)] : undefined;
    return parent?.name ?? 'Master';
  });
  return (
    <div className="strip-section strip-io">
      {track.kind === 'audio' ? (
        <>
          <InputSelect track={track} className="strip-select" />
          <MonitorSelect track={track} className="strip-select" />
        </>
      ) : (
        <>
          <span className="strip-io-none">{track.kind === 'instrument' ? 'MIDI' : track.kind === 'master' ? 'Mix' : 'Bus'}</span>
          <span className="strip-io-none" />
        </>
      )}
      <span className="strip-output" title={`Output: ${output}`}>
        → {output}
      </span>
    </div>
  );
}

/** The track's colour, its name (double-click to rename) and, for a group, its fold (X2, X3). */
function StripName({ track }: { track: Track }) {
  const [editing, setEditing] = useState(false);
  const rename = (name: string) => {
    setEditing(false);
    const trimmed = name.trim();
    if (trimmed && trimmed !== track.name) dispatch({ command: 'renameTrack', args: { id: track.id, name: trimmed } });
  };

  return (
    <div className="strip-name">
      {track.kind === 'group' && (
        <IconButton
          size="s"
          variant="ghost"
          className="strip-fold"
          title={track.folded ? 'Show the members' : 'Hide the members'}
          onClick={() => dispatch({ command: 'setTrackFolded', args: { id: track.id, folded: !track.folded } })}
        >
          {track.folded ? <ChevronRight size={11} /> : <ChevronDown size={11} />}
        </IconButton>
      )}
      {editing ? (
        <input
          className="strip-name-input"
          autoFocus
          defaultValue={track.name}
          onBlur={(e) => rename(e.currentTarget.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter') rename(e.currentTarget.value);
            else if (e.key === 'Escape') setEditing(false);
          }}
        />
      ) : (
        <span
          className="strip-name-text"
          title={track.kind === 'master' ? track.name : `${track.name} (double-click to rename)`}
          onDoubleClick={() => track.kind !== 'master' && setEditing(true)}
        >
          {track.name}
        </span>
      )}
    </div>
  );
}
