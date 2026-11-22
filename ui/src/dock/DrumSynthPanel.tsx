import { Ellipsis, Lock } from 'lucide-react';
import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { NODE_TYPES, type Command, type DrumModel, type DrumPad, type ParamDescriptor } from '../bridge/protocol';
import { font, readTheme } from '../canvas/theme';
import { useCanvas } from '../canvas/useCanvas';
import { Auditioner } from '../pianoroll/audition';
import { dispatch, useDawg } from '../store/store';
import { IconButton, Select, Toggle } from '../ui/controls';
import { Menu, Submenu } from '../ui/overlays';
import { MenuItem } from '../components/MenuItem';
import { useNodeAnalysis } from './analysis';
import {
  DRUM_MODELS,
  DRUM_PAD_KEYS,
  FIRST_DRUM_KEY,
  freeOutputs,
  modelName,
  padEnvelopes,
  padName,
  padParams,
  padsHit,
  padsOf,
  parseDrumParam,
  shortParamName,
} from './drumSynth';
import { padPresetCommands, padPresetsFor, type DrumPadPreset } from './drumKits';
import { ParamSlot } from './Knob';
import { PresetSelector } from './PresetSelector';
import { keyName } from './SamplerPanel';
import { Section, SectionHeader } from './Section';
import type { PanelProps } from './panels';

// The Drum Synth's panel (docs/design/drum-synth.md, DR12): the sixteen pads on the left, as the Sampler's, each with its
// name, its model and, where set, its output and choke group, flashing as it plays; the selected pad on the right, with
// its model, output and choke, the knobs every pad has and its model's own, and a picture of its level and pitch over
// time. Pressing a pad plays it; dragging a pad onto another copies it; a pad's menu and the ⋯ menu hold the rest.

const PAD_MIME = 'application/x-dawg-drum-pad';
const FLASH_MS = 120;

/** A copied pad, for Paste in another pad's menu (in this window only). */
let copiedPad: { nodeId: number; pad: number } | null = null;

