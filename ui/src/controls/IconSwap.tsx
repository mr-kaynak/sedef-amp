// SPDX-License-Identifier: AGPL-3.0-or-later
// An icon that changes with an infrequent state (copy to copied, play to stop) and cross-fades
// between the two. Both icons stay mounted in one grid cell, so either direction animates and
// nothing moves on first render. The button's label or name carries the state as well.
import { Icon, ICON_STROKE, type IconName } from './Icon';
import styles from './IconSwap.module.css';

export function IconSwap({
  active,
  icon,
  activeIcon,
  size = 16,
  stroke = ICON_STROKE,
  filled = false,
}: {
  /** Shows `activeIcon` when true, `icon` when false. */
  active: boolean;
  icon: IconName;
  activeIcon: IconName;
  size?: number;
  stroke?: number;
  filled?: boolean;
}) {
  return (
    <span className={styles.swap} aria-hidden="true">
      <span className={styles.layer} data-shown={!active || undefined}>
        <Icon name={icon} size={size} stroke={stroke} filled={filled} />
      </span>
      <span className={styles.layer} data-shown={active || undefined}>
        <Icon name={activeIcon} size={size} stroke={stroke} filled={filled} />
      </span>
    </span>
  );
}
