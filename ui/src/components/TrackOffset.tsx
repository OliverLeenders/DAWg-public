import { RotateCcw } from 'lucide-react';
import { useState } from 'react';
import type { Track } from '../bridge/protocol';
import { dispatch, useDawg } from '../store/store';
import { DragNumber } from './DragNumber';
import { centredTaper } from './dragScale';
import { IconButton } from '../ui/controls';

// A track's timing offset (docs/PLAN.md, phase 13): positive delays the track, negative pulls it earlier. The value
// is stored in milliseconds; the field can be edited and read in samples at the current device rate too.

const MIN_MS = -1000;
const MAX_MS = 1000;

type Unit = 'ms' | 'samples';

function clampMs(ms: number) {
  return Math.min(MAX_MS, Math.max(MIN_MS, ms));
}

function formatMs(ms: number, compact = false) {
  const value = Math.abs(ms) < 0.0000005 ? 0 : ms;
  const digits = compact ? (Math.abs(value) < 10 ? 2 : 1) : 2;
  const text = value.toFixed(digits).replace(/\.?0+$/, '').replace(/^-0$/, '0');
  return `${value > 0 ? '+' : ''}${text} ms`;
}

function formatSamples(samples: number, compact = false) {
  const value = Math.round(samples);
  return `${value > 0 ? '+' : ''}${value}${compact ? 'smp' : ' smp'}`;
}

function formatLatency(samples: number, sampleRate: number) {
  if (samples <= 0) return '';
  return formatMs((samples * 1000) / sampleRate);
}

export function TrackOffset({ track, compact = false }: { track: Track; compact?: boolean }) {
  const [unit, setUnit] = useState<Unit>('ms');
  const sampleRate = useDawg((s) => s.telemetry.sampleRate || s.audio.sampleRate || 48000);
  const latencySamples = useDawg((s) => s.telemetry.latencySamples);

  const samples = Math.round((track.offsetMs * sampleRate) / 1000);
  const value = unit === 'ms' ? track.offsetMs : samples;
  const min = unit === 'ms' ? MIN_MS : (MIN_MS * sampleRate) / 1000;
  const max = unit === 'ms' ? MAX_MS : (MAX_MS * sampleRate) / 1000;
  // Whole milliseconds or whole samples, finer near zero (components/dragScale.ts).
  const other = unit === 'ms' ? formatSamples(samples, compact) : formatMs(track.offsetMs, compact);
  const latency = formatLatency(latencySamples, sampleRate);
  const title =
    `Track offset ${formatMs(track.offsetMs)} (${formatSamples(samples)}). Positive delays the track; negative pulls it earlier.` +
    (latency ? ` Mix latency: ${latency}.` : '');

  const setOffset = (next: number, gesture: string) => {
    const ms = unit === 'ms' ? next : (next * 1000) / sampleRate;
    const rounded = Math.round(clampMs(ms) * 1e6) / 1e6;
    if (rounded !== track.offsetMs) dispatch({ command: 'setTrackOffset', args: { id: track.id, offsetMs: rounded } }, gesture);
  };

  const row = (
    <div className="offset-row">
      {!compact && <span className="offset-title">Offset</span>}
      <DragNumber
        className="offset-value"
        value={value}
        min={min}
        max={max}
        quantum={1}
        taper={centredTaper(max, 2)}
        title={title}
        format={(v) => (unit === 'ms' ? formatMs(v, compact) : formatSamples(v, compact))}
        onChange={setOffset}
      />
      <button
        className="offset-unit"
        title="Switch between milliseconds and samples"
        onClick={() => setUnit(unit === 'ms' ? 'samples' : 'ms')}
      >
        {unit === 'ms' ? 'ms' : 'smp'}
      </button>
      {!compact && (
        <IconButton
          className="offset-reset"
          title="Reset the offset to zero"
          onClick={() => dispatch({ command: 'setTrackOffset', args: { id: track.id, offsetMs: 0 } })}
        >
          <RotateCcw size={11} />
        </IconButton>
      )}
    </div>
  );

  if (compact) {
    return (
      <div className="track-offset compact" onClick={(e) => e.stopPropagation()}>
        {row}
        <span className="offset-other">{other}</span>
      </div>
    );
  }

  // In the inspector, a folded section that opens by itself when the track has an offset
  // (docs/ui-review-2026-11-10.md, 3.6).
  return (
    <details className="track-offset" open={track.offsetMs !== 0 || undefined} onClick={(e) => e.stopPropagation()}>
      <summary className="inspector-label">Timing{track.offsetMs !== 0 ? ` · ${formatMs(track.offsetMs)}` : ''}</summary>
      {row}
      <div className="offset-readout">
        <span>{other}</span>
        {latency && <span className="offset-latency">mix latency {latency}</span>}
      </div>
    </details>
  );
}
