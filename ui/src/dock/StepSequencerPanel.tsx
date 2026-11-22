import { Eraser, Minus, Play, Plus, Square } from 'lucide-react';
import { useEffect, useMemo, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { STEP_RATES, STEP_RATE_PER_BEAT, TICKS_PER_BEAT, type StepSequence } from '../bridge/protocol';
import { tempoMapOf } from '../time/tempoMap';
import { DragNumber } from '../components/DragNumber';
import { lockToScale } from '../music/scales';
import { alpha, font, LIGHT, readTheme } from '../canvas/theme';
import { keyChangeAt, scaleOf } from '../pianoroll/fold';
import { parsePitch, pitchName } from '../pianoroll/geometry';
import { useCanvas } from '../canvas/useCanvas';
import { dispatch, newGestureId, reportError, useDawg } from '../store/store';
import type { PanelProps } from './panels';
import { Section } from './Section';
import { cellAt, gridLayout, hitGrid, patternName, playingStep, shownSteps, stepIndexAt, velocityAt, type Cell } from './stepGrid';
import { powerTaper } from '../components/dragScale';
import { Button, IconButton } from '../ui/controls';

// A note's length drags finer at its short end (dragScale.ts).
const LENGTH_TAPER = powerTaper(0.1, 16, 2);

// The step sequencer's panel (docs/design/step-sequencer.md, SS9): the shown pattern's grid, rows down and steps across,
// with the pattern, the selected step and the selected row in sections beside it. Click toggles a step and drag paints
// that state; Alt+drag sets a step's velocity; a row's name selects the row.

const EMPTY: StepSequence = { rows: [], patterns: [] };

export function StepSequencerPanel({ track, node }: PanelProps) {
  const steps = node.steps ?? EMPTY;
  const [pattern, setPattern] = useState(0);
  const shown = Math.min(pattern, Math.max(0, steps.patterns.length - 1));
  const [selected, setSelected] = useState<{ row: number; step: number }>({
    row: 0,
    step: 0,
  });
  const [previewing, setPreviewing] = useState(false);
  // Painting shows its steps at once and sends them as one command when the pointer is let go.
  const [painted, setPainted] = useState<Map<string, boolean> | null>(null);
  const paint = useRef<{ on: boolean; cells: Map<string, boolean> } | null>(null);
  const velocityDrag = useRef<{
    row: number;
    step: number;
    gesture: string;
  } | null>(null);
  const canvas = useCanvas();
  const theme = useMemo(readTheme, []);
  const firstKey = useDawg((s) => keyChangeAt(s.project?.keys, 0));

  const base = { trackId: track.id, nodeId: node.id };
  const current = steps.patterns[shown];
  const rowCount = steps.rows.length;
  const stepCount = shownSteps(steps, shown);
  const layout = gridLayout(canvas.size.width, canvas.size.height, rowCount, stepCount);
  const row = Math.min(selected.row, Math.max(0, rowCount - 1));
  const cell = cellAt(steps, shown, row, selected.step);

  const preview = (pattern: number | null) => bridge.call('stepSequencer.preview', { ...base, pattern }).catch(reportError);

  // A held key plays the shown pattern while stopped (docs/design/step-sequencer.md, SS8).
  useEffect(() => {
    bridge.call('stepSequencer.shown', { trackId: track.id, nodeId: node.id, pattern: shown }).catch(() => {});
  }, [track.id, node.id, shown]);

  // The playhead: in a sequencer clip of the shown pattern while playing, or the preview's own clock while it runs.
  const playing = useDawg((s) => s.telemetry.playing);
  const position = useDawg((s) => (s.telemetry.playing ? s.telemetry.positionBeats : -1));
  const bpm = useDawg((s) => tempoMapOf(s.project).bpmAt(s.telemetry.positionBeats * TICKS_PER_BEAT));
  const previewStart = useRef(0);
  const [, setTick] = useState(0);
  useEffect(() => {
    if (!previewing || playing) return;
    previewStart.current = performance.now();
    const timer = window.setInterval(() => setTick((t) => t + 1), 40);
    return () => window.clearInterval(timer);
  }, [previewing, playing, shown]);
  const playhead = (() => {
    if (playing && position >= 0) {
      const at = playingStep(track, steps, position * TICKS_PER_BEAT);
      return at && at.pattern === shown ? at.step : -1;
    }
    if (!previewing) return -1;
    return stepIndexAt(steps, shown, 0, ((performance.now() - previewStart.current) / 60000) * bpm * TICKS_PER_BEAT);
  })();

  // The preview follows the shown pattern, and stops when the panel goes.
  useEffect(() => {
    if (previewing) preview(shown);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [previewing, shown]);
  useEffect(
    () => () => {
      bridge
        .call('stepSequencer.preview', {
          trackId: track.id,
          nodeId: node.id,
          pattern: null,
        })
        .catch(() => {});
    },
    [track.id, node.id],
  );

  const cellShown = (r: number, s: number): Cell => {
    const stored = cellAt(steps, shown, r, s);
    const paintedOn = painted?.get(`${r}:${s}`);
    return paintedOn === undefined ? stored : { ...stored, on: paintedOn };
  };

  useEffect(() => {
    const ctx = canvas.context();
    if (!ctx || !current) return;
    const { width, height } = canvas.size;
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);
    ctx.font = font(theme, 'xs');
    ctx.textBaseline = 'middle';
    const perBeat = STEP_RATE_PER_BEAT[current.rate] ?? 4;

    for (let r = 0; r < rowCount; r++) {
      const y = r * layout.rowHeight;
      const length = current.rows[r]?.length ?? 16;
      // The row's name.
      ctx.fillStyle = r === row ? theme.panel : theme.bgRaised;
      ctx.fillRect(0, y + 1, layout.labelWidth - 2, layout.rowHeight - 2);
      ctx.fillStyle = r === row ? theme.text : theme.textDim;
      const label = steps.rows[r].name || pitchName(steps.rows[r].pitch);
      if (layout.rowHeight >= 9) ctx.fillText(label, 4, y + layout.rowHeight / 2, layout.labelWidth - 8);

      for (let s = 0; s < stepCount; s++) {
        const x = layout.labelWidth + s * layout.cellWidth;
        const inRow = s < length;
        const c = cellShown(r, s);
        // Beats are shaded in turn; steps past the row's length are only outlined.
        ctx.fillStyle = !inRow ? theme.bg : Math.floor(s / perBeat) % 2 === 0 ? alpha(LIGHT, 0.05) : alpha(LIGHT, 0.09);
        ctx.fillRect(x + 1, y + 1, layout.cellWidth - 2, layout.rowHeight - 2);
        if (inRow && c.on) {
          ctx.save();
          ctx.globalAlpha = c.chance < 1 ? 0.45 + 0.4 * c.chance : 1;
          ctx.fillStyle = theme.accent;
          const barHeight = Math.max(2, (layout.rowHeight - 2) * c.velocity);
          ctx.fillRect(x + 1, y + layout.rowHeight - 1 - barHeight, layout.cellWidth - 2, barHeight);
          ctx.restore();
          // Ratchets: a tick per hit along the top.
          if (c.ratchet > 1 && layout.cellWidth >= 8) {
            ctx.fillStyle = theme.text;
            for (let h = 0; h < c.ratchet; h++) ctx.fillRect(x + 2 + ((layout.cellWidth - 4) * (h + 0.5)) / c.ratchet - 1, y + 2, 2, 2);
          }
        }
        if (!inRow) {
          ctx.strokeStyle = alpha(LIGHT, 0.06);
          ctx.strokeRect(x + 1.5, y + 1.5, layout.cellWidth - 3, layout.rowHeight - 3);
        }
      }
    }

    // The playhead: the step each row is on (rows loop on their own lengths).
    if (playhead >= 0) {
      ctx.fillStyle = alpha(LIGHT, 0.28);
      for (let r = 0; r < rowCount; r++) {
        const length = Math.max(1, current.rows[r]?.length ?? 16);
        const s = playhead % length;
        if (s >= stepCount) continue;
        ctx.fillRect(layout.labelWidth + s * layout.cellWidth + 1, r * layout.rowHeight + 1, layout.cellWidth - 2, 2);
        ctx.fillRect(layout.labelWidth + s * layout.cellWidth + 1, (r + 1) * layout.rowHeight - 3, layout.cellWidth - 2, 2);
      }
    }

    // The selected step.
    if (rowCount > 0) {
      ctx.strokeStyle = theme.text;
      ctx.lineWidth = 1;
      ctx.strokeRect(
        layout.labelWidth + selected.step * layout.cellWidth + 0.5,
        row * layout.rowHeight + 0.5,
        layout.cellWidth - 1,
        layout.rowHeight - 1,
      );
    }
  });

  const local = (e: React.PointerEvent) => {
    const rect = canvas.ref.current!.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  };

  const setCell = (fields: Partial<Cell>, gesture?: string) =>
    dispatch(
      {
        command: 'setStepCell',
        args: { ...base, pattern: shown, row, step: selected.step, ...fields },
      },
      gesture,
    );

  const addRow = () => {
    // A new row goes on top, a scale step above the top row in the project's first key (any pitch without one).
    const top = steps.rows[0]?.pitch ?? 59;
    const scale = scaleOf(firstKey);
    const pitch = Math.min(127, scale ? lockToScale(top + 1, scale.root, scale.mask, 'up') : top + 1);
    dispatch({ command: 'addStepRow', args: { ...base, pitch, index: 0 } });
  };

  if (!current) return <div className="device-panel" />;

  // The sections' actions sit in their headers, so their fields line up (docs/ui-review-2026-11-10.md, 3.11).
  const patternActions = (
    <>
      <IconButton
        className={`${previewing ? 'active' : ''}`}
        title={previewing ? 'Stop the preview' : 'Preview this pattern (while the transport is stopped)'}
        onClick={() => {
          if (previewing) preview(null);
          previewStart.current = performance.now();
          setPreviewing(!previewing);
        }}
      >
        {previewing ? <Square size={13} /> : <Play size={13} />}
      </IconButton>
      <IconButton
        aria-label="Clear"
        title={`Clear: switch every step of pattern ${patternName(shown)} off`}
        onClick={() =>
          dispatch({
            command: 'clearStepPattern',
            args: { ...base, pattern: shown },
          })
        }
      >
        <Eraser size={13} />
      </IconButton>
    </>
  );
  const rowActions = (
    <>
      <IconButton aria-label="Add row" title="Add a row on top, a scale step above the top row" onClick={addRow}>
        <Plus size={13} />
      </IconButton>
      {rowCount > 1 && (
        <IconButton
          aria-label="Remove row"
          title="Remove the selected row from every pattern"
          onClick={() =>
            dispatch({
              command: 'removeStepRow',
              args: { ...base, row },
            })
          }
        >
          <Minus size={13} />
        </IconButton>
      )}
    </>
  );

  return (
    <div className="device-panel step-sequencer-panel">
      <div className="panel-main step-grid-main">
        <canvas
          ref={canvas.ref}
          className="step-grid"
          title="Click a step to switch it on or off, drag to paint · Alt+drag sets a step's velocity · click a row's name to select it"
          onPointerDown={(e) => {
            if (e.button !== 0) return;
            const at = local(e);
            const hit = hitGrid(layout, at.x, at.y);
            if (!hit) return;
            if (hit.kind === 'label') {
              setSelected((s) => ({ ...s, row: hit.row }));
              return;
            }
            e.currentTarget.setPointerCapture(e.pointerId);
            setSelected({ row: hit.row, step: hit.step });
            if (e.altKey) {
              const gesture = newGestureId();
              velocityDrag.current = { row: hit.row, step: hit.step, gesture };
              dispatch(
                {
                  command: 'setStepCell',
                  args: {
                    ...base,
                    pattern: shown,
                    row: hit.row,
                    step: hit.step,
                    on: true,
                    velocity: velocityAt(layout, hit.row, at.y),
                  },
                },
                gesture,
              );
              return;
            }
            const on = !cellAt(steps, shown, hit.row, hit.step).on;
            const cells = new Map([[`${hit.row}:${hit.step}`, on]]);
            paint.current = { on, cells };
            setPainted(new Map(cells));
          }}
          onPointerMove={(e) => {
            const at = local(e);
            const v = velocityDrag.current;
            if (v) {
              dispatch(
                {
                  command: 'setStepCell',
                  args: {
                    ...base,
                    pattern: shown,
                    row: v.row,
                    step: v.step,
                    velocity: velocityAt(layout, v.row, at.y),
                  },
                },
                v.gesture,
              );
              return;
            }
            const p = paint.current;
            if (!p) return;
            const hit = hitGrid(layout, at.x, at.y);
            if (hit?.kind !== 'cell' || p.cells.has(`${hit.row}:${hit.step}`)) return;
            p.cells.set(`${hit.row}:${hit.step}`, p.on);
            setPainted(new Map(p.cells));
          }}
          onPointerUp={() => {
            velocityDrag.current = null;
            const p = paint.current;
            paint.current = null;
            if (p) {
              const cells = [...p.cells.keys()].map((key) => {
                const [r, s] = key.split(':').map(Number);
                return { row: r, step: s, on: p.on };
              });
              dispatch({
                command: 'setStepCells',
                args: { ...base, pattern: shown, cells },
              });
            }
            setPainted(null);
          }}
          onPointerCancel={() => {
            velocityDrag.current = null;
            paint.current = null;
            setPainted(null);
          }}
        />
      </div>
      <div className="panel-sections step-sections">
        {/* Each section: its actions in its header, then two rows of labelled fields to drag or click and type, so the
            rows line up across the sections (docs/ui-review-2026-11-10.md, 3.11). */}
        <Section label="Pattern" column actions={patternActions}>
          <div className="step-fields">
            <div className="step-field wide">
              <span>Shown</span>
              <div className="step-patterns">
                {steps.patterns.map((_, index) => (
                  <Button
                    key={index}
                    className={`step-pattern ${index === shown ? 'active' : ''}`}
                    title={`Show pattern ${patternName(index)}`}
                    onClick={() => setPattern(index)}
                  >
                    {patternName(index)}
                  </Button>
                ))}
                {steps.patterns.length < 16 && (
                  <IconButton
                    title={`A new pattern, a copy of ${patternName(shown)}`}
                    onClick={() => {
                      dispatch({
                        command: 'copyStepPattern',
                        args: { ...base, from: shown, to: steps.patterns.length },
                      });
                      setPattern(steps.patterns.length);
                    }}
                  >
                    <Plus size={13} />
                  </IconButton>
                )}
              </div>
            </div>
            <label className="step-field" title="Step size">
              <span>Rate</span>
              <select
                value={current.rate}
                onChange={(e) =>
                  dispatch({
                    command: 'setStepPattern',
                    args: { ...base, pattern: shown, rate: Number(e.target.value) },
                  })
                }
              >
                {STEP_RATES.map((name, index) => (
                  <option key={name} value={index}>
                    {name}
                  </option>
                ))}
              </select>
            </label>
            <label className="step-field" title="Swing: 50 % is straight">
              <span>Swing</span>
              <DragNumber
                className="number-box"
                value={Math.round(current.swing * 100)}
                min={50}
                max={75}
                integer
                format={(v) => `${Math.round(v)} %`}
                onChange={(v, gesture) => dispatch({ command: 'setStepPattern', args: { ...base, pattern: shown, swing: v / 100 } }, gesture)}
              />
            </label>
          </div>
        </Section>
        <Section label={`Step ${selected.step + 1}`} column>
          <div className="step-fields">
            <label className="step-field" title="Velocity, 1 to 127">
              <span>Velocity</span>
              <DragNumber
                className="number-box"
                value={Math.round(cell.velocity * 127)}
                min={1}
                max={127}
                integer
                onChange={(v, g) => setCell({ velocity: v / 127 }, g)}
              />
            </label>
            <label className="step-field" title="Length in steps: above 1 ties over the next steps">
              <span>Length</span>
              <DragNumber
                className="number-box"
                value={cell.length}
                min={0.1}
                max={16}
                quantum={0.1}
                taper={LENGTH_TAPER}
                format={(v) => v.toFixed(1)}
                onChange={(v, g) => setCell({ length: v }, g)}
              />
            </label>
            <label className="step-field" title="Chance: how likely the step plays">
              <span>Chance</span>
              <DragNumber
                className="number-box"
                value={Math.round(cell.chance * 100)}
                min={0}
                max={100}
                integer
                format={(v) => `${Math.round(v)} %`}
                onChange={(v, g) => setCell({ chance: v / 100 }, g)}
              />
            </label>
            <label className="step-field" title="Ratchet: hits in the step, 1 to 4">
              <span>Hits</span>
              <DragNumber
                className="number-box"
                value={cell.ratchet}
                min={1}
                max={4}
                integer
                format={(v) => `×${Math.round(v)}`}
                onChange={(v, g) => setCell({ ratchet: Math.round(v) }, g)}
              />
            </label>
          </div>
        </Section>
        <Section label="Row" column actions={rowActions}>
          <div className="step-fields">
            <label className="step-field wide">
              <span>Name</span>
              <input
                disabled={rowCount === 0}
                className="search step-row-name"
                aria-label="Row name"
                value={steps.rows[row]?.name ?? ''}
                placeholder={rowCount > 0 ? pitchName(steps.rows[row].pitch) : 'No rows'}
                onChange={(e) =>
                  dispatch(
                    {
                      command: 'setStepRow',
                      args: { ...base, row, name: e.target.value },
                    },
                    `row-name-${node.id}-${row}`,
                  )
                }
              />
            </label>
            {rowCount > 0 && (
              <>
                <label className="step-field" title="The pitch this row plays: type a name such as C#3, or a number">
                  <span>Note</span>
                  <DragNumber
                    className="number-box"
                    value={steps.rows[row].pitch}
                    min={0}
                    max={127}
                    integer
                    format={(v) => pitchName(v)}
                    parse={parsePitch}
                    onChange={(v, g) => dispatch({ command: 'setStepRow', args: { ...base, row, pitch: v } }, g)}
                  />
                </label>
                <label className="step-field" title="This row's length in steps, in this pattern: rows of different lengths drift against each other">
                  <span>Steps</span>
                  <DragNumber
                    className="number-box"
                    value={current.rows[row]?.length ?? 16}
                    min={1}
                    max={64}
                    integer
                    onChange={(v, g) => dispatch({ command: 'setStepRow', args: { ...base, row, pattern: shown, length: v } }, g)}
                  />
                </label>
              </>
            )}
          </div>
        </Section>
      </div>
    </div>
  );
}
