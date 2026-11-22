import { useEffect, useMemo, useState } from 'react';
import type { KeyChange } from '../bridge/protocol';
import { SCALES } from '../music/scales';
import { font, readTheme } from '../canvas/theme';
import { pitchName, tickToX, xToTick, type View } from '../pianoroll/geometry';
import { useCanvas } from '../canvas/useCanvas';
import { dispatch } from '../store/store';
import type { TempoMap } from '../time/tempoMap';
import { CornerAdd, nearestBar, playheadBar } from './SignatureStrip';
import { Button } from '../ui/controls';
import { Dialog } from '../ui/overlays';

// The key strip under the signatures (docs/design/scale-highlighting.md, SH9): a flag like "D minor" on each bar where
// the key changes. Double-click adds one, click edits it, drag moves it to another bar, right-click removes it. The
// piano roll shows each stretch in its key, and Scale Lock follows the key where a note plays.

const CHIP_PAD = 4;
const ROOT_NAMES = Array.from({ length: 12 }, (_, i) => pitchName(60 + i).replace(/-?\d+$/, ''));

/** "D Natural minor", or "No key". */
export const keyLabel = (key: Omit<KeyChange, 'bar'>) =>
  key.enabled ? `${ROOT_NAMES[key.root]} ${SCALES[key.scale]?.name ?? ''}`.trim() : 'No key';

export function KeyStrip({ view, map, keys }: { view: View; map: TempoMap; keys: KeyChange[] }) {
  const canvas = useCanvas();
  const theme = useMemo(readTheme, []);
  const [drag, setDrag] = useState<{ change: KeyChange; to: number; startX: number; moved: boolean } | null>(null);
  const [editing, setEditing] = useState<{ change: KeyChange; isNew: boolean } | null>(null);

  // Flags and their chips' extents, for drawing and hit testing.
  const flags = (ctx?: CanvasRenderingContext2D) =>
    keys.map((change) => {
      const bar = drag?.change.bar === change.bar ? drag.to : change.bar;
      const x = Math.round(tickToX(view, map.tickOfBar(bar))) - 0.5;
      const width = (ctx?.measureText(keyLabel(change)).width ?? 48) + 2 * CHIP_PAD;
      return { change, x, width };
    });

  useEffect(() => {
    const ctx = canvas.context();
    if (!ctx) return;
    const { width, height } = canvas.size;
    ctx.fillStyle = theme.bg;
    ctx.fillRect(0, 0, width, height);
    ctx.font = font(readTheme(), 'xs', { mono: true });
    ctx.textBaseline = 'middle';
    for (const flag of flags(ctx)) {
      if (flag.x > width || flag.x + flag.width < 0) continue;
      // The first key without a key set is only drawn faintly, so an empty strip reads as "no key".
      const quiet = flag.change.bar === 0 && !flag.change.enabled;
      const active = drag?.change.bar === flag.change.bar || editing?.change.bar === flag.change.bar;
      ctx.fillStyle = theme.accent;
      ctx.globalAlpha = quiet ? 0.4 : 1;
      ctx.fillRect(flag.x, 0, 1.5, height);
      ctx.globalAlpha = active ? 0.45 : quiet ? 0.08 : 0.22;
      ctx.fillRect(flag.x + 1.5, 2, flag.width, height - 4);
      ctx.globalAlpha = quiet ? 0.6 : 1;
      ctx.fillStyle = active ? theme.text : theme.accent;
      ctx.fillText(keyLabel(flag.change), flag.x + 1.5 + CHIP_PAD, height / 2 + 0.5);
      ctx.globalAlpha = 1;
    }
    ctx.fillStyle = theme.border;
    ctx.fillRect(0, height - 1, width, 1);
  });

  const flagAt = (x: number) => {
    const ctx = canvas.context();
    if (ctx) ctx.font = font(readTheme(), 'xs', { mono: true });
    return flags(ctx ?? undefined)
      .reverse()
      .find((f) => x >= f.x - 3 && x <= f.x + f.width + 2);
  };
  const barAtX = (x: number) => Math.max(0, nearestBar(map, Math.max(0, xToTick(view, x))));

  return (
    <>
      <div className="arrangement-corner marker-label">
        <span>Key</span>
        <span className="spacer" />
        <CornerAdd
          title="Add a key change at the playhead (or double-click the strip)"
          onClick={() => {
            const bar = playheadBar(map);
            const current = [...keys].reverse().find((k) => k.bar <= bar) ?? keys[0];
            setEditing({ change: { ...current, enabled: true, bar }, isNew: bar !== 0 });
          }}
        />
      </div>
      <canvas
        ref={canvas.ref}
        className="signature-strip key-strip"
        title="Keys: double-click to add a key change · click to edit · drag to another bar · right-click to remove"
        onPointerDown={(e) => {
          if (e.button !== 0) return;
          const flag = flagAt(e.nativeEvent.offsetX);
          if (!flag) return;
          e.currentTarget.setPointerCapture(e.pointerId);
          setDrag({ change: flag.change, to: flag.change.bar, startX: e.nativeEvent.offsetX, moved: false });
        }}
        onPointerMove={(e) => {
          if (!drag || e.buttons !== 1) return;
          // The first key stays on bar 1.
          if (drag.change.bar === 0 || (!drag.moved && Math.abs(e.nativeEvent.offsetX - drag.startX) < 4)) return;
          setDrag({ ...drag, to: Math.max(1, barAtX(e.nativeEvent.offsetX)), moved: true });
        }}
        onPointerUp={() => {
          if (!drag) return;
          if (!drag.moved) setEditing({ change: drag.change, isNew: false });
          else if (drag.to !== drag.change.bar) dispatch({ command: 'moveKey', args: { from: drag.change.bar, to: drag.to } });
          setDrag(null);
        }}
        onDoubleClick={(e) => {
          if (flagAt(e.nativeEvent.offsetX)) return;
          const bar = barAtX(e.nativeEvent.offsetX);
          const current = [...keys].reverse().find((k) => k.bar <= bar) ?? keys[0];
          setEditing({ change: { ...current, enabled: true, bar }, isNew: bar !== 0 });
        }}
        onContextMenu={(e) => {
          e.preventDefault();
          const flag = flagAt(e.nativeEvent.offsetX);
          if (flag && flag.change.bar > 0) dispatch({ command: 'removeKey', args: { bar: flag.change.bar } });
        }}
      />
      {editing && (
        <KeyPrompt
          change={editing.change}
          isNew={editing.isNew}
          onDone={(result) => {
            setEditing(null);
            if (result === 'remove') dispatch({ command: 'removeKey', args: { bar: editing.change.bar } });
            else if (result) dispatch({ command: 'setKey', args: result });
          }}
        />
      )}
    </>
  );
}

