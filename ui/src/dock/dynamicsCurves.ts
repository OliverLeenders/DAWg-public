// The Gate's and the Expander's static curves (docs/design/gate-expander.md, N6, N9, N10), as the engine computes
// them (EffectNodes.cpp, GateNode and ExpanderNode::gainChangeDb).

/** The bottom of the range: the Gate is fully closed there (gain 0). */
export const CLOSED_DB = -80;

/** The Expander's gain change in dB for a detector level: 1:ratio below the threshold, a 6 dB soft knee, at most the
    range. */
export function expanderGainChangeDb(levelDb: number, thresholdDb: number, ratio: number, rangeDb: number): number {
  const knee = 6;
  const under = levelDb - thresholdDb;
  const slope = Math.max(0, ratio - 1);
  let change: number;
  if (2 * under >= knee) change = 0;
  else if (2 * Math.abs(under) < knee) change = (-slope * (under - knee / 2) ** 2) / (2 * knee);
  else change = slope * under;
  return Math.max(Math.min(0, rangeDb), change) || 0; // never -0
}

/** The Gate's gain change while closed: the range, or -Infinity at the bottom (fully closed). */
export function gateFloorDb(rangeDb: number): number {
  return rangeDb <= CLOSED_DB ? -Infinity : Math.min(0, rangeDb);
}

/** The Gate's close threshold. */
export function gateCloseDb(thresholdDb: number, hysteresisDb: number): number {
  return thresholdDb - Math.max(0, hysteresisDb);
}

/** The ratio that puts the Expander's curve through (levelDb, outputDb), below the threshold and outside the knee. */
export function expanderRatioThrough(levelDb: number, outputDb: number, thresholdDb: number): number {
  const under = levelDb - thresholdDb;
  if (under >= 0) return 1;
  return 1 + (outputDb - levelDb) / under;
}
