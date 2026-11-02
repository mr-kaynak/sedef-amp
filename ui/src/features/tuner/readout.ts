// SPDX-License-Identifier: AGPL-3.0-or-later
// What the tuner says, decided on every pitch event (about 60 a second) and timed by the clock,
// so the note, the in-tune state and the numbers change only when they should. The engine's
// display (PitchEvent.display*, docs/dsp/tuner.md "Display") is already calm and keeps one note
// per pluck; this adds the decisions that depend on the tolerance shown:
// - In tune needs a settled reading inside the zone for ENTER_SECONDS. It ends only after
//   LEAVE_SECONDS outside the zone plus a margin (hysteresis), sooner when the reading is far out.
// - Until a note's first settled reading, only a reading well out of the zone, for a while, shows
//   as sharp or flat; otherwise "listening". A plucked string reads sharp for its first 100-200 ms.
// - Sharp or flat give way to listening once the needle is well inside the zone, until in tune is
//   confirmed.
// - The cents show tenths only once the reading is settled (and keep them through a re-pluck),
//   change at most every TEXT_SECONDS, and only when the value has moved by more than a step and
//   a half (tenths) or three quarters of one (whole cents).
// - When the note ends, all of it holds.
import type { PitchEvent } from '../../bridge/protocol';
import { centsBetween, midiToHz } from '../../lib/music';
import { formatCents } from '../intonation/math';
import type { GaugeTarget } from './cents';

export type TuneState = 'idle' | 'listening' | 'in-tune' | 'sharp' | 'flat';

/** A settled reading must stay in the zone this long to count as in tune. */
export const ENTER_SECONDS = 0.15;
/** In tune ends after this long outside the zone and its margin... */
export const LEAVE_SECONDS = 0.25;
/** ...or this long when the reading is far outside it (another string, a big turn of the peg). */
export const LEAVE_FAST_SECONDS = 0.06;
/** Before a note's first settled reading, sharp or flat needs this much beyond the zone... */
export const UNSETTLED_MARGIN_CENTS = 5;
/** ...for this long. */
export const UNSETTLED_SECONDS = 0.15;
/** The numbers change at most this often: four times a second can still be read. */
export const TEXT_SECONDS = 0.25;
/** ...and only when the value has moved by this many steps (0.1 or 1 cent). */
const TEXT_STEP_MOVE = { precise: 1.5, whole: 0.75 };
/** Tenths stay this long after the reading stops being settled, unless it moves. */
export const PRECISION_HOLD_SECONDS = 0.5;

/** Margin beyond the zone before in tune ends. */
export function leaveMargin(inTuneCents: number): number {
  return Math.max(0.3, 0.25 * inTuneCents);
}

/** One pitch event as the gauge sees it: cents from the note it shows. */
export interface ReadoutInput {
  note: number;
  cents: number;
  hz: number;
  settled: boolean;
  held: boolean;
}

/** The engine's display, relative to the nearest note or to a fixed target. */
export function readoutInput(
  e: PitchEvent | null,
  target: GaugeTarget,
  referenceHz: number,
): ReadoutInput | null {
  if (!e || !e.displayValid || !(e.displayHz > 0)) return null;
  const base = { hz: e.displayHz, settled: e.displaySettled, held: e.displayHeld };
  if (target.kind === 'chromatic') return { ...base, note: e.displayNote, cents: e.displayCents };
  return {
    ...base,
    note: target.midi,
    cents: centsBetween(e.displayHz, midiToHz(target.midi, referenceHz)),
  };
}

export interface Readout {
  /** MIDI note shown, or null before the first reading. */
  note: number | null;
  /** Where the needle points, cents from the note; null when idle. */
  cents: number | null;
  tune: TuneState;
  /** The note has ended and the last reading stands. */
  held: boolean;
  /** The cents are shown to a tenth. */
  precise: boolean;
  centsText: string;
  hzText: string;
}

const IDLE: Readout = {
  note: null,
  cents: null,
  tune: 'idle',
  held: false,
  precise: false,
  centsText: '—',
  hzText: '',
};

