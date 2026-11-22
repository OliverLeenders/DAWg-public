import { MonoGlyph, PhaseGlyph } from '../components/Glyphs';
import { useEffect } from 'react';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useNodeAnalysis } from './analysis';
import { Knob, SlotToggle } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';

// The utility's panel (docs/design/phase11-faust-panels.md, P4): a goniometer (the stereo image: mid up, side across)
// with a correlation meter, gain, pan and width as knobs, and the phase and mono switches.

export function UtilityPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const analysis = useNodeAnalysis(track.id, node.id);
  const param = (id: string) => info.params.find((p) => p.id === id)!;

  useEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 || canvas.size.height < 16) return;
    const theme = readTheme();
    const s = Math.min(canvas.size.width, canvas.size.height);
    const c = s / 2;
    ctx.fillStyle = theme.bgRaised;
    ctx.fillRect(0, 0, canvas.size.width, canvas.size.height);
    ctx.fillStyle = theme.bg;
    ctx.beginPath();
    ctx.arc(c, c, c - 1, 0, Math.PI * 2);
    ctx.fill();

    // Axes: mono (M) up, left and right on the diagonals.
    ctx.strokeStyle = theme.border;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(c, 4);
    ctx.lineTo(c, s - 4);
    ctx.moveTo(4, c);
    ctx.lineTo(s - 4, c);
    const d = (c - 4) / Math.SQRT2;
    ctx.moveTo(c - d, c - d);
    ctx.lineTo(c + d, c + d);
    ctx.moveTo(c + d, c - d);
    ctx.lineTo(c - d, c + d);
    ctx.stroke();
    ctx.fillStyle = theme.textDim;
    ctx.font = font(theme, 'xs');
    ctx.fillText('M', c + 4, 12);
    ctx.fillText('L', c - d - 10, c - d);
    ctx.fillText('R', c + d + 4, c - d);

    const scope = analysis?.scope;
    if (!scope) return;
    const scale = c - 6;
    ctx.fillStyle = theme.signalAudio;
    ctx.globalAlpha = 0.7;
    for (let i = 0; i + 1 < scope.length; i += 2) {
      const l = scope[i];
      const r = scope[i + 1];
      const x = c + ((r - l) / Math.SQRT2) * scale;
      const y = c - ((l + r) / Math.SQRT2) * scale;
      ctx.fillRect(x - 1, y - 1, 2, 2);
    }
    ctx.globalAlpha = 1;
  });

  const correlation = analysis?.correlation ?? 0;

  return (
    <div className="device-panel utility-panel">
      <div className="panel-main goniometer">
        <canvas ref={canvas.ref} className="panel-canvas" />
        <div className="correlation" title={`Correlation ${correlation.toFixed(2)}`}>
          <span>-1</span>
          <div className="correlation-track">
            <div
              className={`correlation-fill ${correlation < 0 ? 'negative' : ''}`}
              style={{
                left: `${50 + Math.min(0, correlation) * 50}%`,
                width: `${Math.abs(correlation) * 50}%`,
              }}
            />
          </div>
          <span>+1</span>
        </div>
      </div>
      <div className="panel-sections">
        <Section label="Level and image">
          {['gain', 'pan', 'width'].map((id) => (
            <Knob key={id} track={track} node={node} param={param(id)} />
          ))}
        </Section>
        <Section label="Channels">
          <SlotToggle track={track} node={node} param={param('invertLeft')} label="Invert L" icon={<PhaseGlyph />} />
          <SlotToggle track={track} node={node} param={param('invertRight')} label="Invert R" icon={<PhaseGlyph />} />
          <SlotToggle track={track} node={node} param={param('mono')} icon={<MonoGlyph />} />
        </Section>
      </div>
    </div>
  );
}
