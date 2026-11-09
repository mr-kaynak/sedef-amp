// SPDX-License-Identifier: AGPL-3.0-or-later
// Add pedal: a shelf under the board with every pedal there is, drawn as it looks, by category.
// The board stays in view above it: pointing at a pedal (hover or focus) shows where it would
// land, or where it already is, and says what it does. A pedal on the board is marked; picking it
// shows it there. The shelf stays open after a pedal is added, and closes with its Close button,
// Escape or Add pedal.
import { useEffect, useId, useRef, useState } from 'react';
import type { KeyboardEvent, RefObject } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon } from '../../controls/Icon';
import { PEDAL_CATALOG } from './catalog';
import { PedalDensityContext } from './density';
import { addPedal, CATEGORIES, pedalName, PEDALS } from './registry';
import { pedalBox } from './sizes';
import styles from './PedalPicker.module.css';

/** How much smaller than a small pedal the shelf draws each one. */
const THUMB = 0.4;

export interface PedalPickerProps {
  open: boolean;
  board: readonly string[];
  onAdd: (id: string) => void;
  /** A pedal that is on the board already: show it there. */
  onShow: (id: string) => void;
  /** The pedal pointed at, for the board to mark its place; null when none is. */
  onConsider: (id: string | null) => void;
  onClose: () => void;
  /** What takes focus back when the shelf closes from the keyboard. */
  returnFocusTo: RefObject<HTMLElement | null>;
}

/** Where a pedal would go on this board, or where it is. */
function placeText(board: readonly string[], id: string): string {
  const onBoard = board.includes(id);
  const after = onBoard ? board : addPedal(board, id);
  const at = after.indexOf(id);
  const where = at === 0 ? 'first' : `after ${pedalName(after[at - 1] ?? '')}`;
  return onBoard ? `On the board, ${where}.` : `Goes ${where}.`;
}

export function PedalPicker({
  open,
  board,
  onAdd,
  onShow,
  onConsider,
  onClose,
  returnFocusTo,
}: PedalPickerProps) {
  const titleId = useId();
  const ref = useRef<HTMLDivElement>(null);
  const [pointed, setPointed] = useState<string | null>(null);

  useEffect(() => {
    if (!open) return;
    ref.current?.querySelector<HTMLElement>('button[data-pedal-card]')?.focus({
      preventScroll: true,
    });
  }, [open]);

  if (!open) return null;

  const consider = (id: string | null) => {
    setPointed(id);
    onConsider(id);
  };

  const close = () => {
    setPointed(null);
    onClose();
    returnFocusTo.current?.focus({ preventScroll: true });
  };

  const onKeyDown = (e: KeyboardEvent<HTMLDivElement>) => {
    if (e.key === 'Escape') {
      e.stopPropagation();
      close();
      return;
    }
    // The cards are one row: the arrow keys walk it, Home and End jump to its ends.
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(e.key)) return;
    const cards = [
      ...(ref.current?.querySelectorAll<HTMLElement>('button[data-pedal-card]') ?? []),
    ];
    const at = cards.indexOf(document.activeElement as HTMLElement);
    if (at < 0) return;
    e.preventDefault();
    const to =
      e.key === 'Home'
        ? 0
        : e.key === 'End'
          ? cards.length - 1
          : Math.min(cards.length - 1, Math.max(0, at + (e.key === 'ArrowRight' ? 1 : -1)));
    cards[to]?.focus();
  };

  const groups = CATEGORIES.map((category) => ({
    category,
    pedals: PEDALS.filter((p) => p.category === category && PEDAL_CATALOG[p.id]).sort(
      (a, b) => a.rank - b.rank,
    ),
  })).filter((g) => g.pedals.length > 0);

  const about = pointed ? PEDAL_CATALOG[pointed] : undefined;

  return (
    <div
      ref={ref}
      role="dialog"
      aria-labelledby={titleId}
      className={styles.shelf}
      onKeyDown={onKeyDown}
      onPointerLeave={() => consider(null)}
    >
      <div className={styles.head}>
        <h2 id={titleId} className={styles.title}>
          Add a pedal
        </h2>
        {/* What the pedal pointed at does and where it goes; each card carries the same for a
            screen reader, so this line is for the eye. */}
        <p className={styles.about} aria-hidden="true">
          {pointed && about ? (
            <>
              <strong className={styles.aboutName}>{pedalName(pointed)}</strong>
              <span className={styles.aboutKind}>{about.kind}</span>
              <span>{about.description}</span>
              <span className={styles.aboutPlace}>{placeText(board, pointed)}</span>
            </>
          ) : (
            'A pedal goes where it usually sits in the chain. Move it afterwards if you like.'
          )}
        </p>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small} ${buttons.icon}`}
          aria-label="Close"
          title="Close"
          onClick={close}
        >
          <Icon name="close" size={12} />
        </button>
      </div>
      <PedalDensityContext value="small">
        <div className={styles.groups}>
          {groups.map(({ category, pedals }) => (
            <section key={category} className={styles.group} aria-label={category}>
              <h3 className={styles.category}>{category}</h3>
              <ul className={styles.list}>
                {pedals.map((p) => {
                  const entry = PEDAL_CATALOG[p.id]!;
                  const { Panel } = entry;
                  const onBoard = board.includes(p.id);
                  const describedBy = `${titleId}-${p.id}`;
                  const size = pedalBox(p.id);
                  return (
                    <li
                      key={p.id}
                      className={styles.card}
                      data-on-board={onBoard || undefined}
                      onPointerEnter={() => consider(p.id)}
                    >
                      {/* The pedal as it looks, at a fraction of its size: a picture, so nothing
                          in it can be reached or is read. */}
                      <div
                        className={styles.thumb}
                        style={{ width: size.w * THUMB, height: size.h * THUMB }}
                        aria-hidden="true"
                        inert
                      >
                        <div className={styles.thumbScale} style={{ scale: String(THUMB) }}>
                          <Panel />
                        </div>
                      </div>
                      <button
                        type="button"
                        className={styles.pick}
                        data-pedal-card={p.id}
                        aria-label={
                          onBoard
                            ? `${p.name}, ${entry.kind.toLowerCase()}, on the board. Show it`
                            : `Add ${p.name}, ${entry.kind.toLowerCase()}`
                        }
                        aria-describedby={describedBy}
                        onFocus={() => consider(p.id)}
                        onBlur={() => consider(null)}
                        onClick={() => (onBoard ? onShow(p.id) : onAdd(p.id))}
                      >
                        <span className={styles.name}>{p.name}</span>
                        <span className={styles.state}>
                          {onBoard ? (
                            'On the board'
                          ) : (
                            <>
                              <Icon name="plus" size={10} />
                              Add
                            </>
                          )}
                        </span>
                      </button>
                      <span id={describedBy} className="visually-hidden">
                        {entry.description}
                      </span>
                    </li>
                  );
                })}
              </ul>
            </section>
          ))}
        </div>
      </PedalDensityContext>
    </div>
  );
}
