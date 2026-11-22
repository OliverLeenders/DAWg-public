import { useLayoutEffect, useRef, useState } from 'react';
import type { AudioFile, SampleZone } from '../bridge/protocol';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { newGestureId } from '../store/store';
import { usePeaksVersion } from '../timeline/peaks';
import { drawWave } from '../timeline/waveform';
import { moveMarker, waveMarkers, type WaveMarker } from './samplerLayout';

// A zone's sample with its start, end and loop as markers to drag; double-click one to type its time. What is outside
// the played range is dimmed, and a loop's crossfade is shaded.

const GRAB_PX = 6;
const MARKER_NAMES: Record<WaveMarker, string> = { start: 'Start', end: 'End', loopStart: 'Loop start', loopEnd: 'Loop end' };

export function SampleWaveform({
  zone,
  file,
  onChange,
}: {
  zone: SampleZone;
  file: AudioFile | undefined;
  onChange: (fields: Partial<SampleZone>, gesture?: string) => void;
}) {
  const canvas = useCanvas();
  const version = usePeaksVersion((s) => s.version);
  const frames = file?.frames ?? 0;
  const rate = file?.sampleRate ?? 48000;
  const [dragging, setDragging] = useState<WaveMarker | null>(null);
  const [typing, setTyping] = useState<WaveMarker | null>(null);
  const gesture = useRef('');
  const markers = waveMarkers(zone, frames);
  const toX = (frame: number) => (frames > 0 ? (frame / frames) * canvas.size.width : 0);
  const milliseconds = (frame: number) => (frame / rate) * 1000;
  const frameOf = (marker: WaveMarker) => markers.find((m) => m.marker === marker)?.frame ?? 0;

  useLayoutEffect(() => {
    const ctx = canvas.context();
    const { width, height } = canvas.size;
    if (!ctx || width < 4 || height < 4) return;
    const theme = readTheme();
    ctx.fillStyle = theme.lcd;
    ctx.fillRect(0, 0, width, height);
    if (!file || frames <= 0) return;

    // The wave as the arrangement and the audio editor draw it (timeline/waveform.ts), brighter over the played range.
    const framesPerPixel = frames / width;
    const startX = Math.round(toX(frameOf('start')));
    const endX = Math.round(toX(frameOf('end')));
    const wave = (x0: number, x1: number, alpha: number) => {
      ctx.globalAlpha = alpha;
      const spec = { source: { fileId: file.id }, frames, x0, x1, frameAt: (x: number) => x * framesPerPixel };
      drawWave(ctx, { ...spec, mid: height / 2, half: height / 2 - 2, top: 0, bottom: height }, theme.accent);
      ctx.globalAlpha = 1;
    };
    wave(0, startX, 0.3);
    wave(startX, endX, 0.9);
    wave(endX, width, 0.3);

    if (zone.mode === 'loop' && zone.crossfade > 0) {
      const loopEnd = frameOf('loopEnd');
      ctx.fillStyle = theme.text;
      ctx.globalAlpha = 0.12;
      ctx.fillRect(toX(loopEnd - zone.crossfade), 0, toX(zone.crossfade), height);
      ctx.globalAlpha = 1;
    }

    ctx.font = font(theme, 'xs');
    for (const { marker, frame } of markers) {
      const x = Math.min(width - 1, Math.round(toX(frame)));
      ctx.fillStyle = marker === 'start' || marker === 'end' ? theme.text : theme.accent;
      ctx.fillRect(x, 0, 1, height);
      ctx.fillRect(marker === 'end' || marker === 'loopEnd' ? x - 5 : x, 0, 6, 6);
    }
  });

  const nearest = (x: number) => {
    let best: WaveMarker | null = null;
    let distance = GRAB_PX;
    for (const { marker, frame } of markers) {
      const away = Math.abs(toX(frame) - x);
      if (away <= distance) {
        best = marker;
        distance = away;
      }
    }
    return best;
  };

  const shown = dragging ?? typing;

  return (
    <div className="sampler-wave">
      <canvas
        ref={canvas.ref}
        data-peaks={version}
        className="panel-canvas"
        onPointerDown={(e) => {
          const marker = nearest(e.nativeEvent.offsetX);
          if (!marker || frames <= 0) return;
          e.currentTarget.setPointerCapture(e.pointerId);
          gesture.current = newGestureId();
          setDragging(marker);
        }}
        onPointerMove={(e) => {
          e.currentTarget.style.cursor = dragging || nearest(e.nativeEvent.offsetX) ? 'ew-resize' : 'default';
          if (dragging) onChange(moveMarker(zone, frames, dragging, (e.nativeEvent.offsetX / canvas.size.width) * frames), gesture.current);
        }}
        onPointerUp={() => setDragging(null)}
        onPointerCancel={() => setDragging(null)}
        onDoubleClick={(e) => setTyping(nearest(e.nativeEvent.offsetX))}
      />
      <span className="sampler-wave-name">{file?.name ?? 'Missing file'}</span>
      {shown && !typing && (
        <span className="sampler-wave-readout">
          {MARKER_NAMES[shown]} {milliseconds(frameOf(shown)).toFixed(0)} ms
        </span>
      )}
      {typing && (
        <label className="sampler-wave-readout">
          {MARKER_NAMES[typing]}{' '}
          <input
            autoFocus
            type="number"
            defaultValue={Math.round(milliseconds(frameOf(typing)))}
            onBlur={() => setTyping(null)}
            onKeyDown={(e) => {
              e.stopPropagation();
              if (e.key === 'Enter') {
                onChange(moveMarker(zone, frames, typing, (Number(e.currentTarget.value) / 1000) * rate));
                setTyping(null);
              } else if (e.key === 'Escape') {
                setTyping(null);
              }
            }}
          />{' '}
          ms
        </label>
      )}
    </div>
  );
}
