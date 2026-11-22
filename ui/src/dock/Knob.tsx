import { Minus, Plus } from 'lucide-react';
import { memo, useMemo, useRef, useState } from 'react';
import type { GraphNode, ParamDescriptor, Track } from '../bridge/protocol';
import { allHaveGlyphs, glyphFor } from '../components/Glyphs';
import { paramActive } from '../graph/lfo';
import { formatParam, fromPosition, noteHint, parseNumber, parseParam, toPosition } from '../graph/params';
import { DragNumber } from '../components/DragNumber';
import { paramHint, ValueText } from '../components/ValueText';
import { MidiLearnable } from '../components/MidiLearn';
import { assignSource, mappable } from './assign';
import { drivenBy, macrosOf, slotLinks, swingOf, type SlotLink, type Swing } from './macros';
import { ownerOf } from '../graph/voice';
import { dispatch, newGestureId } from '../store/store';
import { sourceDrag, useModulationUi } from './modulationStore';
import { useLinkMarks } from './useLinkMarks';
import { useParam } from './useParam';
import { paramTaper } from '../graph/params';
import { Toggle } from '../ui/controls';

// A rotary control for a node parameter: drag up or down (Shift for fine), double-click to reset. The arc spans
// 270 degrees, along the same curve as the inspector's sliders.

const SWEEP = 270;
const PIXELS = 160; // a full turn per 160 px of drag

function arc(cx: number, cy: number, r: number, from: number, to: number) {
  const point = (deg: number) => {
    const a = ((deg - 90) * Math.PI) / 180;
    return `${cx + r * Math.cos(a)} ${cy + r * Math.sin(a)}`;
  };
  const large = to - from > 180 ? 1 : 0;
  return `M ${point(from)} A ${r} ${r} 0 ${large} 1 ${point(to)}`;
}

/** How a knob shows its modulation (docs/design/macros-and-modulation-column.md, MC13): the swing its sources give it,
    whether the source under the pointer drives it, and a source dragged from the modulation column dropped on it. */
export function useModulationLink(track: Track, node: GraphNode, param: ParamDescriptor, base: number) {
  const links = useMemo(() => slotLinks(track).filter((l) => l.node.id === node.id && l.slot.param === param.id), [track, node.id, param.id]);
  const swing = useMemo(
    () => (links.length > 0 ? swingOf(track, node, param, base, (v) => toPosition(param, v)) : null),
    [links, track, node, param, base],
  );
  const driven = useModulationUi(
    (s) => s.hovered !== null && s.hovered.trackId === track.id && links.some((l) => drivenBy(l, s.hovered!, macrosOf(track))),
  );
  const mapping = useModulationUi((s) => s.mapping?.trackId === track.id && mappable(node, param.modulatable));
  const [over, setOver] = useState(false);
  const accepts = mappable(node, param.modulatable);
  const drop = accepts
    ? {
        onDragOver: (event: React.DragEvent) => {
          if (!sourceDrag.current(event)) return;
          event.preventDefault();
          event.dataTransfer.dropEffect = 'link';
          setOver(true);
        },
        onDragLeave: () => setOver(false),
        onDrop: (event: React.DragEvent) => {
          const source = sourceDrag.current(event);
          setOver(false);
          if (!source) return;
          event.preventDefault();
          assignSource(track, source, node, param.id);
        },
      }
    : {};
  const classes = `${driven ? 'mod-driven' : ''} ${over ? 'mod-drop' : ''} ${mapping ? 'mod-mapping' : ''}`;
  // The modulation an Alt-drag sets the depth of: the one whose source is open in the modulation column, or else the
  // newest (docs/design/macros-and-modulation-column.md, MC16).
  const open = useModulationUi((s) => s.open[track.id]);
  const depthLink =
    (open !== undefined ? links.find((l) => drivenBy(l, { trackId: track.id, nodeId: open }, macrosOf(track))) : undefined) ??
    links[links.length - 1];
  return { swing, classes, drop, depthLink };
}

/** The pixels of Alt-drag that move a modulation's depth by a knob's whole travel, as turning the knob does. */
const DEPTH_PIXELS = 160;

