// SPDX-License-Identifier: AGPL-3.0-or-later
// How the needle moves between pitch events. The engine's display is already smooth (it arrives at
// 60 Hz); the needle follows it like a well-damped meter movement: a critically damped spring,
// integrated exactly for each frame so any frame time is stable and it never overshoots a still
// target (both of its poles are real, as is the engine filter's).

/** Natural frequency (rad/s): a 20-cent jump is within 1 cent after 80 ms, within 0.1 after 115. */
export const NEEDLE_RATE = 60;

/** Opacity of the needle with nothing to show, and once a held reading has faded. */
export const IDLE_OPACITY = 0.25;
export const HELD_OPACITY = 0.45;
/** A held reading stays at full strength this long after the note ends, then fades over FADE. */
export const HOLD_SECONDS = 1.5;
export const FADE_SECONDS = 1;

export class NeedleSpring {
  position = 0;
  velocity = 0;

  /** Moves towards `target` for dt seconds and returns the new position. */
  step(target: number, dt: number): number {
    const w = NEEDLE_RATE;
    const e = this.position - target;
    const k = this.velocity + w * e;
    const decay = Math.exp(-w * dt);
    this.position = target + (e + k * dt) * decay;
    this.velocity = (this.velocity - w * k * dt) * decay;
    return this.position;
  }
}

/** Needle opacity: full while live, held at full for a while after the note ends, then dimmed. */
export function needleOpacity(shown: boolean, heldFor: number | null): number {
  if (!shown) return IDLE_OPACITY;
  if (heldFor === null || heldFor <= HOLD_SECONDS) return 1;
  const k = Math.min(1, (heldFor - HOLD_SECONDS) / FADE_SECONDS);
  const eased = k * k * (3 - 2 * k);
  return 1 + (HELD_OPACITY - 1) * eased;
}
