import { useRef } from 'react';
import type { Track } from '../bridge/protocol';
import { gainToPosition, laneFor, playedPosition, positionToGain } from '../automation/automation';
import { touch, touchProps, trackTarget } from '../automation/touch';
import { DragNumber } from '../components/DragNumber';
import { Meter } from '../components/Meter';
import { MidiLearnable } from '../components/MidiLearn';
import { actions, dispatch, newGestureId, useDawg } from '../store/store';
import { TrackSwitch } from '../ui/controls';

// The track's fader section at the right end of the device dock (docs/design/channel-strip.md, C7): mute and solo,
// a pan slider, a volume fader with a stereo meter beside it, and the gain readout. It controls the dock's track on
// both tabs, through the same commands as the track headers, and follows automation as it plays. Its parts are the
// mixer strips' too (docs/design/mixer.md, X8).

/** The fader's top stretch, above +6 dB, runs linearly to the gain's +12 dB maximum. */
const TOP = 0.9;
const MIN_DB = -96;
const MAX_DB = 12;

/** Fader travel (0..1) for a gain: the gain lanes' curve up to +6 dB, then linear to +12. */
export function dbToFader(db: number) {
  if (db <= MIN_DB) return 0;
  if (db <= 6) return Math.min(1, gainToPosition(db)) * TOP;
  return TOP + ((Math.min(MAX_DB, db) - 6) / (MAX_DB - 6)) * (1 - TOP);
}

/** A gain field drags along the fader's curve (components/dragScale.ts). */
export const gainTaper = { toNorm: (db: number) => dbToFader(db), fromNorm: (f: number) => faderToDb(f) };

/** The gain at a fader travel (0..1), the inverse of dbToFader. */
export function faderToDb(f: number) {
  if (f <= 0) return MIN_DB;
  if (f <= TOP) return Math.min(6, positionToGain(f / TOP));
  return 6 + ((Math.min(1, f) - TOP) / (1 - TOP)) * (MAX_DB - 6);
}

const SCALE = [6, 0, -6, -12, -24];

const formatDb = (db: number) => (db <= MIN_DB ? '−∞ dB' : `${db > 0 ? '+' : ''}${db.toFixed(1)} dB`);
const formatPan = (pan: number) => {
  const amount = Math.round(Math.abs(pan) * 100);
  return amount === 0 ? 'C' : `${pan < 0 ? 'L' : 'R'}${amount}`;
};

/** A drag on a slider: relative to where it started, Shift for fine steps, as one gesture with automation touch. */
function useSliderDrag(
  track: Track,
  kind: 'gain' | 'pan',
  apply: (value: number, gesture: string) => void,
  vertical = kind === 'gain',
) {
  const drag = useRef<{ origin: number; start: number; size: number; fine: boolean; last?: number; gesture: string } | null>(null);
  return {
    begin(e: React.PointerEvent<HTMLElement>, value: number, size: number) {
      if (e.button !== 0) return;
      e.currentTarget.setPointerCapture(e.pointerId);
      drag.current = { origin: vertical ? e.clientY : e.clientX, start: value, size, fine: e.shiftKey, gesture: newGestureId() };
      touch(track.id, trackTarget(kind), true);
    },
    move(e: React.PointerEvent<HTMLElement>, toValue: (start: number, delta: number) => number) {
      const d = drag.current;
      if (!d) return;
      const at = vertical ? e.clientY : e.clientX;
      // Shift is ten times finer, as everywhere (components/dragScale.ts); pressing or letting go of it carries on from
      // where the slider is.
      if (e.shiftKey !== d.fine) Object.assign(d, { origin: at, start: d.last ?? d.start, fine: e.shiftKey });
      const moved = vertical ? d.origin - at : at - d.origin;
      const next = toValue(d.start, (moved / Math.max(1, d.size)) * (d.fine ? 0.1 : 1));
      d.last = next;
      apply(next, d.gesture);
    },
    end() {
      if (!drag.current) return;
      drag.current = null;
      touch(track.id, trackTarget(kind), false);
    },
  };
}

