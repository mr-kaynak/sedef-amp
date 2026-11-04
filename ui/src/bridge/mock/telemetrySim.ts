// SPDX-License-Identifier: AGPL-3.0-or-later
// Plausible meter, gate, compressor and metronome telemetry for the browser mock: someone
// strumming every couple of seconds, shaped by the current gains.
import type { TelemetryEvent } from '../protocol';
import { noteBeats } from '../../lib/noteBeats';
import { stepBeats, stepsFor } from '../../lib/metronome';
import { createRandom } from './random';

const MOCK_HOST_BPM = 120;

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
  ) {}

  step(dt: number): TelemetryEvent {
    this.t += dt;
    if (this.t >= this.nextStrum) {
      this.strumPeakDb = -20 + this.random() * 10;
      this.envDb = this.strumPeakDb;
      this.nextStrum = this.t + 1.1 + this.random() * 1.6;
    } else {
      this.envDb = Math.max(-78 + this.random() * 3, this.envDb - dt * 16);
    }

    const flutter = (this.random() - 0.5) * 1.6;
    const raw = this.envDb + flutter;
    const inDb = raw + this.value('inputGain');

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
      inputClip: inDb > 0,
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
      'driftEnabled',
      'fathomEnabled',
      'doublerEnabled',
    ].filter((id) => this.value(id) >= 0.5).length;
  }
}
