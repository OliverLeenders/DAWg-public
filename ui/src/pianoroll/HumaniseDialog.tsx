import { Drum, Music, Sparkles, X, SlidersHorizontal, RotateCcw } from 'lucide-react';
import { useState } from 'react';
import { type Track } from '../bridge/protocol';
import { dispatch } from '../store/store';
import { useUi, ui } from '../store/uiStore';
import type { TempoMap } from '../time/tempoMap';
import { updateNotesCommand, toEditNote, type EditNote } from './edit';
import type { PlacedNote } from './geometry';
import {
  BUILTIN_DRUM_MAPS,
  DEFAULT_DRUM_MAP,
  drumSynthMap,
  explicitDrumMode,
  isDrumTrack,
  type DrumMap,
  type DrumPiece,
  type DrumRole,
  type DrumLimb,
} from '../store/drumMap';
import {
  DEFAULT_DRUM_OPTIONS,
  DEFAULT_MELODIC_OPTIONS,
  humaniseDrums,
  humaniseMelodic,
  type DrumHumaniseOptions,
  type MelodicHumaniseOptions,
} from './humanise';
import { Button, IconButton } from '../ui/controls';
import { Dialog } from '../ui/overlays';

interface Props {
  track: Track;
  notes: PlacedNote[];
  selectedNotes: ReadonlySet<number>;
  map: TempoMap;
  onClose: () => void;
}

const DRUM_ROLES: Array<{ id: DrumRole; label: string }> = [
  { id: 'kick', label: 'Kick drum' },
  { id: 'snare', label: 'Snare' },
  { id: 'sidestick', label: 'Side stick' },
  { id: 'clap', label: 'Clap' },
  { id: 'hihat_closed', label: 'Closed hi-hat' },
  { id: 'hihat_pedal', label: 'Pedal hi-hat' },
  { id: 'hihat_open', label: 'Open hi-hat' },
  { id: 'crash', label: 'Crash cymbal' },
  { id: 'ride', label: 'Ride cymbal' },
  { id: 'ride_bell', label: 'Ride bell' },
  { id: 'tom_floor', label: 'Floor tom' },
  { id: 'tom_low', label: 'Low tom' },
  { id: 'tom_mid', label: 'Mid tom' },
  { id: 'tom_high', label: 'High tom' },
  { id: 'percussion', label: 'Percussion / other' },
];

const DRUM_LIMBS: Array<{ id: DrumLimb; label: string }> = [
  { id: 'right_foot', label: 'Right foot (kick)' },
  { id: 'left_foot', label: 'Left foot (pedal)' },
  { id: 'right_hand', label: 'Right hand (lead)' },
  { id: 'left_hand', label: 'Left hand (snare)' },
  { id: 'other', label: 'Other / either' },
];

