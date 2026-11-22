import { useEffect, useRef } from 'react';

const toDb = (gain: number) => (gain > 0 ? 20 * Math.log10(gain) : -Infinity);
const toFraction = (gain: number) => Math.min(1, Math.max(0, (toDb(gain) + 60) / 60));

interface Props {
  /** Linear peak gains (0..1+). */
  left: number;
  right: number;
  /** Shows a clip indicator at the right end, lit while true (docs/design/mixing-basics.md, M3). */
  clipped?: boolean;
  onResetClip?: () => void;
  title?: string;
  className?: string;
  /** Bars rising from the bottom (the dock's fader), with the clip indicator on top. */
  vertical?: boolean;
  /** Laid over the lanes, such as a volume knob that uses them as its track. */
  overlay?: React.ReactNode;
}

/** Stereo peak meter with smooth falloff, -60 to 0 dBFS. Animates on requestAnimationFrame so it keeps decaying
    while the input stays at 0. */
export function Meter({ left, right, clipped, onResetClip, title = 'Output peak', className, vertical = false, overlay }: Props) {
  const target = useRef({ l: 0, r: 0 });
  const leftBar = useRef<HTMLDivElement>(null);
  const rightBar = useRef<HTMLDivElement>(null);

  target.current.l = Math.max(target.current.l, toFraction(left));
  target.current.r = Math.max(target.current.r, toFraction(right));

  useEffect(() => {
    let frame = 0;
    let last = performance.now();
    const tick = (now: number) => {
      const t = target.current;
      const falloff = Math.pow(0.05, (now - last) / 1000); // ~-26 dB per second
      last = now;
      // The bar uncovers a gradient fixed to the scale, so a colour always means the same level.
      const clip = (level: number) => {
        const hidden = `${((1 - level) * 100).toFixed(2)}%`;
        return vertical ? `inset(${hidden} 0 0 0)` : `inset(0 ${hidden} 0 0)`;
      };
      if (leftBar.current) leftBar.current.style.clipPath = clip(t.l);
      if (rightBar.current) rightBar.current.style.clipPath = clip(t.r);
      t.l *= falloff;
      t.r *= falloff;
      frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [vertical]);

  return (
    <div className={`meter ${vertical ? 'vertical' : ''} ${onResetClip ? 'with-clip' : ''} ${className ?? ''}`} title={title}>
      <div className="meter-lanes">
        <div className="meter-lane">
          <div ref={leftBar} className="meter-fill" />
        </div>
        <div className="meter-lane">
          <div ref={rightBar} className="meter-fill" />
        </div>
        {overlay}
      </div>
      {onResetClip && (
        <button
          className={`meter-clip ${clipped ? 'on' : ''}`}
          title={clipped ? 'Clipped: the level reached 0 dBFS (click to reset)' : 'Clip indicator'}
          onPointerDown={(e) => e.stopPropagation()}
          onClick={(e) => {
            e.stopPropagation();
            onResetClip();
          }}
        />
      )}
    </div>
  );
}
