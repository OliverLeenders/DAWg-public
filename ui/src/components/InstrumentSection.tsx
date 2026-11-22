import { AppWindow, ExternalLink } from 'lucide-react';
import { NODE_TYPES, type GraphNode, type ParamDescriptor, type Track } from '../bridge/protocol';
import { ParamSlot } from '../dock/Knob';
import { MacroKnobs } from '../dock/ModulationColumn';
import { macrosOf } from '../dock/macros';
import { hasPresetsFor } from '../dock/presets';
import { PresetSelector } from '../dock/PresetSelector';
import { paramApplies } from '../graph/lfo';
import { infoOf } from '../graph/nodeInfo';
import { useDawg } from '../store/store';
import { openInstrument } from './instrumentWindow';
import { Button } from '../ui/controls';

// The instrument in the inspector (docs/design/macros-and-modulation-column.md, MC14): its preset, its main controls and
// the way to its full editor, which opens in a window of its own. The macros follow (MC11).

/** How many of a built-in instrument's controls the inspector shows: the first ones, which are its main controls. Only
    knobs: choices and switches are settings of the instrument, made in its editor, and in a column this narrow they
    don't fit beside knobs. */
const MAIN_CONTROLS = 8;

/** Instruments whose knobs stay in their editor: the MPE Synth's and the Sampler's (pitch, level, velocity) only repeat
    what the editor shows, and the Drum Synth's first knobs are pad 1's alone, so the inspector gives them the preset
    and the way to the editor. */
const NO_INSPECTOR_KNOBS: string[] = [NODE_TYPES.mpeSynth, NODE_TYPES.sampler, NODE_TYPES.drumSynth];

/** The knobs the inspector shows for an instrument: none for a plugin or an instrument whose knobs stay in its editor. */
export function inspectorKnobs(node: GraphNode, params: ParamDescriptor[]): ParamDescriptor[] {
  if (node.plugin || NO_INSPECTOR_KNOBS.includes(node.type)) return [];
  return params.filter((p) => p.kind === 'continuous' && paramApplies(node, p.id)).slice(0, MAIN_CONTROLS);
}

export function InstrumentSection({ track, node }: { track: Track; node: GraphNode }) {
  const types = useDawg((s) => s.nodeTypes);
  const statuses = useDawg((s) => s.nodes);
  const status = statuses[`${track.id}:${node.id}`];
  const info = infoOf(track.id, node, types, statuses);
  const knobs = inspectorKnobs(node, info?.params ?? []);
  const isPluginInstrument = node.type === NODE_TYPES.pluginInstrument;

  return (
    <div className="inspector-instrument">
      <div className="inspector-instrument-row">
        {info && hasPresetsFor(node.type) && <PresetSelector key={`${track.id}:${node.id}`} track={track} node={node} info={info} compact />}
        <Button
          className={`${isPluginInstrument && status?.editorOpen ? 'active' : ''}`}
          title={isPluginInstrument ? "The plugin's own window" : 'Open the full editor in its own window'}
          disabled={isPluginInstrument && status?.status !== 'ready'}
          onClick={() => openInstrument(track, node)}
        >
          {isPluginInstrument ? <ExternalLink size={12} /> : <AppWindow size={12} />} {isPluginInstrument && status?.editorOpen ? 'Close' : 'Edit'}
        </Button>
      </div>
      {knobs.length > 0 && (
        <div className="dock-knobs inspector-knobs">
          {knobs.map((param) => (
            <ParamSlot key={param.id} track={track} node={node} param={param} />
          ))}
        </div>
      )}
    </div>
  );
}

/** The track's macro knobs in the inspector, when it has a Macros device (MC11). */
export function InspectorMacros({ track }: { track: Track }) {
  const macros = macrosOf(track);
  if (!macros) return null;
  return (
    <>
      <div className="inspector-label">Macros</div>
      <div className="inspector-macros">
        <MacroKnobs track={track} macros={macros} />
      </div>
    </>
  );
}
