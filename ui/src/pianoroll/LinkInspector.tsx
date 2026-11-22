import { Unlink } from 'lucide-react';
import { DragNumber } from '../components/DragNumber';
import { dispatch } from '../store/store';
import type { PlacedNote } from './geometry';
import { glideCommand, unlinkCommand } from './links';
import { powerTaper } from '../components/dragScale';
import { IconButton } from '../ui/controls';

// A glide drags finer at its short end (dragScale.ts).
const GLIDE_TAPER = powerTaper(0, 10000, 3);

interface Props {
  /** The selected notes; the inspector edits those that link onward. */
  notes: PlacedNote[];
}

/** The glide time of the selected linked notes, shown in the lane tab bar (docs/design/linked-notes.md). */
export function LinkInspector({ notes }: Props) {
  const linked = notes.filter((n) => n.note.link);

  return (
    <fieldset
      className="vibrato-inspector"
      disabled={linked.length === 0}
      title={linked.length === 0 ? 'Ctrl+Alt+drag from one note to another to link them' : `Editing ${linked.length} linked notes`}
    >
      <label className="field">
        <span>Glide</span>
        <DragNumber
          value={linked[0]?.note.link?.glideMs ?? 100}
          min={0}
          max={10000}
          quantum={1}
          taper={GLIDE_TAPER}
          format={(v) => `${v.toFixed(0)} ms`}
          onChange={(v, gesture) => dispatch(glideCommand(linked, v), gesture)}
        />
      </label>
      <IconButton title="Unlink the selected notes" onClick={() => dispatch(unlinkCommand(linked))}>
        <Unlink size={14} />
      </IconButton>
    </fieldset>
  );
}
