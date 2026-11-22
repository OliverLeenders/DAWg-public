import { TICKS_PER_BEAT, type Command, type GraphNode, type ParamDescriptor } from '../bridge/protocol';
import { keyChangeAt } from '../pianoroll/fold';
import { tempoMapOf } from '../time/tempoMap';
import { CHORD_NOTE_PARAMS, CUSTOM_SHAPE, chordNotes, toggledNotes } from '../music/chords';
import { SCALES, inScale } from '../music/scales';
import { isBlackKey, pitchName } from '../pianoroll/geometry';
import { dispatch, useDawg } from '../store/store';
import { useNodeAnalysis } from './analysis';
import { ParamSlot } from './Knob';
import type { PanelProps } from './panels';
import { Section } from './Section';

// The MIDI effects' panels (docs/design/midi-effects.md, section 7): knobs and choices in sections beside a small
// keyboard (Chord, Scale Lock) or the arpeggio's keys (Arpeggiator), which fill the space the controls leave.

type Params = (id: string) => ParamDescriptor;

const paramsOf = (props: PanelProps): Params => (id) => props.info.params.find((p) => p.id === id)!;
const valueOf = (node: GraphNode, param: ParamDescriptor) => node.params[param.id] ?? param.default;

function Controls({ props, ids, labels = {} }: { props: PanelProps; ids: string[]; labels?: Record<string, string> }) {
  const param = paramsOf(props);
  return (
    <>
      {ids.map((id) => (
        <ParamSlot key={id} track={props.track} node={props.node} param={param(id)} label={labels[id]} />
      ))}
    </>
  );
}

/**
 * Keys from `from` for `count` semitones, drawn in one row: black keys as narrower, darker keys between the white ones,
 * so it reads as a keyboard at any width. Lit keys are filled with the accent; `marked` keys get a dot.
 */
export function MiniKeyboard({
  from,
  count,
  lit,
  marked = new Set<number>(),
  label,
  onKey,
}: {
  from: number;
  count: number;
  lit: Set<number>;
  marked?: Set<number>;
  label: (key: number) => string;
  onKey?: (key: number) => void;
}) {
  const keys = Array.from({ length: count }, (_, i) => from + i);
  return (
    <div className="mini-keyboard" role="group">
      {keys.map((key) => (
        <button
          key={key}
          type="button"
          className={`mini-key ${isBlackKey(key) ? 'black' : 'white'} ${lit.has(key) ? 'lit' : ''} ${marked.has(key) ? 'marked' : ''}`}
          title={label(key)}
          disabled={!onKey}
          onClick={() => onKey?.(key)}
        />
      ))}
    </div>
  );
}

// ---------------------------------------------------------------------------------------------------------

