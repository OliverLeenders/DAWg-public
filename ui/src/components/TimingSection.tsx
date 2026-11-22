import type { AudioFile, Clip } from '../bridge/protocol';
import { clipsOfFile } from '../audioeditor/axis';
import { dispatch, useDawg } from '../store/store';
import { holdRenders } from '../timeline/stretchHold';
import { DragNumber } from './DragNumber';
import { logTaper } from './dragScale';
import { Button } from '../ui/controls';

// A file's tempo drags along a log curve, 1.7 px a BPM around 120 (dragScale.ts).
const BPM_TAPER = logTaper(5, 2000);

// The Timing section for an audio clip's pool file (docs/design/time-stretching.md, TS12): one-shot or musical, the
// musical style, the source tempo, the clip's transpose, resetting the markers and "Make copy". In the inspector's
// Clip section and the audio editor's header. Nothing here changes a file's mode except the mode buttons.

export function TimingSection({ clip, file }: { clip: Clip; file: AudioFile }) {
  const shared = useDawg((s) => clipsOfFile(s.project, file.id).length);
  const timing = file.timing;
  const musical = timing.mode === 'musical';
  const setTiming = (args: { mode?: 'oneShot' | 'musical'; musicalStyle?: 'stretch' | 'repitch'; sourceBpm?: number }, gesture?: string) =>
    dispatch({ command: 'setFileTiming', args: { fileId: file.id, ...args } }, gesture);
  const suggested = file.suggestedBpm > 0 && Math.abs(file.suggestedBpm - timing.sourceBpm) > 0.005 ? file.suggestedBpm : null;

  return (
    <div className="timing-section">
      <div className="inspector-label">Timing</div>
      <div className="inspector-field">
        <span>Mode</span>
        <div className="segmented">
          <button className={musical ? '' : 'active'} onClick={() => musical && setTiming({ mode: 'oneShot' })} title="Plays at its own speed">
            One-shot
          </button>
          <button className={musical ? 'active' : ''} onClick={() => !musical && setTiming({ mode: 'musical' })} title="Follows the tempo">
            Musical
          </button>
        </div>
      </div>
      {(musical || timing.markers.length > 0) && (
        <div className="inspector-field">
          <span>Style</span>
          <div className="segmented">
            <button
              className={timing.musicalStyle === 'stretch' ? 'active' : ''}
              onClick={() => setTiming({ musicalStyle: 'stretch' })}
              title="Keeps the pitch"
            >
              Stretch
            </button>
            <button
              className={timing.musicalStyle === 'repitch' ? 'active' : ''}
              onClick={() => setTiming({ musicalStyle: 'repitch' })}
              title="Speed and pitch change together, like tape"
            >
              Repitch
            </button>
          </div>
        </div>
      )}
      {musical && (
        <>
          <div className="inspector-field">
            <span>Source tempo</span>
            <DragNumber
              className="lcd small"
              value={timing.sourceBpm}
              min={5}
              max={2000}
              quantum={0.01}
              taper={BPM_TAPER}
              rangePixels={1200}
              wheelStep={1}
              format={(v) => `${v.toFixed(2)} BPM`}
              onChange={(sourceBpm, gesture) => setTiming({ sourceBpm }, gesture)}
              onGestureStart={() => holdRenders(true)}
              onGestureEnd={() => holdRenders(false)}
            />
            <Button title="Double the source tempo" onClick={() => setTiming({ sourceBpm: timing.sourceBpm * 2 })}>
              ×2
            </Button>
            <Button title="Halve the source tempo" onClick={() => setTiming({ sourceBpm: timing.sourceBpm / 2 })}>
              ÷2
            </Button>
          </div>
          {suggested !== null && (
            <div className="inspector-field">
              <span />
              <Button onClick={() => setTiming({ sourceBpm: suggested })}>
                Use {+suggested.toFixed(2)} BPM
              </Button>
            </div>
          )}
          {timing.musicalStyle === 'stretch' && (
            <div className="inspector-field">
              <span>Transpose</span>
              <DragNumber
                className="lcd small"
                value={clip.transpose}
                min={-48}
                max={48}
                quantum={0.01}
                wheelStep={1}
                format={(v) => `${v > 0 ? '+' : ''}${v.toFixed(2)} st`}
                onChange={(semitones, gesture) => dispatch({ command: 'setClipTranspose', args: { clipId: clip.id, semitones } }, gesture)}
                onGestureStart={() => holdRenders(true)}
                onGestureEnd={() => holdRenders(false)}
              />
            </div>
          )}
        </>
      )}
      <div className="inspector-field">
        <span />
        <Button
          disabled={timing.markers.length === 0}
          onClick={() => dispatch({ command: 'resetTimingMarkers', args: { fileId: file.id } })}
          title="Remove every timing marker of the file"
        >
          Reset markers
        </Button>
        <Button
          disabled={shared < 2}
          onClick={() => dispatch({ command: 'copyPoolFile', args: { clipId: clip.id } })}
          title="Give this clip its own copy of the file's timing, to stretch it apart from the others"
        >
          Make copy
        </Button>
      </div>
      {shared > 1 && (
        <p className="inspector-note">
          {shared} clips play “{file.name}”: these settings apply to all of them{musical ? ', except transpose' : ''}.
        </p>
      )}
    </div>
  );
}
