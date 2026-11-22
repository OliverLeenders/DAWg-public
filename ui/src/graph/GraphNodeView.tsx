import { Handle, Position, useUpdateNodeInternals, type Node, type NodeProps } from '@xyflow/react';
import { DoorOpen, ExternalLink, Layers, RotateCcw, X } from 'lucide-react';
import { useEffect, useRef, useState, type ReactNode } from 'react';
import { call } from '../bridge/bridge';
import { NODE_TYPES, type Connection, type GraphNode, type NodeTypeInfo, type NodeWidths, type ParamDescriptor, type PortDescriptor, type Track } from '../bridge/protocol';
import { nodeName } from '../components/chain';
import { DragNumber } from '../components/DragNumber';
import { drumParamLabel } from '../dock/drumSynth';
import { ParamSwitch } from '../dock/Knob';
import { paramApplies } from './lfo';
import { filterSlopeSteps, stepFilterSlope } from './filterWheel';
import type { Side } from './layout';
import { exposedControls } from './nodeInfo';
import { Power } from '../ui/controls';
import { depthSpan, depthStep, formatDepth, formatParam } from './params';
import { actions, dispatch, newGestureId, useDawg } from '../store/store';
import type { GraphDirection } from '../store/layoutStore';
import { ui } from '../store/uiStore';
import { paramTaper } from './params';
import { FOLD_TITLE, isFolded, isMono, portChannels, portTitle, PORT_WIDTHS } from './widths';
import { BandSplitView } from './BandSplitView';

// One node on the graph canvas (docs/design/phase8-node-graph.md, G8): a header coloured by category with the
// name, bypass and (for plugins) the editor button; ports on the edges, coloured by type; latency and plugin
// status badges. Parameters are edited in the inspector. Each connected modulation input is one row with its
// target and depth; the first free one shows only its port (docs/design/modulation.md, M2).

export interface GraphNodeData extends Record<string, unknown> {
  trackId: number;
  node: GraphNode;
  info: NodeTypeInfo | undefined;
  connections: Connection[];
  direction: GraphDirection;
  /** A loop's Feedback node, drawn small on the cable going back (feedbackChips): the side its input is on. */
  chip?: Side;
  /** The Voice Instrument or component whose graph is shown, if one is. */
  owner?: number;
  /** The one holding that one, if one does. */
  ownerParent?: number;
}

/** A port of a component's Input or Output: double-click its name to rename it. */
function EditablePort({ port, index, trackId, owner, side }: { port: PortDescriptor; index: number; trackId: number; owner: number; side: 'input' | 'output' }) {
  const [editing, setEditing] = useState(false);

  if (editing) {
    return (
      <input
        className="graph-port-name nodrag"
        defaultValue={port.name}
        autoFocus
        aria-label="Port name"
        onBlur={(e) => {
          setEditing(false);
          if (e.target.value !== port.name) dispatch({ command: 'renameComponentPort', args: { trackId, nodeId: owner, side, index, name: e.target.value } });
        }}
        onKeyDown={(e) => {
          e.stopPropagation();
          if (e.key === 'Enter') e.currentTarget.blur();
          if (e.key === 'Escape') setEditing(false);
        }}
      />
    );
  }

  // An audio port's width: stereo, mono or adaptive (docs/design/mono-ports.md, MP11).
  const width = PORT_WIDTHS.findIndex((w) => w.channels === portChannels(port));
  const current = PORT_WIDTHS[Math.max(0, width)];
  const next = PORT_WIDTHS[(Math.max(0, width) + 1) % PORT_WIDTHS.length];

  return (
    <>
      <span title="Double-click to rename" onDoubleClick={(e) => { e.stopPropagation(); setEditing(true); }}>
        {port.name}
      </span>
      {port.type === 'audio' && (
        <button
          className={`graph-port-width nodrag ${current.channels === 2 ? '' : 'set'}`}
          title={current.title}
          aria-label={`Port ${port.name} width: ${current.title.split(':')[0]}`}
          onClick={() => dispatch({ command: 'setComponentPortChannels', args: { trackId, nodeId: owner, side, index, channels: next.channels } })}
        >
          {current.label}
        </button>
      )}
      <button
        className="graph-port-remove nodrag"
        title="Remove this port and its cables"
        aria-label={`Remove port ${port.name}`}
        onClick={() => dispatch({ command: 'removeComponentPort', args: { trackId, nodeId: owner, side, index } })}
      >
        <X size={9} />
      </button>
    </>
  );
}

