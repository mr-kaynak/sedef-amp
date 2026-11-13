// SPDX-License-Identifier: AGPL-3.0-or-later
// What the recorder says aloud when a take starts and ends.
import { spoken } from './format';

export type TakeStatus = 'idle' | 'starting' | 'recording' | 'saving';

/** What a change of state says aloud (a polite live region), or nothing. */
export function takeAnnouncement(
  previous: TakeStatus | null,
  status: TakeStatus,
  take: { seconds: number; ended: string } | null,
): string {
  if (previous === status || previous === null) return '';
  if (status === 'recording') return 'Recording';
  if (status === 'idle' && previous !== 'idle' && take) {
    if (take.ended === 'empty') return 'Nothing was recorded';
    if (take.ended === 'writeError') return 'The take stopped: the disk could not keep it';
    return `Take saved, ${spoken(take.seconds)}`;
  }
  return '';
}
