// SPDX-License-Identifier: AGPL-3.0-or-later
// The amp a preset uses, as a small plate in that amp's own finish.
import type { PresetInfo } from '../../bridge/protocol';
import { AMPS } from '../amp/amps';
import { ampName } from './library';
import styles from './AmpBadge.module.css';

export function AmpBadge({ p }: { p: Pick<PresetInfo, 'amp'> }) {
  const finish = AMPS[p.amp]?.finish;
  return (
    <span
      className={styles.amp}
      data-amp={ampName(p).toLowerCase()}
      style={
        finish
          ? {
              background: `linear-gradient(180deg, ${finish.plate[0]}, ${finish.plate[1]})`,
              color: finish.legend,
            }
          : undefined
      }
    >
      {ampName(p)}
    </span>
  );
}
