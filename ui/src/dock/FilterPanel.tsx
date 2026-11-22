import { Readout, Readouts } from '../components/Readouts';
import { glyphFor } from '../components/Glyphs';
import { useLayoutEffect, useRef, useState } from 'react';
import { ParamValueText } from '../components/ValueText';
import { readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { fromPosition } from '../graph/params';
import { useNodeAnalysis, useNodeDrag, useResponse } from './analysis';
import { MidiLearnable } from '../components/MidiLearn';
import { Knob } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';
import { drawCurve, drawFrequencyGrid, drawHandle, drawSpectrum, freqToX, xToFreq } from './panelDraw';
import { useDawg } from '../store/store';
import { laneFor, playedPosition } from '../automation/automation';
import { nodeTarget } from '../automation/touch';
import type { ResponseCurve } from '../bridge/protocol';
import { Segmented } from '../ui/controls';

// The filter's panel (docs/design/phase11-faust-panels.md, P4): the response curve with one handle (cutoff on x,
// resonance on y), the mode and slope as dropdowns, and drive as a knob. When modulated (automation, LFO, or ADSR),
// a faint additional response curve is rendered showing the modulated result.

const RANGE = 24;

export interface FilterModulationState {
  isModulated: boolean;
  activeParams: {
    cutoff: number;
    resonance: number;
    drive: number;
    slope: number;
    mode: number;
  };
}

export function computeFilterModulation({
  baseParams,
  meters,
  automatedParams,
  hasModulationSlots,
}: {
  baseParams: { cutoff: number; resonance: number; drive: number; slope: number; mode: number };
  meters?: {
    cutoff?: number;
    resonance?: number;
    drive?: number;
    slope?: number;
    mode?: number;
    hasModulationInput?: boolean;
    isModulated?: boolean;
  } | null;
  automatedParams?: Partial<{ cutoff: number; resonance: number; drive: number; slope: number; mode: number }>;
  hasModulationSlots?: boolean;
}): FilterModulationState {
  const activeCutoff = meters?.cutoff ?? automatedParams?.cutoff ?? baseParams.cutoff;
  const activeResonance = meters?.resonance ?? automatedParams?.resonance ?? baseParams.resonance;
  const activeDrive = meters?.drive ?? automatedParams?.drive ?? baseParams.drive;
  const activeSlope = meters?.slope ?? automatedParams?.slope ?? baseParams.slope;
  const activeMode = meters?.mode ?? automatedParams?.mode ?? baseParams.mode;

  const hasModulationSource = Boolean(
    meters?.hasModulationInput ||
      meters?.isModulated ||
      hasModulationSlots ||
      automatedParams?.cutoff !== undefined ||
      automatedParams?.resonance !== undefined ||
      automatedParams?.drive !== undefined ||
      automatedParams?.slope !== undefined ||
      automatedParams?.mode !== undefined,
  );

  const differsFromBase =
    Math.abs(activeCutoff - baseParams.cutoff) > 0.5 ||
    Math.abs(activeResonance - baseParams.resonance) > 0.2 ||
    Math.abs(activeDrive - baseParams.drive) > 0.1 ||
    Math.abs(activeSlope - baseParams.slope) > 0.1 ||
    Math.abs(activeMode - baseParams.mode) > 0.1;

  return {
    isModulated: hasModulationSource && differsFromBase,
    activeParams: {
      cutoff: activeCutoff,
      resonance: activeResonance,
      drive: activeDrive,
      slope: activeSlope,
      mode: activeMode,
    },
  };
}

export function FilterPanel({ track, node, info }: PanelProps) {
  const canvas = useCanvas();
  const edit = useNodeDrag(track.id, node.id, node.params);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const params = Object.fromEntries(info.params.map((p) => [p.id, value(p.id)]));
  const curve = useResponse(node.type, params);
  const analysis = useNodeAnalysis(track.id, node.id);
  const spectrum = analysis?.spectrum ?? null;
  const dragging = useRef(false);
  const [active, setActive] = useState(false);

  // Automation's played values. Each selector returns one number, so telemetry re-renders the panel only when a value
  // of this filter moved.
  const useAutomatedValue = (paramId: string) =>
    useDawg((s) => {
      if (track.automationMode === 0) return undefined;
      const lane = laneFor(track, nodeTarget(node.id, paramId));
      if (!lane || !lane.active) return undefined;
      const played = playedPosition(s.telemetry.automation, lane.id);
      return played !== undefined ? fromPosition(param(paramId), played) : undefined;
    });

  const autoCutoff = useAutomatedValue('cutoff');
  const autoResonance = useAutomatedValue('resonance');
  const autoDrive = useAutomatedValue('drive');
  const autoSlope = useAutomatedValue('slope');
  const autoMode = useAutomatedValue('mode');

  const meters = analysis?.meters as {
    cutoff?: number;
    resonance?: number;
    drive?: number;
    slope?: number;
    mode?: number;
    hasModulationInput?: boolean;
    isModulated?: boolean;
    curve?: ResponseCurve;
  } | null;

  const baseCutoff = value('cutoff');
  const baseResonance = value('resonance');
  const baseDrive = value('drive');
  const baseSlope = value('slope');
  const baseMode = value('mode');

  const { isModulated, activeParams } = computeFilterModulation({
    baseParams: {
      cutoff: baseCutoff,
      resonance: baseResonance,
      drive: baseDrive,
      slope: baseSlope,
      mode: baseMode,
    },
    meters,
    automatedParams: {
      cutoff: autoCutoff,
      resonance: autoResonance,
      drive: autoDrive,
      slope: autoSlope,
      mode: autoMode,
    },
    hasModulationSlots: Boolean(node.modulations && Object.values(node.modulations).some((slot) => slot.active)),
  });

  const fallbackModCurve = useResponse(node.type, isModulated ? { ...params, ...activeParams } : params);
  const modCurve = meters?.curve ?? fallbackModCurve;

  // Resonance runs from the bottom (0 %) to the top (100 %) of the canvas.
  const handle = () => ({
    x: freqToX(baseCutoff, canvas.size.width),
    y: canvas.size.height - 12 - (baseResonance / 100) * (canvas.size.height - 24),
  });

  const modHandle = () => ({
    x: freqToX(activeParams.cutoff, canvas.size.width),
    y: canvas.size.height - 12 - (activeParams.resonance / 100) * (canvas.size.height - 24),
  });

  useLayoutEffect(() => {
    const ctx = canvas.context();
    if (!ctx || canvas.size.width < 16 || canvas.size.height < 16) return;
    const theme = readTheme();
    drawFrequencyGrid(ctx, canvas.size, theme, RANGE, 12);
    drawSpectrum(ctx, canvas.size, theme, spectrum);

    const h = handle();

    // When modulated (by automation, an LFO, or an ADSR), draw the modulation result as a faint additional curve
    if (isModulated && modCurve) {
      ctx.save();
      ctx.globalAlpha = 0.4;
      drawCurve(ctx, canvas.size, modCurve.x, modCurve.y, RANGE, theme.accent, 1.5, false);

      const mh = modHandle();
      if (Math.hypot(mh.x - h.x, mh.y - h.y) > 3) {
        ctx.beginPath();
        ctx.arc(mh.x, mh.y, 4.5, 0, Math.PI * 2);
        ctx.fillStyle = theme.accent;
        ctx.fill();
      }
      ctx.restore();
    }

    // Base curve and primary interactive handle
    if (curve) drawCurve(ctx, canvas.size, curve.x, curve.y, RANGE, theme.accent, 2, true);
    drawHandle(ctx, theme, h.x, h.y, theme.accent, active);
  });

  const local = (e: React.PointerEvent) => {
    const rect = e.currentTarget.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  };
  const mode = param('mode');
  const slope = info.params.find((p) => p.id === 'slope');
  const oversampling = info.params.find((p) => p.id === 'oversampling');

  return (
    <div className="device-panel filter-panel">
      <div className="panel-main">
        <div className="filter-header">
          <div className="segmented" role="radiogroup" aria-label="Filter mode">
            {mode.choices.map((label, i) => (
              <button
                key={label}
                role="radio"
                aria-checked={Math.round(value('mode')) === i}
                aria-label={label}
                className={Math.round(value('mode')) === i ? 'active' : ''}
                title={label}
                onClick={() => {
                  edit.begin(['mode']);
                  edit.set({ mode: i });
                  edit.end();
                }}
              >
                {glyphFor(label) ?? label}
              </button>
            ))}
          </div>
          {slope && (
            <label className="field">
              <span>Slope</span>
              <select
                value={Math.round(value('slope'))}
                title="Filter slope"
                onChange={(e) => {
                  edit.begin(['slope']);
                  edit.set({ slope: Number(e.target.value) });
                  edit.end();
                }}
              >
                {slope.choices.map((label, i) => (
                  <option key={label} value={i}>
                    {label}
                  </option>
                ))}
              </select>
            </label>
          )}
          {oversampling && (
            // The same segmented choice as the saturator's and the clipper's (design-system.md, step 4).
            <Segmented
              label="Oversampling: resonance and slope true to the analogue filter up to 20 kHz, and cleaner drive, for a few samples of latency and more CPU. Inside a feedback loop the filter runs without it."
              options={oversampling.choices.map((label, i) => ({ value: i, label }))}
              value={Math.round(value('oversampling'))}
              onChange={(i) => {
                edit.begin(['oversampling']);
                edit.set({ oversampling: i });
                edit.end();
              }}
            />
          )}
        </div>
        <canvas
          ref={canvas.ref}
          className="panel-canvas"
          onPointerDown={(e) => {
            const { x, y } = local(e);
            const h = handle();
            // Anywhere on the canvas grabs the handle, which jumps to the pointer.
            e.currentTarget.setPointerCapture(e.pointerId);
            dragging.current = true;
            setActive(true);
            edit.begin(['cutoff', 'resonance']);
            if (Math.hypot(h.x - x, h.y - y) > 10) move(x, y);
          }}
          onPointerMove={(e) => {
            if (!dragging.current) return;
            const { x, y } = local(e);
            move(x, y);
          }}
          onPointerUp={() => {
            if (dragging.current) edit.end();
            dragging.current = false;
            setActive(false);
          }}
          onPointerCancel={() => {
            if (dragging.current) edit.end();
            dragging.current = false;
            setActive(false);
          }}
        />
        <Readouts pairs={2}>
          <Readout label="Cutoff">
            <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: 'cutoff' }}>
              <ParamValueText trackId={track.id} nodeId={node.id} param={param('cutoff')} value={value('cutoff')} />
            </MidiLearnable>
          </Readout>
          <Readout label="Resonance">
            <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: 'resonance' }}>
              <ParamValueText trackId={track.id} nodeId={node.id} param={param('resonance')} value={value('resonance')} />
            </MidiLearnable>
          </Readout>
        </Readouts>
      </div>
      <div className="panel-sections">
        <Section label="Drive">
          <Knob track={track} node={node} param={param('drive')} />
        </Section>
      </div>
    </div>
  );

  function move(x: number, y: number) {
    const cutoff = Math.min(param('cutoff').max, Math.max(param('cutoff').min, xToFreq(x, canvas.size.width)));
    const resonance = Math.min(100, Math.max(0, ((canvas.size.height - 12 - y) / (canvas.size.height - 24)) * 100));
    edit.drag({ cutoff, resonance: Math.round(resonance * 10) / 10 });
  }
}
