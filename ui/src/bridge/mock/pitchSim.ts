// SPDX-License-Identifier: AGPL-3.0-or-later
// A virtual player for the browser mock. It produces PitchEvents the way the C++ PitchTracker
// does for a real guitar (docs/dsp/tuner.md): plucks with a sharp attack glide, a short-window
// reading with jitter, and settled detections, one per 5 ms hop from shortly after the attack,
// whose last 0.4 s give stable, stableHz, stableStdCents and stableCount.
//
// Like a real string it never sits still: every pluck wobbles by about a cent (fretting pressure,
// the neck and body moving), sags slowly flat and fades out. Like a real player it never tunes
// perfectly either: each open string ends up a few cents from the target. With no intent it tunes
// the strings of a guitar one after another; the intonation workbench sets intents so its flow
// can be demoed, and tested, end to end.
import type { SimulatedIntent } from '../backend';
import type { PitchEvent } from '../protocol';
import { hzToMidi, midiToHz, noteOf } from '../../lib/music';
import { createRandom, gaussian } from './random';

const STANDARD_TUNING = [40, 45, 50, 55, 59, 64]; // E2 A2 D3 G3 B3 E4

/** Default 12th-fret errors in cents per string index, low string first. */
export const DEFAULT_INTONATION_ERRORS = [3.6, -2.4, 1.1, 0.6, 4.4, -1.8, 2.2];
/** Without configured offsets, the player leaves each open string within this many cents. */
export const TUNING_SPREAD_CENTS = 3;

/** One settled detection per 5 ms hop. */
const HOPS_PER_SECOND = 200;
/** Settled detections start after the 150 ms attack skip, once the first long window fills. */
const SETTLED_FROM_SECONDS = 0.25;
/** Settled detections average up to 320 ms, so they follow the pitch with this time constant. */
const SETTLED_SMOOTHING_SECONDS = 0.15;
/** The tracker's stability window: span, minimum fill and largest spread for `stable`. */
const STABLE_SPAN_SECONDS = 0.4;
const STABLE_FILL = 0.75;
const STABLE_MAX_STD_CENTS = 0.5;
/** A plucked string starts sharp and settles (time constant 50 ms), as in the engine's tests. */
const GLIDE_CENTS = 9;
const GLIDE_SECONDS = 0.05;
/** The slow flat sag of a decaying string. */
const DRIFT_CENTS_PER_SECOND = -0.6;
/** Largest random wobble of a ringing string, and the range of its rates. */
const WOBBLE_CENTS = 1;
const WOBBLE_MIN_HZ = 1.5;
const WOBBLE_MAX_HZ = 6;
const WOBBLE_COMPONENTS = 3;
/** Tuning, the player turns the peg over this stretch of the first pluck (seconds). */
const TURN_STARTS_AT = 0.8;
const TURN_ENDS_AT = 1.3;

interface Settled {
  t: number;
  cents: number;
}

interface Wobble {
  hz: number;
  phase: number;
}

export interface PitchSimOptions {
  getReferenceHz: () => number;
  intonationErrors?: number[];
  /**
   * Cents from the target at which the player leaves each open string after tuning, low string
   * first. Random within ±TUNING_SPREAD_CENTS, anew each time, when not given.
   */
  tuningOffsets?: number[];
  seed?: number;
}

const smoothstep = (x: number) => {
  const k = Math.min(1, Math.max(0, x));
  return k * k * (3 - 2 * k);
};

