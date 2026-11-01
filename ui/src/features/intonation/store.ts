// SPDX-License-Identifier: AGPL-3.0-or-later
// Workbench state. The instrument profile and results persist in the plugin state (through
// sedef_ui_setState); live progress does not.
import { create } from 'zustand';
import { useShallow } from 'zustand/react/shallow';
import {
  CUSTOM_INSTRUMENT_ID,
  CUSTOM_SCALE_ID,
  INSTRUMENTS,
  MAX_CUSTOM_SCALE_MM,
  MIN_CUSTOM_SCALE_MM,
  SCALE_LENGTHS,
} from './instruments';

export type Phase = 'setup' | 'measure' | 'summary';
export type StringStep = 'tune' | 'fret' | 'result';
/**
 * Where the current pluck is in the step's capture (capture.ts): nothing to measure yet, being
 * measured, or not measurable (its reading stopped being steady, or it is another note).
 */
export type PluckStatus = 'listening' | 'measuring' | 'missed';

/** A steady open-string reading further off than the tolerance, which the player may use. */
export interface TuneOffer {
  hz: number;
  /** Signed distance from the target note. */
  cents: number;
  stdCents: number;
}

export interface StringResult {
  openHz: number;
  frettedHz: number;
  cents: number;
  /**
   * How steady the fretted reading was during the capture, in cents: the scatter of the
   * tracker's detections or the movement of the reading, whichever is larger.
   */
  stdCents: number;
  measuredAt: number;
}

export interface IntonationProfile {
  instrumentId: string;
  customNotes: number[];
  scaleId: string;
  customScaleMm: number;
}

interface IntonationState extends IntonationProfile {
  phase: Phase;
  current: number;
  step: StringStep;
  /** Measured open-string frequency for the current string (set when tuning completes). */
  openHz: number | null;
  results: (StringResult | null)[];
  /**
   * Measuring strings picked from the summary rather than all of them in order: Next goes to the
   * next string without a result, and the summary is one click away.
   */
  fromSummary: boolean;
  pluck: PluckStatus;
  tuneProgress: number;
  /** While the open string is measured: close enough to move on by itself. */
  tuneClose: boolean;
  tuneOffer: TuneOffer | null;
  captureProgress: number;
}

const DEFAULT_PROFILE: IntonationProfile = {
  instrumentId: 'gtr6',
  customNotes: [40, 45, 50, 55, 59, 64],
  scaleId: '25.5',
  customScaleMm: 648,
};

export const useIntonationStore = create<IntonationState>()(() => ({
  ...DEFAULT_PROFILE,
  phase: 'setup',
  current: 0,
  step: 'tune',
  openHz: null,
  results: [],
  fromSummary: false,
  pluck: 'listening',
  tuneProgress: 0,
  tuneClose: false,
  tuneOffer: null,
  captureProgress: 0,
}));

export function stringNotes(s: IntonationProfile): number[] {
  if (s.instrumentId === CUSTOM_INSTRUMENT_ID) return s.customNotes;
  return INSTRUMENTS.find((i) => i.id === s.instrumentId)?.notes ?? DEFAULT_PROFILE.customNotes;
}

export function scaleLengthMm(s: IntonationProfile): number {
  if (s.scaleId === CUSTOM_SCALE_ID) {
    return Math.min(MAX_CUSTOM_SCALE_MM, Math.max(MIN_CUSTOM_SCALE_MM, s.customScaleMm));
  }
  return SCALE_LENGTHS.find((l) => l.id === s.scaleId)?.mm ?? 648;
}

// Narrow subscriptions: live progress changes up to 60 times a second while measuring, and
// components that do not show it must not re-render with it.
export const useStringNotes = () => useIntonationStore(stringNotes);
export const useScaleLength = () => useIntonationStore(scaleLengthMm);
export const useProfile = () =>
  useIntonationStore(
    useShallow((s): IntonationProfile => ({
      instrumentId: s.instrumentId,
      customNotes: s.customNotes,
      scaleId: s.scaleId,
      customScaleMm: s.customScaleMm,
    })),
  );

/** Where Next leads from the current string: a string index, or null for the summary. */
export function nextString(
  s: IntonationProfile & Pick<IntonationState, 'current' | 'results' | 'fromSummary'>,
): number | null {
  const count = stringNotes(s).length;
  if (!s.fromSummary) return s.current + 1 < count ? s.current + 1 : null;
  for (let k = 1; k < count; k += 1) {
    const i = (s.current + k) % count;
    if (!s.results[i]) return i;
  }
  return null;
}

const freshTune = {
  pluck: 'listening' as PluckStatus,
  tuneProgress: 0,
  tuneClose: false,
  tuneOffer: null,
};

const fresh = {
  current: 0,
  step: 'tune' as StringStep,
  openHz: null,
  ...freshTune,
  captureProgress: 0,
};

