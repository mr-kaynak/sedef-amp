// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from 'zustand';
import { defaultSteps, MAX_STEPS } from '../lib/metronome';

export const SECTIONS = ['pedals', 'amp', 'eq', 'cab', 'fx'] as const;
export type Section = (typeof SECTIONS)[number];
export const SECTION_LABELS: Record<Section, string> = {
  pedals: 'Pedals',
  amp: 'Amp',
  eq: 'EQ',
  cab: 'Cab',
  fx: 'FX',
};

export type Overlay = 'tuner' | 'intonation' | null;
export type Popover = 'settings' | 'metronome' | 'input' | 'recorder' | null;
export const SCALES = [0.75, 1, 1.25, 1.5, 2] as const;

interface UiStore {
  section: Section;
  overlay: Overlay;
  popover: Popover;
  scale: number;
  metronomeSteps: number[];
}

export const useUiStore = create<UiStore>()(() => ({
  section: 'amp',
  overlay: null,
  popover: null,
  scale: 1,
  metronomeSteps: defaultSteps(),
}));

export const uiActions = {
  setSection: (section: Section) => useUiStore.setState({ section }),
  openOverlay: (overlay: Overlay) => useUiStore.setState({ overlay, popover: null }),
  toggleOverlay: (overlay: Exclude<Overlay, null>) =>
    useUiStore.setState((s) => ({
      overlay: s.overlay === overlay ? null : overlay,
      popover: null,
    })),
  setPopover: (popover: Popover) => useUiStore.setState({ popover }),
  togglePopover: (popover: Exclude<Popover, null>) =>
    useUiStore.setState((s) => ({ popover: s.popover === popover ? null : popover })),
  setScale: (scale: number) => useUiStore.setState({ scale }),
  setMetronomeSteps: (steps: number[]) => useUiStore.setState({ metronomeSteps: steps }),
};

export function isSection(value: unknown): value is Section {
  return typeof value === 'string' && (SECTIONS as readonly string[]).includes(value);
}

export function sanitizeSteps(value: unknown): number[] {
  const fallback = defaultSteps();
  if (!Array.isArray(value)) return fallback;
  return fallback
    .map((d, i) => {
      const v: unknown = value[i];
      return typeof v === 'number' && v >= 0 && v <= 3 ? Math.round(v) : d;
    })
    .slice(0, MAX_STEPS);
}
