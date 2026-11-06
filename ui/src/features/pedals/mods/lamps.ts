// SPDX-License-Identifier: AGPL-3.0-or-later
// Brightness curves for the modulation pedals' lamps. They follow each pedal's own LFO shape
// (docs/dsp/gyre.md, jetty.md, beacon.md) so the lamp shows what the sweep is doing.

/** Triangle 0..1..0 over one cycle, peak at mid-cycle: the phaser's and flanger's sweep. */
export function triangle(phase: number): number {
  const p = phase - Math.floor(phase);
  return 1 - Math.abs(2 * p - 1);
}

/** Lamp brightness for a 0..1 sweep position: never fully dark, so the lamp still reads as on. */
export function lampBrightness(sweep: number): number {
  return 0.35 + 0.65 * Math.min(1, Math.max(0, sweep));
}

// The vibe's lamp and photocells, as in engine/src/dsp/Beacon.cpp: the LFO's sine drives the
// filament (heats in 12 ms, cools in 30 ms), the cells follow the light (5 ms on, 45 ms off), and
// the drive grows with speed (x (1 + speed / 3 Hz)).
const LAMP_HEAT = 0.012;
const LAMP_COOL = 0.03;
const CELL_ON = 0.005;
const CELL_OFF = 0.045;
const DRIVE_SPEED_HZ = 3;
const STEP = 0.001; // seconds per simulation step

/** The vibe's light, simulated in 1 ms steps; `light` is 0..1. */
export class VibeLamp {
  phase = 0;
  lamp = 0.25;
  light = 0.25;

  /** Advances by `dt` seconds at `speedHz` and `depth` (0..1); returns the light. */
  advance(dt: number, speedHz: number, depth: number): number {
    const swing = depth * (1 + speedHz / DRIVE_SPEED_HZ);
    for (let t = 0; t < dt; t += STEP) {
      const step = Math.min(STEP, dt - t);
      this.phase = (this.phase + speedHz * step) % 1;
      const drive = Math.min(
        1,
        Math.max(0, 0.5 * (1 + swing * Math.sin(2 * Math.PI * this.phase))),
      );
      const power = drive * drive;
      this.lamp = follow(this.lamp, power, step, power > this.lamp ? LAMP_HEAT : LAMP_COOL);
      this.light = follow(this.light, this.lamp, step, this.lamp > this.light ? CELL_ON : CELL_OFF);
    }
    return this.light;
  }
}

function follow(value: number, target: number, dt: number, tau: number): number {
  return target + Math.exp(-dt / tau) * (value - target);
}
