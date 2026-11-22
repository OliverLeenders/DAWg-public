import { Plus } from 'lucide-react';
import { useEffect, useMemo, useState } from 'react';
import { TICKS_PER_BEAT, type TimeSignature } from '../bridge/protocol';
import { font, readTheme } from '../canvas/theme';
import { tickToX, xToTick, type View } from '../pianoroll/geometry';
import { useCanvas } from '../canvas/useCanvas';
import { dispatch, useDawg } from '../store/store';
import type { TempoMap } from '../time/tempoMap';
import { Button, IconButton } from '../ui/controls';
import { Dialog } from '../ui/overlays';

// The signature strip under the ruler (docs/design/tempo-map.md, TM12): a flag like "7/8" on each bar where the time
// signature changes. Double-click adds one, click edits it, drag moves it to another bar, right-click removes it.

const CHIP_PAD = 4;
const DENOMINATORS = [2, 4, 8, 16];

/** The bar line nearest a tick. */
export function nearestBar(map: TempoMap, tick: number) {
  const bar = map.barAt(tick).bar;
  return tick - map.tickOfBar(bar) <= map.tickOfBar(bar + 1) - tick ? bar : bar + 1;
}

const label = (s: TimeSignature) => `${s.numerator}/${s.denominator}`;

export function SignatureStrip({ view, map, corner }: { view: View; map: TempoMap; corner: React.ReactNode }) {
  const canvas = useCanvas();
  const theme = useMemo(readTheme, []);
  const [drag, setDrag] = useState<{ signature: TimeSignature; to: number; startX: number; moved: boolean } | null>(null);
  const [editing, setEditing] = useState<{ signature: TimeSignature; isNew: boolean } | null>(null);

  // Flags and their chips' extents, for drawing and hit testing.
  const flags = (ctx?: CanvasRenderingContext2D) =>
    map.signatures.map((signature) => {
      const bar = drag?.signature.bar === signature.bar ? drag.to : signature.bar;
      const x = Math.round(tickToX(view, map.tickOfBar(bar))) - 0.5;
      const width = (ctx?.measureText(label(signature)).width ?? 24) + 2 * CHIP_PAD;
      return { signature, x, width };
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
      const active = drag?.signature.bar === flag.signature.bar || editing?.signature.bar === flag.signature.bar;
      ctx.fillStyle = theme.accent;
      ctx.fillRect(flag.x, 0, 1.5, height);
      ctx.globalAlpha = active ? 0.45 : 0.22;
      ctx.fillRect(flag.x + 1.5, 2, flag.width, height - 4);
      ctx.globalAlpha = 1;
      ctx.fillStyle = active ? theme.text : theme.accent;
      ctx.fillText(label(flag.signature), flag.x + 1.5 + CHIP_PAD, height / 2 + 0.5);
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
        {corner}
        <CornerAdd
          title="Add a time signature at the playhead (or double-click the strip)"
          onClick={() => {
            const bar = playheadBar(map);
            setEditing({ signature: { ...map.signatureAt(map.tickOfBar(bar)), bar }, isNew: bar !== 0 });
          }}
        />
      </div>
      <canvas
        ref={canvas.ref}
        className="signature-strip"
        title="Time signatures: double-click to add · click to edit · drag to another bar · right-click to remove"
        onPointerDown={(e) => {
          if (e.button !== 0) return;
          const flag = flagAt(e.nativeEvent.offsetX);
          if (!flag) return;
          e.currentTarget.setPointerCapture(e.pointerId);
          setDrag({ signature: flag.signature, to: flag.signature.bar, startX: e.nativeEvent.offsetX, moved: false });
        }}
        onPointerMove={(e) => {
          if (!drag || e.buttons !== 1) return;
          // The first signature stays on bar 1.
          if (drag.signature.bar === 0 || (!drag.moved && Math.abs(e.nativeEvent.offsetX - drag.startX) < 4)) return;
          setDrag({ ...drag, to: Math.max(1, barAtX(e.nativeEvent.offsetX)), moved: true });
        }}
        onPointerUp={() => {
          if (!drag) return;
          if (!drag.moved) setEditing({ signature: drag.signature, isNew: false });
          else if (drag.to !== drag.signature.bar) dispatch({ command: 'moveSignature', args: { from: drag.signature.bar, to: drag.to } });
          setDrag(null);
        }}
        onDoubleClick={(e) => {
          if (flagAt(e.nativeEvent.offsetX)) return;
          const bar = barAtX(e.nativeEvent.offsetX);
          const current = map.signatureAt(map.tickOfBar(bar));
          setEditing({ signature: { ...current, bar }, isNew: true });
        }}
        onContextMenu={(e) => {
          e.preventDefault();
          const flag = flagAt(e.nativeEvent.offsetX);
          if (flag && flag.signature.bar > 0) dispatch({ command: 'removeSignature', args: { bar: flag.signature.bar } });
        }}
      />
      {editing && (
        <SignaturePrompt
          signature={editing.signature}
          isNew={editing.isNew}
          onDone={(result) => {
            setEditing(null);
            if (result === 'remove') dispatch({ command: 'removeSignature', args: { bar: editing.signature.bar } });
            else if (result) dispatch({ command: 'setSignature', args: result });
          }}
        />
      )}
    </>
  );
}

/** Edits a time signature: the beats per bar and the beat's note value. Enter applies, Escape cancels. */
export function SignaturePrompt({
  signature,
  isNew,
  onDone,
}: {
  signature: TimeSignature;
  isNew: boolean;
  onDone: (result: TimeSignature | 'remove' | null) => void;
}) {
  const [numerator, setNumerator] = useState(signature.numerator);
  const [denominator, setDenominator] = useState(signature.denominator);

  return (
    <Dialog
      onClose={() => onDone(null)}
      className="signature-prompt"
      onSubmit={(e) => {
        e.preventDefault();
        onDone({ bar: signature.bar, numerator, denominator });
      }}
    >
      <h3>
        {isNew ? 'New time signature' : 'Time signature'} at bar {signature.bar + 1}
      </h3>
      <div className="signature-fields">
        <label className="modal-field">
          <span>Beats per bar</span>
          <input
            className="search"
            type="number"
            min={1}
            max={32}
            autoFocus
            value={numerator}
            onFocus={(e) => e.target.select()}
            onChange={(e) => setNumerator(Math.max(1, Math.min(32, Math.round(Number(e.target.value) || 1))))}
          />
        </label>
        <div className="modal-field">
          <span>Beat</span>
          <div className="segmented">
            {DENOMINATORS.map((d) => (
              <button key={d} type="button" className={d === denominator ? 'active' : ''} onClick={() => setDenominator(d)}>
                1/{d}
              </button>
            ))}
          </div>
        </div>
      </div>
      <div className="modal-buttons">
        {!isNew && signature.bar > 0 && (
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

/** The tick the playhead is on. */
export const playheadTick = () => Math.max(0, Math.round(useDawg.getState().telemetry.positionBeats * TICKS_PER_BEAT));

/** The bar nearest the playhead, where a corner's + adds a signature or a key change. */
export const playheadBar = (map: TempoMap) => Math.max(0, nearestBar(map, playheadTick()));

/** A strip's add button in its corner: what double-clicking the strip does, at the playhead, in sight
    (docs/ui-review-2026-11-10.md, 3.4). */
export function CornerAdd({ title, onClick }: { title: string; onClick: () => void }) {
  return (
    <IconButton size="s" className="tiny corner-add" title={title} aria-label={title} onClick={onClick}>
      <Plus size={10} />
    </IconButton>
  );
}
