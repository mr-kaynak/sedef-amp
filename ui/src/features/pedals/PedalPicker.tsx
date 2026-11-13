// SPDX-License-Identifier: AGPL-3.0-or-later
// Add pedal: a shelf under the board with every pedal there is, drawn as it looks, by category.
// The board stays in view above it: pointing at a pedal (hover or focus) shows where it would
// land, rings the copies already on the board, and says what it does. A pedal can be on the
// board twice: its card adds the first copy where it usually sits, then a second right after the
// first. Modulation cards add after the amp too. The shelf stays open after a pedal is added, and
// closes with its Close button, Escape or Add pedal (docs/board.md §5.3).
import { useEffect, useId, useRef, useState } from 'react';
import type { KeyboardEvent, RefObject } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon } from '../../controls/Icon';
import { PEDAL_CATALOG } from './catalog';
import { PedalScaleContext } from './density';
import {
  addAfterAmp,
  addBlocked,
  addPedal,
  AMP,
  CATEGORIES,
  LATENCY_SLOTS,
  nextCopy,
  pedalName,
  PEDALS,
  sideOf,
  sides,
  tokenOf,
} from './registry';
import { MIN_SCALE, pedalBox } from './sizes';
import styles from './PedalPicker.module.css';

/** How much smaller than a small pedal the shelf draws each one. */
const THUMB = 0.4;

/** Where Add puts a pedal: where it usually sits, or after the amp. */
export type AddWhere = 'pre' | 'post';

export interface PedalPickerProps {
  open: boolean;
  board: readonly string[];
  onAdd: (id: string, where: AddWhere) => void;
  /** A pedal whose copies are all on the board already: show it there. */
  onShow: (id: string) => void;
  /** The pedal pointed at and the button, for the board to mark its place; null when none is. */
  onConsider: (id: string | null, where: AddWhere) => void;
  onClose: () => void;
  /** What takes focus back when the shelf closes from the keyboard. */
  returnFocusTo: RefObject<HTMLElement | null>;
}