export function DrumSynthPanel({ track, node, info }: PanelProps) {
  const pads = padsOf(node);
  const [selected, setSelected] = useState(0);
  const [menu, setMenu] = useState<{ pad?: number; x: number; y: number } | null>(null);
  const [flashing, setFlashing] = useState<Set<number>>(new Set());
  const auditioner = useRef(new Auditioner());
  const padButtons = useRef<Array<HTMLButtonElement | null>>([]);
  const target = { trackId: track.id, nodeId: node.id };
  const tracks = useDawg((s) => s.project?.tracks);
  const pad = pads[selected];

  useEffect(() => {
    const player = auditioner.current;
    return () => player.stop();
  }, []);

  // A pad flashes for a moment each time it plays (DR10).
  const hits = (useNodeAnalysis(track.id, node.id)?.meters as { hits?: number[] } | null | undefined)?.hits;
  const lastHits = useRef<number[] | undefined>(undefined);
  useEffect(() => {
    const hit = padsHit(lastHits.current, hits);
    lastHits.current = hits;
    if (hit.size === 0) return;
    setFlashing(hit);
    const timer = window.setTimeout(() => setFlashing(new Set()), FLASH_MS);
    return () => window.clearTimeout(timer);
  }, [hits]);

  const setPad = (index: number, fields: Partial<{ name: string; model: DrumModel; output: number; chokeGroup: number; mono: boolean; locked: boolean }>) =>
    dispatch({ command: 'setDrumPad', args: { ...target, pad: index, ...fields } });
  // A pad sound onto a pad, in one step (DR13).
  const loadSound = (index: number, sound: DrumPadPreset) => {
    const commands = padPresetCommands(target, node, info.params, index, sound);
    if (commands.length > 0) dispatch({ command: 'batch', args: { label: `Pad sound: ${sound.name}`, commands } } as Command);
  };
  const copyPad = (from: number, to: number) => from !== to && dispatch({ command: 'copyDrumPad', args: { ...target, from, to } });
  const randomise = (mode: 'randomise' | 'vary', only?: number[]) =>
    dispatch({ command: 'randomiseDrumPads', args: { ...target, mode, ...(only ? { pads: only } : {}) } } as Command);
  const split = (groups: Array<{ name: string; pads: number[] }>) => {
    if (groups.length === 0) {
      useDawg.setState({ lastHint: 'Every pad already plays from an output of its own' });
      return;
    }
    dispatch({ command: 'splitInstrumentOutputs', args: { ...target, outputs: groups } });
  };
  // Each pad on Main to an output of its own, as many as there are free outputs; the rest stay on Main.
  const eachPad = () => {
    const groups = pads
      .map((p, i) => ({ p, i }))
      .filter(({ p }) => p.model !== 'off' && p.output === 1)
      .map(({ p, i }) => ({ name: padName(p), pads: [i] }));
    const free = freeOutputs(pads, wiredPorts);
    split(groups.slice(0, free));
    if (groups.length > free)
      useDawg.setState({ lastHint: `${groups.length - free} pads stay on Main: ${groups.slice(free).map((g) => g.name).join(', ')}` });
  };

  const wiredPorts = Object.values(track.graph.connections)
    .filter((c) => c.fromNode === node.id)
    .map((c) => c.fromPort);

  // The outputs by port, Main first, named after the output track each leads to (MO1, MO5).
  const outputNames = Array.from({ length: 16 }, (_, port) => {
    if (port === 0) return 'Main';
    const cable = Object.values(track.graph.connections).find((c) => c.fromNode === node.id && c.fromPort === port);
    const to = cable ? track.graph.nodes[String(cable.toNode)] : undefined;
    const out = to?.type === NODE_TYPES.busOutput && to.target ? tracks?.[String(to.target)] : undefined;
    return out ? out.name : cable ? `Out ${port + 1}` : `Out ${port + 1} (plays in Main)`;
  });

  const { common, own } = padParams(info.params, selected, pad.model);

  return (
    <div className="device-panel drum-panel">
      <div className="drum-pads-column">
        <SectionHeader
          label="Pads"
          actions={
            <>
              <PresetSelector track={track} node={node} info={info} compact />
              <IconButton
                title="The whole kit: randomise, vary, each pad to its own track"
                onClick={(e) => {
                  const r = e.currentTarget.getBoundingClientRect();
                  setMenu({ x: r.left, y: r.bottom + 2 });
                }}
              >
                <Ellipsis size={14} />
              </IconButton>
            </>
          }
        />
        <div className="sampler-pads drum-pads" role="group" aria-label="Pads: Enter or Space plays, the arrows move">
          {DRUM_PAD_KEYS.map((key, position) => {
            const index = key - FIRST_DRUM_KEY;
            const p = pads[index];
            // The arrows move through the grid as drawn: four pads a row.
            const move = (step: number) => {
              const next = DRUM_PAD_KEYS[position + step];
              if (next === undefined) return;
              setSelected(next - FIRST_DRUM_KEY);
              padButtons.current[position + step]?.focus();
            };
            return (
              <button
                key={key}
                ref={(b) => {
                  padButtons.current[position] = b;
                }}
                className={`sampler-pad drum-pad ${p.model !== 'off' ? 'filled' : ''} ${index === selected ? 'selected' : ''} ${flashing.has(index) ? 'hit' : ''}`}
                title={`${padName(p)}: ${modelName(p.model)}, key ${keyName(key)}. Press to play, drag onto another pad to copy it`}
                aria-current={index === selected}
                tabIndex={index === selected ? 0 : -1}
                onKeyDown={(e) => {
                  if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    if (e.repeat) return;
                    setSelected(index);
                    if (p.model !== 'off') auditioner.current.play(track.id, key, 1);
                  } else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight' || e.key === 'ArrowUp' || e.key === 'ArrowDown') {
                    e.preventDefault();
                    move({ ArrowLeft: -1, ArrowRight: 1, ArrowUp: -4, ArrowDown: 4 }[e.key]);
                  }
                }}
                onKeyUp={(e) => {
                  if (e.key !== 'Enter' && e.key !== ' ') return;
                  e.preventDefault();
                  auditioner.current.stop();
                }}
                draggable
                onDragStart={(e) => e.dataTransfer.setData(PAD_MIME, String(index))}
                onDragOver={(e) => e.dataTransfer.types.includes(PAD_MIME) && e.preventDefault()}
                onDrop={(e) => {
                  const from = e.dataTransfer.getData(PAD_MIME);
                  if (from === '') return;
                  e.preventDefault();
                  copyPad(Number(from), index);
                  setSelected(index);
                }}
                onPointerDown={(e) => {
                  if (e.button !== 0) return;
                  setSelected(index);
                  if (p.model !== 'off') auditioner.current.play(track.id, key, 1);
                }}
                onPointerUp={() => auditioner.current.stop()}
                onPointerLeave={() => auditioner.current.stop()}
                onContextMenu={(e) => {
                  e.preventDefault();
                  setSelected(index);
                  setMenu({ pad: index, x: e.clientX, y: e.clientY });
                }}
              >
                <span className="sampler-pad-top">
                  <span className="sampler-pad-key">{padName(p)}</span>
                  <span className="sampler-pad-note" aria-hidden>
                    {keyName(key)}
                  </span>
                </span>
                <span className="drum-pad-chips">
                  <span className="drum-pad-model">{p.model === 'off' ? '' : modelName(p.model)}</span>
                  {p.output > 1 && <span className="drum-chip" title={`Plays from ${outputNames[p.output - 1]}`}>Out {p.output}</span>}
                  {p.choke > 0 && <span className="drum-chip" title={`A hit fades the other pads of choke group ${p.choke}`}>Choke {p.choke}</span>}
                  {p.locked && <Lock size={10} aria-label="Locked" />}
                </span>
              </button>
            );
          })}
        </div>
      </div>
      <div className="drum-editor">
        <div className="drum-pad-header">
          <PadName key={`${node.id}:${selected}`} pad={pad} onRename={(name) => setPad(selected, { name })} />
          <Select aria-label="Model" title="What the pad plays" value={pad.model} onChange={(e) => setPad(selected, { model: e.target.value as DrumModel })}>
            {DRUM_MODELS.map(({ model, name }) => (
              <option key={model} value={model}>
                {name}
              </option>
            ))}
          </Select>
          <Select
            aria-label="Output"
            title="The output it plays from: Main, or an output wired to a track of its own"
            value={pad.output}
            onChange={(e) => (e.target.value === 'own' ? split([{ name: padName(pad), pads: [selected] }]) : setPad(selected, { output: Number(e.target.value) }))}
          >
            {outputNames.map((name, port) => (
              <option key={port} value={port + 1}>
                {name}
              </option>
            ))}
            <option value="own">Own output track</option>
          </Select>
          <Select aria-label="Choke group" title="A hit in a choke group fades the group's other pads" value={pad.choke} onChange={(e) => setPad(selected, { chokeGroup: Number(e.target.value) })}>
            {Array.from({ length: 9 }, (_, group) => (
              <option key={group} value={group}>
                {group === 0 ? 'No choke' : `Choke ${group}`}
              </option>
            ))}
          </Select>
          <Toggle on={pad.mono} onChange={(mono) => setPad(selected, { mono })} title="A new hit fades the pad's last one">
            Mono
          </Toggle>
          <Toggle icon on={pad.locked} onChange={(locked) => setPad(selected, { locked })} title="Locked: randomising the kit leaves this pad alone">
            <Lock size={12} />
          </Toggle>
        </div>
        {pad.model === 'off' ? (
          <p className="dock-empty">This pad plays nothing. Choose a model for it.</p>
        ) : (
          <div className="drum-sound">
            <div className="panel-sections">
              <Section label="Pad">
                {common.map((param) => (
                  <ParamSlot key={param.id} track={track} node={node} param={param} label={shortParamName(param)} />
                ))}
              </Section>
              <Section label={modelName(pad.model)}>
                {own.map((param) => (
                  <ParamSlot key={param.id} track={track} node={node} param={param} label={shortParamName(param)} />
                ))}
              </Section>
            </div>
            <PadEnvelope model={pad.model} params={[...common, ...own]} values={node.params} />
          </div>
        )}
      </div>
      {menu && (
        <Menu
          onClose={() => setMenu(null)}
          at={{ x: menu.x, y: menu.y }}
          onClick={(e) => {
            // An item closes the menu; a row that opens a submenu doesn't.
            const item = (e.target as HTMLElement).closest('.menu-item');
            if (item && !item.classList.contains('submenu-row')) setMenu(null);
          }}
        >
          {menu.pad !== undefined ? (
            <PadMenu
              index={menu.pad}
              pad={pads[menu.pad]}
              onModel={(model) => setPad(menu.pad!, { model })}
              onSound={(sound) => loadSound(menu.pad!, sound)}
              onCopy={() => (copiedPad = { nodeId: node.id, pad: menu.pad! })}
              onPaste={copiedPad?.nodeId === node.id ? () => copyPad(copiedPad!.pad, menu.pad!) : undefined}
              onOwnOutput={() => split([{ name: padName(pads[menu.pad!]), pads: [menu.pad!] }])}
              onRandomise={(mode) => randomise(mode, [menu.pad!])}
              onLock={() => setPad(menu.pad!, { locked: !pads[menu.pad!].locked })}
            />
          ) : (
            <>
              <div className="menu-title">The kit</div>
              <MenuItem label="Randomise kit" title="New settings for every pad that plays and isn't locked, within ranges that stay playable" onClick={() => randomise('randomise')} />
              <MenuItem label="Vary kit" title="Moves every unlocked pad's settings a little" onClick={() => randomise('vary')} />
              <div className="menu-separator" />
              <MenuItem label="Each pad to its own track" title="An output track for each pad that plays from Main, wired in one step" onClick={eachPad} />
            </>
          )}
        </Menu>
      )}
    </div>
  );
}