export function ArpeggiatorPanel(props: PanelProps) {
  const analysis = useNodeAnalysis(props.track.id, props.node.id);
  const meters = analysis?.meters as { keys?: number[]; step?: number } | null | undefined;
  const keys = meters?.keys ?? [];
  const step = meters?.step ?? -1;

  return (
    <div className="device-panel midi-effect-panel">
      <div className="panel-main midi-effect-main">
        <div className="arp-keys" title="The held keys in the order the arpeggio plays them">
          {keys.length === 0 ? (
            <span className="arp-empty">Hold some keys</span>
          ) : (
            keys.map((pitch, i) => (
              <span key={i} className={`arp-key ${step < 0 || step === i ? 'lit' : ''}`}>
                {pitchName(pitch)}
              </span>
            ))
          )}
        </div>
      </div>
      <div className="panel-sections midi-effect-sections">
        <Section label="Pattern">
          <Controls props={props} ids={['rate', 'mode', 'octaves']} />
        </Section>
        <Section label="Feel">
          <Controls props={props} ids={['gate', 'swing', 'velocity', 'fixedVelocity']} labels={{ fixedVelocity: 'Fixed' }} />
        </Section>
        <Section label="Hold">
          <Controls props={props} ids={['latch', 'retrigger']} />
        </Section>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------------------------------------------------

/** The Chord's keyboard runs an octave below the played note to two above it: room for inversions and spreads. */
const CHORD_FROM = -12;
const CHORD_KEYS = 37;
const CHORD_ROOT = 60; // drawn as if C4 were played

export function ChordPanel(props: PanelProps) {
  const { track, node } = props;
  const param = paramsOf(props);
  const notes = chordNotes((id) => valueOf(node, param(id)));
  const lit = new Set(notes.map((n) => CHORD_ROOT + n));

  // A click switches to Custom with that key in or out; the inversion is folded into the notes.
  const toggle = (key: number) => {
    const semitones = key - CHORD_ROOT;
    if (semitones === 0) return;
    const next = toggledNotes(notes, semitones);
    const set = (id: string, value: number): Command => ({ command: 'setNodeParam', args: { trackId: track.id, nodeId: node.id, param: id, value } });
    const commands = [set('shape', CUSTOM_SHAPE), set('inversion', 0), ...CHORD_NOTE_PARAMS.map((id, i) => set(id, next[i] ?? 0))];
    dispatch({ command: 'batch', args: { label: 'Edit chord', commands } });
  };

  return (
    <div className="device-panel midi-effect-panel">
      <div className="panel-main midi-effect-main">
        <MiniKeyboard
          from={CHORD_ROOT + CHORD_FROM}
          count={CHORD_KEYS}
          lit={lit}
          marked={new Set([CHORD_ROOT])}
          label={(key) => `${key - CHORD_ROOT >= 0 ? '+' : ''}${key - CHORD_ROOT} semitones (click to add or remove)`}
          onKey={toggle}
        />
        <div className="panel-readout">
          <span>{notes.map((n) => pitchName(CHORD_ROOT + n)).join(' ')}</span>
          <span className="dim">from C4</span>
        </div>
      </div>
      <div className="panel-sections midi-effect-sections">
        <Section label="Chord">
          <Controls props={props} ids={['shape', 'inversion']} />
        </Section>
        <Section label="Strum">
          <Controls props={props} ids={['strum', 'strumOrder', 'velocity']} labels={{ strumOrder: 'Order' }} />
        </Section>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------------------------------------------------

export function ScaleLockPanel(props: PanelProps) {
  const { node } = props;
  const param = paramsOf(props);
  // Following, the project's key at the playhead replaces Root and Scale while one is set
  // (docs/design/scale-highlighting.md, SH6, SH9).
  const projectKey = useDawg((s) =>
    keyChangeAt(s.project?.keys, tempoMapOf(s.project).barAt(s.telemetry.positionBeats * TICKS_PER_BEAT).bar),
  );
  const following = Math.round(valueOf(node, param('follow'))) === 1 && projectKey?.enabled === true;
  const root = following ? projectKey.root : Math.round(valueOf(node, param('root')));
  const scale = SCALES[following ? projectKey.scale : Math.round(valueOf(node, param('scale')))] ?? SCALES[0];
  const keys = Array.from({ length: 12 }, (_, i) => 60 + i);
  const lit = new Set(keys.filter((key) => inScale(key, root, scale.mask)));

  return (
    <div className="device-panel midi-effect-panel">
      <div className="panel-main midi-effect-main">
        <MiniKeyboard from={60} count={12} lit={lit} marked={new Set([60 + root])} label={(key) => pitchName(key).replace(/-?\d+$/, '')} />
        <div className="panel-readout">
          <span>
            {pitchName(60 + root).replace(/-?\d+$/, '')} {scale.name}
            {following ? ' · project key' : ''}
          </span>
        </div>
      </div>
      <div className="panel-sections midi-effect-sections">
        <Section label="Scale">
          <Controls props={props} ids={['follow', 'root', 'scale', 'mode']} />
        </Section>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------------------------------------------------

export function NoteRepeatPanel(props: PanelProps) {
  return (
    <div className="device-panel midi-effect-panel">
      <div className="panel-sections midi-effect-sections">
        <Section label="Repeat">
          <Controls props={props} ids={['rate', 'gate', 'swing']} />
        </Section>
        <Section label="Velocity">
          <Controls props={props} ids={['velocity', 'fixedVelocity']} labels={{ velocity: 'Source', fixedVelocity: 'Fixed' }} />
        </Section>
      </div>
    </div>
  );
}