/** Sets a modulation's depth (-1..1 of its span) by an Alt-drag on its knob (MC16). */
function setDepth(track: Track, link: SlotLink, depth: number, gesture: string) {
  const owner = ownerOf(track, link.node.id);
  dispatch(
    {
      command: 'setModulation',
      args: {
        trackId: track.id,
        ...(owner ? { voiceNodeId: owner.id } : {}),
        nodeId: link.node.id,
        port: link.port,
        param: link.slot.param,
        depth: Math.round(Math.min(1, Math.max(-1, depth)) * 1000) / 1000,
      },
    },
    gesture,
  );
}

/** The ring outside a modulated knob's arc: the swing, and a dot where the macros put it now. */
function SwingRing({ swing }: { swing: Swing }) {
  const start = -SWEEP / 2;
  const from = start + Math.min(swing.low, swing.high) * SWEEP;
  const to = start + Math.max(swing.low, swing.high) * SWEEP;
  const dot = swing.now === undefined ? null : ((start + swing.now * SWEEP - 90) * Math.PI) / 180;
  return (
    <g className="knob-swing">
      {to - from > 0.5 && <path d={arc(20, 20, 18.5, from, to)} />}
      {dot !== null && <circle cx={20 + 18.5 * Math.cos(dot)} cy={20 + 18.5 * Math.sin(dot)} r={1.8} />}
    </g>
  );
}

/** Whole numbers with this many steps or fewer get a stepper, not a knob (docs/ui-review-2026-11-10.md, 1.6). */
const MAX_STEPPER_STEPS = 16;
/** Choices this short (all names together) and this few are buttons side by side, not a menu (1.5). */
const MAX_SEGMENT_CHARACTERS = 22;
const MAX_SEGMENTS = 4;

/** Whether an integer parameter has few enough values for a stepper. */
export const steppable = (param: ParamDescriptor) => param.kind === 'integer' && param.max - param.min <= MAX_STEPPER_STEPS;

/** Choices with a picture for every value (waveforms, filter types, directions) are pictures side by side, up to this
    many (2.1, 2.2). */
const MAX_GLYPH_SEGMENTS = 6;

/** Whether a choice parameter's values fit as buttons side by side: a few short words, or pictures. */
export const segmentable = (param: ParamDescriptor) =>
  param.kind === 'choice' &&
  param.choices.length >= 2 &&
  ((param.choices.length <= MAX_SEGMENTS && param.choices.join('').length <= MAX_SEGMENT_CHARACTERS) ||
    (param.choices.length <= MAX_GLYPH_SEGMENTS && allHaveGlyphs(param.choices)));

type SlotProps = { track: Track; node: GraphNode; param: ParamDescriptor; label?: string };

/** The class a slot gets when its parameter does nothing in the current mode (1.8). */
const inactiveClass = (node: GraphNode, param: ParamDescriptor) => (paramActive(node, param.id) ? '' : 'inactive');

/** A knob for a number, or a stepper for a whole number with a few values. Memoised: a panel redrawing for its
    analysis 30 times a second leaves its knobs alone. */
export const Knob = memo(function Knob(props: SlotProps) {
  return steppable(props.param) ? <StepperSlot {...props} /> : <RotaryKnob {...props} />;
});

/** A whole number with a few values in a knob-sized slot: − value +, the value typed or the buttons pressed. */
function StepperSlot({ track, node, param, label }: SlotProps) {
  const edit = useParam(track, node, param);
  const name = label ?? param.name;
  const value = Math.round(edit.value);
  const step = (by: number) => edit.set(Math.min(param.max, Math.max(param.min, value + by)));
  return (
    <div className={`knob ${inactiveClass(node, param)}`}>
      <div className="stepper-slot">
        <button className="stepper-button" aria-label={`${name}: lower`} title={`${name}: lower`} disabled={value <= param.min} onClick={() => step(-1)}>
          <Minus size={12} />
        </button>
        <ValueText className="stepper-value" text={formatParam(param, value)} title={name} parse={(typed) => parseParam(param, typed)} onSet={(v) => edit.set(v)} />
        <button className="stepper-button" aria-label={`${name}: higher`} title={`${name}: higher`} disabled={value >= param.max} onClick={() => step(1)}>
          <Plus size={12} />
        </button>
      </div>
      <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }}>
        <span className="knob-label" title={name}>
          {name}
        </span>
      </MidiLearnable>
    </div>
  );
}