export type GraphFlowNode = Node<GraphNodeData, 'dawg'>;

/** Handle ids: "in-<port>" and "out-<port>". */
export const inputHandle = (port: number) => `in-${port}`;
export const outputHandle = (port: number) => `out-${port}`;
export const portOf = (handle: string | null | undefined) => Number(handle?.split('-')[1] ?? 0);

function Ports({
  ports,
  side,
  direction,
  modulationTarget,
  editable,
  widths,
}: {
  ports: Array<{ port: PortDescriptor; index: number }>;
  side: 'in' | 'out';
  direction: GraphDirection;
  modulationTarget?: (index: number) => ReactNode;
  editable?: { trackId: number; owner: number; side: 'input' | 'output' };
  widths?: NodeWidths;
}) {
  const position =
    direction === 'vertical'
      ? side === 'in'
        ? Position.Top
        : Position.Bottom
      : side === 'in'
        ? Position.Left
        : Position.Right;
  return (
    <div className={`graph-ports ${side}`}>
      {ports.filter(({ port }) => !port.hidden).map(({ port, index }) => {
        // A mono port is a ring, and an input where stereo is folded to mono has a marker (mono-ports.md, MP8, MP9).
        const mono = isMono(port, side, index, widths);
        return (
          <div className={`graph-port ${port.type}`} key={index}>
            <Handle
              type={side === 'in' ? 'target' : 'source'}
              position={position}
              id={side === 'in' ? inputHandle(index) : outputHandle(index)}
              className={`graph-handle ${port.type} ${mono ? 'mono' : ''} ${port.tone ? `tone-${port.tone}` : ''}`}
              title={portTitle(port, mono)}
              aria-label={`${side === 'in' ? 'Input' : 'Output'} ${portTitle(port, mono)}`}
            />
            {side === 'in' && isFolded(index, widths) && <span className="graph-fold" title={FOLD_TITLE} aria-label={FOLD_TITLE} />}
            {editable && port.type !== 'any' ? <EditablePort port={port} index={index} {...editable} /> : <span>{port.name}</span>}
            {side === 'in' && port.type === 'modulation' && modulationTarget?.(index)}
          </div>
        );
      })}
    </div>
  );
}

function usePluginParameters(trackId: number, nodeId: number, enabled: boolean) {
  const [params, setParams] = useState<Array<{ id: string; name: string }>>([]);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [retry, setRetry] = useState(0);

  useEffect(() => {
    if (!enabled) {
      setParams([]);
      setError(null);
      setLoading(false);
      return;
    }

    let cancelled = false;
    setLoading(true);

    call('plugin.parameters', { trackId, nodeId })
      .then((list) => {
        if (!cancelled) {
          setParams(list);
          setError(null);
          setLoading(false);
        }
      })
      .catch((cause: unknown) => {
        if (!cancelled) {
          setParams([]);
          setError(cause instanceof Error ? cause.message : 'Could not read plugin parameters');
          setLoading(false);
        }
      });

    return () => {
      cancelled = true;
    };
  }, [trackId, nodeId, enabled, retry]);

  return { params, loading, error, retry: () => setRetry((value) => value + 1) };
}

