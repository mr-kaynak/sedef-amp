// SPDX-License-Identifier: AGPL-3.0-or-later
// Turns the pitch stream into workbench progress. Runs outside React: it reads every event (about
// 60 per second from the plugin) and writes to the store only on transitions and visible progress
// changes. Holds are timed with the clock, so they do not depend on the event rate.
import type { PitchEvent } from '../../bridge/protocol';
import { getValue } from '../../params/paramStore';
import { getBackend } from '../../state/backendRef';
import { pitch } from '../../state/live';
import { acquirePitchAnalysis } from '../../state/pitchLease';
import { centsBetween, hzToMidi, midiToHz } from '../../lib/music';
import { octaveErrorCents } from './math';
import { intonationActions, stringNotes, useIntonationStore } from './store';

/** The open string must stay within this window ... */
export const TUNE_WINDOW_CENTS = 1;
/** ... for this long before the workbench moves on. */
export const TUNE_HOLD_SECONDS = 1;
/**
 * The fretted note is captured once the tracker has called it stable, without a break and from
 * one pluck, for this long, and stableHz is averaged over that time. The first stable windows
 * still carry the end of the attack glide (docs/dsp/tuner.md), so one alone does not decide.
 */
export const CAPTURE_SECONDS = 0.5;
/** Gaps shorter than this (between plucks, a dropped frame) do not reset the tuning hold. */
const DROPOUT_GRACE_SECONDS = 0.25;

const quantise = (x: number) => Math.round(Math.min(1, Math.max(0, x)) * 50) / 50;
const nowSeconds = () => performance.now() / 1000;
const mean = (xs: number[]) => xs.reduce((a, b) => a + b, 0) / xs.length;

export function centsFromTarget(e: PitchEvent, targetMidi: number): number {
  return centsBetween(e.frequencyHz, midiToHz(targetMidi, getValue('tunerReference')));
}

interface TuneHold {
  since: number;
  lastInWindow: number;
  hz: number[];
}

interface Capture {
  onsetId: number;
  stableCount: number;
  since: number;
  stableHz: number[];
  stableStdCents: number[];
}

export class IntonationController {
  private hold: TuneHold | null = null;
  private capture: Capture | null = null;

  /**
   * Feeds one pitch event received at time t (seconds). Exposed for tests; start() wires it to the
   * live channel.
   */
  handle(e: PitchEvent | null, t = nowSeconds()): void {
    const s = useIntonationStore.getState();
    if (s.phase !== 'measure' || !e) return;
    const target = stringNotes(s)[s.current];
    if (target === undefined) return;

    if (s.step === 'tune') this.handleTune(e, target, t);
    else if (s.step === 'fret' && s.openHz) this.handleFret(e, target + 12, s.openHz, t);
  }

  reset(): void {
    this.hold = null;
    this.capture = null;
  }

  private handleTune(e: PitchEvent, target: number, t: number): void {
    const inWindow =
      e.valid &&
      Math.round(hzToMidi(e.frequencyHz, getValue('tunerReference'))) === target &&
      Math.abs(centsFromTarget(e, target)) <= TUNE_WINDOW_CENTS;

    if (inWindow) {
      this.hold ??= { since: t, lastInWindow: t, hz: [] };
      this.hold.lastInWindow = t;
      this.hold.hz.push(e.frequencyHz);
    } else if (this.hold && (e.valid || t - this.hold.lastInWindow > DROPOUT_GRACE_SECONDS)) {
      this.hold = null;
    }

    const held = this.hold ? this.hold.lastInWindow - this.hold.since : 0;
    if (this.hold && held >= TUNE_HOLD_SECONDS) {
      const openHz = mean(this.hold.hz);
      this.reset();
      intonationActions.tuned(openHz);
      return;
    }
    intonationActions.setProgress(quantise(held / TUNE_HOLD_SECONDS), 0);
  }

  private handleFret(e: PitchEvent, octaveMidi: number, openHz: number, t: number): void {
    const onTarget =
      e.valid &&
      e.stable &&
      Math.round(hzToMidi(e.stableHz, getValue('tunerReference'))) === octaveMidi;
    if (!onTarget) {
      this.capture = null;
      intonationActions.setProgress(1, 0);
      return;
    }

    // A new pluck, or a segment the tracker restarted (stableCount falls back), starts again.
    let c = this.capture;
    if (!c || c.onsetId !== e.onsetId || e.stableCount < c.stableCount) {
      c = {
        onsetId: e.onsetId,
        stableCount: e.stableCount,
        since: t,
        stableHz: [],
        stableStdCents: [],
      };
      this.capture = c;
    }
    c.stableCount = e.stableCount;
    c.stableHz.push(e.stableHz);
    c.stableStdCents.push(e.stableStdCents);

    const progress = (t - c.since) / CAPTURE_SECONDS;
    if (progress < 1) {
      intonationActions.setProgress(1, quantise(progress));
      return;
    }
    // Steadiness: how far single detections scatter, or how much the reading moved over the
    // hold, whichever is larger.
    const frettedHz = mean(c.stableHz);
    const drift = c.stableHz.map((hz) => centsBetween(hz, frettedHz));
    const stdCents = Math.max(
      mean(c.stableStdCents),
      Math.sqrt(drift.reduce((sum, x) => sum + x * x, 0) / Math.max(1, drift.length - 1)),
    );
    this.capture = null;
    intonationActions.captured({
      openHz,
      frettedHz,
      cents: octaveErrorCents(openHz, frettedHz),
      stdCents,
      measuredAt: Date.now(),
    });
  }
}

/** Tells the mock's virtual player what a real player would be doing at this step. */
function syncSimulator(): void {
  const sim = getBackend()?.simulator;
  if (!sim) return;
  const s = useIntonationStore.getState();
  const target = stringNotes(s)[s.current];
  if (s.phase !== 'measure' || target === undefined || s.step === 'result') {
    sim.setIntent({ kind: 'idle' });
  } else if (s.step === 'tune') {
    sim.setIntent({ kind: 'tune', midi: target });
  } else {
    sim.setIntent({ kind: 'fret', midi: target + 12, stringIndex: s.current });
  }
}

/** Starts measuring: holds pitch analysis and follows the store. Returns a stop function. */
export function startIntonationSession(): () => void {
  const controller = new IntonationController();
  const release = acquirePitchAnalysis();
  let lastKey = '';
  const unsubscribeStore = useIntonationStore.subscribe((s) => {
    const key = `${s.phase}/${s.current}/${s.step}`;
    if (key === lastKey) return;
    lastKey = key;
    controller.reset();
    syncSimulator();
  });
  lastKey = (() => {
    const s = useIntonationStore.getState();
    return `${s.phase}/${s.current}/${s.step}`;
  })();
  syncSimulator();
  const unsubscribePitch = pitch.subscribe(() => controller.handle(pitch.latest()));
  return () => {
    unsubscribePitch();
    unsubscribeStore();
    getBackend()?.simulator?.setIntent({ kind: 'idle' });
    release();
  };
}
