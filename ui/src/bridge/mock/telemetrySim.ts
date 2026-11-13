// SPDX-License-Identifier: AGPL-3.0-or-later
// Plausible meter, gate, compressor and metronome telemetry for the browser mock: someone
// strumming every couple of seconds, shaped by the current gains. The strums peak between −11
// and −3 dBFS at the interface, inside the input meter's zone (docs/ui/input-level.md);
// `interfaceLevel` plays an interface set too low, one clipping, or no input at all.
import type { TelemetryEvent } from '../protocol';
import { noteBeats } from '../../lib/noteBeats';
import { stepBeats, stepsFor } from '../../lib/metronome';
import { createRandom } from './random';

const MOCK_HOST_BPM = 120;

/** What the simulated audio interface delivers (the mock's ?input= flag). */
export type InterfaceLevel = 'normal' | 'silent' | 'quiet' | 'clipping';

export function isInterfaceLevel(value: unknown): value is InterfaceLevel {
  return value === 'normal' || value === 'silent' || value === 'quiet' || value === 'clipping';
}

/** The quietest and loudest strum peak, in dBFS at the interface. */
const STRUM_PEAKS: Record<Exclude<InterfaceLevel, 'silent'>, [number, number]> = {
  normal: [-11, -3],
  // The musician's own recordings peak around −30 dBFS (docs/design/guitar-to-midi.md).
  quiet: [-33, -27],
  // Flat tops at full scale.
  clipping: [0, 0],
};

export class TelemetrySimulator {
  private t = 0;
  private nextStrum = 0.4;
  private envDb = -90;
  private strumPeakDb = -14;
  private metronomeT = 0;
  private wasMetronomeOn = false;
  private readonly random = createRandom(0xcafe);

  constructor(
    private readonly value: (id: string) => number,
    /** driftNote choice labels from the schema. */
    private readonly noteLabels: readonly string[],
    private readonly level: InterfaceLevel = 'normal',
  ) {}

  step(dt: number): TelemetryEvent {
    this.t += dt;
    const range = this.level === 'silent' ? null : STRUM_PEAKS[this.level];
    if (this.t >= this.nextStrum) {
      this.strumPeakDb = range ? range[0] + this.random() * (range[1] - range[0]) : -100;
      this.envDb = this.strumPeakDb;
      this.nextStrum = this.t + 1.1 + this.random() * 1.6;
    } else {
      this.envDb = Math.max(-78 + this.random() * 3, this.envDb - dt * 16);
    }

    // The attack itself is the peak; the decay flutters. No input at all reads as the floor.
    const jitter = (this.random() - 0.5) * 1.6;
    const flutter = this.envDb === this.strumPeakDb ? 0 : jitter;
    const raw = range ? Math.min(0, this.envDb + flutter) : -100;
    const inDb = Math.max(-100, raw + this.value('inputGain'));

    const gateOn = this.value('gateEnabled') >= 0.5;
    const gateOpen = !gateOn || raw > this.value('gateThreshold');
    const gateReductionDb = gateOpen ? 0 : -24;

    const tideOn = this.value('tideEnabled') >= 0.5;
    const tideReductionDb = tideOn
      ? -Math.max(0, (inDb + 34) * 0.35) * (0.4 + this.value('tideComp'))
      : 0;

    const outBase = gateOpen ? inDb + 6 + tideReductionDb * 0.5 : -90;
    const outDb = Math.min(3, outBase + this.value('outputGain'));
    const spread = (this.random() - 0.5) * 2;

    return {
      inputDb: [inDb],
      outputDb: [outDb + spread * 0.5, outDb - spread * 0.5],
      // As the engine: a sample at full scale latches the clip light.
      inputClip: inDb >= 0,
      outputClip: outDb > 0,
      gateOpen,
      gateReductionDb,
      tideReductionDb,
      metronomeStep: this.metronomeStep(dt),
      driftTimeMs: this.driftTimeMs(),
      cpu: 0.05 + this.random() * 0.012 + this.enabledCount() * 0.007,
    };
  }

  private metronomeStep(dt: number): number {
    const on = this.value('metronomeEnabled') >= 0.5;
    if (!on) {
      this.wasMetronomeOn = false;
      return -1;
    }
    if (!this.wasMetronomeOn) this.metronomeT = 0;
    this.wasMetronomeOn = true;
    this.metronomeT += dt;
    const signature = Math.round(this.value('metronomeSignature'));
    const stepSeconds = (60 / this.value('tempo')) * stepBeats(signature);
    return Math.floor(this.metronomeT / stepSeconds) % stepsFor(signature);
  }

  private driftTimeMs(): number {
    const sync = Math.round(this.value('driftSync'));
    if (sync === 0) return this.value('driftTime');
    const bpm = sync === 1 ? MOCK_HOST_BPM : this.value('tempo');
    const beats = noteBeats(this.noteLabels[Math.round(this.value('driftNote'))] ?? '1/4');
    return Math.min(2500, Math.max(16, (beats * 60000) / bpm));
  }

  private enabledCount(): number {
    return [
      'tideEnabled',
      'pearlEnabled',
      'coralEnabled',
      'rippleEnabled',
      'trenchEnabled',
      'urchinEnabled',
      'kelpEnabled',
      'remoraEnabled',
      'sargassoEnabled',
      'breakerEnabled',
      'conchEnabled',
      'anemoneEnabled',
      'gyreEnabled',
      'jettyEnabled',
      'beaconEnabled',
      'driftEnabled',
      'fathomEnabled',
      'doublerEnabled',
    ].filter((id) => this.value(id) >= 0.5).length;
  }
}