export class PitchSimulator {
  private intent: SimulatedIntent = { kind: 'idle' };
  private t = 0; // seconds since the intent started
  private pluckT = 0; // seconds since the last pluck, -1 while silent
  private plucks = 0; // since the intent started
  private onsetId = 0;
  private detune = 0; // cents the "string" is away from the target
  private startDetune = 0; // where a tune intent finds the string
  private idleString = 0;
  private inTuneFor = 0;
  /** Where the player left each open string, by string index. */
  private readonly tuned: number[] = [];
  private wobble: Wobble[] = [];
  private smoothed = 0;
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
    if (intent.kind === 'tune') {
      this.tuned[intent.stringIndex] =
        this.options.tuningOffsets?.[intent.stringIndex] ??
        (this.random() * 2 - 1) * TUNING_SPREAD_CENTS;
    }
    this.restart();
  }

  private restart(): void {
    this.t = 0;
    this.inTuneFor = 0;
    this.pluckT = -1;
    this.plucks = 0;
    this.clearSegment();
    // Strings drift a little flat or sharp between sessions.
    this.detune = (this.random() < 0.5 ? -1 : 1) * (12 + this.random() * 16);
    this.startDetune = this.detune;
  }

  private clearSegment(): void {
    this.settled = [];
    this.stableCount = 0;
    this.hopDebt = 0;
  }

  private pluck(): void {
    this.pluckT = 0;
    this.plucks += 1;
    this.onsetId += 1;
    this.clearSegment();
    // Every pluck wobbles differently: a few slow sinusoids, together within ±WOBBLE_CENTS.
    this.wobble = Array.from({ length: WOBBLE_COMPONENTS }, () => ({
      hz: WOBBLE_MIN_HZ * Math.pow(WOBBLE_MAX_HZ / WOBBLE_MIN_HZ, this.random()),
      phase: 2 * Math.PI * this.random(),
    }));
  }

  /** Advances the simulation by dt seconds and returns the tracker reading. */
  step(dt: number): PitchEvent {
    this.t += dt;
    const target = this.currentTarget();

    if (target === null) return this.silent();

    // The fretting hand needs a moment before the first pluck of a fretted note. Tuning, the
    // player plucks, turns the peg while the string rings, then plucks again to check.
    const fretted = this.intent.kind === 'fret';
    const firstPluckAt = fretted ? 0.7 : 0.25;
    const checkPluckAt = this.intent.kind === 'tune' ? TURN_ENDS_AT + 0.2 : Infinity;
    const repluckEvery = fretted ? 3.6 : 2.8;
    if (this.pluckT < 0) {
      if (this.t < firstPluckAt) return this.silent();
      this.pluck();
    } else {
      this.pluckT += dt;
      if (this.pluckT > repluckEvery || (this.plucks === 1 && this.t >= checkPluckAt)) {
        this.pluck();
      }
    }

    const deviation = this.deviationCents(dt);
    const levelDb = -14 - this.pluckT * 7;
    if (levelDb < -62) return this.silent();

    const wobble = this.wobble.reduce(
      (sum, w) => sum + Math.sin(2 * Math.PI * w.hz * this.pluckT + w.phase),
      0,
    );
    const pitch =
      deviation +
      GLIDE_CENTS * Math.exp(-this.pluckT / GLIDE_SECONDS) +
      DRIFT_CENTS_PER_SECOND * this.pluckT +
      (wobble * WOBBLE_CENTS) / WOBBLE_COMPONENTS;
    this.settle(pitch, dt, fretted);
    const jitter = gaussian(this.random) * (fretted ? 0.6 : 0.5);
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
    const intent = this.intent;
    if (intent.kind === 'fret') {
      // The octave sits on the open string as the player left it.
      const open =
        this.tuned[intent.stringIndex] ?? this.options.tuningOffsets?.[intent.stringIndex] ?? 0;
      return open + (this.errors[intent.stringIndex % this.errors.length] ?? 0);
    }
    if (intent.kind === 'tune') {
      const offset = this.tuned[intent.stringIndex] ?? 0;
      const turned = smoothstep((this.t - TURN_STARTS_AT) / (TURN_ENDS_AT - TURN_STARTS_AT));
      return this.startDetune + (offset - this.startDetune) * turned;
    }
    // Idle: the player turns the peg towards pitch with a slight overshoot, then leaves it.
    const pull = Math.min(1, dt * 1.6);
    this.detune += (0 - this.detune) * pull;
    if (Math.abs(this.detune) < 0.25) this.detune *= 0.5;
    this.inTuneFor = Math.abs(this.detune) < 0.5 ? this.inTuneFor + dt : 0;
    if (this.inTuneFor > 2.2) {
      this.idleString += 1;
      this.restart();
    }
    return this.detune;
  }

  /** Adds the settled detections of the hops in the last dt and drops those out of the window. */
  private settle(cents: number, dt: number, fretted: boolean): void {
    if (this.pluckT < SETTLED_FROM_SECONDS) {
      this.smoothed = cents;
      return;
    }
    this.smoothed += (cents - this.smoothed) * (1 - Math.exp(-dt / SETTLED_SMOOTHING_SECONDS));
    const noise = fretted ? 0.06 : 0.04;
    this.hopDebt += dt * HOPS_PER_SECOND;
    for (; this.hopDebt >= 1; this.hopDebt -= 1) {
      const t = this.pluckT - (this.hopDebt - 1) / HOPS_PER_SECOND;
      this.settled.push({ t, cents: this.smoothed + gaussian(this.random) * noise });
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