export class TunerReadout {
  private r: Readout = IDLE;
  private settledOnce = false;
  private enterSince: number | null = null;
  private leaveSince: number | null = null;
  private offSince: number | null = null;
  private lastSettledAt = -Infinity;
  private textAt = -Infinity;
  private shownCents = 0;

  constructor(private readonly inTuneCents: number) {}

  get current(): Readout {
    return this.r;
  }

  /** Feeds one event at time `now` (seconds); null when there is nothing to show. */
  update(input: ReadoutInput | null, now: number): Readout {
    if (!input) {
      this.r = IDLE;
      this.settledOnce = false;
      this.enterSince = this.leaveSince = this.offSince = null;
      return this.r;
    }
    const first = this.r.tune === 'idle';
    const newNote = input.note !== this.r.note;
    let tune: TuneState = newNote ? 'listening' : this.r.tune;
    if (newNote) {
      this.settledOnce = false;
      this.enterSince = this.leaveSince = this.offSince = null;
      this.textAt = -Infinity;
    }
    if (!input.held) {
      if (input.settled) {
        this.settledOnce = true;
        this.lastSettledAt = now;
      }
      tune = this.decide(tune, input, now, first);
    }

    let { precise, centsText, hzText } = this.r;
    if (!input.held) {
      // Tenths once settled, kept through a re-pluck (the engine holds the needle still while the
      // new pluck's long window fills); whole cents while the reading moves.
      const wantPrecise =
        input.settled ||
        (precise &&
          now - this.lastSettledAt < PRECISION_HOLD_SECONDS &&
          Math.abs(input.cents - this.shownCents) < 1.5);
      const step = wantPrecise ? 0.1 : 1;
      const due = now - this.textAt >= TEXT_SECONDS;
      const moved =
        Math.abs(input.cents - this.shownCents) >=
        (wantPrecise ? TEXT_STEP_MOVE.precise : TEXT_STEP_MOVE.whole) * step;
      if (this.textAt === -Infinity || (due && (moved || wantPrecise !== precise))) {
        precise = wantPrecise;
        this.shownCents = Math.round(input.cents / step) * step;
        centsText = formatCents(this.shownCents, precise ? 1 : 0);
        hzText = `${input.hz.toFixed(precise ? 2 : 1)} Hz`;
        this.textAt = now;
      }
    }

    this.r = {
      note: input.note,
      cents: input.cents,
      tune,
      held: input.held,
      precise,
      centsText,
      hzText,
    };
    return this.r;
  }

  private decide(tune: TuneState, input: ReadoutInput, now: number, first: boolean): TuneState {
    const zone = this.inTuneCents;
    const margin = leaveMargin(zone);
    const off = Math.abs(input.cents);
    const side: TuneState = input.cents > 0 ? 'sharp' : 'flat';

    if (tune === 'in-tune') {
      if (off <= zone + margin) {
        this.leaveSince = null;
        return tune;
      }
      this.leaveSince ??= now;
      const far = off > zone + Math.max(3, 2 * zone);
      return now - this.leaveSince >= (far ? LEAVE_FAST_SECONDS : LEAVE_SECONDS) ? side : tune;
    }

    if (off <= zone && input.settled) {
      // A first reading that is already settled (the tuner opened on a ringing note) shows at once.
      this.enterSince ??= now;
      if (first || now - this.enterSince >= ENTER_SECONDS) {
        this.leaveSince = this.offSince = null;
        return 'in-tune';
      }
    } else this.enterSince = null;

    if (off > zone) {
      if (this.settledOnce) return side;
      // The attack: only a reading well out of the zone, and for a while, is called sharp or flat.
      if (off <= zone + UNSETTLED_MARGIN_CENTS) this.offSince = null;
      else if (now - (this.offSince ??= now) >= UNSETTLED_SECONDS) return side;
      return tune === 'idle' ? 'listening' : tune;
    }
    this.offSince = null;
    // Inside the zone but not confirmed yet: sharp or flat give way to listening once the needle is
    // well inside (the band between keeps the boundary from flickering).
    if ((tune === 'sharp' || tune === 'flat') && off <= zone - margin) return 'listening';
    return tune === 'idle' ? 'listening' : tune;
  }
}
