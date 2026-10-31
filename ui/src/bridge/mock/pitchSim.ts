// SPDX-License-Identifier: AGPL-3.0-or-later
// A virtual player for the browser mock. It produces PitchEvents the way the C++ PitchTracker
// does for a real guitar (docs/dsp/tuner.md): plucks with a sharp attack glide, a short-window
// reading with jitter, and settled detections, one per 5 ms hop from shortly after the attack,
// whose last 0.4 s give stable, stableHz, stableStdCents and stableCount. With no intent it tunes
// the strings of a guitar one after another; the intonation workbench sets intents so its flow
// can be demoed end to end.
import type { SimulatedIntent } from '../backend';
import type { PitchEvent } from '../protocol';
import { hzToMidi, midiToHz, noteOf } from '../../lib/music';
import { createRandom, gaussian } from './random';

const STANDARD_TUNING = [40, 45, 50, 55, 59, 64]; // E2 A2 D3 G3 B3 E4

/** Default 12th-fret errors in cents per string index, low string first. */
export const DEFAULT_INTONATION_ERRORS = [3.6, -2.4, 1.1, 0.6, 4.4, -1.8, 2.2];

/** One settled detection per 5 ms hop. */
const HOPS_PER_SECOND = 200;
/** Settled detections start after the 150 ms attack skip, once the first long window fills. */
const SETTLED_FROM_SECONDS = 0.25;
/** The tracker's stability window: span, minimum fill and largest spread for `stable`. */
const STABLE_SPAN_SECONDS = 0.4;
const STABLE_FILL = 0.75;
const STABLE_MAX_STD_CENTS = 0.5;
/** A plucked string starts sharp and settles (time constant 50 ms), as in the engine's tests. */
const GLIDE_CENTS = 9;
const GLIDE_SECONDS = 0.05;

interface Settled {
  t: number;
  cents: number;
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
  /** Settled detections of the last STABLE_SPAN_SECONDS, oldest first. */
  private settled: Settled[] = [];
  private stableCount = 0;
  private hopDebt = 0;
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
    this.clearSegment();
    // Strings drift a little flat or sharp between sessions.
    this.detune = (this.random() < 0.5 ? -1 : 1) * (12 + this.random() * 16);
  }

  private clearSegment(): void {
    this.settled = [];
    this.stableCount = 0;
    this.hopDebt = 0;
  }

  private pluck(): void {
    this.pluckT = 0;
    this.onsetId += 1;
    this.clearSegment();
  }

  /** Advances the simulation by dt seconds and returns the tracker reading. */
  step(dt: number): PitchEvent {
    this.t += dt;
    const target = this.currentTarget();

    if (target === null) return this.silent();

    // Fretting hand needs a moment before the first pluck of a fretted note.
    const fretted = this.intent.kind === 'fret';
    const firstPluckAt = fretted ? 0.7 : 0.25;
    const repluckEvery = fretted ? 3.6 : 2.8;
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

    const pitch = deviation + GLIDE_CENTS * Math.exp(-this.pluckT / GLIDE_SECONDS);
    this.settle(pitch, dt, fretted);
    const jitter = gaussian(this.random) * (fretted ? 0.32 : 0.18);
    return this.reading(target, pitch + jitter, levelDb);
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

  /** Adds the settled detections of the hops in the last dt and drops those out of the window. */
  private settle(cents: number, dt: number, fretted: boolean): void {
    if (this.pluckT < SETTLED_FROM_SECONDS) return;
    const noise = fretted ? 0.08 : 0.05;
    this.hopDebt += dt * HOPS_PER_SECOND;
    for (; this.hopDebt >= 1; this.hopDebt -= 1) {
      const t = this.pluckT - (this.hopDebt - 1) / HOPS_PER_SECOND;
      this.settled.push({ t, cents: cents + gaussian(this.random) * noise });
      this.stableCount += 1;
    }
    const start = this.pluckT - STABLE_SPAN_SECONDS;
    while (this.settled.length && (this.settled[0]?.t ?? 0) <= start) this.settled.shift();
  }

  private reading(target: number, cents: number, levelDb: number): PitchEvent {
    const reference = this.options.getReferenceHz();
    const targetHz = midiToHz(target, reference);
    const hz = targetHz * Math.pow(2, cents / 1200);
    const midiFloat = hzToMidi(hz, reference);
    const midiNote = Math.round(midiFloat);
    const { note, octave } = noteOf(midiNote);

    const values = this.settled.map((s) => s.cents);
    const n = values.length;
    const mean = n ? values.reduce((a, b) => a + b, 0) / n : 0;
    const std = n > 1 ? Math.sqrt(values.reduce((a, c) => a + (c - mean) ** 2, 0) / (n - 1)) : 0;
    const oldest = this.settled[0]?.t ?? Infinity;
    const full =
      oldest <= this.pluckT - STABLE_SPAN_SECONDS + 1 / HOPS_PER_SECOND &&
      n >= STABLE_FILL * STABLE_SPAN_SECONDS * HOPS_PER_SECOND;

    return {
      valid: true,
      frequencyHz: hz,
      midiNote,
      note,
      octave,
      cents: 100 * (midiFloat - midiNote),
      clarity: this.pluckT < 0.16 ? 0.82 : 0.96,
      levelDb,
      stable: full && std < STABLE_MAX_STD_CENTS,
      stableHz: n ? targetHz * Math.pow(2, mean / 1200) : 0,
      stableStdCents: std,
      stableCount: this.stableCount,
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