/** The pad's name, typed in place; Enter or leaving the field keeps it, Escape goes back. */
function PadName({ pad, onRename }: { pad: DrumPad; onRename: (name: string) => void }) {
  const [text, setText] = useState(pad.name);
  const commit = () => text.trim() !== pad.name && onRename(text.trim());
  return (
    <input
      className="drum-pad-name"
      aria-label="Pad name"
      value={text}
      placeholder={modelName(pad.model)}
      onChange={(e) => setText(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === 'Enter') e.currentTarget.blur();
        if (e.key === 'Escape') {
          setText(pad.name);
          e.currentTarget.blur();
        }
      }}
    />
  );
}

function PadMenu({
  index,
  pad,
  onModel,
  onSound,
  onCopy,
  onPaste,
  onOwnOutput,
  onRandomise,
  onLock,
}: {
  index: number;
  pad: DrumPad;
  onModel: (model: DrumModel) => void;
  onSound: (sound: DrumPadPreset) => void;
  onCopy: () => void;
  onPaste?: () => void;
  onOwnOutput: () => void;
  onRandomise: (mode: 'randomise' | 'vary') => void;
  onLock: () => void;
}) {
  const sounds = padPresetsFor(pad.model);
  return (
    <>
      <div className="menu-title">
        {padName(pad)}, pad {index + 1}
      </div>
      {sounds.length > 0 && (
        <Submenu label={<MenuLabel text={`${modelName(pad.model)} sounds`} />}>
          {sounds.map((sound) => (
            <MenuItem key={sound.id} label={sound.name} onClick={() => onSound(sound)} />
          ))}
        </Submenu>
      )}
      <Submenu label={<MenuLabel text="Model" />} detail={modelName(pad.model)}>
        {DRUM_MODELS.map(({ model, name }) => (
          <MenuItem key={model} label={name} checked={model === pad.model} onClick={() => model !== pad.model && onModel(model)} />
        ))}
      </Submenu>
      <div className="menu-separator" />
      <MenuItem label="Copy pad" onClick={onCopy} />
      <MenuItem label="Paste pad" disabled={!onPaste} onClick={() => onPaste?.()} />
      <div className="menu-separator" />
      <MenuItem label="Randomise" disabled={pad.model === 'off'} onClick={() => onRandomise('randomise')} />
      <MenuItem label="Vary" disabled={pad.model === 'off'} onClick={() => onRandomise('vary')} />
      <MenuItem label={pad.locked ? 'Unlock' : 'Lock'} onClick={onLock} />
      <div className="menu-separator" />
      <MenuItem label="Own output track" disabled={pad.model === 'off'} onClick={onOwnOutput} />
    </>
  );
}