/** A parameter shown on a component's node (docs/design/component-nodes.md, N6): the inner parameter itself. */
function ExposedControl({ trackId, node, param, name }: { trackId: number; node: GraphNode; param: ParamDescriptor; name: string }) {
  const value = node.params[param.id] ?? param.default;
  const set = (next: number) => dispatch({ command: 'setNodeParam', args: { trackId, nodeId: node.id, param: param.id, value: next } });

  // A choice is picked from its names and a toggle switched, not dragged as a number (docs/ui-review-2026-11-10.md, 1.2).
  if (param.kind === 'choice' || param.kind === 'toggle') {
    return (
      <div className="graph-exposed">
        <span title={name}>{name}</span>
        {param.kind === 'choice' ? (
          <select className="graph-exposed-choice nodrag" aria-label={name} value={Math.round(value)} onChange={(e) => set(Number(e.target.value))}>
            {param.choices.map((choice, i) => (
              <option key={choice} value={i}>
                {choice}
              </option>
            ))}
          </select>
        ) : (
          <ParamSwitch className="nodrag" name={name} on={value >= 0.5} onChange={(on) => set(on ? 1 : 0)} />
        )}
      </div>
    );
  }

  const whole = param.kind !== 'continuous';
  return (
    <div className="graph-exposed">
      <span title={name}>{name}</span>
      <DragNumber
        className="graph-mod-depth nodrag"
        value={node.params[param.id] ?? param.default}
        min={param.min}
        max={param.max}
        integer={whole}
        taper={paramTaper(param)}
        format={(v) => formatParam(param, v)}
        onChange={(value, gesture) => dispatch({ command: 'setNodeParam', args: { trackId, nodeId: node.id, param: param.id, value } }, gesture)}
      />
    </div>
  );
}

/** A slot's depth: drag, scroll or click to type, like other number fields. It shows how far a full-scale source
    moves a built-in parameter, in the parameter's units (+100% on a gain's amplitude, ±2.4 st on a pitch); a plugin
    parameter's, which are normalised, as a share of its range. */
function DepthControl({
  trackId,
  nodeId,
  port,
  param,
  target,
  depth,
}: {
  trackId: number;
  nodeId: number;
  port: number;
  param: string;
  target?: ParamDescriptor;
  depth: number;
}) {
  const span = target ? depthSpan(target) : 100;
  const step = target ? depthStep(target) : 1;
  const set = (value: number, gesture?: string) =>
    dispatch({ command: 'setModulation', args: { trackId, nodeId, port, param, depth: value / span } }, gesture);

  return (
    <DragNumber
      className="graph-mod-depth nodrag"
      value={Math.round((depth * span) / step) * step}
      min={-span}
      max={span}
      quantum={step}
      format={(v) => (target ? formatDepth(target, v / span) : `${Math.round(v)}%`)}
      title="Depth: how far a full-scale source moves the parameter"
      onChange={(value, gesture) => set(value, gesture)}
    />
  );
}

/** The length a Feedback node shows on its chip. */
export function loopLength(node: GraphNode, info: NodeTypeInfo | undefined): string {
  const value = (id: string) => node.params[id] ?? info?.params.find((p) => p.id === id)?.default ?? 0;
  const mode = Math.round(value('mode'));
  if (mode === 1) return info?.params.find((p) => p.id === 'note')?.choices?.[Math.round(value('note'))] ?? '';
  if (mode === 2) return 'Note';
  const ms = value('time');
  return ms < 10 ? `${ms.toFixed(1)} ms` : `${Math.round(ms)} ms`;
}

const SIDES: Record<Side, Position> = { left: Position.Left, right: Position.Right, top: Position.Top, bottom: Position.Bottom };
const OPPOSITE: Record<Side, Side> = { left: 'right', right: 'left', top: 'bottom', bottom: 'top' };

/** A loop's Feedback node as a chip: its input faces the node feeding it, which on the way back is usually at its far end. A cable to its
    free port makes it a full node, as that needs room for its target and depth. */
