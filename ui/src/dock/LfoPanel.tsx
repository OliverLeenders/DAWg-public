import { LfoShapeEditor } from '../graph/LfoShapeEditor';
import { lfoSynced } from '../graph/lfo';
import { ChoiceSlot, Knob, SlotToggle } from './Knob';
import type { PanelProps } from './panels';
import { Section } from './Section';

// The LFO's panel (docs/design/modulation.md, M4): its one-cycle shape with the presets and the zero position,
// then its timing. A tempo-synced LFO picks its cycle as a note length; a free one has a rate in ms.

export function LfoPanel({ track, node, info }: PanelProps) {
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const synced = lfoSynced(node);

  return (
    <div className="device-panel lfo-panel">
      <div className="panel-main">
        <LfoShapeEditor track={track} node={node} />
      </div>
      <div className="panel-sections">
        <Section label="Timing">
          <SlotToggle track={track} node={node} param={param('sync')} label="Sync" />
          {synced ? (
            <ChoiceSlot track={track} node={node} param={param('cycle')} />
          ) : (
            <Knob track={track} node={node} param={param('rateMs')} />
          )}
        </Section>
        <Section label="Start">
          <SlotToggle track={track} node={node} param={param('retrigger')} />
          <Knob track={track} node={node} param={param('phase')} />
        </Section>
      </div>
    </div>
  );
}
