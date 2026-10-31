// SPDX-License-Identifier: AGPL-3.0-or-later
// Workbench state. The instrument profile and results persist in the plugin state (through
// sedef_ui_setState); live progress does not.
import { create } from 'zustand';
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

export interface StringResult {
  openHz: number;
  frettedHz: number;
  cents: number;
  /** Standard error of the fretted reading, in cents. */
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
  tuneProgress: number;
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
  tuneProgress: 0,
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

const fresh = {
  current: 0,
  step: 'tune' as StringStep,
  openHz: null,
  tuneProgress: 0,
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
      results: stringNotes(s).map((_, i) => s.results[i] ?? null),
    }));
  },
  goToString(index: number) {
    useIntonationStore.setState({ ...fresh, phase: 'measure', current: index });
  },
  tuned(openHz: number) {
    useIntonationStore.setState({ step: 'fret', openHz, tuneProgress: 1, captureProgress: 0 });
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
      tuneProgress: 0,
      captureProgress: 0,
    });
  },
  next() {
    useIntonationStore.setState((s) => {
      const count = stringNotes(s).length;
      if (s.current + 1 >= count) return { phase: 'summary' };
      return { ...fresh, current: s.current + 1 };
    });
  },
  showSummary() {
    useIntonationStore.setState({ phase: 'summary' });
  },
  backToSetup() {
    useIntonationStore.setState({ ...fresh, phase: 'setup' });
  },
  setProgress(tuneProgress: number, captureProgress: number) {
    useIntonationStore.setState((s) =>
      s.tuneProgress === tuneProgress && s.captureProgress === captureProgress
        ? s
        : { tuneProgress, captureProgress },
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
