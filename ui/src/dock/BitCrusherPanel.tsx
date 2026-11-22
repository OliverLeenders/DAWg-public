import { useLayoutEffect } from 'react';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { useDawg } from '../store/store';
import { useNodeDrag } from './analysis';
import { bitsText, holdRate, rateText, scope, SCOPE_HZ, transfer } from './bitCrusher';
import { ChoiceSlot, Knob, SlotToggle } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';

// The Bit Crusher's panel (docs/design/bit-crusher.md, section 4): the graph shows the staircase transfer curve (output
// over input from -1 to 1, through Drive, the clip, the bias and the quantiser) beside, or above, one cycle of a 220 Hz
// test tone, dry and crushed. Both views split the graph along its longer side and fill it across. Every control sits
// beside it in sections, as the other devices have them: Bits, Rate (its mode with it), the two filters, Output; the
// quantiser and the clip as pictures. Two rows of sections, one in a short dock; the card's measured min-content width
// keeps them off the graph.

const FALLBACK_RATE = 48000;
const GAP = 6;

export function BitCrusherPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const audio = useDawg((s) => s.audio);
  const sampleRate = audio.open && audio.sampleRate ? audio.sampleRate : FALLBACK_RATE;
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const on = (id: string) => value(id) >= 0.5;
  const params = Object.fromEntries(info.params.map((p) => [p.id, value(p.id)]));
  const noteMode = on('rateMode');

  useLayoutEffect(() => {
    const ctx = canvas.context();
    const { width, height } = canvas.size;
    if (!ctx) return;
    ctx.clearRect(0, 0, width, height);
    if (width < 40 || height < 24) return;
    const theme = readTheme();
    const midRise = params.quantiser >= 0.5;

    // Split along the longer side: the transfer curve at the start, square where the graph is long enough and at most
    // half of it otherwise, filling it across either way, so neither view leaves empty space beside the other.
    const side = width >= height ? Math.min(height, (width - GAP) / 2) : Math.min(width, (height - GAP) / 2);
    const square = width >= height ? { x: 0, y: 0, w: side, h: height } : { x: 0, y: 0, w: width, h: side };
    const rest =
      width >= height
        ? { x: side + GAP, y: 0, w: width - side - GAP, h: height }
        : { x: 0, y: side + GAP, w: width, h: height - side - GAP };

    const frame = (r: typeof square) => {
      ctx.fillStyle = theme.bg;
      ctx.fillRect(r.x, r.y, r.w, r.h);
      ctx.lineWidth = 1;
      ctx.strokeStyle = theme.border;
      ctx.strokeRect(r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);
    };
    const label = (text: string, x: number, y: number, align: CanvasTextAlign = 'left') => {
      ctx.fillStyle = theme.textDim;
      ctx.font = font(theme, 'xs');
      ctx.textAlign = align;
      ctx.fillText(text, x, y);
      ctx.textAlign = 'left';
    };

    // The transfer curve, output over input, both -1..1, the identity dashed for reference.
    frame(square);
    {
      const { x: ox, y: oy, w: sw, h: sh } = square;
      const toX = (x: number) => ox + ((x + 1) / 2) * sw;
      const toY = (y: number) => oy + sh - ((y + 1) / 2) * sh;
      ctx.save();
      ctx.beginPath();
      ctx.rect(ox, oy, sw, sh);
      ctx.clip();
      ctx.strokeStyle = theme.border;
      ctx.globalAlpha = 0.6;
      ctx.beginPath();
      ctx.moveTo(Math.round(toX(0)) + 0.5, oy);
      ctx.lineTo(Math.round(toX(0)) + 0.5, oy + sh);
      ctx.moveTo(ox, Math.round(toY(0)) + 0.5);
      ctx.lineTo(ox + sw, Math.round(toY(0)) + 0.5);
      ctx.stroke();
      ctx.globalAlpha = 1;
      ctx.setLineDash([3, 3]);
      ctx.strokeStyle = theme.textDim;
      ctx.beginPath();
      ctx.moveTo(toX(-1), toY(-1));
      ctx.lineTo(toX(1), toY(1));
      ctx.stroke();
      ctx.setLineDash([]);
      // Two points per pixel, drawn as steps: each value holds until the next.
      const points = Math.max(2, Math.ceil(sw * 2));
      ctx.beginPath();
      let previous = 0;
      for (let i = 0; i <= points; i++) {
        const x = -1 + (2 * i) / points;
        const y = transfer(x, params);
        if (i === 0) ctx.moveTo(toX(x), toY(y));
        else {
          ctx.lineTo(toX(x), toY(previous));
          ctx.lineTo(toX(x), toY(y));
        }
        previous = y;
      }
      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = 2;
      ctx.stroke();
      ctx.restore();
      if (sw >= 90 && sh >= 40) label(bitsText(params.bits, midRise), ox + 4, oy + 12);
    }

    // The scope: one cycle of the test tone, the dry wave dim, the crushed one in the accent colour.
    if (rest.w >= 24 && rest.h >= 24) {
      frame(rest);
      const { dry, wet } = scope(params, sampleRate);
      const toX = (i: number) => rest.x + (i / dry.length) * rest.w;
      const toY = (v: number) => rest.y + rest.h / 2 - v * (rest.h / 2 - 4);
      ctx.save();
      ctx.beginPath();
      ctx.rect(rest.x, rest.y, rest.w, rest.h);
      ctx.clip();
      ctx.strokeStyle = theme.border;
      ctx.globalAlpha = 0.6;
      ctx.beginPath();
      ctx.moveTo(rest.x, Math.round(toY(0)) + 0.5);
      ctx.lineTo(rest.x + rest.w, Math.round(toY(0)) + 0.5);
      ctx.stroke();
      ctx.globalAlpha = 1;
      ctx.strokeStyle = theme.textDim;
      ctx.lineWidth = 1;
      ctx.beginPath();
      dry.forEach((v, i) => (i === 0 ? ctx.moveTo(toX(i), toY(v)) : ctx.lineTo(toX(i), toY(v))));
      ctx.stroke();
      ctx.beginPath();
      wet.forEach((v, i) => {
        if (i === 0) ctx.moveTo(toX(0), toY(v));
        else {
          ctx.lineTo(toX(i), toY(wet[i - 1]));
          ctx.lineTo(toX(i), toY(v));
        }
      });
      ctx.lineTo(toX(wet.length), toY(wet[wet.length - 1]));
      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = 2;
      ctx.stroke();
      ctx.restore();
      if (rest.w >= 110) label(rateText(holdRate(params), sampleRate), rest.x + 4, rest.y + 12);
      if (rest.w >= 150 && rest.h >= 40) label(`${SCOPE_HZ} Hz${on('smooth') ? ' · Smooth not drawn' : ''}`, rest.x + rest.w - 4, rest.y + rest.h - 5, 'right');
    }
  });

  const knobs = (ids: string[]) => ids.map((id) => <Knob key={id} track={track} node={node} param={param(id)} />);

  return (
    <div className="device-panel bitcrusher-panel">
      <div className="panel-main">
        <canvas ref={canvas.ref} className="panel-canvas" />
      </div>
      <div className="bitcrusher-sections">
        <Section label="Bits">
          {knobs(['bits', 'bias'])}
          <ChoiceSlot track={track} node={node} param={param('quantiser')} />
          <SlotToggle track={track} node={node} param={param('dither')} />
        </Section>
        <Section label="Rate">
          <ChoiceSlot track={track} node={node} param={param('rateMode')} label="Mode" />
          {knobs([noteMode ? 'note' : 'rate', 'jitter'])}
        </Section>
        <Section label="Output">
          {knobs(['drive'])}
          <ChoiceSlot track={track} node={node} param={param('clip')} />
          {knobs(['mix', 'output'])}
        </Section>
        <Section label="Filters">
          <SlotToggle
            track={track}
            node={node}
            param={param('antiAlias')}
            label="Anti-alias"
            hint="a low-pass at 0.45 × the rate before the hold (less fold-back)"
          />
          <SlotToggle track={track} node={node} param={param('smooth')} hint="a low-pass at 0.45 × the rate after the quantiser (rounds the steps)" />
        </Section>
      </div>
    </div>
  );
}
