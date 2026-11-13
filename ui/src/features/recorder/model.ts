// SPDX-License-Identifier: AGPL-3.0-or-later
// What the recorder says aloud when a take starts, drops out and ends.
import { spoken } from './format';

export type TakeStatus = 'idle' | 'starting' | 'recording' | 'saving';

/** What a change of state says aloud (a polite live region), or nothing. */
export function takeAnnouncement(
  previous: TakeStatus | null,
  status: TakeStatus,
  take: { seconds: number; ended: string; dropouts?: number } | null,
): string {
  if (previous === status || previous === null) return '';
  // Not just "Recording": the looper says that for its own take.
  if (status === 'recording') return 'Recording a take';
  if (status === 'idle' && previous !== 'idle' && take) {
    if (take.ended === 'empty') return 'Nothing was recorded';
    if (take.ended === 'writeError') return 'The take stopped: the disk could not keep it';
    const lost = take.dropouts ?? 0;
    const dropouts = lost === 0 ? '' : lost === 1 ? ', 1 dropout' : `, ${lost} dropouts`;
    return `Take saved, ${spoken(take.seconds)}${dropouts}`;
  }
  return '';
}

/** Said once, when a take's first dropout happens. */
export const DROPOUT_ANNOUNCEMENT = 'Dropout: the disk could not keep up';
