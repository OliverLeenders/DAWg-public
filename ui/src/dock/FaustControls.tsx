import { useRef, useState } from 'react';
import { ValueText } from '../components/ValueText';
import type { FaustLayoutItem, FaustProgramInfo, GraphNode, ParamDescriptor, Track } from '../bridge/protocol';
import { MidiLearnable } from '../components/MidiLearn';
import { formatParam, fromPosition, parseParam, toPosition } from '../graph/params';
import { useDawg } from '../store/store';
import { Knob } from './Knob';
import { SectionHeader } from './Section';
import { useParam } from './useParam';
import { Toggle as OnOff } from '../ui/controls';

// A Faust node's generated controls (docs/design/phase11-faust-panels.md, F8): laid out by the script's hgroup, vgroup
// and tgroup; sliders as knobs or sliders by their style, checkboxes as toggles, buttons as momentary buttons and
// bargraphs as meters.

interface Context {
  track: Track;
  node: GraphNode;
  params: Map<string, ParamDescriptor>;
}

export function FaustControls({ track, node, program }: { track: Track; node: GraphNode; program: FaustProgramInfo }) {
  const context: Context = { track, node, params: new Map(program.params.map((p) => [p.id, p])) };
  const root = program.layout;
  // The compiler wraps everything in a group named after the script: show its contents, in a row like a device.
  const items = root.items?.length === 1 && root.items[0].items ? root.items[0].items : (root.items ?? []);

  if (items.length === 0) return <p className="dock-empty">This script has no controls.</p>;

  return (
    <div className="faust-controls">
      <Group item={{ ...root, kind: 'hgroup', items, label: '' }} context={context} />
    </div>
  );
}

function Group({ item, context }: { item: FaustLayoutItem; context: Context }) {
  const [tab, setTab] = useState(0);
  const items = item.items ?? [];

  if (item.kind === 'tgroup') {
    const shown = items[Math.min(tab, items.length - 1)];
    return (
      <div className="faust-group tabs">
        <div className="segmented">
          {items.map((child, i) => (
            <button key={i} className={i === tab ? 'active' : ''} onClick={() => setTab(i)}>
              {child.label || `Tab ${i + 1}`}
            </button>
          ))}
        </div>
        {shown && <Item item={shown} context={context} hideLabel />}
      </div>
    );
  }

  return (
    <div className={`faust-group ${item.kind === 'hgroup' ? 'row' : 'column'}`}>
      {item.label && <SectionHeader label={item.label} />}
      <div className="faust-group-items">
        {items.map((child, i) => (
          <Item key={i} item={child} context={context} />
        ))}
      </div>
    </div>
  );
}

function Item({ item, context, hideLabel = false }: { item: FaustLayoutItem; context: Context; hideLabel?: boolean }) {
  if (item.items) return <Group item={hideLabel ? { ...item, label: '' } : item} context={context} />;
  if (item.kind === 'hbargraph' || item.kind === 'vbargraph') return <Bargraph item={item} context={context} />;

  const param = item.path ? context.params.get(item.path) : undefined;
  if (!param) return null;
  const { track, node } = context;

  if (item.kind === 'button') return <MomentaryButton track={track} node={node} param={param} />;
  if (item.kind === 'checkbox') return <Toggle track={track} node={node} param={param} />;
  if (param.kind === 'choice') return <Choice track={track} node={node} param={param} radio={item.style.startsWith('radio')} />;
  if (item.style === 'knob' || (item.kind !== 'hslider' && item.kind !== 'vslider' && item.kind !== 'nentry'))
    return <Knob track={track} node={node} param={param} />;
  return <Slider track={track} node={node} param={param} vertical={item.kind === 'vslider'} />;
}

interface ParamProps {
  track: Track;
  node: GraphNode;
  param: ParamDescriptor;
}

function Learnable({ track, node, param }: ParamProps) {
  return (
    <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }}>
      <span className="faust-control-label">{param.name}</span>
    </MidiLearnable>
  );
}

