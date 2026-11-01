// SPDX-License-Identifier: AGPL-3.0-or-later
// Turns the pitch stream into workbench progress. Runs outside React: it reads every event (about
// 60 per second from the plugin) and writes to the store only on transitions and visible progress
// changes. Captures are timed with the clock, so they do not depend on the event rate.
import type { PitchEvent } from '../../bridge/protocol';
import { getValue } from '../../params/paramStore';
import { getBackend } from '../../state/backendRef';
import { pitch } from '../../state/live';
import { acquirePitchAnalysis } from '../../state/pitchLease';
import { centsBetween, hzToMidi, midiToHz } from '../../lib/music';
import { StableCapture } from './capture';
import { isOctaveOf, octaveErrorCents } from './math';
import { intonationActions, stringNotes, useIntonationStore } from './store';

/**
 * The open string moves on by itself when its capture lands within this many cents of the target.
 * How close it is barely matters to the result: the octave is measured against what the string
 * actually plays, and 5 cents changes its tension by 0.6%, which changes the few cents of
 * fretting sharpness by a few hundredths of a cent. It only has to be close enough that the guitar
 * is still in tune afterwards. Further off, on the right note, the player can use the reading as
 * it is (docs/intonation.md).
 */
export const TUNE_TOLERANCE_CENTS = 5;

const quantise = (x: number) => Math.round(Math.min(1, Math.max(0, x)) * 50) / 50;
const nowSeconds = () => performance.now() / 1000;

export class IntonationController {
  /** One capture rule for both steps (capture.ts). */
  private readonly capture = new StableCapture();
  /** The pluck the tracker reported last, in any step. */
  private lastOnsetId: number | null = null;

  /**
   * Feeds one pitch event received at time t (seconds). Exposed for tests; start() wires it to the
   * live channel.
   */
  handle(e: PitchEvent | null, t = nowSeconds()): void {
    if (e) this.lastOnsetId = e.onsetId;
    const s = useIntonationStore.getState();
    if (s.phase !== 'measure' || !e) return;
    const target = stringNotes(s)[s.current];
    if (target === undefined) return;

    if (s.step === 'tune') this.handleTune(e, target, t);
    else if (s.step === 'fret' && s.openHz) this.handleFret(e, s.openHz, t);
  }

  /** Starts over for a new step; the note still ringing from the last one does not count. */
  reset(): void {
    this.capture.reset(this.lastOnsetId);
  }

  private handleTune(e: PitchEvent, target: number, t: number): void {
    // The open string counts on the right note: its nearest semitone is the target.
    const reference = getValue('tunerReference');
    const status = this.capture.feed(e, t, (hz) => Math.round(hzToMidi(hz, reference)) === target);
    const targetHz = midiToHz(target, getValue('tunerReference'));
    switch (status.kind) {
      case 'captured': {
        const { hz, stdCents } = status.result;
        const cents = centsBetween(hz, targetHz);
        if (Math.abs(cents) <= TUNE_TOLERANCE_CENTS) {
          this.reset();
          intonationActions.tuned(hz);
        } else {
          intonationActions.setTune('measuring', 1, false, { hz, cents, stdCents });
        }
        return;
      }
      case 'held':
        // A reading on offer stands until the string is plucked again.
        return;
      case 'measuring': {
        const close = Math.abs(centsBetween(status.hz, targetHz)) <= TUNE_TOLERANCE_CENTS;
        intonationActions.setTune('measuring', quantise(status.progress), close, null);
        return;
      }
      case 'listening':
      case 'missed':
        intonationActions.setTune(status.kind, 0, false, null);
    }
  }

  private handleFret(e: PitchEvent, openHz: number, t: number): void {
    // The octave counts near twice the open string as measured, wherever that falls on the grid.
    const status = this.capture.feed(e, t, (hz) => isOctaveOf(hz, openHz));
    if (status.kind === 'captured') {
      const { hz, stdCents } = status.result;
      this.reset();
      intonationActions.captured({
        openHz,
        frettedHz: hz,
        cents: octaveErrorCents(openHz, hz),
        stdCents,
        measuredAt: Date.now(),
      });
      return;
    }
    if (status.kind === 'measuring') {
      intonationActions.setCapture('measuring', quantise(status.progress));
    } else {
      intonationActions.setCapture(status.kind === 'missed' ? 'missed' : 'listening', 0);
    }
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
    sim.setIntent({ kind: 'tune', midi: target, stringIndex: s.current });
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
