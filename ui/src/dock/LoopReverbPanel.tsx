import { Dices } from 'lucide-react';
import { useEffect, useRef } from 'react';
import { DragNumber } from '../components/DragNumber';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { dispatch, newGestureId } from '../store/store';
import { useNodeAnalysis } from './analysis';
import { Knob, SlotToggle } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';
import { useParam } from './useParam';
import { IconButton } from '../ui/controls';

// The Loop Reverb's panel (docs/design/loop-reverb.md): when each channel's main early reflections arrive, after the
// pre-delay, a live level strip of the last two seconds behind them, the tap pattern's seed with a dice button, and the
// controls in groups, in two rows to the right.

// The controls in signal-flow order, three groups to a row, so two rows end at the output on the right.
const groups = [
  { label: 'Early', ids: ['predelay', 'earlyTiming'] },
  { label: 'Space', ids: ['size', 'width'] },
  { label: 'Loop', ids: ['feedback', 'diffusion'] },
  { label: 'Tone', ids: ['damp', 'thin'] },
  { label: 'Motion', ids: ['modulation'] },
  { label: 'Output', ids: ['mix'] },
];

const HISTORY = 60; // frames of 1/30 s: two seconds
const FLOOR_DB = -60;

/** The reverb's meters: arrival times in ms after the input (pre-delay included), ascending, per channel. */
interface ReverbMeters {
  arrivalsLeft?: number[];
  arrivalsRight?: number[];
}

// A step of 1, 2 or 5 times a power of ten giving about five labels.
function tickStep(range: number) {
  const raw = range / 5;
  const power = Math.pow(10, Math.floor(Math.log10(raw)));
  return [1, 2, 5, 10].map((m) => m * power).find((s) => s >= raw) ?? raw;
}

export function LoopReverbPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const analysis = useNodeAnalysis(track.id, node.id);
  const meters = analysis?.meters as unknown as ReverbMeters | null | undefined;
  const history = useRef<number[]>([]);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const predelay = useParam(track, node, param('predelay')).value;

  // One level per analysis frame, the louder channel.
  useEffect(() => {
    if (!analysis?.level) return;
    history.current = [...history.current, Math.max(...analysis.level)].slice(-HISTORY);
  }, [analysis]);

  useEffect(() => {
    const ctx = canvas.context();
    const { width, height } = canvas.size;
    if (!ctx || width < 16 || height < 16) return;
    const theme = readTheme();
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);

    // The level strip: the last two seconds, newest on the right.
    const levels = history.current;
    if (levels.length > 1) {
      // Along the bottom from under the oldest level, over the levels left to right, down at the newest.
      const xOf = (i: number) => width - ((levels.length - 1 - i) / (HISTORY - 1)) * width;
      ctx.beginPath();
      ctx.moveTo(xOf(0), height);
      levels.forEach((db, i) => {
        const y = height - Math.max(0, Math.min(1, (db - FLOOR_DB) / -FLOOR_DB)) * height;
        ctx.lineTo(xOf(i), y);
      });
      ctx.lineTo(xOf(levels.length - 1), height);
      ctx.closePath();
      ctx.fillStyle = theme.signalAudio;
      ctx.globalAlpha = 0.12;
      ctx.fill();
      ctx.globalAlpha = 1;
    }

    ctx.font = font(theme, 'xs');
    ctx.fillStyle = theme.textDim;
    ctx.textBaseline = 'top';
    ctx.textAlign = 'right';
    ctx.fillText('level, last 2 s', width - 4, 3);
    ctx.textAlign = 'left';

    const sides = [meters?.arrivalsLeft, meters?.arrivalsRight];
    if (!sides[0]?.length || !sides[1]?.length) {
      ctx.fillText('Waiting for the reverb…', 6, height / 2);
      return;
    }

    const times = sides as number[][];
    const longest = Math.max(20, predelay, ...times.map((a) => a.at(-1) ?? 0)) * 1.05;
    const axis = 14;
    const plot = height - axis;
    const toX = (ms: number) => 6 + (ms / longest) * (width - 12);

    // The pre-delay: nothing arrives before it.
    if (predelay > 0) {
      ctx.fillStyle = theme.border;
      ctx.globalAlpha = 0.35;
      ctx.fillRect(toX(0), 0, toX(predelay) - toX(0), plot);
      ctx.globalAlpha = 1;
    }

    // Time grid and labels.
    const step = tickStep(longest);
    for (let ms = 0; ms <= longest; ms += step) {
      const x = Math.round(toX(ms)) + 0.5;
      ctx.strokeStyle = theme.border;
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, plot);
      ctx.stroke();
      ctx.fillStyle = theme.textDim;
      ctx.fillText(`${Math.round(ms)}${ms === 0 ? ' ms' : ''}`, x + 2, plot + 1);
    }

    // Each channel's arrivals, shorter as they come later.
    times.forEach((arrivals, side) => {
      const top = side === 0 ? 4 : plot / 2 + 2;
      const rowHeight = plot / 2 - 6;
      ctx.fillStyle = theme.textDim;
      ctx.fillText(side === 0 ? 'L' : 'R', 2, top);
      ctx.strokeStyle = theme.accent;
      ctx.lineWidth = 2;

      for (const ms of arrivals) {
        const x = Math.round(toX(ms)) + 0.5;
        const h = (rowHeight - 8) * (1 - (0.6 * (ms - predelay)) / Math.max(1, longest - predelay));
        ctx.beginPath();
        ctx.moveTo(x, top + rowHeight - h);
        ctx.lineTo(x, top + rowHeight);
        ctx.stroke();
      }
    });
  });

  return (
    <div className="device-panel loop-reverb-panel">
      <div className="panel-main">
        <canvas ref={canvas.ref} className="panel-canvas" />
        <Seed props={{ track, node, info }} />
      </div>
      <div className="loop-reverb-groups">
        {groups.map(({ label, ids }) => (
          <Section key={label} label={label}>
            {ids.map((id) => (
              <Knob key={id} track={track} node={node} param={param(id)} />
            ))}
            {label === 'Output' && <SlotToggle track={track} node={node} param={param('mute')} />}
          </Section>
        ))}
      </div>
    </div>
  );
}

// The tap pattern's seed: typed or dragged to return to a pattern, or the dice for a new one (one undo step).
function Seed({ props }: { props: PanelProps }) {
  const { track, node, info } = props;
  const param = info.params.find((p) => p.id === 'seed')!;
  const edit = useParam(track, node, param);
  const set = (value: number, gesture = newGestureId()) =>
    dispatch({ command: 'setNodeParam', args: { trackId: track.id, nodeId: node.id, param: 'seed', value } }, gesture);

  return (
    <div className="loop-reverb-seed">
      <span className="faust-control-label">Pattern</span>
      <DragNumber
        className="number-box"
        value={Math.round(edit.value)}
        min={param.min}
        max={param.max}
        integer
        title="The tap pattern's seed: drag or double-click to type"
        onChange={(value, gesture) => set(Math.round(value), gesture)}
      />
      <IconButton
        title="New random pattern"
        onClick={() => set(1 + Math.floor(Math.random() * (param.max - 1)))}
      >
        <Dices size={14} />
      </IconButton>
    </div>
  );
}
