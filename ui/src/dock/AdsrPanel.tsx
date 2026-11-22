import { AdsrEditor } from '../graph/AdsrEditor';
import { Knob } from './Knob';
import type { PanelProps } from './panels';
import { Section } from './Section';

// The ADSR's panel (docs/design/modulation.md, M3): its envelope as an editable graph, with the knobs for its times and
// sustain level underneath. The segments' curves are edited only in the graph.

export function AdsrPanel({ track, node, info }: PanelProps) {
  const param = (id: string) => info.params.find((p) => p.id === id)!;

  return (
    <div className="device-panel adsr-panel">
      <div className="panel-main">
        <AdsrEditor track={track} node={node} info={info} />
        <div className="adsr-knobs">
          <Section label="Envelope">
            {['attackMs', 'decayMs', 'sustain', 'releaseMs'].map((id) => (
              <Knob key={id} track={track} node={node} param={param(id)} />
            ))}
          </Section>
        </div>
      </div>
    </div>
  );
}