/** The gain and pan the controls show: the track's, or the automated values while automation plays
    (docs/design/phase10-automation.md, A4). */
export function useTrackValues(track: Track) {
  // Select the played numbers rather than the automation map: the map's identity changes every telemetry tick.
  const gainLane = laneFor(track, trackTarget('gain'))?.id;
  const panLane = laneFor(track, trackTarget('pan'))?.id;
  const gainPlayed = useDawg((s) => playedPosition(track.automationMode === 0 ? undefined : s.telemetry.automation, gainLane));
  const panPlayed = useDawg((s) => playedPosition(track.automationMode === 0 ? undefined : s.telemetry.automation, panLane));
  return {
    gainDb: gainPlayed !== undefined ? positionToGain(gainPlayed) : track.gainDb,
    pan: panPlayed !== undefined ? panPlayed * 2 - 1 : track.pan,
  };
}

const setGain = (track: Track, db: number, gesture?: string) =>
  dispatch({ command: 'setTrackGain', args: { id: track.id, gainDb: Math.round(Math.min(MAX_DB, Math.max(MIN_DB, db)) * 10) / 10 } }, gesture);
const setPan = (track: Track, value: number, gesture?: string) =>
  dispatch({ command: 'setTrackPan', args: { id: track.id, pan: Math.round(Math.min(1, Math.max(-1, value)) * 100) / 100 } }, gesture);

/** The mute and solo switches (none on the master). */
export function TrackSwitches({ track, children }: { track: Track; children?: React.ReactNode }) {
  if (track.kind === 'master') return null;
  return (
    <div className="fader-switches">
      <TrackSwitch
        kind="mute"
        on={track.mute}
        title={track.mute ? 'Muted (click to unmute)' : 'Mute'}
        onClick={() => dispatch({ command: 'setTrackSwitches', args: { id: track.id, mute: !track.mute } })}
      >
        M
      </TrackSwitch>
      <TrackSwitch
        kind="solo"
        on={track.solo}
        title={track.solo ? 'Soloed (click to unsolo)' : 'Solo'}
        onClick={() => dispatch({ command: 'setTrackSwitches', args: { id: track.id, solo: !track.solo } })}
      >
        S
      </TrackSwitch>
      {children}
    </div>
  );
}

/** The pan slider and its readout (docs/design/mixer.md, X8). */
export function PanSlider({ track, pan, compact = false }: { track: Track; pan: number; compact?: boolean }) {
  const drag = useSliderDrag(track, 'pan', (value, gesture) => setPan(track, value, gesture));
  return (
    <>
      <MidiLearnable target={{ trackId: track.id, nodeId: 0, param: 'pan' }}>
        <div
          className="pan-slider"
          title={`Pan ${formatPan(pan)}: drag sideways (Shift: fine), double-click to centre, scroll to nudge`}
          onPointerDown={(e) => drag.begin(e, pan, e.currentTarget.clientWidth / 2)}
          onPointerMove={(e) => drag.move(e, (start, delta) => start + delta)}
          onPointerUp={drag.end}
          onDoubleClick={() => setPan(track, 0)}
          onWheel={(e) => setPan(track, pan - Math.sign(e.deltaY) * 0.02)}
        >
          <span className="pan-centre" />
          <span className={`pan-fill ${pan === 0 ? 'centred' : ''}`} style={pan < 0 ? { right: '50%', width: `${-pan * 50}%` } : { left: '50%', width: `${pan * 50}%` }} />
        </div>
      </MidiLearnable>
      <div className="pan-readout">
        {!compact && <span>L</span>}
        <span className={pan === 0 ? '' : 'value'}>{formatPan(pan)}</span>
        {!compact && <span>R</span>}
      </div>
    </>
  );
}

