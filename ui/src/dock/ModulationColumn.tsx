import { GripVertical, Plus, Target, X } from 'lucide-react';
import { useEffect, useMemo, useState } from 'react';
import { NODE_TYPES, type GraphNode, type Track } from '../bridge/protocol';
import { DeviceMenu } from '../components/DeviceMenu';
import { DragNumber } from '../components/DragNumber';
import { isModulationSource, nodeName } from '../components/chain';
import { infoOf } from '../graph/nodeInfo';
import { depthSpan, depthStep, formatDepth } from '../graph/params';
import { ownerOf } from '../graph/voice';
import { dispatch, useDawg } from '../store/store';
import { MOD_COLUMN_WIDTH } from './deviceWidths';
import { Knob } from './Knob';
import { KeySelector } from './KeySelector';
import { groupByNodes, linksOfTrack } from './links';
import { LinksEditor } from './LinksEditor';
import { MACRO_COUNT, NEUTRALS, macroName, macroParam, macrosOf, macroTargets, neutralOf, neutralParam, type SlotLink } from './macros';
import { modulationUi, sourceDrag, useModulationUi } from './modulationStore';
import { panelFor } from './panels';
import { IconButton } from '../ui/controls';
import { Menu } from '../ui/overlays';

// The dock's modulation column (docs/design/macros-and-modulation-column.md, MC11 to MC13): the track's macro knobs, a chip
// per modulation source in its graph, and the editor of the one chosen. Sources are dragged from here onto any knob.

const ADDABLE = [
  { type: NODE_TYPES.lfo, label: 'LFO' },
  { type: NODE_TYPES.adsr, label: 'ADSR' },
  { type: NODE_TYPES.noteExpression, label: 'Note Expression' },
];

/** A source's chip label: "LFO 2", "ADSR", "Expr", "Macros". */
function chipLabel(node: GraphNode, all: GraphNode[]) {
  const same = all.filter((n) => n.type === node.type);
  const base = node.type === NODE_TYPES.lfo ? 'LFO' : node.type === NODE_TYPES.adsr ? 'ADSR' : node.type === NODE_TYPES.noteExpression ? 'Expr' : 'Macros';
  return node.type === NODE_TYPES.lfo || same.length > 1 ? `${base} ${same.indexOf(node) + 1}` : base;
}

/** Adds a source to the track's graph, left of its nodes where the graph view keeps sources. */
function addSource(track: Track, type: string) {
  const nodes = Object.values(track.graph.nodes);
  const x = Math.min(0, ...nodes.map((n) => n.x)) - 240;
  const y = Math.min(0, ...nodes.map((n) => n.y)) + nodes.filter(isModulationSource).length * 140;
  dispatch({ command: 'addNode', args: { trackId: track.id, type, x, y } });
}

/** What the column's open source is while the Links chip is open (docs/design/parameter-linking.md, L16). */
const LINKS_OPEN = -1;

