// SPDX-License-Identifier: AGPL-3.0-or-later
// Parameter values in real units. Components subscribe with narrow selectors (one value each),
// so a host automation burst re-renders only the controls whose values changed.
import { create } from 'zustand';
import type { ParamSpec, ParamValues } from '../bridge/protocol';
import { bundledSchema, defaultValues, indexSchema } from './schema';
import { isLocallyOwned } from './sender';

interface ParamState {
  specs: Record<string, ParamSpec>;
  values: ParamValues;
}

export const useParamStore = create<ParamState>()(() => ({
  specs: indexSchema(bundledSchema),
  values: defaultValues(bundledSchema),
}));

export function initParams(schema: readonly ParamSpec[], values: ParamValues): void {
  useParamStore.setState({
    specs: indexSchema(schema),
    values: { ...defaultValues(schema), ...values },
  });
}

export function setLocalValue(id: string, value: number): void {
  useParamStore.setState((s) =>
    s.values[id] === value ? s : { values: { ...s.values, [id]: value } },
  );
}

/**
 * Applies values from the host. Values for a control the user is dragging (or just released)
 * are skipped: the host echoes our own changes back a little later, and applying a stale echo
 * would make the control jump backwards under the pointer.
 */
export function applyRemoteValues(incoming: ParamValues): void {
  useParamStore.setState((s) => {
    let changed = false;
    const values = { ...s.values };
    for (const [id, value] of Object.entries(incoming)) {
      if (!(id in s.specs) || values[id] === value || isLocallyOwned(id)) continue;
      values[id] = value;
      changed = true;
    }
    return changed ? { values } : s;
  });
}

export function getValue(id: string): number {
  const s = useParamStore.getState();
  return s.values[id] ?? s.specs[id]?.default ?? 0;
}