/** Edits a key change: no key, or a root and a scale. Enter applies, Escape cancels. */
function KeyPrompt({
  change,
  isNew,
  onDone,
}: {
  change: KeyChange;
  isNew: boolean;
  onDone: (result: KeyChange | 'remove' | null) => void;
}) {
  const [enabled, setEnabled] = useState(change.enabled);
  const [root, setRoot] = useState(change.root);
  const [scale, setScale] = useState(change.scale);

  return (
    <Dialog
      onClose={() => onDone(null)}
      className="signature-prompt"
      onSubmit={(e) => {
        e.preventDefault();
        onDone({ bar: change.bar, enabled, root, scale });
      }}
    >
      <h3>
        {isNew ? 'New key' : 'Key'} {change.bar > 0 ? `from bar ${change.bar + 1}` : 'from the start'}
      </h3>
      <div className="signature-fields">
        <label className="modal-field">
          <span>Root</span>
          <select
            autoFocus
            value={enabled ? String(root) : 'off'}
            onChange={(e) => {
              setEnabled(e.target.value !== 'off');
              if (e.target.value !== 'off') setRoot(Number(e.target.value));
            }}
          >
            <option value="off">No key</option>
            {ROOT_NAMES.map((name, index) => (
              <option key={name} value={index}>
                {name}
              </option>
            ))}
          </select>
        </label>
        <label className="modal-field">
          <span>Scale</span>
          <select disabled={!enabled} value={scale} onChange={(e) => setScale(Number(e.target.value))}>
            {SCALES.map((s, index) => (
              <option key={s.name} value={index}>
                {s.name}
              </option>
            ))}
          </select>
        </label>
      </div>
      <div className="modal-buttons">
        {!isNew && change.bar > 0 && (
          <Button type="button" className="danger" onClick={() => onDone('remove')}>
            Remove
          </Button>
        )}
        <span className="spacer" />
        <Button type="button" onClick={() => onDone(null)}>
          Cancel
        </Button>
        <Button type="submit" className="primary">
          {isNew ? 'Add' : 'Apply'}
        </Button>
      </div>
    </Dialog>
  );
}
