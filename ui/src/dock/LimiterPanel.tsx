import { useLayoutEffect, useRef } from 'react';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useNodeAnalysis } from './analysis';
import { Knob } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';

// The limiter's panel (docs/design/sampler-midi-delay-limiter.md, section 2): the gain reduction hanging from the top,
// scrolling right to left, with the gain and the ceiling and release as knobs.

const RANGE_DB = 24; // of gain reduction, top to bottom
const HISTORY = 240; // frames kept

export function LimiterPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const analysis = useNodeAnalysis(track.id, node.id);
  const reduction = (analysis?.meters as { reduction?: number } | null | undefined)?.reduction ?? 0;
  const history = useRef<number[]>([]);
  const lastFrame = useRef(analysis);
  const param = (id: string) => info.params.find((p) => p.id === id)!;

  if (lastFrame.current !== analysis) {
    lastFrame.current = analysis;
    history.current = [...history.current.slice(1 - HISTORY), reduction];
  }

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 || canvas.size.height < 16) return;
    const theme = readTheme();
    const { width, height } = canvas.size;
    const toY = (db: number) => (Math.min(db, RANGE_DB) / RANGE_DB) * height;
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);

    ctx.lineWidth = 1;
    ctx.font = font(theme, 'xs');
    for (const db of [6, 12, 18]) {
      const y = Math.round(toY(db)) + 0.5;
      ctx.strokeStyle = theme.border;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(width, y);
      ctx.stroke();
      ctx.fillStyle = theme.textDim;
      ctx.fillText(`-${db}`, 4, y - 3);
    }

    const values = history.current;
    const step = width / HISTORY;
    ctx.fillStyle = theme.accent;
    ctx.globalAlpha = 0.6;
    values.forEach((db, i) => {
      if (db > 0) ctx.fillRect(width - (values.length - i) * step, 0, Math.ceil(step), toY(db));
    });
    ctx.globalAlpha = 1;
  });

  return (
    <div className="device-panel limiter-panel">
      <div className="panel-main">
        <canvas ref={canvas.ref} className="panel-canvas" />
        <div className="panel-readout">
          <span>Reduction {reduction.toFixed(1)} dB</span>
        </div>
      </div>
      <div className="panel-sections">
        <Section label="Limiter">
          {['gain', 'ceiling', 'release'].map((id) => (
            <Knob key={id} track={track} node={node} param={param(id)} />
          ))}
        </Section>
      </div>
    </div>
  );
}
