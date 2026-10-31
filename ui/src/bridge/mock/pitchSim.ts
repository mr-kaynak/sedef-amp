// SPDX-License-Identifier: AGPL-3.0-or-later
// A virtual player for the browser mock. It produces PitchEvents the way the C++ PitchTracker
// would for a real guitar: plucks with a sharp attack, settling, jitter, decay, and a stability
// window with mean and standard error. With no intent it tunes the strings of a guitar one after
// another; the intonation workbench sets intents so its flow can be demoed end to end.
import type { SimulatedIntent } from '../backend';
import type { PitchEvent } from '../protocol';
import { hzToMidi, midiToHz, noteOf } from '../../lib/music';
import { createRandom, gaussian } from './random';

const STANDARD_TUNING = [40, 45, 50, 55, 59, 64]; // E2 A2 D3 G3 B3 E4

/** Default 12th-fret errors in cents per string index, low string first. */
export const DEFAULT_INTONATION_ERRORS = [3.6, -2.4, 1.1, 0.6, 4.4, -1.8, 2.2];

interface Segment {
  hz: number[];
  cents: number[];
}

export interface PitchSimOptions {
  getReferenceHz: () => number;
  intonationErrors?: number[];
  seed?: number;
}

export class PitchSimulator {
  private intent: SimulatedIntent = { kind: 'idle' };
  private t = 0; // seconds since the intent started
  private pluckT = 0; // seconds since the last pluck, -1 while silent
  private onsetId = 0;
  private detune = 0; // cents the "string" is away from the target
  private idleString = 0;
  private inTuneFor = 0;
  private segment: Segment = { hz: [], cents: [] };
  private readonly random: () => number;
  private readonly errors: number[];

  constructor(private readonly options: PitchSimOptions) {
    this.random = createRandom(options.seed);
    this.errors = options.intonationErrors ?? DEFAULT_INTONATION_ERRORS;
    this.restart();
  }

  setIntent(intent: SimulatedIntent): void {
    this.intent = intent;
    this.restart();
  }

  private restart(): void {
    this.t = 0;
    this.inTuneFor = 0;
    this.pluckT = -1;
    this.segment = { hz: [], cents: [] };
    // Strings drift a little flat or sharp between sessions.
    this.detune = (this.random() < 0.5 ? -1 : 1) * (12 + this.random() * 16);
  }

  private pluck(): void {
    this.pluckT = 0;
    this.onsetId += 1;
    this.segment = { hz: [], cents: [] };
  }

  /** Advances the simulation by dt seconds and returns the tracker reading. */
  step(dt: number): PitchEvent {
    this.t += dt;
    const target = this.currentTarget();

    if (target === null) return this.silent();

    // Fretting hand needs a moment before the first pluck of a fretted note.
    const firstPluckAt = this.intent.kind === 'fret' ? 0.7 : 0.25;
    const repluckEvery = this.intent.kind === 'fret' ? 3.6 : 2.8;
    if (this.pluckT < 0) {
      if (this.t < firstPluckAt) return this.silent();
      this.pluck();
    } else {
      this.pluckT += dt;
      if (this.pluckT > repluckEvery) this.pluck();
    }

    const deviation = this.deviationCents(dt);
    const levelDb = -14 - this.pluckT * 7;
    if (levelDb < -62) return this.silent();

    const attack = this.pluckT < 0.16;
    const attackSharpness = attack ? 9 * (1 - this.pluckT / 0.16) : 0;
    const jitter = gaussian(this.random) * (this.intent.kind === 'fret' ? 0.32 : 0.18);
    const cents = deviation + attackSharpness + jitter;
    const hz = midiToHz(target, this.options.getReferenceHz()) * Math.pow(2, cents / 1200);

    if (!attack) {
      this.segment.hz.push(hz);
      this.segment.cents.push(cents);
    }
    return this.reading(hz, levelDb, attack);
  }

  private currentTarget(): number | null {
    switch (this.intent.kind) {
      case 'tune':
      case 'fret':
        return this.intent.midi;
      case 'idle':
        return STANDARD_TUNING[this.idleString % STANDARD_TUNING.length] ?? 40;
    }
  }

  /** Where the string sits relative to the target right now, in cents. */
  private deviationCents(dt: number): number {
    if (this.intent.kind === 'fret') {
      return this.errors[this.intent.stringIndex % this.errors.length] ?? 0;
    }
    // The player turns the peg towards pitch with a slight overshoot, then leaves it.
    const pull = Math.min(1, dt * 1.6);
    this.detune += (0 - this.detune) * pull;
    if (Math.abs(this.detune) < 0.25) this.detune *= 0.5;

    if (this.intent.kind === 'idle') {
      this.inTuneFor = Math.abs(this.detune) < 0.5 ? this.inTuneFor + dt : 0;
      if (this.inTuneFor > 2.2) {
        this.idleString += 1;
        this.restart();
      }
    }
    return this.detune;
  }

  private reading(hz: number, levelDb: number, attack: boolean): PitchEvent {
    const reference = this.options.getReferenceHz();
    const midiFloat = hzToMidi(hz, reference);
    const midiNote = Math.round(midiFloat);
    const { note, octave } = noteOf(midiNote);

    const n = this.segment.cents.length;
    const window = this.segment.cents.slice(-24);
    const mean = (xs: number[]) => xs.reduce((a, b) => a + b, 0) / Math.max(1, xs.length);
    const windowMean = mean(window);
    const windowStd = Math.sqrt(mean(window.map((c) => (c - windowMean) ** 2)));
    const stable = !attack && window.length >= 16 && windowStd < 1.5;

    const stableHz = stable ? mean(this.segment.hz) : 0;
    const segMean = mean(this.segment.cents);
    const segStd = Math.sqrt(mean(this.segment.cents.map((c) => (c - segMean) ** 2)));

    return {
      valid: true,
      frequencyHz: hz,
      midiNote,
      note,
      octave,
      cents: 100 * (midiFloat - midiNote),
      clarity: attack ? 0.82 : 0.96,
      levelDb,
      stable,
      stableHz,
      stableStdCents: stable ? segStd / Math.sqrt(n) : 0,
      stableCount: stable ? n : 0,
      onsetId: this.onsetId,
    };
  }

  private silent(): PitchEvent {
    return {
      valid: false,
      frequencyHz: 0,
      midiNote: 0,
      note: '',
      octave: 0,
      cents: 0,
      clarity: 0,
      levelDb: -90,
      stable: false,
      stableHz: 0,
      stableStdCents: 0,
      stableCount: 0,
      onsetId: this.onsetId,
    };
  }
}