/** A parameter as a labelled number field, its label above a box to drag or click and type: for offsets and counts
    where a knob would be more than they need (an oscillator's coarse and fine, its unison voices). */
export function ParamField({ track, node, param, label }: SlotProps) {
  const edit = useParam(track, node, param);
  const dragging = useRef(false);
  const name = label ?? param.name;
  const whole = param.kind !== 'continuous';
  // A frequency names its nearest note, and takes one typed (DS17).
  const note = noteHint(param, edit.value);
  return (
    <label className={`param-field ${inactiveClass(node, param)}`} title={note ? `${param.name}: ${note}` : param.name}>
      <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }}>
        <span className="param-field-label">{name}</span>
      </MidiLearnable>
      <DragNumber
        className="number-box"
        value={whole ? Math.round(edit.value) : edit.value}
        min={param.min}
        max={param.max}
        integer={whole}
        taper={paramTaper(param)}
        format={(v) => formatParam(param, v)}
        parse={(typed) => parseParam(param, typed)}
        title={note ? `${note}: drag to change (Shift for fine), click to type a value or a note (A4, C#3+20)` : undefined}
        onGestureStart={() => {
          dragging.current = true;
          edit.begin();
        }}
        onGestureEnd={() => {
          dragging.current = false;
          edit.end();
        }}
        onChange={(v) => (dragging.current ? edit.drag(v) : edit.set(v))}
      />
    </label>
  );
}

function RotaryKnob({ track, node, param, label }: SlotProps) {
  const edit = useParam(track, node, param);
  // A drag: the knob's value, or with Alt on a modulated knob its modulation's depth (MC16). Pressing or letting go of
  // Shift (ten times finer) carries on from where it is.
  const drag = useRef<{
    y: number;
    position: number;
    fine: boolean;
    last?: number;
    depth?: { link: SlotLink; gesture: string };
  } | null>(null);
  const position = toPosition(param, edit.value);
  const start = -SWEEP / 2;
  const angle = start + position * SWEEP;
  const link = useModulationLink(track, node, param, edit.value);
  const marks = useLinkMarks(track.id, node.id, param.id);

  return (
    <div
      className={`knob ${link.classes} ${marks.classes} ${inactiveClass(node, param)}`}
      title={
        edit.linking
          ? marks.title
          : edit.mapping
            ? 'Map mode: move it to map it to the macro at that depth'
            : `Drag to change (Shift: fine), double-click to reset${link.depthLink ? '; Alt+drag: modulation depth' : ''}${marks.title ? `\n${marks.title}` : ''}`
      }
      {...link.drop}
      {...marks.events}
    >
      <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }} contents>
      <svg
        width={40}
        height={40}
        viewBox="0 0 40 40"
        onPointerDown={(e) => {
          e.currentTarget.setPointerCapture(e.pointerId);
          if (e.altKey && link.depthLink) {
            e.preventDefault();
            const depthLink = link.depthLink;
            drag.current = { y: e.clientY, position: depthLink.slot.depth, fine: e.shiftKey, depth: { link: depthLink, gesture: newGestureId() } };
            return;
          }
          drag.current = { y: e.clientY, position, fine: e.shiftKey };
          edit.begin();
        }}
        onPointerMove={(e) => {
          const d = drag.current;
          if (!d) return;
          if (e.shiftKey !== d.fine) Object.assign(d, { y: e.clientY, position: d.last ?? d.position, fine: e.shiftKey });
          const scale = d.fine ? PIXELS * 10 : PIXELS;
          if (d.depth) {
            const depth = Math.min(1, Math.max(-1, d.position + ((d.y - e.clientY) / (d.fine ? DEPTH_PIXELS * 10 : DEPTH_PIXELS))));
            d.last = depth;
            setDepth(track, d.depth.link, depth, d.depth.gesture);
            return;
          }
          const next = Math.min(1, Math.max(0, d.position + (d.y - e.clientY) / scale));
          d.last = next;
          edit.drag(fromPosition(param, next));
        }}
        onPointerUp={() => {
          const wasDepth = drag.current?.depth !== undefined;
          drag.current = null;
          if (!wasDepth) edit.end();
        }}
        onPointerCancel={() => {
          const wasDepth = drag.current?.depth !== undefined;
          drag.current = null;
          if (!wasDepth) edit.end();
        }}
        onDoubleClick={() => edit.set(param.default)}
      >
        <path d={arc(20, 20, 15, start, start + SWEEP)} className="knob-track" />
        {position > 0.001 && <path d={arc(20, 20, 15, start, angle)} className="knob-value" />}
        {link.swing && <SwingRing swing={link.swing} />}
        <line
          x1={20}
          y1={20}
          x2={20 + 11 * Math.sin((angle * Math.PI) / 180)}
          y2={20 - 11 * Math.cos((angle * Math.PI) / 180)}
          className="knob-pointer"
        />
      </svg>
      </MidiLearnable>
      <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }}>
        <span className="knob-label" title={label ?? param.name}>
          {label ?? param.name}
        </span>
      </MidiLearnable>
      <ValueText
        className="knob-value-text"
        text={formatParam(param, edit.value)}
        title={label ?? param.name}
        parse={(typed) => parseParam(param, typed)}
        hint={paramHint(param, edit.value)}
        onSet={(value) => edit.set(value)}
      />
    </div>
  );
}

