import { MidiPort } from 'lucide-react';
import { useState } from 'react';
import { actions, useDawg } from '../store/store';
import { IconButton } from '../ui/controls';
import { Menu } from '../ui/overlays';

// MIDI inputs in the top bar (docs/design/phase9-export-midi-input.md, M1): an activity light, and a popover to
// enable devices and set MPE mode and bend ranges per device.

export function MidiSettings() {
  const devices = useDawg((s) => s.midiDevices);
  const active = useDawg((s) => s.telemetry.midiActivity > 0);
  const [anchor, setAnchor] = useState<DOMRect | null>(null);

  return (
    <>
      <IconButton
        size="l"
        className="midi-button"
        title="MIDI inputs"
        onClick={(e) => setAnchor(anchor ? null : e.currentTarget.getBoundingClientRect())}
      >
        <MidiPort size={16} />
        <span className={`midi-activity ${active ? 'on' : ''}`} />
      </IconButton>
      {anchor && (
        <>
          <Menu
            onClose={() => setAnchor(null)}
            className="midi-popup"
            // Its right edge under the button's: .midi-popup is 340 px wide.
            at={{ x: Math.max(8, anchor.right - 340), y: anchor.bottom + 6 }}
          >
            <div className="menu-heading">MIDI inputs</div>
            {devices.length === 0 && <p className="empty">No MIDI input devices found.</p>}
            {devices.map((d) => (
              <div className="midi-device" key={d.identifier}>
                <input
                  type="checkbox"
                  checked={d.enabled}
                  title="Use this device"
                  onChange={(e) => actions.configureMidi(d.identifier, { enabled: e.target.checked })}
                />
                <span>{d.name}</span>
                <select
                  value={d.mpe ? 'mpe' : 'midi'}
                  title="MPE controllers send each note on its own channel"
                  onChange={(e) => actions.configureMidi(d.identifier, { mpe: e.target.value === 'mpe' })}
                >
                  <option value="midi">MIDI</option>
                  <option value="mpe">MPE</option>
                </select>
                <div className="midi-device-settings">
                  {d.mpe && (
                    <label>
                      Note bend ±
                      <input
                        type="number"
                        min={1}
                        max={96}
                        value={d.memberBendRange}
                        onChange={(e) => actions.configureMidi(d.identifier, { memberBendRange: Number(e.target.value) })}
                      />
                    </label>
                  )}
                  <label>
                    {d.mpe ? 'Zone bend ±' : 'Pitch bend ±'}
                    <input
                      type="number"
                      min={1}
                      max={96}
                      value={d.channelBendRange}
                      onChange={(e) => actions.configureMidi(d.identifier, { channelBendRange: Number(e.target.value) })}
                    />
                  </label>
                </div>
              </div>
            ))}
          </Menu>
        </>
      )}
    </>
  );
}