export const intonationActions = {
  chooseInstrument(id: string) {
    const preset = INSTRUMENTS.find((i) => i.id === id);
    useIntonationStore.setState((s) => ({
      instrumentId: id,
      // A bass needs a bass scale; carry the custom tuning over from the previous preset.
      scaleId: preset ? preset.defaultScaleId : s.scaleId,
      customNotes: preset ? s.customNotes : stringNotes(s),
      results: [],
    }));
  },
  setCustomNote(index: number, midi: number) {
    useIntonationStore.setState((s) => {
      const customNotes = [...s.customNotes];
      customNotes[index] = midi;
      return { customNotes, results: [] };
    });
  },
  setCustomStringCount(count: number) {
    useIntonationStore.setState((s) => {
      const notes = [...s.customNotes];
      while (notes.length < count) notes.push(Math.min(76, (notes[notes.length - 1] ?? 40) + 5));
      return { customNotes: notes.slice(0, count), results: [] };
    });
  },
  chooseScale(id: string) {
    useIntonationStore.setState({ scaleId: id });
  },
  setCustomScaleMm(mm: number) {
    useIntonationStore.setState({ customScaleMm: mm });
  },
  start() {
    useIntonationStore.setState((s) => ({
      ...fresh,
      phase: 'measure',
      fromSummary: false,
      results: stringNotes(s).map((_, i) => s.results[i] ?? null),
    }));
  },
  goToString(index: number) {
    useIntonationStore.setState({ ...fresh, phase: 'measure', current: index, fromSummary: true });
  },
  tuned(openHz: number) {
    useIntonationStore.setState({
      step: 'fret',
      openHz,
      ...freshTune,
      tuneProgress: 1,
      captureProgress: 0,
    });
  },
  /** Measures the 12th fret against the open-string reading on offer, as it is. */
  acceptTuning() {
    const offer = useIntonationStore.getState().tuneOffer;
    if (offer) intonationActions.tuned(offer.hz);
  },
  captured(result: StringResult) {
    useIntonationStore.setState((s) => {
      const results = [...s.results];
      results[s.current] = result;
      return { step: 'result', results, captureProgress: 1 };
    });
  },
  remeasure() {
    useIntonationStore.setState({
      step: 'tune',
      openHz: null,
      ...freshTune,
      captureProgress: 0,
    });
  },
  next() {
    useIntonationStore.setState((s) => {
      const next = nextString(s);
      return next === null ? { phase: 'summary' } : { ...fresh, current: next };
    });
  },
  showSummary() {
    useIntonationStore.setState({ phase: 'summary' });
  },
  backToSetup() {
    useIntonationStore.setState({ ...fresh, phase: 'setup', fromSummary: false });
  },
  setTune(
    pluck: PluckStatus,
    tuneProgress: number,
    tuneClose: boolean,
    tuneOffer: TuneOffer | null,
  ) {
    useIntonationStore.setState((s) =>
      s.pluck === pluck &&
      s.tuneProgress === tuneProgress &&
      s.tuneClose === tuneClose &&
      s.tuneOffer === tuneOffer
        ? s
        : { pluck, tuneProgress, tuneClose, tuneOffer },
    );
  },
  setCapture(pluck: PluckStatus, captureProgress: number) {
    useIntonationStore.setState((s) =>
      s.pluck === pluck && s.captureProgress === captureProgress ? s : { pluck, captureProgress },
    );
  },
};

// --- Persistence ------------------------------------------------------------------------------

export interface PersistedIntonation extends IntonationProfile {
  results: (StringResult | null)[];
}

export function snapshotIntonation(): PersistedIntonation {
  const s = useIntonationStore.getState();
  return {
    instrumentId: s.instrumentId,
    customNotes: s.customNotes,
    scaleId: s.scaleId,
    customScaleMm: s.customScaleMm,
    results: s.results,
  };
}

function isResult(value: unknown): value is StringResult {
  if (!value || typeof value !== 'object') return false;
  const r = value as Record<string, unknown>;
  return ['openHz', 'frettedHz', 'cents', 'stdCents', 'measuredAt'].every(
    (k) => typeof r[k] === 'number' && Number.isFinite(r[k]),
  );
}

/** Restores persisted state, ignoring anything malformed (state may come from older builds). */
export function hydrateIntonation(value: unknown): void {
  if (!value || typeof value !== 'object') return;
  const v = value as Record<string, unknown>;
  const instrumentIds = [...INSTRUMENTS.map((i) => i.id), CUSTOM_INSTRUMENT_ID];
  const scaleIds = [...SCALE_LENGTHS.map((l) => l.id), CUSTOM_SCALE_ID];
  const notes = Array.isArray(v.customNotes)
    ? v.customNotes.filter((n): n is number => typeof n === 'number' && n >= 0 && n <= 127)
    : [];
  useIntonationStore.setState((s) => ({
    instrumentId:
      typeof v.instrumentId === 'string' && instrumentIds.includes(v.instrumentId)
        ? v.instrumentId
        : s.instrumentId,
    scaleId: typeof v.scaleId === 'string' && scaleIds.includes(v.scaleId) ? v.scaleId : s.scaleId,
    customScaleMm:
      typeof v.customScaleMm === 'number' && Number.isFinite(v.customScaleMm)
        ? v.customScaleMm
        : s.customScaleMm,
    customNotes: notes.length >= 1 && notes.length <= 8 ? notes : s.customNotes,
    results: Array.isArray(v.results) ? v.results.map((r) => (isResult(r) ? r : null)) : [],
  }));
}
