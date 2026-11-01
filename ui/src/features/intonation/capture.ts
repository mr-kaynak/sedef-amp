// SPDX-License-Identifier: AGPL-3.0-or-later
// One measurement from one pluck, made the same way for the open string and the 12th-fret note.
// Both are read over the same stretch of their decay, so whatever a pluck does to the pitch (the
// end of the attack glide, the slow flat drift as the string decays) is the same in both readings
// and drops out of the octave between them.
//
// A capture averages the tracker's stableHz (the mean of its settled detections, which already
// skip the attack; docs/dsp/tuner.md) over CAPTURE_SECONDS from CAPTURE_FROM_SECONDS after the
// pluck. It carries on through brief losses of the tracker's strict `stable` flag: a string
// wobbling by a cent drops it now and then, and starting again each time made captures late and,
// on a drifting string, off by up to 1.5 cents. A pluck whose reading stops being steady (a peg
// turned while it rings, another note) is not measured; the player plucks again.
// engine/tests/analysis/PitchIntonationTests.cpp runs this rule on the real tracker, and
// docs/intonation.md has the numbers.
import type { PitchEvent } from '../../bridge/protocol';
import { centsBetween } from '../../lib/music';

/**
 * A capture starts this long after the pluck: the tracker's earliest stable reading (0.15 s attack
 * skip, its first long window, then 0.4 s of stability window).
 */
export const CAPTURE_FROM_SECONDS = 0.6;
/** A pluck whose reading is not steady by this long after it is not measured. */
export const CAPTURE_LATEST_START_SECONDS = 1;
/**
 * Length of a capture. One second averages a wobble of a cent (fretting pressure, the neck and
 * body moving) down to a few hundredths of a cent, or about 0.3 cent when it is slow; half a
 * second left up to 0.6 cent of a slow one.
 */
export const CAPTURE_SECONDS = 1;
/**
 * Largest spread of the tracker's settled detections (stableStdCents) during a capture. The
 * tracker's own `stable` wants 0.5 cent, and a capture needs it at least once; a string wobbling
 * by a cent stays well under 1.
 */
export const CAPTURE_MAX_SPREAD_CENTS = 1;
/**
 * How far stableHz may move during a capture. A decaying string drifts and wobbles by under three
 * cents in a second; a peg being turned moves further.
 */
export const CAPTURE_MAX_MOVE_CENTS = 4;
/** Gaps shorter than this (a dropped frame, a brief invalid reading) do not break a capture. */
const DROPOUT_GRACE_SECONDS = 0.25;

export interface CaptureResult {
  /** Mean stableHz over the capture. */
  hz: number;
  /**
   * Steadiness in cents: how far single detections scatter, or how much the reading moved over
   * the capture, whichever is larger.
   */
  stdCents: number;
}

export type CaptureStatus =
  /** No pluck of the note yet, or too soon after it. */
  | { kind: 'listening' }
  /** A capture is running; hz is the latest stableHz. */
  | { kind: 'measuring'; progress: number; hz: number }
  /** This reading completed the capture. */
  | { kind: 'captured'; result: CaptureResult }
  /** Captured earlier from the same pluck; it stands until the string is plucked again. */
  | { kind: 'held'; result: CaptureResult }
  /** This pluck's reading was not steady when it had to be: pluck again. */
  | { kind: 'missed' };

interface Pluck {
  onsetId: number;
  /** When the UI first saw this pluck. */
  onsetAt: number;
  stableCount: number;
  lastValid: number;
  span: Span | null;
  result: CaptureResult | null;
  missed: boolean;
}

interface Span {
  since: number;
  hz: number[];
  spread: number[];
  lo: number;
  hi: number;
  sawStable: boolean;
}

const mean = (xs: number[]) => xs.reduce((a, b) => a + b, 0) / xs.length;

function summarise(s: Span): CaptureResult {
  const hz = mean(s.hz);
  const moved = s.hz.map((x) => centsBetween(x, hz));
  const movement = Math.sqrt(
    moved.reduce((sum, x) => sum + x * x, 0) / Math.max(1, moved.length - 1),
  );
  return { hz, stdCents: Math.max(mean(s.spread), movement) };
}

export class StableCapture {
  private pluck: Pluck | null = null;

  reset(): void {
    this.pluck = null;
  }

  /**
   * Feeds one tracker reading received at time t (seconds). `onTarget` says whether a frequency
   * is the note being measured.
   */
  feed(e: PitchEvent, t: number, onTarget: (hz: number) => boolean): CaptureStatus {
    let p = this.pluck;
    // A new pluck, or a segment the tracker restarted (stableCount falls back): a new note.
    if (!p || e.onsetId !== p.onsetId || (e.valid && e.stableCount < p.stableCount)) {
      p = {
        onsetId: e.onsetId,
        onsetAt: t,
        stableCount: 0,
        lastValid: t,
        span: null,
        result: null,
        missed: false,
      };
      this.pluck = p;
    }
    if (p.result) return { kind: 'held', result: p.result };
    if (p.missed) return { kind: 'missed' };

    const since = t - p.onsetAt;
    if (!e.valid) {
      if (p.span && t - p.lastValid > DROPOUT_GRACE_SECONDS) return this.miss(p);
      return this.progress(p, t);
    }
    p.lastValid = t;
    p.stableCount = e.stableCount;
    if (since < CAPTURE_FROM_SECONDS) return { kind: 'listening' };

    const s = p.span;
    const steady =
      e.stableCount > 0 &&
      e.stableHz > 0 &&
      e.stableStdCents <= CAPTURE_MAX_SPREAD_CENTS &&
      onTarget(e.stableHz) &&
      (!s ||
        centsBetween(Math.max(s.hi, e.stableHz), Math.min(s.lo, e.stableHz)) <=
          CAPTURE_MAX_MOVE_CENTS);
    if (!steady) {
      // Once measuring, or once it is too late to start, this pluck is spent.
      return s || since > CAPTURE_LATEST_START_SECONDS ? this.miss(p) : { kind: 'listening' };
    }
    if (!s) {
      if (since > CAPTURE_LATEST_START_SECONDS) return this.miss(p);
      p.span = { since: t, hz: [], spread: [], lo: e.stableHz, hi: e.stableHz, sawStable: false };
    }
    const span = p.span as Span;
    span.hz.push(e.stableHz);
    span.spread.push(e.stableStdCents);
    span.lo = Math.min(span.lo, e.stableHz);
    span.hi = Math.max(span.hi, e.stableHz);
    span.sawStable ||= e.stable;

    // The tracker must have called the note stable at least once: that keeps a mixture of two
    // ringing strings out (docs/dsp/tuner.md).
    if (t - span.since >= CAPTURE_SECONDS && span.sawStable) {
      p.result = summarise(span);
      p.span = null;
      return { kind: 'captured', result: p.result };
    }
    return this.progress(p, t);
  }

  private miss(p: Pluck): CaptureStatus {
    p.span = null;
    p.missed = true;
    return { kind: 'missed' };
  }

  private progress(p: Pluck, t: number): CaptureStatus {
    const s = p.span;
    if (!s) return { kind: 'listening' };
    return {
      kind: 'measuring',
      progress: Math.min(1, (t - s.since) / CAPTURE_SECONDS),
      hz: s.hz[s.hz.length - 1] ?? 0,
    };
  }
}