function FeedbackChip({ data, facing, selected }: { data: GraphNodeData; facing: Side; selected: boolean | undefined }) {
  const { trackId, node, info } = data;
  const limiting = useDawg((s) => !!s.telemetry.loops?.[`${trackId}:${node.id}`]?.limiting);
  const mono = useDawg((s) => s.widths[`${trackId}:${node.id}`]?.outputs[0] === 1);
  const across = facing === 'left' || facing === 'right' ? Position.Top : Position.Left;
  const updateInternals = useUpdateNodeInternals();
  useEffect(() => updateInternals(String(node.id)), [facing, node.id, updateInternals]);
  const amount = node.params.amount ?? info?.params.find((p) => p.id === 'amount')?.default ?? 0;

  return (
    <div
      className={`graph-chip ${node.bypassed ? 'bypassed' : ''} ${selected ? 'selected' : ''}`}
      title={`Feedback: ${Math.round(amount)} % goes round every ${loopLength(node, info)}. Click for its panel.`}
    >
      <Handle type="target" position={SIDES[facing]} id={inputHandle(0)} className={`graph-handle audio ${mono ? 'mono' : ''}`} aria-label="Input Audio" />
      <Handle type="source" position={SIDES[OPPOSITE[facing]]} id={outputHandle(0)} className={`graph-handle audio ${mono ? 'mono' : ''}`} aria-label="Output Audio" />
      <Handle
        type="target"
        position={across}
        id={inputHandle(1)}
        className="graph-handle modulation"
        aria-label="Input Mod"
      />
      <RotateCcw size={11} />
      <span>{loopLength(node, info)}</span>
      {limiting && <span className="graph-node-limit" title="This loop is at its limit" />}
    </div>
  );
}

/** An Output to track node's track: one of the track's output tracks, or a new one (docs/design/multi-output-instruments.md, MO3). */
function OutputTarget({ trackId, node }: { trackId: number; node: GraphNode }) {
  const tracks = useDawg((s) => s.project?.tracks);
  const order = useDawg((s) => s.project?.trackOrder);
  const outputs = (order ?? []).map((id) => tracks?.[String(id)]).filter((t): t is Track => t?.kind === 'output' && t.source === trackId);
  const name = tracks?.[String(trackId)]?.name ?? 'Track';
  return (
    <select
      className="graph-node-target nodrag"
      value={node.target ?? 0}
      title="The output track this feeds"
      onChange={(e) =>
        e.target.value === 'new'
          ? dispatch({ command: 'setNodeTarget', args: { trackId, nodeId: node.id, newTrack: `${name} ${outputs.length + 2}` } })
          : dispatch({ command: 'setNodeTarget', args: { trackId, nodeId: node.id, target: Number(e.target.value) } })
      }
    >
      <option value={0}>No track</option>
      {outputs.map((t) => (
        <option key={t.id} value={t.id}>
          {t.name}
        </option>
      ))}
      <option value="new">New output track</option>
    </select>
  );
}

export function GraphNodeView(props: NodeProps<GraphFlowNode>) {
  return props.data.chip ? <FeedbackChip data={props.data} facing={props.data.chip} selected={props.selected} /> : <FullNode {...props} />;
}

