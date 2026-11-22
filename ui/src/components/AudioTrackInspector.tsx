import type { Track } from '../bridge/protocol';
import { dispatch, useDawg } from '../store/store';
import { TrackInspector } from './TrackInspector';

// Audio tracks in the inspector (docs/design/phase7-audio-tracks.md, section 9): input and monitoring, then
// the insert chain (the audio player heads it and isn't shown as a slot).

const MONITOR = ['Off', 'Automatic', 'On'];

/** An audio track's input: none, a mono input or a stereo pair (also the mixer's I/O section). */
export function InputSelect({ track, className = 'wide' }: { track: Track; className?: string }) {
  const inputs = useDawg((s) => s.inputs);
  const value = track.inputChannel < 0 ? 'none' : `${track.inputChannel}:${track.inputStereo ? 's' : 'm'}`;

  // Mono inputs, then stereo pairs of neighbouring inputs.
  const options = [
    ...inputs.map((name, i) => ({ value: `${i}:m`, label: name })),
    ...inputs.slice(0, -1).map((name, i) => ({ value: `${i}:s`, label: `${name} + ${inputs[i + 1]}` })),
  ];

  return (
    <select
      className={className}
      value={value}
      title="Input"
      onChange={(e) => {
        const [channel, kind] = e.target.value === 'none' ? ['-1', 'm'] : e.target.value.split(':');
        dispatch({ command: 'setTrackSwitches', args: { id: track.id, inputChannel: Number(channel), inputStereo: kind === 's' } });
      }}
    >
      <option value="none">None</option>
      {options.map((o) => (
        <option key={o.value} value={o.value}>
          {o.label}
        </option>
      ))}
    </select>
  );
}

/** An audio track's monitoring: off, automatic (while armed) or on. */
export function MonitorSelect({ track, className = 'wide' }: { track: Track; className?: string }) {
  return (
    <select
      className={className}
      value={track.monitor}
      title="Monitoring"
      onChange={(e) => dispatch({ command: 'setTrackSwitches', args: { id: track.id, monitor: Number(e.target.value) } })}
    >
      {MONITOR.map((label, i) => (
        <option key={label} value={i}>
          {label}
        </option>
      ))}
    </select>
  );
}

export function AudioTrackInspector({ track }: { track: Track }) {
  return (
    <>
      <div className="inspector-label">Recording</div>
      <label className="inspector-field">
        <span>Input</span>
        <InputSelect track={track} />
      </label>
      <label className="inspector-field">
        <span>Monitor</span>
        <MonitorSelect track={track} />
      </label>
      <TrackInspector track={track} />
    </>
  );
}
