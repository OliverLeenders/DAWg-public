import { AudioWaveform, SlidersHorizontal } from 'lucide-react';
import { useState } from 'react';
import { TICKS_PER_BEAT, type Track } from '../bridge/protocol';
import { DragNumber } from '../components/DragNumber';
import { newGestureId } from '../store/store';
import { NO_VIBRATO, type PlacedNote } from './geometry';
import type { TempoMap } from '../time/tempoMap';
import {
  applyVibrato,
  hasVibrato,
  setVibratoIntensity,
  VIBRATO_LIMITS,
  VIBRATO_SHAPES,
  VIBRATO_SYNC_OPTIONS,
  vibratoOf,
} from './vibrato';
import { IconButton } from '../ui/controls';
import { Popover } from '../ui/overlays';

interface Props {
  track: Track;
  /** The selected notes; the inspector shows the first one and edits all of them. */
  notes: PlacedNote[];
  map: TempoMap;
}

/** Compact vibrato controls for the selected notes, shown in the lane tab bar. */
export function VibratoInspector({ track, notes, map }: Props) {
  // Always rendered (disabled without a selection) so the toolbar layout never shifts when selecting; its settings open
  // in a popover.
  const disabled = notes.length === 0;
  const [open, setOpen] = useState(false);
  const v = disabled ? NO_VIBRATO : vibratoOf(notes[0]);
  const anyOn = notes.some((n) => hasVibrato(n.note));
  const set = (change: Parameters<typeof applyVibrato>[2], gesture?: string) =>
    applyVibrato(track, notes, change, map, gesture);
  const beats = (ticks: number) => ticks / TICKS_PER_BEAT;
  const sync = VIBRATO_SYNC_OPTIONS.find((o) => Math.abs(o.beats - v.syncBeats) < 1e-6) ?? VIBRATO_SYNC_OPTIONS[0];

  return (
    <fieldset
      className="vibrato-inspector"
      disabled={disabled}
      title={disabled ? 'Select notes to edit their vibrato' : notes.length > 1 ? `Editing ${notes.length} notes` : undefined}
    >
      {/* Says what the controls edit, the selected notes' vibrato (docs/design/ux-review.md, U20); the fieldset's
          tooltip says how many. */}
      <span className="toolbar-group-label">Vibrato</span>
      <IconButton
        className={`${anyOn ? 'active' : ''}`}
        title={anyOn ? 'Vibrato on (intensity lane) — click to turn off' : 'Vibrato off — click to turn on at full intensity'}
        onClick={() => setVibratoIntensity(notes, !anyOn)}
      >
        <AudioWaveform size={14} />
      </IconButton>
      {/* The settings open below the toolbar rather than filling it (docs/ui-review-2026-11-10.md, 3.12). */}
      <IconButton
        className={`${open ? 'active' : ''}`}
        title={disabled ? 'Select notes to edit their vibrato' : 'Vibrato settings: depth, rate, shape, delay and fade'}
        aria-expanded={open}
        onClick={() => setOpen(!open)}
      >
        <SlidersHorizontal size={14} />
      </IconButton>
      {open && !disabled && (
        <>
          <Popover onClose={() => setOpen(false)} className="toolbar-popover" onPointerDown={(e) => e.stopPropagation()}>
            <label className="field">
              <span>Depth</span>
              <DragNumber
                className="lcd small"
                value={v.depth}
                min={VIBRATO_LIMITS.minDepth}
                max={VIBRATO_LIMITS.maxDepth}
                quantum={0.01}
                format={(d) => `${d.toFixed(2)} st`}
                onChange={(depth, gesture) => set({ depth }, gesture)}
              />
            </label>
            <label className="field">
              <span>Rate</span>
              <select
                value={sync.label}
                onChange={(e) =>
                  set({ syncBeats: VIBRATO_SYNC_OPTIONS.find((o) => o.label === e.target.value)?.beats ?? 0 }, newGestureId())
                }
              >
                {VIBRATO_SYNC_OPTIONS.map((o) => (
                  <option key={o.label}>{o.label}</option>
                ))}
              </select>
              {v.syncBeats === 0 && (
                <DragNumber
                  className="lcd small"
                  value={v.rateHz}
                  min={VIBRATO_LIMITS.minRate}
                  max={VIBRATO_LIMITS.maxRate}
                  quantum={0.01}
                  format={(r) => `${r.toFixed(2)} Hz`}
                  onChange={(rateHz, gesture) => set({ rateHz }, gesture)}
                />
              )}
            </label>
            <label className="field">
              <span>Shape</span>
              <select
                value={v.shape}
                onChange={(e) => set({ shape: Number(e.target.value) as 0 | 1 | 2 | 3 | 4 }, newGestureId())}
              >
                {VIBRATO_SHAPES.map((name, i) => (
                  <option key={name} value={i}>
                    {name}
                  </option>
                ))}
              </select>
            </label>
            <label className="field">
              <span>Delay</span>
              <DragNumber
                className="lcd small"
                value={beats(v.delay)}
                min={0}
                max={VIBRATO_LIMITS.maxBeats}
                quantum={0.01}
                format={(b) => `${b.toFixed(2)} b`}
                onChange={(b, gesture) => set({ delay: Math.round(b * TICKS_PER_BEAT) }, gesture)}
              />
            </label>
            <label className="field">
              <span>Fade</span>
              <DragNumber
                className="lcd small"
                value={beats(v.fadeIn)}
                min={0}
                max={VIBRATO_LIMITS.maxBeats}
                quantum={0.01}
                format={(b) => `${b.toFixed(2)} b`}
                onChange={(b, gesture) => set({ fadeIn: Math.round(b * TICKS_PER_BEAT) }, gesture)}
              />
            </label>
          </Popover>
        </>
      )}
    </fieldset>
  );
}
