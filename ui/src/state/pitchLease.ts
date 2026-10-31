// SPDX-License-Identifier: AGPL-3.0-or-later
// Pitch analysis costs CPU on the plugin side, so it runs only while something needs it. The
// tuner and the intonation workbench each hold a lease; analysis stops when the last one ends.
import { invoke } from './backendRef';
import { pitch } from './live';

let leases = 0;

export function acquirePitchAnalysis(): () => void {
  leases += 1;
  if (leases === 1) void invoke('sedef_tuner_setActive', true);
  let released = false;
  return () => {
    if (released) return;
    released = true;
    leases -= 1;
    if (leases === 0) {
      void invoke('sedef_tuner_setActive', false);
      pitch.clear();
    }
  };
}
