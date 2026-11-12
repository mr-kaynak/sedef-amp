// SPDX-License-Identifier: AGPL-3.0-or-later
// Is the guitar coming in, and at the level the presets were voiced for? The verdict behind the
// chip under the input meter (docs/ui/input-level.md, where every number is explained). Pure and
// driven by explicit time, so tests need no clock.
import { create } from 'zustand';
import type { TelemetryEvent } from '../../bridge/protocol';
import { getValue, useParamStore } from '../../params/paramStore';
import { telemetry } from '../../state/live';

export type InputLevelState = 'none' | 'waiting' | 'quiet' | 'good' | 'hot' | 'clip';

export const INPUT_LEVEL = {
  /** At or under this the plug-in is reporting digital silence (it floors peaks at −100 dB). */
  floorDb: -99,
  /** Events under this (after Input) are not playing: the bottom of the header meter. */
  playingDb: -60,
  /** The zone: the loudest playing (after Input) from here up to `hotDb` is good. */
  quietDb: -12,
  /** From quiet, good needs this much more than `quietDb`. */
  quietHysteresisDb: 1,
  /** Where the engine's clip light latches. */
  hotDb: 0,
  /** The interface's own peak this close to full scale counts as clipping. */
  clipRawDb: -0.5,
  quietWindowS: 8,
  hotWindowS: 3,
  silenceS: 3,
} as const;

interface Peak {
  t: number;
  /** The interface's own level: the peak with the Input knob taken back out. */
  raw: number;
}

export class InputLevelJudge {
  private peaks: Peak[] = [];
  private heldLong: number | null = null;
  private heldShort: number | null = null;
  private silentSince: number | null = null;
  private current: InputLevelState = 'waiting';

  get state(): InputLevelState {
    return this.current;
  }

  /** The loudest playing of the last 8 s (held when the playing stops), after Input. */
  loudestDb(gainDb: number): number | null {
    return this.heldLong === null ? null : this.heldLong + gainDb;
  }

  /** One telemetry event: peaks per channel after Input, Input's value, the time in seconds. */
  push(peaksDb: readonly number[], gainDb: number, now: number): InputLevelState {
    const peak = peaksDb.length ? Math.max(...peaksDb) : Number.NEGATIVE_INFINITY;
    if (peak > INPUT_LEVEL.floorDb) {
      this.silentSince = null;
      if (peak >= INPUT_LEVEL.playingDb) this.peaks.push({ t: now, raw: peak - gainDb });
    } else {
      this.silentSince ??= now;
      if (now - this.silentSince >= INPUT_LEVEL.silenceS) {
        this.peaks = [];
        this.heldLong = null;
        this.heldShort = null;
        this.current = 'none';
        return this.current;
      }
    }
    this.hold(now);
    return this.judge(gainDb);
  }

  /** Judges again with another Input value, for a knob turned between events. */
  judge(gainDb: number): InputLevelState {
    if (this.current === 'none' && this.silentSince !== null) return this.current;
    this.current = this.verdict(gainDb);
    return this.current;
  }

  private verdict(gainDb: number): InputLevelState {
    const long = this.heldLong;
    const short = this.heldShort;
    if (long === null || short === null) return 'waiting';
    if (short >= INPUT_LEVEL.clipRawDb) return 'clip';
    if (short + gainDb >= INPUT_LEVEL.hotDb) return 'hot';
    const threshold =
      this.current === 'quiet'
        ? INPUT_LEVEL.quietDb + INPUT_LEVEL.quietHysteresisDb
        : INPUT_LEVEL.quietDb;
    return long + gainDb < threshold ? 'quiet' : 'good';
  }

  /** Drops peaks older than the long window; each window keeps its last value once empty. */
  private hold(now: number): void {
    const longFrom = now - INPUT_LEVEL.quietWindowS;
    const shortFrom = now - INPUT_LEVEL.hotWindowS;
    let first = 0;
    while (first < this.peaks.length && (this.peaks[first]?.t ?? now) <= longFrom) first += 1;
    if (first) this.peaks.splice(0, first);
    let long: number | null = null;
    let short: number | null = null;
    for (const p of this.peaks) {
      if (long === null || p.raw > long) long = p.raw;
      if (p.t > shortFrom && (short === null || p.raw > short)) short = p.raw;
    }
    if (long !== null) this.heldLong = long;
    if (short !== null) this.heldShort = short;
  }
}

/** The panel's sentences per verdict; `none` depends on where Sedef runs. */
export function levelAdvice(state: InputLevelState, standalone: boolean): string {
  switch (state) {
    case 'none':
      return standalone
        ? "Nothing is coming in. Pick your interface's input in Audio settings, and switch off Mute audio input there."
        : "Nothing is coming in. Check that this track's input is the one your guitar is plugged into, and that the track is monitoring.";
    case 'waiting':
      return 'Play your hardest chords for a few seconds.';
    case 'quiet':
      return 'The amps hear less guitar than the presets expect, so they sound cleaner and thinner. Turn up the gain on your audio interface, or Input here.';
    case 'good':
      return 'Your guitar reaches the amps at the level the presets were made for.';
    case 'hot':
      return 'Louder than the presets expect: drives and amps break up more. Turn Input down.';
    case 'clip':
      return 'Turn the gain on your audio interface down until its clip light stays off, then bring the level back up with Input here.';
  }
}

interface InputLevelView {
  state: InputLevelState;
  /** The loudest playing of the last 8 s after Input, in whole dB; null before any. */
  loudestDb: number | null;
}

export const useInputLevel = create<InputLevelView>()(() => ({
  state: 'waiting',
  loudestDb: null,
}));

function publish(judge: InputLevelJudge, gainDb: number): void {
  const loudest = judge.loudestDb(gainDb);
  const next: InputLevelView = {
    state: judge.state,
    loudestDb: loudest === null ? null : Math.round(loudest),
  };
  const s = useInputLevel.getState();
  if (s.state !== next.state || s.loudestDb !== next.loudestDb) useInputLevel.setState(next);
}

/**
 * Feeds the judge from the telemetry channel and the Input knob. Returns a function that
 * detaches it. `clock` is in seconds.
 */
export function startInputLevel(
  clock: () => number = () => performance.now() / 1000,
  judge = new InputLevelJudge(),
): () => void {
  useInputLevel.setState({ state: 'waiting', loudestDb: null });
  let gain = getValue('inputGain');
  const onEvent = () => {
    const t: TelemetryEvent | null = telemetry.latest();
    if (!t) return;
    gain = getValue('inputGain');
    judge.push(t.inputDb, gain, clock());
    publish(judge, gain);
  };
  // A turn of the Input knob moves the verdict at once, also when no telemetry is flowing.
  const offGain = useParamStore.subscribe((s) => {
    const next = s.values.inputGain ?? gain;
    if (next === gain) return;
    gain = next;
    judge.judge(gain);
    publish(judge, gain);
  });
  const offTelemetry = telemetry.subscribe(onEvent);
  return () => {
    offTelemetry();
    offGain();
  };
}
