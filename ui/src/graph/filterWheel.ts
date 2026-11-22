/** Wheel delta per filter slope step: one notch of a mouse wheel. */
export const FILTER_WHEEL_STEP = 100;

/** Turns accumulated wheel movement into whole slope steps, keeping the remainder so a touchpad's fine scrolls add
    up. Scrolling up (a negative deltaY) makes the slope steeper. */
export function filterSlopeSteps(accumulated: number, deltaY: number): { steps: number; accumulated: number } {
  const total = accumulated - deltaY;
  const steps = Math.trunc(total / FILTER_WHEEL_STEP);
  return { steps, accumulated: total - steps * FILTER_WHEEL_STEP };
}

/** A filter slope after `steps` choices, clamped to its range. */
export function stepFilterSlope(current: number, steps: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, Math.round(current) + steps));
}
