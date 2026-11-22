import { FolderOpen, RotateCcw, Search } from 'lucide-react';
import { useLayoutEffect, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { PATHS_MIME } from '../arrangement/importAudio';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { dispatch, projectActions, reportError, useDawg } from '../store/store';
import { useNodeAnalysis } from './analysis';
import {
  impulseName,
  keptRange,
  reverbTime,
  reverbTimeReadout,
  shapedEnvelope,
  tickStep,
  type ConvolutionMeters,
} from './convolution';
import { Knob, SlotToggle } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';
import { useParam } from './useParam';
import { IconButton } from '../ui/controls';

// The Convolution Reverb's panel (docs/design/convolution-reverb.md, the panel): the impulse response's name with Load,
// Locate and back-to-default buttons and the reverb time, the file's envelope in dB with the kept part and the shaped
// envelope over it, and the controls in signal-flow order in two rows to the right. Files dropped from the sound
// explorer load as the IR.

// Two rows of groups, so the controls stay in view in a short dock.
const rows = [
  [
    { label: 'Impulse', ids: ['start', 'length', 'decay', 'stretch'] },
    { label: 'Space', ids: ['predelay', 'width'] },
  ],
  [
    { label: 'Shape', ids: ['reverse', 'normalise'], toggles: true },
    { label: 'Tone', ids: ['lowCut', 'highCut'] },
    { label: 'Output', ids: ['gain', 'mix'] },
  ],
];

const FLOOR_DB = -72;

export function ConvolutionReverbPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const analysis = useNodeAnalysis(track.id, node.id);
  const meters = (analysis?.meters ?? null) as ConvolutionMeters | null;
  const files = useDawg((s) => s.project?.audioFiles);
  const [dropping, setDropping] = useState(false);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const startMs = useParam(track, node, param('start')).value;
  const lengthPercent = useParam(track, node, param('length')).value;
  const decayPercent = useParam(track, node, param('decay')).value;
  const reverse = useParam(track, node, param('reverse')).value >= 0.5;
  const stretch = useParam(track, node, param('stretch')).value;
  const shape = { startMs, lengthPercent, decayPercent, reverse };
  const fileId = node.fileId ?? 0;
  const file = fileId ? files?.[String(fileId)] : undefined;
  const missing = !!meters?.missing || (fileId !== 0 && file === undefined);
  const name = impulseName(fileId, file?.name, missing);
  const target = { trackId: track.id, nodeId: node.id };
  const rt = reverbTime(meters?.decay ?? 0, shape.decayPercent, stretch);

  useLayoutEffect(() => {
    const ctx = canvas.context();
    const { width, height } = canvas.size;
    if (!ctx || width < 16 || height < 16) return;
    const theme = readTheme();
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);
    ctx.font = font(theme, 'xs');
    ctx.textBaseline = 'top';

    const envelope = meters?.envelope ?? [];
    const seconds = meters?.seconds ?? 0;

    if (missing || envelope.length === 0 || seconds <= 0) {
      ctx.fillStyle = theme.textDim;
      ctx.textAlign = 'center';
      ctx.fillText(missing ? 'The file is missing' : 'Building…', width / 2, height / 2 - 6);
      return;
    }

    const axis = 14;
    const plot = height - axis;
    const peak = Math.max(...envelope);
    const toX = (t: number) => 4 + (t / seconds) * (width - 8);
    const toY = (db: number) => Math.min(plot, Math.max(0, ((peak - db) / -FLOOR_DB) * plot));
    const span = seconds / envelope.length;

    // The file's envelope, dim.
    ctx.beginPath();
    ctx.moveTo(toX(0), plot);
    envelope.forEach((db, i) => ctx.lineTo(toX((i + 0.5) * span), toY(db)));
    ctx.lineTo(toX(seconds), plot);
    ctx.closePath();
    ctx.fillStyle = theme.textDim;
    ctx.globalAlpha = 0.18;
    ctx.fill();
    ctx.globalAlpha = 1;

    // The kept part, and the cut parts shaded.
    const kept = keptRange(seconds, shape.startMs, shape.lengthPercent);
    ctx.fillStyle = theme.border;
    ctx.globalAlpha = 0.45;
    ctx.fillRect(toX(0), 0, toX(kept.from) - toX(0), plot);
    ctx.fillRect(toX(kept.to), 0, toX(seconds) - toX(kept.to), plot);
    ctx.globalAlpha = 1;

    // The shaped envelope, filled in the accent colour.
    const shaped = shapedEnvelope(envelope, seconds, meters?.decay ?? 0, shape);
    if (shaped.length > 1) {
      ctx.beginPath();
      ctx.moveTo(toX(shaped[0].t), plot);
      shaped.forEach((p) => ctx.lineTo(toX(p.t), toY(p.db)));
      ctx.lineTo(toX(shaped[shaped.length - 1].t), plot);
      ctx.closePath();
      ctx.fillStyle = theme.accent;
      ctx.globalAlpha = 0.3;
      ctx.fill();
      ctx.globalAlpha = 1;
      ctx.beginPath();
      shaped.forEach((p, i) => (i === 0 ? ctx.moveTo(toX(p.t), toY(p.db)) : ctx.lineTo(toX(p.t), toY(p.db))));
      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }

    // The time axis.
    const step = tickStep(seconds, Math.max(2, Math.floor(width / 70)));
    ctx.textAlign = 'left';
    for (let t = 0; t <= seconds + 1e-9; t += step) {
      const x = Math.round(toX(t)) + 0.5;
      ctx.strokeStyle = theme.border;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, plot);
      ctx.lineTo(x, plot + 3);
      ctx.stroke();
      ctx.fillStyle = theme.textDim;
      ctx.fillText(step < 1 ? `${Math.round(t * 1000)}${t === 0 ? ' ms' : ''}` : `${t.toFixed(step < 1 ? 1 : 0)}${t === 0 ? ' s' : ''}`, x + 2, plot + 2);
    }

    if (meters?.truncated) {
      ctx.textAlign = 'right';
      ctx.fillStyle = theme.textDim;
      ctx.fillText('cut at 20 s', width - 4, 3);
    }
  });

  const drop = {
    onDragOver: (e: React.DragEvent) => {
      if (!e.dataTransfer.types.includes(PATHS_MIME)) return;
      e.preventDefault();
      e.stopPropagation();
      setDropping(true);
    },
    onDragLeave: () => setDropping(false),
    onDrop: (e: React.DragEvent) => {
      setDropping(false);
      const paths = e.dataTransfer.getData(PATHS_MIME);
      if (!paths) return;
      e.preventDefault();
      e.stopPropagation();
      bridge.call('convolution.import', { ...target, paths: JSON.parse(paths) as string[] }).catch(reportError);
    },
  };

  return (
    <div className="device-panel convolution-panel">
      <div className={`panel-main convolution-main ${dropping ? 'dropping' : ''}`} {...drop} title="Drop a sound here to use it as the impulse response">
        <div className="convolution-header">
          <span className={`convolution-name ${missing ? 'missing' : ''}`} title={file?.path ?? name}>
            {name}
          </span>
          {missing && fileId !== 0 && (
            <IconButton title="Locate the missing file" onClick={() => projectActions.locateAudio(fileId)}>
              <Search size={12} />
            </IconButton>
          )}
          <IconButton
            title="Load an impulse response"
            onClick={() => bridge.call('convolution.importDialog', target).catch(reportError)}
          >
            <FolderOpen size={12} />
          </IconButton>
          {fileId !== 0 && (
            <IconButton
              title="Back to the default room"
              onClick={() => dispatch({ command: 'setNodeFile', args: { ...target, fileId: 0 } })}
            >
              <RotateCcw size={12} />
            </IconButton>
          )}
          <span className="convolution-rt" title="The reverb time to −60 dB, after Decay and Stretch">
            {reverbTimeReadout(rt)}
          </span>
        </div>
        <canvas ref={canvas.ref} className="panel-canvas" />
      </div>
      <div className="convolution-groups">
        {rows.map((row, r) => (
          <div key={r} className="convolution-row">
            {row.map(({ label, ids, toggles }) => (
              <Section key={label} label={label}>
                {ids.map((id) =>
                  toggles ? (
                    <SlotToggle key={id} track={track} node={node} param={param(id)} />
                  ) : (
                    <Knob key={id} track={track} node={node} param={param(id)} />
                  ),
                )}
              </Section>
            ))}
          </div>
        ))}
      </div>
    </div>
  );
}