/** A submenu's label in the items' icon column, so it lines up with the MenuItems beside it. */
const MenuLabel = ({ text }: { text: string }) => (
  <span className="menu-check-label">
    <span className="menu-icon" />
    {text}
  </span>
);

/** The pad's level (in the accent), its pitch sweep and a cymbal's wash (in tones) over the hit's length (DR12). */
function PadEnvelope({ model, params, values }: { model: DrumModel; params: ParamDescriptor[]; values: Record<string, number> }) {
  const canvas = useCanvas();
  const value = (id: string) => {
    const param = params.find((p) => parseDrumParam(p.id)?.id === id);
    return param ? (values[param.id] ?? param.default) : 0;
  };
  const seconds = Math.min(4, Math.max(0.05, padEnvelopes(model, value, 1, 2).decay * 1.1));
  const { level, pitch, wash, sweep } = padEnvelopes(model, value, seconds);

  useLayoutEffect(() => {
    const ctx = canvas.context();
    const { width, height } = canvas.size;
    if (!ctx || width < 16 || height < 16) return;
    const theme = readTheme();
    ctx.clearRect(0, 0, width, height);
    const toX = (t: number) => (t / seconds) * (width - 2) + 1;
    const line = (points: Array<{ t: number; v: number }>, toY: (v: number) => number, colour: string) => {
      ctx.strokeStyle = colour;
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      points.forEach(({ t, v }, i) => (i === 0 ? ctx.moveTo(toX(t), toY(v)) : ctx.lineTo(toX(t), toY(v))));
      ctx.stroke();
    };
    if (sweep > 0) line(pitch, (v) => height - 2 - (v / sweep) * (height - 4), theme.tones[2]);
    if (wash.length > 0) line(wash, (v) => height - 2 - v * (height - 4), theme.tones[1]);
    line(level, (v) => height - 2 - v * (height - 4), theme.accent);
    // A legend in the lines' colours, and the time the picture covers.
    ctx.font = font(theme, 'xs');
    ctx.textAlign = 'right';
    ctx.fillStyle = theme.accent;
    ctx.fillText('Level', width - 6, 14);
    if (sweep > 0) {
      ctx.fillStyle = theme.tones[2];
      ctx.fillText(`Pitch +${sweep.toFixed(0)} st`, width - 6, 28);
    }
    if (wash.length > 0) {
      ctx.fillStyle = theme.tones[1];
      ctx.fillText('Wash', width - 6, 28);
    }
    ctx.fillStyle = theme.textDim;
    ctx.fillText(seconds < 1 ? `${Math.round(seconds * 1000)} ms` : `${seconds.toFixed(1)} s`, width - 6, height - 6);
    ctx.textAlign = 'left';
  });

  return <canvas ref={canvas.ref} className="panel-canvas drum-envelope" aria-label="Level and pitch over the hit" />;
}
