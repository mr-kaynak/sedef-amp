// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback, useMemo } from 'react';
import type { ParamSpec } from '../bridge/protocol';
import { formatValue } from './format';
import { fromNormalised, snapValue, toNormalised } from './normalise';
import { getValue, setLocalValue, useParamStore } from './paramStore';
import { beginGesture, endGesture, resetParam, sendValue } from './sender';

export interface ParamHandle {
  id: string;
  spec: ParamSpec;
  /** Real units (dB, Hz, 0..1 for knobs, index for choices, 0/1 for bools). */
  value: number;
  normalised: number;
  display: string;
  set(value: number): void;
  setNormalised(normalised: number): void;
  begin(): void;
  end(): void;
  reset(): void;
}

/** Sets a parameter outside React (keyboard shortcuts, flows): optimistic local update + send. */
export function setParam(id: string, value: number): void {
  const spec = useParamStore.getState().specs[id];
  if (!spec) return;
  const v = snapValue(spec, value);
  if (v === getValue(id)) return;
  setLocalValue(id, v);
  sendValue(id, v);
}

export function useParamValue(id: string): number {
  return useParamStore((s) => s.values[id] ?? s.specs[id]?.default ?? 0);
}

export function useParamSpec(id: string): ParamSpec {
  const spec = useParamStore((s) => s.specs[id]);
  if (!spec) throw new Error(`Unknown parameter "${id}"`);
  return spec;
}

export function useParam(id: string): ParamHandle {
  const spec = useParamSpec(id);
  const value = useParamValue(id);

  const set = useCallback((v: number) => setParam(id, v), [id]);
  const setNormalised = useCallback(
    (n: number) => setParam(id, fromNormalised(spec, n)),
    [id, spec],
  );
  const begin = useCallback(() => beginGesture(id), [id]);
  const end = useCallback(() => endGesture(id), [id]);
  const reset = useCallback(() => {
    setLocalValue(id, spec.default);
    resetParam(id);
  }, [id, spec]);

  return useMemo(
    () => ({
      id,
      spec,
      value,
      normalised: toNormalised(spec, value),
      display: formatValue(spec, value),
      set,
      setNormalised,
      begin,
      end,
      reset,
    }),
    [id, spec, value, set, setNormalised, begin, end, reset],
  );
}