function FullNode({ data, selected }: NodeProps<GraphFlowNode>) {
  const { trackId, node, info, connections, direction, owner, ownerParent } = data;
  const types = useDawg((s) => s.nodeTypes);
  const status = useDawg((s) => s.nodes[`${trackId}:${node.id}`]);
  const limiting = useDawg((s) => node.type === NODE_TYPES.feedback && !!s.telemetry.loops?.[`${trackId}:${node.id}`]?.limiting);
  const voices = useDawg((s) => (node.type === NODE_TYPES.voiceInstrument ? (s.telemetry.voices?.[`${trackId}:${node.id}`] ?? 0) : 0));
  const category = info?.category ?? 'effect';
  const fixed = info ? !info.userEditable : false;
  const pluginParameterState = usePluginParameters(trackId, node.id, !!node.plugin && status?.status === 'ready');
  const wheel = useRef(0);
  const slope = info?.params.find((param) => param.id === 'slope');
  const statuses = useDawg((s) => s.nodes);
  // Widths are known for the track's graph and inside its components; a voice graph's ports show as declared.
  const widths = useDawg((s) => s.widths[`${trackId}:${node.id}`]);
  const track = useDawg((s) => (node.component ? (s.project?.master?.id === trackId ? s.project.master : s.project?.tracks[String(trackId)]) : undefined));
  const exposed = track && node.component ? exposedControls(track, node.component, types, statuses) : [];

  /** A filter's slope steps through its choices with the wheel, one notch per step, while the pointer is over the
      node (docs/design/phase8-node-graph.md, G8). The `nowheel` class keeps the canvas from zooming. */
  const onWheel = (event: React.WheelEvent<HTMLDivElement>) => {
    if (node.type !== NODE_TYPES.filter || !slope) return;
    const target = event.target;
    if (target instanceof Element && target.closest('button, input, select, textarea, .nodrag')) return;
    if (event.ctrlKey || event.metaKey) return;

    const stepped = filterSlopeSteps(wheel.current, event.deltaY);
    wheel.current = stepped.accumulated;
    if (stepped.steps === 0) return;

    const current = node.params.slope ?? slope.default;
    const next = stepFilterSlope(current, stepped.steps, slope.min, slope.max);
    if (next === Math.round(current)) return;

    dispatch(
      { command: 'setNodeParam', args: { trackId, nodeId: node.id, param: 'slope', value: next } },
      newGestureId(),
    );
  };

  const inputs = (info?.inputs ?? []).map((port, index) => ({ port, index }));
  const outputs = (info?.outputs ?? []).map((port, index) => ({ port, index }));
  const modulationPorts = inputs.filter(({ port }) => port.type === 'modulation');
  const used = new Set<number>();

  for (const connection of connections) {
    if (connection.toNode === node.id) used.add(connection.toPort);
  }
  // A macro slot is fed by its knob, without a cable (macros-and-modulation-column.md, MC7).
  for (const [port, slot] of Object.entries(node.modulations ?? {})) if ((slot.macro ?? -1) >= 0) used.add(Number(port));

  const visibleModulationPorts = modulationPorts.filter(({ index }) => used.has(index));
  const firstFree = modulationPorts.find(({ index }) => !used.has(index));
  if (firstFree) visibleModulationPorts.push(firstFree);

  // A component and its Output show every port they have: there, a modulation port isn't a slot that waits for a source.
  const ownPorts = node.component !== undefined || node.type === NODE_TYPES.componentOutput;
  const visibleInputs = ownPorts
    ? inputs
    : [...inputs.filter(({ port }) => port.type !== 'modulation'), ...visibleModulationPorts.sort((a, b) => a.index - b.index)];
  const targets = node.plugin
    ? pluginParameterState.params
    : (info?.params ?? [])
        .filter((param) => param.modulatable && paramApplies(node, param.id))
        .map((param) => ({ id: param.id, name: node.type === NODE_TYPES.drumSynth ? drumParamLabel(node, param) : param.name }));
  const modulationTarget = (index: number) => {
    // A free slot shows only its port: there is nothing to target until a source is connected.
    if (!used.has(index)) return null;
    const slot = node.modulations?.[String(index)];
    const value = slot?.param ?? '';
    const macro = slot?.macro ?? -1;
    return (
      <span className="graph-mod-slot">
      {macro >= 0 && (
        <span className="graph-mod-macro" title={`Fed by macro ${macro + 1}`}>
          M{macro + 1}
        </span>
      )}
      <select
        className="graph-mod-target nodrag"
        aria-label={`Modulation target for slot ${index + 1}`}
        value={value}
        onChange={(event) =>
          dispatch({
            command: 'setModulation',
            args: { trackId, nodeId: node.id, port: index, param: event.target.value, depth: slot?.depth ?? 0 },
          })
        }
      >
        <option value="">Choose target</option>
        {node.plugin && (pluginParameterState.loading || pluginParameterState.error) && (
          <option value="" disabled>
            {pluginParameterState.loading ? 'Loading parameters...' : 'Parameters unavailable'}
          </option>
        )}
        {targets.map((target) => (
          <option key={target.id} value={target.id}>
            {target.name}
          </option>
        ))}
        {value && !targets.some((target) => target.id === value) && <option value={value}>{value}</option>}
      </select>
      {value && (
        <DepthControl
          trackId={trackId}
          nodeId={node.id}
          port={index}
          param={value}
          target={node.plugin ? undefined : info?.params.find((param) => param.id === value)}
          depth={slot?.depth ?? 0}
        />
      )}
      </span>
    );
  };

  return (
    <div
      className={`graph-node ${category} ${node.bypassed ? 'bypassed' : ''} ${selected ? 'selected' : ''} ${
        node.type === NODE_TYPES.filter ? 'nowheel' : ''
      }`}
      onWheel={onWheel}
      onMouseLeave={() => {
        wheel.current = 0;
      }}
    >
      <div className="graph-node-header">
        <span className="graph-node-name" title={node.plugin ? `${node.plugin.name} (${node.plugin.manufacturer})` : undefined}>
          {nodeName(node, types)}
        </span>
        {limiting && <span className="graph-node-limit" title="This loop is at its limit" />}
        {node.type === NODE_TYPES.voiceInstrument && (
          <>
            <span className="graph-node-voices" title="Sounding voices">
              {voices}
            </span>
            <button
              className="graph-node-button nodrag"
              title="Edit the voice graph (double-click)"
              onClick={() => ui.enterGraph(trackId, node.id)}
            >
              <Layers size={11} />
            </button>
          </>
        )}
        {node.plugin && status?.status === 'ready' && (
          <button
            className="graph-node-button nodrag"
            title="Open the plugin's editor"
            onClick={() => actions.openEditor(trackId, node.id)}
          >
            <ExternalLink size={11} />
          </button>
        )}
        {node.component && (
          <button className="graph-node-button nodrag" title="Open this component (double-click)" onClick={() => ui.enterGraph(trackId, node.id)}>
            <Layers size={11} />
          </button>
        )}
        {(node.type === NODE_TYPES.voiceOutput || node.type === NODE_TYPES.componentOutput) && (
          <button className="graph-node-button graph-node-exit nodrag" title="Back up a level" onClick={() => ui.leaveGraph(ownerParent)}>
            <DoorOpen size={11} />
          </button>
        )}
        {node.type === NODE_TYPES.trackOutput && (
          <button className="graph-node-button graph-node-exit nodrag" title="Back to the arrangement" onClick={() => ui.setCentre('arrangement')}>
            <DoorOpen size={11} />
          </button>
        )}
        {!fixed && (
          <Power
            size="s"
            className="nodrag"
            on={!node.bypassed}
            onChange={(on) => dispatch({ command: 'setNodeBypass', args: { trackId, nodeId: node.id, bypassed: !on } })}
          />
        )}
      </div>
      <div className="graph-node-body">
        <Ports
          ports={visibleInputs}
          side="in"
          direction={direction}
          modulationTarget={ownPorts ? undefined : modulationTarget}
          editable={node.type === NODE_TYPES.componentOutput && owner !== undefined ? { trackId, owner, side: 'output' } : undefined}
          widths={widths}
        />
        <Ports
          ports={outputs}
          side="out"
          direction={direction}
          editable={node.type === NODE_TYPES.componentInput && owner !== undefined ? { trackId, owner, side: 'input' } : undefined}
          widths={widths}
        />
      </div>
      {node.type === NODE_TYPES.busOutput && owner === undefined && <OutputTarget trackId={trackId} node={node} />}
      {node.type === NODE_TYPES.bandSplit && <BandSplitView trackId={trackId} node={node} info={info} />}
      {exposed.length > 0 && (
        <div className="graph-exposed-list">
          {exposed.map((control) => (
            <ExposedControl key={`${control.exposed.nodeId}:${control.exposed.param}`} trackId={trackId} node={control.node} param={control.param} name={control.exposed.name} />
          ))}
        </div>
      )}
      {node.plugin && pluginParameterState.error && !pluginParameterState.loading && modulationPorts.some(({ index }) => used.has(index)) && (
        <button
          className="graph-mod-retry nodrag"
          onClick={pluginParameterState.retry}
          title={pluginParameterState.error}
        >
          Retry plugin parameters
        </button>
      )}
      {(status?.status === 'loading' || status?.status === 'failed' || status?.status === 'compiling' || (status?.latency ?? 0) > 0) && (
        <div className="graph-node-badges">
          {status.status === 'loading' && <span className="badge">Loading</span>}
          {status.status === 'compiling' && <span className="badge">Compiling</span>}
          {status.status === 'failed' && (
            <span className="badge failed" title={status.error}>
              Failed
            </span>
          )}
          {!node.bypassed && (status.latency ?? 0) > 0 && <span className="badge">{status.latency} smp</span>}
        </div>
      )}
    </div>
  );
}
