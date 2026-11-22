import { ExternalLink, Pin } from 'lucide-react';
import { useRef } from 'react';
import { NODE_TYPES, type GraphNode, type ParamDescriptor, type Track } from '../bridge/protocol';
import { paramApplies } from '../graph/lfo';
import { exposedControls, infoOf } from '../graph/nodeInfo';
import { BAND_NAMES, bandCount, isLeftRight } from '../graph/splits';
import { PLUGIN_CHANNELS, pluginChannels } from '../graph/widths';
import { componentHolding, ownerOf } from '../graph/voice';
import { formatParam, fromPosition, parseParam, toPosition } from '../graph/params';
import { paramHint, ValueText } from './ValueText';
import { actions, dispatch, newGestureId, useDawg } from '../store/store';
import { nodeName } from './chain';
import { laneFor, playedPosition } from '../automation/automation';
import { nodeTarget, touch } from '../automation/touch';
import { MidiLearnable } from './MidiLearn';
import { ParamSwitch } from '../dock/Knob';
import { ui } from '../store/uiStore';
import { convertToVoiceGraph } from './voiceGraph';
import { Button, IconButton, Power } from '../ui/controls';

// The selected graph node's parameters in the inspector (docs/design/phase8-node-graph.md, section 5).

const STEPS = 1000;

/** Parameters edited only in a node's shape editor in the device dock: the LFO's zero position and the ADSR's curve
    handles. */
const DOCK_ONLY: Record<string, readonly string[]> = {
  [NODE_TYPES.lfo]: ['zero'],
  [NODE_TYPES.adsr]: [
    'attackHandleTime',
    'attackHandleLevel',
    'decayHandleTime',
    'decayHandleLevel',
    'releaseHandleTime',
    'releaseHandleLevel',
  ],
};

/** Shows a parameter on the component holding its node, or takes it off (docs/design/component-nodes.md, N6). */
function ExposePin({ track, node, param, holder }: { track: Track; node: GraphNode; param: ParamDescriptor; holder: GraphNode }) {
  const shown = holder.component?.exposed.some((e) => e.nodeId === node.id && e.param === param.id) ?? false;
  const args = { trackId: track.id, nodeId: holder.id, target: node.id, param: param.id };

  return (
    <IconButton
      className={`param-pin ${shown ? 'active' : ''}`}
      title={shown ? 'Take it off the component' : 'Show on the component'}
      aria-label={`${shown ? 'Take off' : 'Show on'} the component: ${param.name}`}
      onClick={(e) => {
        e.preventDefault();
        dispatch(shown ? { command: 'unexposeParam', args } : { command: 'exposeParam', args });
      }}
    >
      <Pin size={10} />
    </IconButton>
  );
}

function ParamControl({ track, node, param }: { track: Track; node: GraphNode; param: ParamDescriptor }) {
  // While automation plays, the control follows it (docs/design/phase10-automation.md, A4).
  const target = nodeTarget(node.id, param.id);
  const played = useDawg((s) =>
    track.automationMode === 0 ? undefined : playedPosition(s.telemetry.automation, laneFor(track, target)?.id),
  );
  const value = played !== undefined ? fromPosition(param, played) : (node.params[param.id] ?? param.default);
  const gesture = useRef(newGestureId());
  const set = (next: number, id = newGestureId()) =>
    dispatch({ command: 'setNodeParam', args: { trackId: track.id, nodeId: node.id, param: param.id, value: next } }, id);

  if (param.kind === 'choice') {
    return (
      <select className="wide" value={Math.round(value)} onChange={(e) => set(Number(e.target.value))}>
        {param.choices.map((label, i) => (
          <option key={label} value={i}>
            {label}
          </option>
        ))}
      </select>
    );
  }

  // The same switch as on a component's node, not a bare checkbox (docs/ui-review-2026-11-10, 1.4).
  if (param.kind === 'toggle') return <ParamSwitch name={param.name} on={value >= 0.5} onChange={(on) => set(on ? 1 : 0)} />;

  // An integer moves in whole steps along its own range, like the dock's knobs.
  if (param.kind === 'integer') {
    return (
      <div className="param-slider" title="Drag to change, double-click to reset">
        <input
          type="range"
          min={param.min}
          max={param.max}
          step={1}
          value={Math.round(value)}
          onPointerDown={() => {
            gesture.current = newGestureId();
            touch(track.id, target, true);
          }}
          onPointerUp={() => touch(track.id, target, false)}
          onChange={(e) => set(Number(e.target.value), gesture.current)}
          onDoubleClick={() => set(param.default)}
        />
        <ValueText className="param-value" text={formatParam(param, value)} title={param.name} parse={(t) => parseParam(param, t)} hint={paramHint(param, value)} onSet={(v, g) => set(v, g)} />
      </div>
    );
  }

  return (
    <div className="param-slider" title="Drag to change, double-click to reset">
      <input
        type="range"
        min={0}
        max={STEPS}
        value={Math.round(toPosition(param, value) * STEPS)}
        onPointerDown={() => {
          gesture.current = newGestureId();
          touch(track.id, target, true);
        }}
        onPointerUp={() => touch(track.id, target, false)}
        onChange={(e) => set(fromPosition(param, Number(e.target.value) / STEPS), gesture.current)}
        onDoubleClick={() => set(param.default)}
      />
      <ValueText className="param-value" text={formatParam(param, value)} title={param.name} parse={(t) => parseParam(param, t)} hint={paramHint(param, value)} onSet={(v, g) => set(v, g)} />
    </div>
  );
}