/** The same control for a value that isn't a node parameter, such as a sample zone's: linear over its range. */
export function DragKnob({
  label,
  value,
  min,
  max,
  resetValue,
  text,
  taper = 1,
  onBegin,
  onChange,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  resetValue: number;
  text: (value: number) => string;
  /** The knob's curve: its position to the power of this is the share of the range, so above 1 the low values get
      more of the sweep (times in ms). */
  taper?: number;
  onBegin: () => void;
  onChange: (value: number) => void;
}) {
  // A typed value is in the units shown (what text() writes), clamped to the range.
  const typed = (value: number) => {
    onBegin();
    onChange(value);
  };
  const drag = useRef<{ y: number; position: number; fine: boolean; last?: number } | null>(null);
  const position = Math.pow(Math.min(1, Math.max(0, (value - min) / (max - min))), 1 / taper);
  const start = -SWEEP / 2;
  const angle = start + position * SWEEP;

  return (
    <div className="knob" title="Drag to change (Shift: fine), double-click to reset">
      <svg
        width={40}
        height={40}
        viewBox="0 0 40 40"
        onPointerDown={(e) => {
          e.currentTarget.setPointerCapture(e.pointerId);
          drag.current = { y: e.clientY, position, fine: e.shiftKey };
          onBegin();
        }}
        onPointerMove={(e) => {
          const d = drag.current;
          if (!d) return;
          // Pressing or letting go of Shift carries on from where the knob is.
          if (e.shiftKey !== d.fine) Object.assign(d, { y: e.clientY, position: d.last ?? d.position, fine: e.shiftKey });
          const scale = d.fine ? PIXELS * 10 : PIXELS;
          const next = Math.min(1, Math.max(0, d.position + (d.y - e.clientY) / scale));
          d.last = next;
          onChange(min + Math.pow(next, taper) * (max - min));
        }}
        onPointerUp={() => (drag.current = null)}
        onPointerCancel={() => (drag.current = null)}
        onDoubleClick={() => {
          onBegin();
          onChange(resetValue);
        }}
      >
        <path d={arc(20, 20, 15, start, start + SWEEP)} className="knob-track" />
        {position > 0.001 && <path d={arc(20, 20, 15, start, angle)} className="knob-value" />}
        <line
          x1={20}
          y1={20}
          x2={20 + 11 * Math.sin((angle * Math.PI) / 180)}
          y2={20 - 11 * Math.cos((angle * Math.PI) / 180)}
          className="knob-pointer"
        />
      </svg>
      <span className="knob-label" title={label}>
        {label}
      </span>
      <ValueText className="knob-value-text" text={text(value)} title={label} parse={(t) => parseNumber(t, min, max)} onSet={typed} />
    </div>
  );
}

/** An on/off parameter in a knob-sized slot: a lit button saying On or Off where the knob would be, its name below, so
    toggles line up with knobs in a panel section (docs/ui-review-2026-11-10.md, 1.7: one switch style). */
