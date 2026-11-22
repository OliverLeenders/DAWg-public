import { ChoiceSlot, Knob, ParamField, SlotToggle } from './Knob';
import type { PanelProps } from './panels';
import { Section } from './Section';

// The Oscillator's panel (docs/design/phase12-voice-scope.md): its controls in sections by what they do, as the other
// devices have them. The waveform as pictures; Pitch a knob, since it's what gets modulated, and its coarse and fine
// offsets fields to type or drag; the level; the unison voices with their detune and spread; the start phase. What
// doesn't apply (pulse width off the pulse wave, detune and spread with one voice, the phase when it runs free) is
// dimmed in place, so nothing moves when the mode changes.

export function OscillatorPanel({ track, node, info }: PanelProps) {
  const param = (id: string) => info.params.find((p) => p.id === id)!;
  const knob = (id: string, label?: string) => <Knob track={track} node={node} param={param(id)} label={label} />;
  const field = (id: string, label?: string) => <ParamField track={track} node={node} param={param(id)} label={label} />;

  return (
    <div className="device-panel osc-panel">
      <div className="panel-sections osc-sections">
        <Section label="Wave">
          <ChoiceSlot track={track} node={node} param={param('waveform')} />
          {knob('pulseWidth', 'Width')}
        </Section>
        <Section label="Pitch">
          {knob('pitch')}
          <div className="osc-fields">
            {field('coarse')}
            {field('fine')}
          </div>
        </Section>
        <Section label="Output">{knob('level')}</Section>
        <Section label="Unison">
          <div className="osc-fields">{field('unison', 'Voices')}</div>
          {knob('detune')}
          {knob('spread')}
        </Section>
        <Section label="Phase">
          {knob('phase', 'Start')}
          {knob('randomPhase', 'Random')}
          <SlotToggle track={track} node={node} param={param('free')} label="Free" />
        </Section>
      </div>
    </div>
  );
}