/** Split parameters the inspector shows its own way, not as plain fields. */
const SPLIT_OWN: Record<string, string[]> = { [NODE_TYPES.bandSplit]: ['solo', 'phase'] };

/** The graph scope a nested node's commands need: the component or voice graph that holds it. */
function scopeOf(track: Track, node: GraphNode): { voiceNodeId?: number } {
  const owner = ownerOf(track, node.id);
  return owner ? { voiceNodeId: owner.id } : {};
}

/** A Band Split's band count, Solo per band and Phase (docs/design/split-nodes.md, section 6). */
function BandSplitControls({ track, node }: { track: Track; node: GraphNode }) {
  const count = bandCount(node);
  const solo = Math.round(node.params.solo ?? 0);
  const linear = Math.round(node.params.phase ?? 0) === 1;
  const scope = scopeOf(track, node);
  const setParam = (param: string, value: number) =>
    dispatch({ command: 'setNodeParam', args: { trackId: track.id, ...scope, nodeId: node.id, param, value } });

  return (
    <>
      <div className="inspector-field">
        <span>Bands</span>
        <div className="segmented">
          {[2, 3, 4].map((bands) => (
            <button key={bands} className={bands === count ? 'active' : ''} onClick={() => dispatch({ command: 'setBandCount', args: { trackId: track.id, ...scope, nodeId: node.id, bands } })}>
              {bands}
            </button>
          ))}
        </div>
      </div>
      <label className="inspector-field" title="Only one band plays, to hear it while setting the crossovers">
        <span>Solo</span>
        <select className="wide" value={solo} onChange={(e) => setParam('solo', Number(e.target.value))}>
          {['Off', ...(BAND_NAMES[count] ?? [])].map((name, i) => (
            <option key={name} value={i}>
              {name}
            </option>
          ))}
        </select>
      </label>
      <div className="inspector-field">
        <span>Phase</span>
        <div className="segmented">
          <button className={linear ? '' : 'active'} onClick={() => setParam('phase', 0)}>
            Minimum
          </button>
          <button
            className={linear ? 'active' : ''}
            onClick={() => setParam('phase', 1)}
            title="No phase turn between the bands, at the cost of about 0.1 s of latency"
          >
            Linear
          </button>
        </div>
      </div>
    </>
  );
}

