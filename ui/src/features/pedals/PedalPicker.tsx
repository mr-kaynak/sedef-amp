// SPDX-License-Identifier: AGPL-3.0-or-later
// Add pedal: every pedal there is, by category, with one line about each. A pedal on the board
// already says so, and picking it shows where it is.
import { useId } from 'react';
import type { RefObject } from 'react';
import { Icon } from '../../controls/Icon';
import { Popover } from '../../controls/Popover';
import { PEDAL_CATALOG } from './catalog';
import { CATEGORIES, PEDALS } from './registry';
import styles from './PedalPicker.module.css';

export interface PedalPickerProps {
  open: boolean;
  anchorRef: RefObject<HTMLElement | null>;
  board: readonly string[];
  onAdd: (id: string) => void;
  /** A pedal that is on the board already: show it there. */
  onShow: (id: string) => void;
  onClose: () => void;
  /** Under the toolbar's button (default) or centred under the empty board's. */
  placement?: 'toolbar' | 'below-centre';
}

export function PedalPicker({
  open,
  anchorRef,
  board,
  onAdd,
  onShow,
  onClose,
  placement = 'toolbar',
}: PedalPickerProps) {
  const titleId = useId();
  const groups = CATEGORIES.map((category) => ({
    category,
    pedals: PEDALS.filter((p) => p.category === category && PEDAL_CATALOG[p.id]).sort(
      (a, b) => a.rank - b.rank,
    ),
  })).filter((g) => g.pedals.length > 0);

  return (
    <Popover
      open={open}
      onClose={onClose}
      anchorRef={anchorRef}
      label="Add a pedal"
      className={`${styles.picker} ${placement === 'below-centre' ? styles.centred : ''}`}
    >
      <h2 id={titleId} className={styles.title}>
        Add a pedal
      </h2>
      <div className={styles.groups}>
        {groups.map(({ category, pedals }) => (
          <section key={category} className={styles.group} aria-label={category}>
            <h3 className={styles.category}>{category}</h3>
            <ul className={styles.list}>
              {pedals.map((p) => {
                const entry = PEDAL_CATALOG[p.id]!;
                const onBoard = board.includes(p.id);
                const describedBy = `${titleId}-${p.id}`;
                return (
                  <li key={p.id}>
                    <button
                      type="button"
                      className={styles.row}
                      data-on-board={onBoard || undefined}
                      aria-label={
                        onBoard
                          ? `${p.name}, ${entry.kind.toLowerCase()}, on the board. Show it`
                          : `Add ${p.name}, ${entry.kind.toLowerCase()}`
                      }
                      aria-describedby={describedBy}
                      onClick={() => (onBoard ? onShow(p.id) : onAdd(p.id))}
                    >
                      <span className={styles.name}>{p.name}</span>
                      <span className={styles.kind}>{entry.kind}</span>
                      <span id={describedBy} className={styles.text}>
                        {entry.description}
                      </span>
                      <span className={styles.state} aria-hidden="true">
                        {onBoard ? (
                          'On the board'
                        ) : (
                          <>
                            <Icon name="plus" size={12} />
                            Add
                          </>
                        )}
                      </span>
                    </button>
                  </li>
                );
              })}
            </ul>
          </section>
        ))}
      </div>
    </Popover>
  );
}