function Slider({ track, node, param, vertical }: ParamProps & { vertical: boolean }) {
  const edit = useParam(track, node, param);
  const STEPS = 1000;
  return (
    <div className={`faust-slider ${vertical ? 'vertical' : ''}`}>
      <Learnable track={track} node={node} param={param} />
      <input
        type="range"
        min={0}
        max={STEPS}
        value={Math.round(toPosition(param, edit.value) * STEPS)}
        onPointerDown={edit.begin}
        onPointerUp={edit.end}
        onChange={(e) => edit.drag(fromPosition(param, Number(e.target.value) / STEPS))}
        onDoubleClick={() => edit.set(param.default)}
        title="Drag to change, double-click to reset"
      />
      <ValueText
        className="faust-control-value"
        text={formatParam(param, edit.value)}
        title={param.name}
        parse={(typed) => parseParam(param, typed)}
        onSet={(value) => edit.set(value)}
      />
    </div>
  );
}

function Toggle({ track, node, param }: ParamProps) {
  const edit = useParam(track, node, param);
  const on = edit.value >= 0.5;
  return (
    <div className="faust-toggle">
      <OnOff on={on} title={`${param.name}: ${on ? 'on' : 'off'} (click to switch)`} onChange={(next) => edit.set(next ? 1 : 0)}>
        {on ? 'On' : 'Off'}
      </OnOff>
      <Learnable track={track} node={node} param={param} />
    </div>
  );
}

function MomentaryButton({ track, node, param }: ParamProps) {
  const edit = useParam(track, node, param);
  const held = useRef(false);
  const release = () => {
    if (!held.current) return;
    held.current = false;
    edit.drag(0);
    edit.end();
  };
  return (
    <div className="faust-toggle">
      <button
        className={`toggle text-button momentary ${edit.value >= 0.5 ? 'on' : ''}`}
        aria-pressed={edit.value >= 0.5}
        onPointerDown={(e) => {
          e.currentTarget.setPointerCapture(e.pointerId);
          held.current = true;
          edit.begin();
          edit.drag(1);
        }}
        onPointerUp={release}
        onPointerCancel={release}
      >
        {param.name}
      </button>
    </div>
  );
}

function Choice({ track, node, param, radio }: ParamProps & { radio: boolean }) {
  const edit = useParam(track, node, param);
  const index = Math.round(edit.value);
  return (
    <div className="faust-choice">
      <Learnable track={track} node={node} param={param} />
      {radio ? (
        <div className="segmented">
          {param.choices.map((label, i) => (
            <button key={label} className={i === index ? 'active' : ''} onClick={() => edit.set(i)}>
              {label}
            </button>
          ))}
        </div>
      ) : (
        <select value={index} onChange={(e) => edit.set(Number(e.target.value))}>
          {param.choices.map((label, i) => (
            <option key={label} value={i}>
              {label}
            </option>
          ))}
        </select>
      )}
    </div>
  );
}

function Bargraph({ item, context }: { item: FaustLayoutItem; context: Context }) {
  const value = useDawg((s) => s.telemetry.faustMeters?.[`${context.track.id}:${context.node.id}`]?.[item.meter ?? -1] ?? 0);
  const min = item.min ?? 0;
  const max = item.max ?? 1;
  const fraction = Math.min(1, Math.max(0, (value - min) / (max - min || 1)));
  const vertical = item.kind === 'vbargraph';
  return (
    <div className={`faust-bargraph ${vertical ? 'vertical' : ''}`} title={`${value.toFixed(2)}${item.unit ? ` ${item.unit}` : ''}`}>
      <span className="faust-control-label">{item.label}</span>
      <div className="faust-bargraph-track">
        <div className="faust-bargraph-fill" style={vertical ? { height: `${fraction * 100}%` } : { width: `${fraction * 100}%` }} />
      </div>
    </div>
  );
}
