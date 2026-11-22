import { useEffect, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { TICKS_PER_BEAT } from '../bridge/protocol';
import { dispatch, reportError, useDawg } from '../store/store';
import { Button } from '../ui/controls';
import { Dialog } from '../ui/overlays';

// Slice to Sampler (docs/design/audio-features.md, AU3): cuts an audio clip at its transients, the grid or its timing
// markers into a new Sampler track, one zone per slice, with a MIDI clip that plays them.

export type SliceMode = 'transients' | 'grid' | 'markers';

export const DIVISIONS = [
  { label: '1/4', ticks: TICKS_PER_BEAT },
  { label: '1/8', ticks: TICKS_PER_BEAT / 2 },
  { label: '1/16', ticks: TICKS_PER_BEAT / 4 },
  { label: '1/32', ticks: TICKS_PER_BEAT / 8 },
];

/** The most slices a Sampler takes (one per key); the start key moves down to fit them (sliceToSampler). */
export const MAX_SLICES = 128;

/** The first key the slices get: the chosen one, lower when they wouldn't fit below the top key. */
export function startKeyFor(chosen: number, slices: number): number {
  return Math.max(0, Math.min(chosen, MAX_SLICES - slices));
}

export function SliceDialog() {
  const clipId = useDawg((s) => s.slice);
  const [mode, setMode] = useState<SliceMode>('transients');
  const [sensitivity, setSensitivity] = useState(0.75);
  const [division, setDivision] = useState(TICKS_PER_BEAT / 4);
  const [startKey, setStartKey] = useState(36);
  const [gate, setGate] = useState(false);
  const [points, setPoints] = useState<number[] | null>(null);

  // The cut points for the current settings, again when the file's onsets are found.
  useEffect(() => {
    if (clipId === null) return;
    let live = true;
    const fetch = () =>
      bridge
        .call('audio.slicePoints', { clipId, mode, sensitivity, division })
        .then((reply) => live && setPoints(reply.ready ? (reply.slices ?? []) : null))
        .catch(reportError);
    fetch();
    const off = bridge.on('dawg_onsets', fetch);
    return () => {
      live = false;
      off();
    };
  }, [clipId, mode, sensitivity, division]);

  if (clipId === null) return null;

  const close = () => useDawg.setState({ slice: null });
  const slices = points ? points.length + 1 : 0;
  const tooMany = slices > MAX_SLICES;
  const apply = () => {
    if (!points || tooMany) return;
    close();
    dispatch({ command: 'sliceToSampler', args: { clipId, slices: points, startKey, mode: gate ? 'gate' : 'oneShot' } });
  };

  return (
    <Dialog onClose={() => close()} aria-labelledby="slice-title">
      <h3 id="slice-title">Slice to Sampler</h3>
      <p>Makes a Sampler track with one key per slice, and a clip that plays them where they were.</p>
      <label className="modal-field">
        Cut at
        <select value={mode} onChange={(e) => setMode(e.target.value as SliceMode)}>
          <option value="transients">Transients</option>
          <option value="grid">The grid</option>
          <option value="markers">Timing markers</option>
        </select>
      </label>
      {mode === 'transients' && (
        <label className="modal-field">
          Sensitivity {Math.round(sensitivity * 100)} %
          <input type="range" min={0} max={1} step={0.05} value={sensitivity} onChange={(e) => setSensitivity(Number(e.target.value))} />
        </label>
      )}
      {mode === 'grid' && (
        <label className="modal-field">
          Every
          <select value={division} onChange={(e) => setDivision(Number(e.target.value))}>
            {DIVISIONS.map((d) => (
              <option key={d.label} value={d.ticks}>
                {d.label}
              </option>
            ))}
          </select>
        </label>
      )}
      <label className="modal-field">
        First key (MIDI note)
        <input
          className="search"
          type="number"
          min={0}
          max={127}
          value={startKey}
          onChange={(e) => setStartKey(Math.max(0, Math.min(127, Math.round(Number(e.target.value) || 0))))}
        />
      </label>
      <label className="export-row">
        <input type="checkbox" checked={gate} onChange={(e) => setGate(e.target.checked)} />
        <span>Gate: slices stop when their notes end</span>
      </label>
      <p>
        {points === null
          ? 'Finding transients…'
          : tooMany
            ? `${slices} slices: a Sampler takes at most ${MAX_SLICES}.`
            : `${slices} slice${slices === 1 ? '' : 's'}, keys ${startKeyFor(startKey, slices)} to ${startKeyFor(startKey, slices) + slices - 1}.`}
      </p>
      <div className="modal-buttons">
        <Button onClick={close}>
          Cancel
        </Button>
        <span className="spacer" />
        <Button className="primary" disabled={!points || tooMany} onClick={apply}>
          Slice
        </Button>
      </div>
    </Dialog>
  );
}
