import { useEffect, useRef, useState } from 'react';
import * as bridge from '../bridge/bridge';
import { useDawg } from '../store/store';
import { Button } from '../ui/controls';
import { Dialog } from '../ui/overlays';

// Normalise (docs/design/audio-features.md, AU2): sets the clips' gain so their sample peak reaches a target. The
// owner asked for the target in a dialog each time; the last one is remembered.

const storageKey = 'dawg.normaliseTarget';

/** The target last used, 0 dBFS at first. */
export function lastNormaliseTarget(): number {
  try {
    const saved = Number(window.localStorage.getItem(storageKey));
    return Number.isFinite(saved) && saved <= 0 && saved >= -60 ? saved : 0;
  } catch {
    return 0;
  }
}

/** Parses the target field: a level in dBFS from -60 to 0, or null. */
export function parseTarget(text: string): number | null {
  const value = Number(text.trim().replace(/\s*dB(FS)?$/i, '').replace(',', '.'));
  return text.trim() !== '' && Number.isFinite(value) && value <= 0 && value >= -60 ? value : null;
}

export function NormaliseDialog() {
  const clipIds = useDawg((s) => s.normalise);
  const [text, setText] = useState('0');
  const [together, setTogether] = useState(false);
  const input = useRef<HTMLInputElement>(null);

  useEffect(() => {
    if (!clipIds) return;
    setText(String(lastNormaliseTarget()));
    setTogether(false);
    input.current?.select();
  }, [clipIds]);

  if (!clipIds) return null;

  const target = parseTarget(text);
  const close = () => useDawg.setState({ normalise: null });
  const apply = () => {
    if (target === null) return;
    close();
    try {
      window.localStorage.setItem(storageKey, String(target));
    } catch {
      // Storage may be unavailable; the default comes back next time.
    }
    bridge.call('audio.normaliseClips', { clipIds, targetDb: target, together }).catch(() => {});
  };

  return (
    <Dialog onClose={() => close()} aria-labelledby="normalise-title">
      <h3 id="normalise-title">Normalise {clipIds.length === 1 ? 'clip' : `${clipIds.length} clips`}</h3>
      <p>Sets the clip gain so the loudest sample reaches the target. Clip gain goes up to +12 dB.</p>
      <label className="modal-field">
        Peak target (dBFS)
        <input
          ref={input}
          className="search"
          type="text"
          inputMode="decimal"
          value={text}
          aria-invalid={target === null}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => e.key === 'Enter' && apply()}
        />
      </label>
      {clipIds.length > 1 && (
        <label className="export-row">
          <input type="checkbox" checked={together} onChange={(e) => setTogether(e.target.checked)} />
          <span>Together: one change for all, keeping their balance</span>
        </label>
      )}
      <div className="modal-buttons">
        <Button onClick={close}>
          Cancel
        </Button>
        <span className="spacer" />
        <Button className="primary" disabled={target === null} onClick={apply}>
          Normalise
        </Button>
      </div>
    </Dialog>
  );
}
