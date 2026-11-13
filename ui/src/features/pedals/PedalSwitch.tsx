// SPDX-License-Identifier: AGPL-3.0-or-later
import { Segmented } from '../../controls/Segmented';
import { useCopyParam } from './copy';
import styles from './Pedal.module.css';

/** A mode switch on an enclosure: the segmented control in an ink well that reads on any paint. */
export function PedalSwitch({ param, labels }: { param: string; labels?: readonly string[] }) {
  const copyParam = useCopyParam();
  return <Segmented param={copyParam(param)} size="sm" labels={labels} className={styles.switch} />;
}
