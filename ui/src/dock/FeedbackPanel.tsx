import { Eraser } from 'lucide-react';
import { actions, useDawg } from '../store/store';
import { useNodeDrag } from './analysis';
import { Knob } from './Knob';
import { Section } from './Section';
import type { PanelProps } from './panels';
import { Button, IconButton } from '../ui/controls';

// The Feedback node's panel (docs/design/feedback-routing.md, section 3): how long the trip round the loop is, how much goes round
// again, and what the node's safety is doing.

const MODES = ['Time', 'Sync', 'Note'];

export function FeedbackPanel({ track, node, info }: PanelProps) {
  const edit = useNodeDrag(track.id, node.id, node.params);
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const value = (id: string) => edit.values[id] ?? param(id).default;
  const set = (id: string, v: number) => {
    edit.begin([id]);
    edit.set({ [id]: v });
    edit.end();
  };
  const meters = useDawg((s) => s.telemetry.loops?.[`${track.id}:${node.id}`]);

  const mode = Math.round(value('mode'));
  const above = value('above') >= 0.5;
  const amount = param('amount');
  const reach = above ? amount : { ...amount, min: -100, max: 100 };
  const notes = param('note').choices;
  const tooShort = mode === 0 && meters !== undefined && value('time') < meters.shortestMs;

  return (
    <div className="device-panel feedback-panel">
      <div className="panel-main">
        <div className="delay-header">
          <div className="segmented">
            {MODES.map((name, i) => (
              <button key={name} className={mode === i ? 'active' : ''} title={`The loop's length as a ${name.toLowerCase()}`} onClick={() => set('mode', i)}>
                {name}
              </button>
            ))}
          </div>
          {mode === 1 && (
            <label className="field">
              <span>Length</span>
              <select value={Math.round(value('note'))} title="The loop's length as a note" onChange={(e) => set('note', Number(e.target.value))}>
                {notes.map((name, i) => (
                  <option key={name} value={i}>
                    {name}
                  </option>
                ))}
              </select>
            </label>
          )}
        </div>
        <div className="feedback-status">
          <span
            className={`feedback-limit ${meters?.limiting ? 'on' : ''}`}
            title="Lights when the loop reaches the node's limit, which is normal for a loop above 100 %"
          >
            Limit
          </span>
          {meters && (
            <span className={`feedback-shortest ${tooShort ? 'feedback-short' : ''}`} title="A loop can't be shorter, with what's in it">
              Shortest loop {meters.shortestMs.toFixed(1)} ms
            </span>
          )}
          <IconButton title="Clear: empty the loop now" aria-label="Clear" onClick={() => actions.clearNode(track.id, node.id)}>
            <Eraser size={13} />
          </IconButton>
        </div>
      </div>
      <div className="panel-sections">
        <Section label="Length">
          {mode === 0 && <Knob track={track} node={node} param={param('time')} />}
          {mode === 2 && <Knob track={track} node={node} param={param('pitch')} />}
          {mode === 2 && <Knob track={track} node={node} param={param('tune')} />}
          {mode === 2 && <Knob track={track} node={node} param={param('trim')} />}
        </Section>
        <Section label="Goes round">
          <Knob track={track} node={node} param={reach} />
          <Button
            className={`${above ? 'active' : ''}`}
            title="Let the loop grow. It stays under the node's limit, and rings until it's cleared."
            onClick={() => set('above', above ? 0 : 1)}
          >
            Above 100 %
          </Button>
        </Section>
      </div>
    </div>
  );
}
