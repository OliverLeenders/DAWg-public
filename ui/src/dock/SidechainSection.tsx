import { Headphones } from 'lucide-react';
import { HighPassGlyph, LowPassGlyph } from '../components/Glyphs';
import type { GraphNode, ParamDescriptor, Track } from '../bridge/protocol';
import { nodeName } from '../components/chain';
import { useDawg } from '../store/store';
import { KeySelector } from './KeySelector';
import { Knob } from './Knob';
import { Section } from './Section';
import { Button } from '../ui/controls';

// The dynamics panels' Sidechain column (docs/design/sidechain.md, K8; gate-expander.md, N4): the key source, the
// key's high-pass and low-pass (on what the detector hears, not on the audio), Listen, and how late a key from another
// track arrives.

interface Props {
  track: Track;
  node: GraphNode;
  param: (id: string) => ParamDescriptor;
  value: (id: string) => number;
  toggle: (id: string) => void;
  keyLateMs: number;
}

export function SidechainSection({ track, node, param, value, toggle, keyLateMs }: Props) {
  const types = useDawg((s) => s.nodeTypes);
  const name = nodeName(node, types).toLowerCase();
  const filters = (['keyHp', 'keyLp'] as const).filter((id) => value(id) > 0.5);

  return (
    <div className="panel-sections key-sections">
      <Section label="Sidechain" column>
        <KeySelector track={track} node={node} label="Source" />
        <div className="key-switches">
          <Button
            className={`${value('keyHp') > 0.5 ? 'active' : ''}`}
            title={`High-pass on what the ${name} reacts to (not on the audio)`}
            aria-label="High-pass"
            onClick={() => toggle('keyHp')}
          >
            <HighPassGlyph />
          </Button>
          <Button
            className={`${value('keyLp') > 0.5 ? 'active' : ''}`}
            title={`Low-pass on what the ${name} reacts to (not on the audio)`}
            aria-label="Low-pass"
            onClick={() => toggle('keyLp')}
          >
            <LowPassGlyph />
          </Button>
          <Button
            className={`${value('listen') > 0.5 ? 'active' : ''}`}
            title={`Listen: play what the ${name} reacts to`}
            aria-label="Listen"
            onClick={() => toggle('listen')}
          >
            <Headphones size={13} />
          </Button>
        </div>
        {filters.length > 0 && (
          <div className="key-knobs">
            {filters.map((id) => (
              <Knob key={id} track={track} node={node} param={param(`${id}Freq`)} label={id === 'keyHp' ? 'HP' : 'LP'} />
            ))}
          </div>
        )}
        {keyLateMs > 0.5 && (
          <span className="key-late" title={`The key source has more latency than the path to this ${name}, so its key arrives late`}>
            Sidechain {keyLateMs.toFixed(1)} ms late
          </span>
        )}
      </Section>
    </div>
  );
}