export function ModulationColumn({ track }: { track: Track }) {
  const types = useDawg((s) => s.nodeTypes);
  const sources = useMemo(
    () =>
      Object.values(track.graph.nodes)
        .filter((n) => isModulationSource(n))
        .sort((a, b) => (a.type === NODE_TYPES.macros ? -1 : b.type === NODE_TYPES.macros ? 1 : a.id - b.id)),
    [track],
  );
  const macros = macrosOf(track);
  const openId = useModulationUi((s) => s.open[track.id]);
  const open = sources.find((n) => n.id === openId);
  // How many pairs of devices the track's links join: a mirrored EQ counts once.
  const linkCount = useDawg((s) => groupByNodes(linksOfTrack(s.project, track.id)).length);
  const linksOpen = openId === LINKS_OPEN && linkCount > 0;
  const mapping = useModulationUi((s) => (s.mapping?.trackId === track.id ? s.mapping.knob : null));
  const [adding, setAdding] = useState<{ x: number; y: number } | null>(null);
  const [menu, setMenu] = useState<{ node: GraphNode; x: number; y: number } | null>(null);

  // Escape ends Map mode.
  useEffect(() => {
    if (mapping === null) return;
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') modulationUi.stopMapping();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [mapping]);

  return (
    <aside className="mod-column" style={{ width: MOD_COLUMN_WIDTH }} aria-label="Modulation">
      {macros ? (
        <MacroKnobs track={track} macros={macros} mapping={mapping} />
      ) : (
        <button className="mod-add-macros" onClick={() => addSource(track, NODE_TYPES.macros)} title="Eight knobs that each drive any parameters of this track">
          <Plus size={12} /> Add macros
        </button>
      )}
      <div className="mod-chips" onPointerLeave={() => modulationUi.hover(null)}>
        {sources.map((node) => (
          <button
            key={node.id}
            className={`mod-chip ${open?.id === node.id ? 'active' : ''} ${node.bypassed ? 'bypassed' : ''}`}
            title={`${nodeName(node, types)}: click to edit, drag onto a knob to modulate it, right-click for more`}
            draggable={node.type !== NODE_TYPES.macros}
            onDragStart={(event) => sourceDrag.start(event, { trackId: track.id, nodeId: node.id, port: 0 })}
            onDragEnd={sourceDrag.end}
            onPointerEnter={() => modulationUi.hover({ trackId: track.id, nodeId: node.id })}
            onClick={() => modulationUi.openSource(track.id, open?.id === node.id ? null : node.id)}
            onContextMenu={(event) => {
              event.preventDefault();
              setMenu({ node, x: event.clientX, y: event.clientY });
            }}
          >
            {chipLabel(node, sources)}
          </button>
        ))}
        {linkCount > 0 && (
          <button
            className={`mod-chip links ${linksOpen ? 'active' : ''}`}
            title="Parameter links with this track's devices: click to see and edit them"
            onClick={() => modulationUi.openSource(track.id, linksOpen ? null : LINKS_OPEN)}
          >
            Links {linkCount}
          </button>
        )}
        <button
          className="mod-chip add"
          title="Add a modulation source"
          onClick={(event) => {
            const rect = event.currentTarget.getBoundingClientRect();
            setAdding({ x: rect.left, y: rect.bottom });
          }}
        >
          <Plus size={11} />
          {sources.length === 0 && ' Source'}
        </button>
      </div>
      {open && (
        <div className="mod-editor" key={open.id}>
          <SourceEditor track={track} node={open} />
        </div>
      )}
      {linksOpen && (
        <div className="mod-editor">
          <LinksEditor track={track} />
        </div>
      )}
      {!open && !linksOpen && sources.length > 0 && <p className="mod-hint">Click a source to edit it. Drag it onto a knob to modulate that.</p>}
      {adding && (
        <>
          <Menu onClose={() => setAdding(null)} at={{ x: adding.x, y: adding.y }}>
            {ADDABLE.map(({ type, label }) => (
              <button
                key={type}
                className="menu-item"
                onClick={() => {
                  setAdding(null);
                  addSource(track, type);
                }}
              >
                {label}
              </button>
            ))}
            {!macros && (
              <button
                className="menu-item"
                onClick={() => {
                  setAdding(null);
                  addSource(track, NODE_TYPES.macros);
                }}
              >
                Macros
              </button>
            )}
          </Menu>
        </>
      )}
      {menu && <DeviceMenu track={track} node={menu.node} x={menu.x} y={menu.y} onClose={() => setMenu(null)} />}
    </aside>
  );
}

/** The eight macro knobs (MC5): each with its Map switch, and a grip that drags it onto a knob to map it there. */
export function MacroKnobs({ track, macros, mapping }: { track: Track; macros: GraphNode; mapping?: number | null }) {
  const info = useDawg((s) => s.nodeTypes[NODE_TYPES.macros]);
  if (!info) return null;
  return (
    <div className="mod-macros">
      {Array.from({ length: MACRO_COUNT }, (_, knob) => {
        const param = info.params.find((p) => p.id === macroParam(knob));
        if (!param) return null;
        return (
          <div
            key={knob}
            className={`mod-macro ${mapping === knob ? 'mapping' : ''}`}
            onPointerEnter={() => modulationUi.hover({ trackId: track.id, nodeId: macros.id, knob })}
            onPointerLeave={() => modulationUi.hover(null)}
          >
            <Knob track={track} node={macros} param={param} label={macroName(macros, knob)} />
            {mapping !== undefined && (
              <div className="mod-macro-tools">
                <span
                  className="mod-grip"
                  draggable
                  title="Drag onto a knob to map it to this macro"
                  onDragStart={(event) => sourceDrag.start(event, { trackId: track.id, nodeId: macros.id, knob, port: knob })}
                  onDragEnd={sourceDrag.end}
                >
                  <GripVertical size={10} />
                </span>
                <span className="mod-macro-tool-group">
                  <IconButton
                    size="s"
                    className={`tiny ${mapping === knob ? 'active' : ''}`}
                    title={mapping === knob ? 'Map mode: move parameters to map them; click or Escape to end' : 'Map: then move parameters to map them to this macro'}
                    onClick={() => modulationUi.toggleMapping(track.id, knob)}
                  >
                    <Target size={10} />
                  </IconButton>
                  <NeutralButton track={track} macros={macros} knob={knob} />
                </span>
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}

/** A macro's neutral position (where it leaves its targets at their own values) as one small button that shows it on a
    knob's arc and steps Bottom, Centre, Top (docs/ui-review-2026-11-10.md, 1.11). */
function NeutralButton({ track, macros, knob }: { track: Track; macros: GraphNode; knob: number }) {
  const neutral = neutralOf(macros, knob);
  // The arc from 225° to 135° clockwise (as on a knob), and the tick at its start, top or end.
  const angle = [-135, 0, 135][neutral] ?? -135;
  const rad = ((angle - 90) * Math.PI) / 180;
  return (
    <IconButton
      size="s"
      className="tiny mod-neutral"
      title={`Neutral position: ${NEUTRALS[neutral]} (click for ${NEUTRALS[(neutral + 1) % NEUTRALS.length]}). The macro leaves its targets at their own values there.`}
      aria-label={`Macro ${knob + 1} neutral position: ${NEUTRALS[neutral]}`}
      onClick={() =>
        dispatch({
          command: 'setNodeParam',
          args: { trackId: track.id, nodeId: macros.id, param: neutralParam(knob), value: (neutral + 1) % NEUTRALS.length },
        })
      }
    >
      <svg width={10} height={10} viewBox="0 0 10 10" aria-hidden>
        <path d="M 2.17 7.83 A 4 4 0 1 1 7.83 7.83" fill="none" stroke="currentColor" strokeWidth={1} opacity={0.5} />
        <line x1={5} y1={5} x2={5 + 4 * Math.cos(rad)} y2={5 + 4 * Math.sin(rad)} stroke="currentColor" strokeWidth={1.6} strokeLinecap="round" />
      </svg>
    </IconButton>
  );
}

/** The editor of one source in the column. */
function SourceEditor({ track, node }: { track: Track; node: GraphNode }) {
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const info = infoOf(track.id, node, types, statuses);
  if (node.type === NODE_TYPES.macros) return <MacrosEditor track={track} macros={node} />;
  if (node.type === NODE_TYPES.noteExpression) return <ExpressionEditor track={track} node={node} />;
  const Panel = panelFor(node.type);
  if (!Panel || !info) return null;
  return (
    <>
      <div className="mod-editor-header">
        <span>{nodeName(node, types)}</span>
        <KeySelector track={track} node={node} label="Trigger" />
      </div>
      <Panel track={track} node={node} info={info} />
    </>
  );
}

/** Note Expression's five outputs, each dragged onto a knob on its own. */
function ExpressionEditor({ track, node }: { track: Track; node: GraphNode }) {
  const outputs = useDawg((s) => s.nodeTypes[node.type]?.outputs ?? []);
  return (
    <div className="mod-expression">
      <p className="mod-hint">The newest held note's expression. Drag an output onto a knob.</p>
      <div className="mod-chips">
        {outputs.map((output, port) => (
          <button
            key={port}
            className="mod-chip"
            draggable
            onDragStart={(event) => sourceDrag.start(event, { trackId: track.id, nodeId: node.id, knob: port, port })}
            onDragEnd={sourceDrag.end}
            onPointerEnter={() => modulationUi.hover({ trackId: track.id, nodeId: node.id, knob: port })}
            onPointerLeave={() => modulationUi.hover(null)}
          >
            {output.name}
          </button>
        ))}
      </div>
    </div>
  );
}

/** The macros' names, neutral positions and MIDI controllers, and each one's targets with their depths (MC10, MC11). */
function MacrosEditor({ track, macros }: { track: Track; macros: GraphNode }) {
  const mappings = useDawg((s) => s.project?.midiMappings ?? []);
  return (
    <div className="mod-macros-editor">
      {Array.from({ length: MACRO_COUNT }, (_, knob) => {
        const cc = mappings.find((m) => m.trackId === track.id && m.nodeId === macros.id && m.param === macroParam(knob));
        const targets = macroTargets(track, knob);
        return (
          <section key={knob} className="mod-macro-row">
            <div className="mod-macro-fields">
              <span className="mod-macro-number">{knob + 1}</span>
              <input
                className="mod-macro-name"
                defaultValue={macros.macroNames?.[knob] ?? ''}
                placeholder={macroName(undefined, knob)}
                aria-label={`Name of macro ${knob + 1}`}
                onBlur={(event) => {
                  const name = event.currentTarget.value.trim();
                  if (name !== (macros.macroNames?.[knob] ?? ''))
                    dispatch({ command: 'renameMacro', args: { trackId: track.id, nodeId: macros.id, index: knob, name } });
                }}
                onKeyDown={(event) => event.key === 'Enter' && event.currentTarget.blur()}
              />
              <NeutralButton track={track} macros={macros} knob={knob} />
              <CcField track={track} macros={macros} knob={knob} cc={cc?.controller} />
            </div>
            {targets.length > 0 && (
              <ul className="mod-targets">
                {targets.map((link) => (
                  <TargetRow key={`${link.node.id}:${link.port}`} track={track} link={link} />
                ))}
              </ul>
            )}
          </section>
        );
      })}
    </div>
  );
}

/** A MIDI controller number for a macro (MC10): writes a mapping for any device on channel 1; empty forgets it. */
function CcField({ track, macros, knob, cc }: { track: Track; macros: GraphNode; knob: number; cc?: number }) {
  const [text, setText] = useState(cc === undefined ? '' : String(cc));
  useEffect(() => setText(cc === undefined ? '' : String(cc)), [cc]);
  const commit = () => {
    const trimmed = text.trim();
    const param = macroParam(knob);
    if (trimmed === '') {
      if (cc !== undefined) dispatch({ command: 'removeMidiMapping', args: { trackId: track.id, nodeId: macros.id, param } });
      return;
    }
    const controller = Number(trimmed);
    if (!Number.isInteger(controller) || controller < 0 || controller > 127) {
      setText(cc === undefined ? '' : String(cc));
      return;
    }
    if (controller !== cc) dispatch({ command: 'setMidiMapping', args: { trackId: track.id, nodeId: macros.id, param, channel: 1, controller } });
  };
  return (
    <label className="mod-macro-cc" title="MIDI controller (CC) number, channel 1, any device; empty for none">
      CC
      <input
        value={text}
        inputMode="numeric"
        placeholder="–"
        aria-label={`MIDI controller for macro ${knob + 1}`}
        onChange={(event) => setText(event.target.value)}
        onBlur={commit}
        onKeyDown={(event) => event.key === 'Enter' && event.currentTarget.blur()}
      />
    </label>
  );
}

/** One parameter a macro drives: where it is, its depth, and a way to remove it. */
function TargetRow({ track, link }: { track: Track; link: SlotLink }) {
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const info = infoOf(track.id, link.node, types, statuses);
  const param = info?.params.find((p) => p.id === link.slot.param);
  const owner = ownerOf(track, link.node.id);
  const where = owner ? `${nodeName(owner, types)} › ${nodeName(link.node, types)}` : nodeName(link.node, types);
  const span = param ? depthSpan(param) : 1;
  const step = param ? depthStep(param) : 0.01;
  const scope = owner ? { voiceNodeId: owner.id } : {};
  return (
    <li className="mod-target">
      <span className="mod-target-name" title={`${where}: ${param?.name ?? link.slot.param}`}>
        {nodeName(link.node, types)} · {param?.name ?? link.slot.param}
      </span>
      <DragNumber
        className="graph-mod-depth"
        value={Math.round((link.slot.depth * span) / step) * step}
        min={-span}
        max={span}
        quantum={step}
        format={(v) => (param ? formatDepth(param, v / span) : `${Math.round((v / span) * 100)}%`)}
        title="Depth: how far the macro, turned all the way from its neutral position, moves the parameter"
        onChange={(value, gesture) =>
          dispatch(
            {
              command: 'setModulation',
              args: { trackId: track.id, ...scope, nodeId: link.node.id, port: link.port, param: link.slot.param, depth: value / span },
            },
            gesture,
          )
        }
      />
      <IconButton
        size="s"
        className="tiny"
        title="Remove this mapping"
        onClick={() => {
          // A macro slot goes; a cable from the Macros device goes with its slot.
          const cable = link.macro === undefined ? link.sources.find((s) => s.node.type === NODE_TYPES.macros) : undefined;
          if (cable)
            dispatch({
              command: 'disconnect',
              args: { trackId: track.id, ...scope, fromNode: cable.node.id, fromPort: cable.port, toNode: link.node.id, toPort: link.port },
            });
          else dispatch({ command: 'setModulation', args: { trackId: track.id, ...scope, nodeId: link.node.id, port: link.port, param: '' } });
        }}
      >
        <X size={10} />
      </IconButton>
    </li>
  );
}