/** The volume fader with its scale and the track's stereo meter beside it (X8). */
export function FaderWithMeter({ track, gainDb, scale = true }: { track: Track; gainDb: number; scale?: boolean }) {
  // Primitive selectors: a track with no signal no longer re-renders its strip for every telemetry tick.
  const id = String(track.id);
  const left = useDawg((s) => s.telemetry.meters?.[id]?.[0] ?? 0);
  const right = useDawg((s) => s.telemetry.meters?.[id]?.[1] ?? 0);
  const clipped = useDawg((s) => s.clipped.has(track.id));
  const drag = useSliderDrag(track, 'gain', (f, gesture) => setGain(track, faderToDb(Math.min(1, Math.max(0, f))), gesture));
  const fader = dbToFader(gainDb);

  return (
    <div className="fader-body">
      {scale && (
        <div className="fader-scale">
          {SCALE.map((db) => (
            <span key={db} style={{ bottom: `${dbToFader(db) * 100}%` }}>
              {db > 0 ? `+${db}` : db}
            </span>
          ))}
          <span style={{ bottom: 0 }}>−∞</span>
        </div>
      )}
      <MidiLearnable target={{ trackId: track.id, nodeId: 0, param: 'gain' }}>
        <div
          className="fader-track"
          title="Volume: drag (Shift: fine), double-click for 0 dB, scroll to nudge"
          onPointerDown={(e) => drag.begin(e, fader, e.currentTarget.clientHeight)}
          onPointerMove={(e) => drag.move(e, (start, delta) => start + delta)}
          onPointerUp={drag.end}
          onDoubleClick={() => setGain(track, 0)}
          onWheel={(e) => setGain(track, (gainDb <= MIN_DB ? -60 : gainDb) - Math.sign(e.deltaY) * 0.5)}
        >
          <span className="fader-unity" style={{ bottom: `${dbToFader(0) * 100}%` }} />
          <span className="fader-cap" style={{ bottom: `${fader * 100}%` }} />
        </div>
      </MidiLearnable>
      <Meter
        className="fader-meter"
        vertical
        left={left}
        right={right}
        title="Track level"
        clipped={clipped}
        onResetClip={() => actions.resetClip(track.id)}
      />
    </div>
  );
}

/** A horizontal volume slider laid over a track header's meter: the meter's lanes are its track, with a knob at the
    fader's travel (the same curve as the dock's fader). */
export function MeterGainKnob({ track, gainDb }: { track: Track; gainDb: number }) {
  const drag = useSliderDrag(track, 'gain', (f, gesture) => setGain(track, faderToDb(Math.min(1, Math.max(0, f))), gesture), false);
  const fader = dbToFader(gainDb);
  return (
    <div
      className="meter-gain"
      title={`Volume ${formatDb(gainDb)}: drag sideways (Shift: fine), double-click for 0 dB, scroll to nudge`}
      onPointerDown={(e) => drag.begin(e, fader, e.currentTarget.clientWidth)}
      onPointerMove={(e) => drag.move(e, (start, delta) => start + delta)}
      onPointerUp={drag.end}
      onDoubleClick={(e) => {
        e.stopPropagation();
        setGain(track, 0);
      }}
      onWheel={(e) => setGain(track, (gainDb <= MIN_DB ? -60 : gainDb) - Math.sign(e.deltaY) * 0.5)}
    >
      <span className="meter-gain-knob" style={{ left: `${fader * 100}%` }} />
    </div>
  );
}

/** The gain readout: drag or type (X8). */
export function GainReadout({ track, gainDb }: { track: Track; gainDb: number }) {
  return (
    <DragNumber
      className="fader-readout"
      value={gainDb}
      min={MIN_DB}
      max={MAX_DB}
      quantum={0.1}
      taper={gainTaper}
      resetValue={0}
      title="Gain: drag, double-click for 0 dB"
      format={formatDb}
      onChange={(db, gesture) => setGain(track, db, gesture)}
      {...touchProps(track.id, trackTarget('gain'))}
    />
  );
}

export function TrackFader({ track }: { track: Track }) {
  const { gainDb, pan } = useTrackValues(track);
  return (
    <aside className="track-fader" style={{ '--track': track.colour } as React.CSSProperties}>
      <TrackSwitches track={track} />
      <PanSlider track={track} pan={pan} />
      <FaderWithMeter track={track} gainDb={gainDb} />
      <GainReadout track={track} gainDb={gainDb} />
    </aside>
  );
}
