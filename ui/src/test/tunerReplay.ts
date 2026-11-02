// SPDX-License-Identifier: AGPL-3.0-or-later
// Plays a stream of tracker readings through the tuner gauge's readout and needle, frame by frame,
// the way TunerGauge does: each reading is handled when it arrives (60 Hz, the editor's timer) and
// the needle moves once per display frame. Used by features/tuner/replay.test.ts on streams
// recorded from the real PitchTracker (features/tuner/fixtures), and by the plots in
// docs/dsp/tuner.md.
import type { GaugeTarget } from '../features/tuner/cents';
import { NeedleSpring } from '../features/tuner/needle';
import { readoutInput, TunerReadout, type TuneState } from '../features/tuner/readout';
import type { PitchEvent } from '../bridge/protocol';

export interface TimedEvent {
  /** Seconds (any origin). */
  t: number;
  event: PitchEvent;
}

export interface ReplayFrame {
  t: number;
  /** Needle position, cents from the note shown (0 while idle). */
  needle: number;
  /** A reading is shown (the needle at full strength or held). */
  visible: boolean;
  note: number | null;
  tune: TuneState;
  held: boolean;
  centsText: string;
}

export interface ReplayOptions {
  inTuneCents: number;
  target?: GaugeTarget;
  referenceHz?: number;
  /** Display frame rate. */
  fps?: number;
  /** Phase of the frames against the readings, as a fraction of a frame. */
  phase?: number;
}

export function replay(stream: TimedEvent[], options: ReplayOptions): ReplayFrame[] {
  const { inTuneCents, target = { kind: 'chromatic' }, referenceHz = 440 } = options;
  const fps = options.fps ?? 60;
  const readout = new TunerReadout(inTuneCents);
  const spring = new NeedleSpring();
  const frames: ReplayFrame[] = [];
  if (stream.length === 0) return frames;
  const dt = 1 / fps;
  const end = stream[stream.length - 1]?.t ?? 0;
  let next = 0;
  for (let t = (stream[0]?.t ?? 0) + (options.phase ?? 0.4) * dt; t <= end; t += dt) {
    for (; next < stream.length && (stream[next]?.t ?? Infinity) <= t; next += 1) {
      const e = stream[next] as TimedEvent;
      readout.update(readoutInput(e.event, target, referenceHz), e.t);
    }
    const r = readout.current;
    const cents = r.cents === null ? 0 : Math.max(-50, Math.min(50, r.cents));
    frames.push({
      t,
      needle: spring.step(cents, dt),
      visible: r.cents !== null,
      note: r.note,
      tune: r.tune,
      held: r.held,
      centsText: r.centsText,
    });
  }
  return frames;
}

/** A recorded row (fixtures/streams.json) as the PitchEvent the plugin would send. */
export function displayEvent(
  valid: boolean,
  held: boolean,
  settled: boolean,
  note: number,
  cents: number,
  referenceHz = 440,
): PitchEvent {
  const hz = valid ? referenceHz * Math.pow(2, (note - 69) / 12 + cents / 1200) : 0;
  return {
    valid: valid && !held,
    frequencyHz: hz,
    midiNote: note,
    note: '',
    octave: 0,
    cents,
    clarity: valid ? 0.95 : 0,
    levelDb: valid ? -24 : -90,
    stable: false,
    stableHz: 0,
    stableStdCents: 0,
    stableCount: 0,
    onsetId: 1,
    displayValid: valid,
    displayHeld: held,
    displaySettled: settled,
    displayHz: hz,
    displayNote: note,
    displayCents: cents,
  };
}