export function NodeInspector({ track, node }: { track: Track; node: GraphNode }) {
  const types = useDawg((s) => s.nodeTypes);
  const status = useDawg((s) => s.nodes[`${track.id}:${node.id}`]);
  const statuses = useDawg((s) => s.nodes);
  // A split's ports and parameters are named for its count or mode (docs/design/split-nodes.md, section 6).
  const info = status?.program ?? infoOf(track.id, node, types, statuses);
  const holder = componentHolding(track, node.id);
  // An LFO shows its rate or its cycle, whichever its sync mode uses. Shapes (an LFO's, an ADSR's curves) are edited
  // in the device dock.
  const visibleParams = info?.params.filter(
    (param) => paramApplies(node, param.id) && !DOCK_ONLY[node.type]?.includes(param.id) && !SPLIT_OWN[node.type]?.includes(param.id),
  );

  return (
    <div className="node-inspector">
      <div className="inspector-label node-inspector-title">
        <span>{nodeName(node, types)}</span>
        {node.plugin && status?.status === 'ready' && (
          <IconButton title="Open the plugin's editor" onClick={() => actions.openEditor(track.id, node.id)}>
            <ExternalLink size={13} />
          </IconButton>
        )}
        {info?.userEditable && (
          <Power
            on={!node.bypassed}
            onChange={(on) => dispatch({ command: 'setNodeBypass', args: { trackId: track.id, nodeId: node.id, bypassed: !on } })}
          />
        )}
      </div>
      {node.type === NODE_TYPES.bandSplit && <BandSplitControls track={track} node={node} />}
      {(node.type === NODE_TYPES.stereoSplit || node.type === NODE_TYPES.stereoMerge) && (
        <div className="inspector-field">
          <span>Mode</span>
          <div className="segmented">
            {(['midSide', 'leftRight'] as const).map((mode) => (
              <button
                key={mode}
                className={(mode === 'leftRight') === isLeftRight(node) ? 'active' : ''}
                onClick={() => dispatch({ command: 'setStereoMode', args: { trackId: track.id, ...scopeOf(track, node), nodeId: node.id, mode } })}
              >
                {mode === 'midSide' ? 'Mid/side' : 'Left/right'}
              </button>
            ))}
          </div>
        </div>
      )}
      {node.type === NODE_TYPES.pluginEffect && (
        <label className="inspector-field" title="The plugin's main channels (docs/design/mono-ports.md)">
          <span>Channels</span>
          <select
            value={pluginChannels(node)}
            onChange={(e) =>
              dispatch({
                command: 'setPluginChannels',
                args: { trackId: track.id, nodeId: node.id, channels: e.target.value as (typeof PLUGIN_CHANNELS)[number]['value'] },
              })
            }
          >
            {PLUGIN_CHANNELS.map((option) => (
              <option key={option.value} value={option.value} title={option.title}>
                {option.label}
              </option>
            ))}
          </select>
        </label>
      )}
      {visibleParams?.map((param) => (
        <label className="inspector-field" key={param.id}>
          <MidiLearnable target={{ trackId: track.id, nodeId: node.id, param: param.id }}>
            <span>{param.name}</span>
          </MidiLearnable>
          <ParamControl track={track} node={node} param={param} />
          {holder && <ExposePin track={track} node={node} param={param} holder={holder} />}
        </label>
      ))}
      {node.component &&
        exposedControls(track, node.component, types, statuses).map(({ exposed, node: inner, param }) => (
          <label className="inspector-field" key={`${exposed.nodeId}:${exposed.param}`}>
            <MidiLearnable target={{ trackId: track.id, nodeId: inner.id, param: param.id }}>
              <span>{exposed.name}</span>
            </MidiLearnable>
            <ParamControl track={track} node={inner} param={param} />
          </label>
        ))}
      {(node.type === NODE_TYPES.mpeSynth || node.type === NODE_TYPES.sampler) && track.graph.nodes[String(node.id)] && (
        <Button title="Rebuild this instrument as an editable voice graph" onClick={() => convertToVoiceGraph(track, node)}>
          Convert to voice graph
        </Button>
      )}
      {node.type === NODE_TYPES.voiceInstrument && (
        <Button onClick={() => ui.enterGraph(track.id, node.id)}>
          Edit voice graph
        </Button>
      )}
      {node.component && (
        <Button onClick={() => ui.enterGraph(track.id, node.id)}>
          Open component
        </Button>
      )}
      {info && info.params.length === 0 && node.type !== NODE_TYPES.voiceInstrument && !node.component && (
        <p className="empty">{node.plugin ? 'Plugin parameters are edited in its own editor.' : 'No parameters.'}</p>
      )}
    </div>
  );
}