export function HumaniseDialog({ track, notes, selectedNotes, map, onClose }: Props) {
  const activeDrumMapId = useUi((s) => s.activeDrumMapId);
  const customDrumMaps = useUi((s) => s.customDrumMaps);

  // Auto-detect drum mode or use explicit track toggle
  const initialIsDrum = isDrumTrack(track.name, explicitDrumMode(track));
  const [mode, setMode] = useState<'drums' | 'melodic'>(initialIsDrum ? 'drums' : 'melodic');

  // Active drum map
  const activeMap: DrumMap =
    drumSynthMap(track) ??
    customDrumMaps[activeDrumMapId] ??
    BUILTIN_DRUM_MAPS.find((m) => m.id === activeDrumMapId) ??
    DEFAULT_DRUM_MAP;

  const [drumOptions, setDrumOptions] = useState<DrumHumaniseOptions>(DEFAULT_DRUM_OPTIONS(activeMap));
  const [melodicOptions, setMelodicOptions] = useState<MelodicHumaniseOptions>(DEFAULT_MELODIC_OPTIONS);
  const [seed, setSeed] = useState(() => Math.floor(Math.random() * 100000));
  const [showMapEditor, setShowMapEditor] = useState(false);

  // Notes to act on: selection if any, otherwise all notes in the active clip/track
  const targetPlaced = notes.filter((n) => selectedNotes.size === 0 || selectedNotes.has(n.note.id));
  const targetEditNotes: EditNote[] = targetPlaced.map(toEditNote);

  // Presets
  const applyDrumPreset = (name: string) => {
    setSeed(Math.floor(Math.random() * 100000));
    switch (name) {
      case 'tight':
        setDrumOptions((o) => ({
          ...o,
          tightnessMs: 4,
          snarePocketMs: -2,
          hihatLeadMs: 3,
          kickTightness: 0.2,
          velocityDynamics: 0.25,
        }));
        break;
      case 'dilla':
        setDrumOptions((o) => ({
          ...o,
          tightnessMs: 14,
          snarePocketMs: -12,
          hihatLeadMs: 6,
          kickTightness: 0.45,
          velocityDynamics: 0.45,
        }));
        break;
      case 'rock':
        setDrumOptions((o) => ({
          ...o,
          tightnessMs: 6,
          snarePocketMs: 4,
          hihatLeadMs: 2,
          kickTightness: 0.2,
          velocityDynamics: 0.35,
        }));
        break;
      case 'loose':
        setDrumOptions((o) => ({
          ...o,
          tightnessMs: 16,
          snarePocketMs: -5,
          hihatLeadMs: 5,
          kickTightness: 0.4,
          velocityDynamics: 0.5,
        }));
        break;
    }
  };

  const applyMelodicPreset = (name: string) => {
    setSeed(Math.floor(Math.random() * 100000));
    switch (name) {
      case 'subtle':
        setMelodicOptions((o) => ({
          ...o,
          timingMs: 4,
          velocityVariance: 0.2,
          chordFlamMs: 3,
          drift: 0.15,
        }));
        break;
      case 'expressive':
        setMelodicOptions((o) => ({
          ...o,
          timingMs: 8,
          velocityVariance: 0.35,
          chordFlamMs: 7,
          drift: 0.3,
        }));
        break;
      case 'loose':
        setMelodicOptions((o) => ({
          ...o,
          timingMs: 15,
          velocityVariance: 0.5,
          chordFlamMs: 12,
          drift: 0.5,
        }));
        break;
    }
  };

  const apply = () => {
    if (targetEditNotes.length === 0) return onClose();
    const humanised =
      mode === 'drums'
        ? humaniseDrums(targetEditNotes, { ...drumOptions, drumMap: activeMap }, map, seed)
        : humaniseMelodic(targetEditNotes, melodicOptions, map, seed);

    const command = updateNotesCommand(track, humanised, map, mode === 'drums' ? 'Humanise drums' : 'Humanise notes');
    if (command) dispatch(command);
    onClose();
  };

  const presentPitches = [...new Set(targetPlaced.map((n) => n.note.pitch))].sort((a, b) => a - b);

  return (
    <Dialog className="humanise-dialog" style={{ maxWidth: 540 }} onClose={onClose} closeOnBackdrop aria-label="Humanise notes">
      <div className="modal-header">
        <div className="modal-title">
          <Sparkles size={18} style={{ color: track.colour }} />
          <span>Humanise notes</span>
        </div>
        <IconButton size="l" onClick={onClose} title="Close">
          <X size={16} />
        </IconButton>
      </div>

      {/* Mode selector */}
      <div className="humanise-mode-tabs" style={{ display: 'flex', gap: 8, marginBottom: 14 }}>
        <Button
          className={`${mode === 'drums' ? 'primary' : ''}`}
          style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6 }}
          onClick={() => {
            setMode('drums');
            dispatch({ command: 'setTrackDrumMode', args: { id: track.id, mode: 'drums' } });
          }}
        >
          <Drum size={15} />
          <span>Drum part</span>
        </Button>
        <Button
          className={`${mode === 'melodic' ? 'primary' : ''}`}
          style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6 }}
          onClick={() => {
            setMode('melodic');
            dispatch({ command: 'setTrackDrumMode', args: { id: track.id, mode: 'melodic' } });
          }}
        >
          <Music size={15} />
          <span>Melodic</span>
        </Button>
      </div>

      {mode === 'drums' ? (
        <div className="humanise-content" style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          {/* Drum Map Selector */}
          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 10 }}>
            <label className="field" style={{ flex: 1 }}>
              <span>Drum map</span>
              <select
                value={activeDrumMapId}
                onChange={(e) => {
                  const id = e.target.value;
                  ui.setActiveDrumMapId(id);
                  const chosen =
                    customDrumMaps[id] ?? BUILTIN_DRUM_MAPS.find((m) => m.id === id) ?? DEFAULT_DRUM_MAP;
                  setDrumOptions((o) => ({ ...o, drumMap: chosen }));
                }}
              >
                {BUILTIN_DRUM_MAPS.map((m) => (
                  <option key={m.id} value={m.id}>
                    {m.name}
                  </option>
                ))}
                {Object.values(customDrumMaps).map((m) =>
                  BUILTIN_DRUM_MAPS.some((b) => b.id === m.id) ? null : (
                    <option key={m.id} value={m.id}>
                      {m.name} (Custom)
                    </option>
                  ),
                )}
              </select>
            </label>
            <Button
              className={`${showMapEditor ? 'primary' : ''}`}
              style={{ marginTop: 14 }}
              onClick={() => setShowMapEditor(!showMapEditor)}
              title="View and edit drum note mappings"
            >
              <SlidersHorizontal size={13} style={{ marginRight: 4 }} />
              {showMapEditor ? 'Hide mapping' : 'Edit mapping'}
            </Button>
          </div>

          {/* Collapsible Drum Map Editor */}
          {showMapEditor && (
            <div
              style={{
                background: 'var(--lcd)',
                border: '1px solid var(--border)',
                borderRadius: 6,
                padding: 10,
                maxHeight: 180,
                overflowY: 'auto',
              }}
            >
              <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-dim)', marginBottom: 6 }}>
                Pitches in this clip ({presentPitches.length}):
              </div>
              {presentPitches.length === 0 ? (
                <div style={{ fontSize: 11, color: 'var(--text-dim)' }}>No notes in clip.</div>
              ) : (
                <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
                  {presentPitches.map((pitch) => {
                    const piece = activeMap.pieces[pitch] ?? {
                      pitch,
                      name: `MIDI ${pitch}`,
                      role: 'percussion' as DrumRole,
                      limb: 'other' as DrumLimb,
                    };
                    return (
                      <div
                        key={pitch}
                        style={{
                          display: 'flex',
                          alignItems: 'center',
                          gap: 6,
                          fontSize: 11,
                        }}
                      >
                        <span style={{ width: 28, fontFamily: 'monospace', color: 'var(--accent)' }}>
                          {pitch}
                        </span>
                        <input
                          type="text"
                          value={piece.name}
                          style={{ flex: 1, padding: '2px 6px', fontSize: 11 }}
                          onChange={(e) => {
                            const updated: DrumPiece = { ...piece, name: e.target.value };
                            ui.setCustomDrumPiece(activeMap.id, pitch, updated);
                          }}
                        />
                        <select
                          value={piece.role}
                          style={{ fontSize: 11, padding: '2px 4px' }}
                          onChange={(e) => {
                            const updated: DrumPiece = { ...piece, role: e.target.value as DrumRole };
                            ui.setCustomDrumPiece(activeMap.id, pitch, updated);
                          }}
                        >
                          {DRUM_ROLES.map((r) => (
                            <option key={r.id} value={r.id}>
                              {r.label}
                            </option>
                          ))}
                        </select>
                        <select
                          value={piece.limb}
                          style={{ fontSize: 11, padding: '2px 4px' }}
                          onChange={(e) => {
                            const updated: DrumPiece = { ...piece, limb: e.target.value as DrumLimb };
                            ui.setCustomDrumPiece(activeMap.id, pitch, updated);
                          }}
                        >
                          {DRUM_LIMBS.map((l) => (
                            <option key={l.id} value={l.id}>
                              {l.label}
                            </option>
                          ))}
                        </select>
                      </div>
                    );
                  })}
                </div>
              )}
            </div>
          )}

          {/* Presets */}
          <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
            <span style={{ fontSize: 11, color: 'var(--text-dim)', alignSelf: 'center', marginRight: 4 }}>
              Presets:
            </span>
            <Button onClick={() => applyDrumPreset('tight')}>
              Tight pocket
            </Button>
            <Button onClick={() => applyDrumPreset('dilla')}>
              Laid-back neo-soul
            </Button>
            <Button onClick={() => applyDrumPreset('rock')}>
              Driving rock
            </Button>
            <Button onClick={() => applyDrumPreset('loose')}>
              Loose live
            </Button>
          </div>

          {/* Sliders */}
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
            <label className="field">
              <span>Timing tightness (±{drumOptions.tightnessMs} ms)</span>
              <input
                type="range"
                min="1"
                max="28"
                value={drumOptions.tightnessMs}
                onChange={(e) => setDrumOptions({ ...drumOptions, tightnessMs: Number(e.target.value) })}
              />
            </label>

            <label className="field">
              <span>
                Snare pocket ({drumOptions.snarePocketMs > 0 ? `+${drumOptions.snarePocketMs}` : drumOptions.snarePocketMs} ms{' '}
                {drumOptions.snarePocketMs < 0 ? 'behind' : drumOptions.snarePocketMs > 0 ? 'ahead' : 'on-grid'})
              </span>
              <input
                type="range"
                min="-18"
                max="12"
                value={drumOptions.snarePocketMs}
                onChange={(e) => setDrumOptions({ ...drumOptions, snarePocketMs: Number(e.target.value) })}
              />
            </label>

            <label className="field">
              <span>Hi-hat / snare lead ({drumOptions.hihatLeadMs} ms)</span>
              <input
                type="range"
                min="0"
                max="10"
                value={drumOptions.hihatLeadMs}
                onChange={(e) => setDrumOptions({ ...drumOptions, hihatLeadMs: Number(e.target.value) })}
              />
            </label>

            <label className="field">
              <span>Kick grid lock ({Math.round((1 - drumOptions.kickTightness) * 100)}%)</span>
              <input
                type="range"
                min="0.1"
                max="1.0"
                step="0.05"
                value={drumOptions.kickTightness}
                onChange={(e) => setDrumOptions({ ...drumOptions, kickTightness: Number(e.target.value) })}
              />
            </label>
          </div>

          <label className="field">
            <span>Velocity dynamics ({Math.round(drumOptions.velocityDynamics * 100)}%)</span>
            <input
              type="range"
              min="0"
              max="1"
              step="0.05"
              value={drumOptions.velocityDynamics}
              onChange={(e) => setDrumOptions({ ...drumOptions, velocityDynamics: Number(e.target.value) })}
            />
          </label>

          {/* Checkboxes */}
          <div style={{ display: 'flex', gap: 16, fontSize: 12 }}>
            <label style={{ display: 'flex', alignItems: 'center', gap: 6, cursor: 'pointer' }}>
              <input
                type="checkbox"
                checked={drumOptions.hihatAlternation}
                onChange={(e) => setDrumOptions({ ...drumOptions, hihatAlternation: e.target.checked })}
              />
              Alternating hat accents
            </label>
            <label style={{ display: 'flex', alignItems: 'center', gap: 6, cursor: 'pointer' }}>
              <input
                type="checkbox"
                checked={drumOptions.ghostLooseTiming}
                onChange={(e) => setDrumOptions({ ...drumOptions, ghostLooseTiming: e.target.checked })}
              />
              Loose ghost notes
            </label>
          </div>
        </div>
      ) : (
        <div className="humanise-content" style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          {/* Presets */}
          <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
            <span style={{ fontSize: 11, color: 'var(--text-dim)', alignSelf: 'center', marginRight: 4 }}>
              Presets:
            </span>
            <Button onClick={() => applyMelodicPreset('subtle')}>
              Subtle feel
            </Button>
            <Button onClick={() => applyMelodicPreset('expressive')}>
              Expressive keys
            </Button>
            <Button onClick={() => applyMelodicPreset('loose')}>
              Loose rubato
            </Button>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
            <label className="field">
              <span>Timing variance (±{melodicOptions.timingMs} ms)</span>
              <input
                type="range"
                min="1"
                max="28"
                value={melodicOptions.timingMs}
                onChange={(e) => setMelodicOptions({ ...melodicOptions, timingMs: Number(e.target.value) })}
              />
            </label>

            <label className="field">
              <span>Chord strum / flam ({melodicOptions.chordFlamMs} ms)</span>
              <input
                type="range"
                min="0"
                max="20"
                value={melodicOptions.chordFlamMs}
                onChange={(e) => setMelodicOptions({ ...melodicOptions, chordFlamMs: Number(e.target.value) })}
              />
            </label>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
            <label className="field">
              <span>Dynamic velocity ({Math.round(melodicOptions.velocityVariance * 100)}%)</span>
              <input
                type="range"
                min="0"
                max="1"
                step="0.05"
                value={melodicOptions.velocityVariance}
                onChange={(e) => setMelodicOptions({ ...melodicOptions, velocityVariance: Number(e.target.value) })}
              />
            </label>

            <label className="field">
              <span>Bar tempo drift ({Math.round(melodicOptions.drift * 100)}%)</span>
              <input
                type="range"
                min="0"
                max="1"
                step="0.05"
                value={melodicOptions.drift}
                onChange={(e) => setMelodicOptions({ ...melodicOptions, drift: Number(e.target.value) })}
              />
            </label>
          </div>

          <label style={{ display: 'flex', alignItems: 'center', gap: 6, fontSize: 12, cursor: 'pointer' }}>
            <input
              type="checkbox"
              checked={melodicOptions.downbeatAnchor}
              onChange={(e) => setMelodicOptions({ ...melodicOptions, downbeatAnchor: e.target.checked })}
            />
            Anchor Downbeats (tighten timing on bar starts and main beats)
          </label>
        </div>
      )}

      {/* Action bar */}
      <div
        className="modal-buttons"
        style={{
          marginTop: 18,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          borderTop: '1px solid var(--border)',
          paddingTop: 12,
        }}
      >
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <span style={{ fontSize: 11, color: 'var(--text-dim)' }}>
            {selectedNotes.size > 0
              ? `${selectedNotes.size} selected notes`
              : `${targetPlaced.length} notes in clip`}
          </span>
          <IconButton
            title="New random seed"
            onClick={() => setSeed(Math.floor(Math.random() * 100000))}
          >
            <RotateCcw size={13} />
          </IconButton>
        </div>

        <div style={{ display: 'flex', gap: 8 }}>
          <Button onClick={onClose}>
            Cancel
          </Button>
          <Button className="primary" onClick={apply}>
            Humanise
          </Button>
        </div>
      </div>
    </Dialog>
  );
}
