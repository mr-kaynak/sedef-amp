// SPDX-License-Identifier: AGPL-3.0-or-later
// Which copy of a pedal a panel draws (docs/board.md §3.1). The board wraps each panel in the
// copy it stands for; a panel reads its parameters through useCopyParam, so the second Kelp's
// knobs are kelp2Drive and so on (registry.copyParam), and its name gets its number.
import { createContext, useCallback, useContext } from 'react';
import { copyParam } from './registry';

export interface PedalCopy {
  /** The pedal's id ("kelp"). */
  pedal: string;
  /** 1 or 2. */
  copy: number;
}

export const PedalCopyContext = createContext<PedalCopy | null>(null);

export function usePedalCopy(): PedalCopy | null {
  return useContext(PedalCopyContext);
}

/** Maps a first copy's parameter id to the copy this panel draws. */
export function useCopyParam(): (param: string) => string {
  const copy = useContext(PedalCopyContext);
  return useCallback(
    (param: string) => (copy ? copyParam(param, copy.pedal, copy.copy) : param),
    [copy],
  );
}
