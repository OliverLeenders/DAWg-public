import { PanelRightClose } from 'lucide-react';
import { layout, useLayout } from '../store/layoutStore';
import { MidiFilesBrowser } from './MidiFilesBrowser';
import { PluginBrowser } from './PluginBrowser';
import { SoundExplorer } from './SoundExplorer';
import { IconButton } from '../ui/controls';
import { Tab, Tabs } from '../ui/Tabs';

// The right panel: Sounds | MIDI files | Plugins tabs (docs/design/phase7-audio-tracks.md, section 9, item 8).

export function RightPanel() {
  const tab = useLayout((s) => s.rightTab);

  return (
    <section className="panel plugins">
      <div className="panel-header">
        <Tabs variant="view" label="Browser">
          <Tab selected={tab === 'sounds'} onClick={() => layout.setRightTab('sounds')}>
            Sounds
          </Tab>
          <Tab selected={tab === 'midiFiles'} onClick={() => layout.setRightTab('midiFiles')}>
            MIDI files
          </Tab>
          <Tab selected={tab === 'plugins'} onClick={() => layout.setRightTab('plugins')}>
            Plugins
          </Tab>
        </Tabs>
        <IconButton title="Hide this panel (View menu shows it again)" onClick={layout.togglePlugins}>
          <PanelRightClose size={14} />
        </IconButton>
      </div>
      {tab === 'sounds' ? <SoundExplorer /> : tab === 'midiFiles' ? <MidiFilesBrowser /> : <PluginBrowser />}
    </section>
  );
}
