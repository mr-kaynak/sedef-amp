// SPDX-License-Identifier: AGPL-3.0-or-later
// Turns the pitch stream into workbench progress. Runs outside React: it reads ~50 events per
// second and writes to the store only on transitions and visible progress changes.
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
/** Stable detections (about 50 per second) a fretted capture needs. */
export const CAPTURE_COUNT = 40;
/** A capture is rejected when its standard error exceeds this. */
export const CAPTURE_MAX_STD_CENTS = 0.6;
/** Gaps shorter than this (between plucks, a dropped frame) do not reset the tuning hold. */
const DROPOUT_GRACE_SECONDS = 0.25;
const EVENT_SECONDS = 0.02;

const quantise = (x: number) => Math.round(Math.min(1, Math.max(0, x)) * 50) / 50;

export function centsFromTarget(e: PitchEvent, targetMidi: number): number {
  return centsBetween(e.frequencyHz, midiToHz(targetMidi, getValue('tunerReference')));
}

export class IntonationController {
  private holdSeconds = 0;
  private holdHz: number[] = [];
  private gapSeconds = 0;

  /** Feeds one pitch event. Exposed for tests; start() wires it to the live channel. */
  handle(e: PitchEvent | null): void {
    const s = useIntonationStore.getState();
    if (s.phase !== 'measure' || !e) return;
    const target = stringNotes(s)[s.current];
    if (target === undefined) return;

    if (s.step === 'tune') this.handleTune(e, target);
    else if (s.step === 'fret' && s.openHz) this.handleFret(e, target + 12, s.openHz);
  }

  reset(): void {
    this.holdSeconds = 0;
    this.holdHz = [];
    this.gapSeconds = 0;
  }

  private handleTune(e: PitchEvent, target: number): void {
    const inWindow =
      e.valid &&
      Math.round(hzToMidi(e.frequencyHz, getValue('tunerReference'))) === target &&
      Math.abs(centsFromTarget(e, target)) <= TUNE_WINDOW_CENTS;

    if (inWindow) {
      this.gapSeconds = 0;
      this.holdSeconds += EVENT_SECONDS;
      this.holdHz.push(e.frequencyHz);
    } else {
      this.gapSeconds += EVENT_SECONDS;
      if (e.valid || this.gapSeconds > DROPOUT_GRACE_SECONDS) {
        this.holdSeconds = 0;
        this.holdHz = [];
      }
    }

    if (this.holdSeconds >= TUNE_HOLD_SECONDS && this.holdHz.length) {
      const openHz = this.holdHz.reduce((a, b) => a + b, 0) / this.holdHz.length;
      this.reset();
      intonationActions.tuned(openHz);
      return;
    }
    intonationActions.setProgress(quantise(this.holdSeconds / TUNE_HOLD_SECONDS), 0);
  }

  private handleFret(e: PitchEvent, octaveMidi: number, openHz: number): void {
    const onTarget =
      e.valid &&
      e.stable &&
      Math.round(hzToMidi(e.stableHz, getValue('tunerReference'))) === octaveMidi;
    if (!onTarget || e.stableStdCents > CAPTURE_MAX_STD_CENTS) {
      if (e.valid && !e.stable) intonationActions.setProgress(1, 0);
      return;
    }
    const progress = Math.min(1, e.stableCount / CAPTURE_COUNT);
    if (progress >= 1) {
      intonationActions.captured({
        openHz,
        frettedHz: e.stableHz,
        cents: octaveErrorCents(openHz, e.stableHz),
        stdCents: e.stableStdCents,
        measuredAt: Date.now(),
      });
      return;
    }
    intonationActions.setProgress(1, quantise(progress));
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
