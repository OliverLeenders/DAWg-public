import { Check } from 'lucide-react';
import { PALETTE } from '../colours';

/** The 12-colour palette as a grid of swatches; `reset` adds a "use track colour" entry. */
export function Swatches({
  current,
  onPick,
  reset,
}: {
  current: string;
  onPick: (colour: string) => void;
  reset?: { label: string; colour: string };
}) {
  return (
    <div className="swatches">
      {PALETTE.map((colour) => (
        <button
          key={colour}
          className="swatch"
          style={{ background: colour }}
          title={colour}
          onClick={() => onPick(colour)}
        >
          {current === colour && <Check size={11} />}
        </button>
      ))}
      {reset && (
        // One checkbox in the track's colour, ticked while the clip follows it, as the swatch above is ticked when chosen;
        // picking a swatch unticks it.
        <label className="menu-item swatch-reset">
          <input
            type="checkbox"
            className="swatch-check"
            style={{ '--swatch': reset.colour } as React.CSSProperties}
            checked={current === ''}
            onChange={() => onPick('')}
          />
          {reset.label}
        </label>
      )}
    </div>
  );
}
