// SPDX-License-Identifier: AGPL-3.0-or-later
// Parameter values in real units. Components subscribe with narrow selectors (one value each),
// so a host automation burst re-renders only the controls whose values changed.
import { create } from 'zustand';
import type { ParamSpec, ParamValues } from '../bridge/protocol';
import { bundledSchema, defaultValues, indexSchema } from './schema';
import { deferHostValue, isLocallyOwned, onHostValuesReleased } from './sender';

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
 * are held back, so a stale value never makes the control jump under the pointer, and applied
 * once the user lets go unless the user has changed the parameter since (see deferHostValue).
 */
export function applyRemoteValues(incoming: ParamValues): void {
  const s = useParamStore.getState();
  let values: ParamValues | null = null;
  for (const [id, value] of Object.entries(incoming)) {
    if (!(id in s.specs)) continue;
    if (isLocallyOwned(id)) {
      deferHostValue(id, value);
      continue;
    }
    if (s.values[id] === value) continue;
    values ??= { ...s.values };
    values[id] = value;
  }
  if (values) useParamStore.setState({ values });
}

onHostValuesReleased(applyRemoteValues);

export function getValue(id: string): number {
  const s = useParamStore.getState();
  return s.values[id] ?? s.specs[id]?.default ?? 0;
}