export function SlotToggle({
  track,
  node,
  param,
  label,
  icon,
  hint,
}: {
  track: Track;
  node: GraphNode;
  param: ParamDescriptor;
  label?: string;
  /** A picture to show instead of On or Off (Utility's Ø and mono); lit when on. */
  icon?: React.ReactNode;
  /** What the switch does, for its tooltip. */
  hint?: string;
}) {
  const edit = useParam(track, node, param);
  const on = edit.value >= 0.5;
  return (
    <div className={`knob ${inactiveClass(node, param)}`}>
      <Toggle
        className="slot-toggle"
        on={on}
        title={`${label ?? param.name}${hint ? `, ${hint}` : ''}: ${on ? 'on' : 'off'} (click to switch)`}
        onChange={(next) => edit.set(next ? 1 : 0)}
      >
        {icon ?? (on ? 'On' : 'Off')}
      </Toggle>
      <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }}>
        <span className="knob-label" title={label ?? param.name}>
          {label ?? param.name}
        </span>
      </MidiLearnable>
    </div>
  );
}

/** A choice parameter in a knob-sized slot: a menu where the knob would be, its name below, so choices line up with
    knobs in a panel section. A few short choices are buttons side by side instead (docs/ui-review-2026-11-10.md, 1.5). */
export function ChoiceSlot({
  track,
  node,
  param,
  label,
}: {
  track: Track;
  node: GraphNode;
  param: ParamDescriptor;
  label?: string;
}) {
  const edit = useParam(track, node, param);
  const name = label ?? param.name;
  const chosen = Math.round(edit.value);
  if (segmentable(param)) {
    const pictures = allHaveGlyphs(param.choices);
    return (
      <div className={`knob segmented-knob ${inactiveClass(node, param)}`}>
        <div className="segmented-slot" role="radiogroup" aria-label={name}>
          {param.choices.map((choice, i) => (
            <button
              key={choice}
              role="radio"
              aria-checked={i === chosen}
              className={`segment ${i === chosen ? 'active' : ''}`}
              title={`${name}: ${choice}`}
              aria-label={choice}
              onClick={() => edit.set(i)}
            >
              {pictures ? glyphFor(choice) : choice}
            </button>
          ))}
        </div>
        <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }}>
          <span className="knob-label" title={name}>
            {name}
          </span>
        </MidiLearnable>
      </div>
    );
  }
  return (
    <div className={`knob ${inactiveClass(node, param)}`}>
      <select
        className="choice-slot"
        value={Math.round(edit.value)}
        title={`${param.name}: ${param.choices[Math.round(edit.value)] ?? ''}`}
        onChange={(e) => edit.set(Number(e.target.value))}
      >
        {param.choices.map((choice, i) => (
          <option key={choice} value={i}>
            {choice}
          </option>
        ))}
      </select>
      <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }}>
        <span className="knob-label" title={label ?? param.name}>
          {label ?? param.name}
        </span>
      </MidiLearnable>
    </div>
  );
}

/** A parameter's control in a panel's row of knobs, by its kind (docs/ui-review-2026-11-10.md, 1.1): a menu for a
    choice, a switch for a toggle, a knob for a number. Every panel that lists parameters uses this, so no switch ends
    up on a knob. */
export function ParamSlot({ track, node, param, label }: { track: Track; node: GraphNode; param: ParamDescriptor; label?: string }) {
  if (param.kind === 'choice') return <ChoiceSlot track={track} node={node} param={param} label={label} />;
  if (param.kind === 'toggle') return <SlotToggle track={track} node={node} param={param} label={label} />;
  return <Knob track={track} node={node} param={param} label={label} />;
}

/** An on/off switch for a toggle parameter where a row has room for a word, not a knob slot (the node inspector, a
    component's node). */
export function ParamSwitch({ name, on, onChange, className = '' }: { name: string; on: boolean; onChange: (on: boolean) => void; className?: string }) {
  return (
    <Toggle className={`param-switch ${className}`} on={on} title={`${name}: ${on ? 'on' : 'off'} (click to switch)`} onChange={onChange}>
      {on ? 'On' : 'Off'}
    </Toggle>
  );
}
