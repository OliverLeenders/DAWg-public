import { pitchToY, tickToX, type PlacedNote, type View } from './geometry';

/** The ghost note (another clip's note, or a loop repeat) under a point in the roll, if any. */
export function ghostAt(ghosts: PlacedNote[], view: View, x: number, y: number): PlacedNote | undefined {
  return ghosts.find((g) => {
    const top = pitchToY(view, g.note.pitch);
    return x >= tickToX(view, g.start) && x <= tickToX(view, g.end) && y >= top && y <= top + view.keyHeight;
  });
}
