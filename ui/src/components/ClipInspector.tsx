import { TICKS_PER_BEAT, type Clip } from '../bridge/protocol';
import { dispatch, useDawg } from '../store/store';
import { DragNumber } from './DragNumber';
import { TimingSection } from './TimingSection';
import { coveredEdges } from '../timeline/crossfades';
import { gainTaper } from '../dock/TrackFader';

// The "Clip" section of the inspector for one selected audio clip (docs/design/phase7-audio-tracks.md,
// section 9): name, gain, fades, the file's timing (docs/design/time-stretching.md, TS12) and the source file.

const SHAPES = ['Linear', 'Equal power', 'Exponential', 'S-curve'];

export function ClipInspector({ clip }: { clip: Clip }) {
  const file = useDawg((s) => s.project?.audioFiles[String(clip.fileId)]);
  const missing = useDawg((s) => s.missingAudio.some((m) => m.id === clip.fileId));
  const set = (args: Omit<Extract<Parameters<typeof dispatch>[0], { command: 'setClipProps' }>['args'], 'id'>, gesture?: string) =>
    dispatch({ command: 'setClipProps', args: { id: clip.id, ...args } }, gesture);
  const beats = (ticks: number) => ticks / TICKS_PER_BEAT;
  // A fade over audio below is a crossfade (docs/design/audio-features.md, AU8).
  const track = useDawg((s) => Object.values(s.project?.tracks ?? {}).find((t) => t.clips[String(clip.id)]));
  const covered = track ? coveredEdges(track, clip) : { start: false, end: false };

  return (
    <div className="clip-inspector">
      <div className="inspector-label">Clip</div>
      <label className="inspector-field">
        <span>Name</span>
        <input
          className="track-name-input"
          key={clip.id}
          defaultValue={clip.name}
          onBlur={(e) => e.target.value !== clip.name && set({ name: e.target.value })}
          onKeyDown={(e) => {
            e.stopPropagation();
            if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
          }}
        />
      </label>
      <label className="inspector-field">
        <span>Gain</span>
        <DragNumber
          className="lcd small"
          value={clip.gainDb}
          min={-60}
          max={12}
          quantum={0.1}
          taper={gainTaper}
          format={(v) => `${v > 0 ? '+' : ''}${v.toFixed(1)} dB`}
          onChange={(gainDb, gesture) => set({ gainDb }, gesture)}
        />
      </label>
      {(['in', 'out'] as const).map((edge) => {
        const length = edge === 'in' ? clip.fadeIn : clip.fadeOut;
        const shape = edge === 'in' ? clip.fadeInShape : clip.fadeOutShape;
        return (
          <label className="inspector-field" key={edge}>
            <span>{(edge === 'in' ? covered.start : covered.end) ? 'Crossfade' : 'Fade'} {edge}</span>
            <DragNumber
              className="lcd small"
              value={beats(length)}
              min={0}
              max={beats(clip.length)}
              quantum={1 / 64}
              format={(v) => `${v.toFixed(2)} b`}
              onChange={(v, gesture) =>
                set(edge === 'in' ? { fadeIn: Math.round(v * TICKS_PER_BEAT) } : { fadeOut: Math.round(v * TICKS_PER_BEAT) }, gesture)
              }
            />
            <select
              value={shape}
              onChange={(e) =>
                set(edge === 'in' ? { fadeInShape: Number(e.target.value) } : { fadeOutShape: Number(e.target.value) })
              }
            >
              {SHAPES.map((label, i) => (
                <option key={label} value={i}>
                  {label}
                </option>
              ))}
            </select>
          </label>
        );
      })}
      {file && <TimingSection clip={clip} file={file} />}
      <div className="inspector-file" title={file?.path}>
        {file ? `${file.name} · ${(file.sampleRate / 1000).toFixed(1)} kHz · ${file.channels === 1 ? 'mono' : 'stereo'}` : 'No file'}
        {missing && <span className="missing-tag">missing</span>}
      </div>
    </div>
  );
}