/** Where a pedal would go on this board with this button, or where it is. */
function placeText(board: readonly string[], id: string, where: AddWhere): string {
  const blocked = addBlocked(board, id);
  if (blocked === 'drives') return `The board holds ${LATENCY_SLOTS} drives and fuzzes at most.`;
  const next = where === 'post' ? addAfterAmp(board, id) : addPedal(board, id);
  const added = next.find((t) => t !== AMP && !board.includes(t));
  if (!added) {
    const side = sideOf(board, id) ?? sideOf(board, tokenOf(id, 1));
    return `Both on the board${side === 'post' ? ', after the amp' : ''}.`;
  }
  const side = sideOf(next, added)!;
  const list = sides(next)[side];
  const at = list.indexOf(added);
  const where_ = at === 0 ? 'first' : `after ${pedalName(list[at - 1] ?? '')}`;
  const zone = side === 'pre' ? 'in front of the amp' : 'after the amp';
  const which = added.includes('#') ? `${pedalName(added)} goes` : 'Goes';
  return `${which} ${zone}, ${where_}.`;
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
  const [pointed, setPointed] = useState<{ id: string; where: AddWhere } | null>(null);

  useEffect(() => {
    if (!open) return;
    ref.current?.querySelector<HTMLElement>('button[data-pedal-card]')?.focus({
      preventScroll: true,
    });
  }, [open]);

  if (!open) return null;

  const consider = (id: string | null, where: AddWhere = 'pre') => {
    setPointed(id ? { id, where } : null);
    onConsider(id, where);
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

  const about = pointed ? PEDAL_CATALOG[pointed.id] : undefined;

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
        {/* What the pedal pointed at does and where it goes; each card's description says the
            same to a screen reader, so this line is for the eye. */}
        <p className={styles.about} aria-hidden="true">
          {pointed && about ? (
            <>
              <strong className={styles.aboutName}>{pedalName(pointed.id)}</strong>
              <span className={styles.aboutKind}>{about.kind}</span>
              <span>{about.description}</span>
              <span className={styles.aboutPlace}>
                {placeText(board, pointed.id, pointed.where)}
              </span>
            </>
          ) : (
            'A pedal goes where it usually sits; modulation can also go after the amp. Each pedal can be on the board twice.'
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
      <PedalScaleContext value={MIN_SCALE}>
        <div className={styles.groups}>
          {groups.map(({ category, pedals }) => (
            <section key={category} className={styles.group} aria-label={category}>
              <h3 className={styles.category}>{category}</h3>
              <ul className={styles.list}>
                {pedals.map((p) => {
                  const entry = PEDAL_CATALOG[p.id]!;
                  const { Panel } = entry;
                  const kind = entry.kind.toLowerCase();
                  const copies = [0, 1].filter((c) => board.includes(tokenOf(p.id, c))).length;
                  const blocked = addBlocked(board, p.id);
                  const afterAmp = p.afterAmp && nextCopy(board, p.id, 'post') !== null;
                  const describedBy = `${titleId}-${p.id}`;
                  const afterId = `${titleId}-${p.id}-after`;
                  const size = pedalBox(p.id);
                  // The card's line is short, so sixteen cards fit the shelf's width; the name and
                  // the description say it in full.
                  const drivesMax = `${LATENCY_SLOTS} drives max`;
                  const label =
                    blocked === 'both'
                      ? `${p.name}, ${kind}, both on the board. Show them`
                      : `Add ${copies > 0 ? 'another ' : ''}${p.name}, ${kind}${blocked === 'drives' ? `, ${drivesMax}` : ''}`;
                  const state =
                    blocked === 'both' ? (
                      'Both on board'
                    ) : blocked === 'drives' ? (
                      drivesMax
                    ) : copies > 0 ? (
                      <>
                        <Icon name="plus" size={10} />
                        Another
                      </>
                    ) : (
                      <>
                        <Icon name="plus" size={10} />
                        Add
                      </>
                    );
                  return (
                    <li
                      key={p.id}
                      className={styles.card}
                      data-on-board={copies > 0 || undefined}
                      data-blocked={blocked === 'drives' || undefined}
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
                        aria-label={label}
                        aria-describedby={describedBy}
                        aria-disabled={blocked === 'drives' || undefined}
                        onFocus={() => consider(p.id)}
                        onBlur={() => consider(null)}
                        onClick={() => {
                          if (blocked === 'both') onShow(p.id);
                          else if (!blocked) onAdd(p.id, 'pre');
                        }}
                      >
                        <span className={styles.name}>{p.name}</span>
                        <span className={styles.state}>{state}</span>
                      </button>
                      {p.afterAmp && (
                        <button
                          type="button"
                          className={`${buttons.press} ${styles.after}`}
                          data-pedal-card={p.id}
                          aria-label={`Add ${copies > 0 ? 'another ' : ''}${p.name} after the amp`}
                          aria-describedby={afterId}
                          aria-disabled={!afterAmp || undefined}
                          onPointerEnter={() => consider(p.id, 'post')}
                          onPointerLeave={() => consider(p.id)}
                          onFocus={() => consider(p.id, 'post')}
                          onBlur={() => consider(null)}
                          onClick={() => {
                            if (afterAmp) onAdd(p.id, 'post');
                          }}
                        >
                          After
                          <br />
                          the amp
                        </button>
                      )}
                      <span id={describedBy} className="visually-hidden">
                        {entry.description} {placeText(board, p.id, 'pre')}
                      </span>
                      {p.afterAmp && (
                        <span id={afterId} className="visually-hidden">
                          {placeText(board, p.id, 'post')}
                        </span>
                      )}
                    </li>
                  );
                })}
              </ul>
            </section>
          ))}
        </div>
      </PedalScaleContext>
    </div>
  );
}
