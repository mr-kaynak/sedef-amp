// SPDX-License-Identifier: AGPL-3.0-or-later
// The Pedals page: the pedalboard. Pedals sit in signal order, left to right and row by row, each
// with a strip for moving it (drag the handle, its arrow keys, or the move buttons) and taking it
// off the board. Add pedal opens the picker. Moves and additions are announced politely; removals
// show a notice with Undo.
import { useCallback, useEffect, useId, useLayoutEffect, useRef, useState } from 'react';
import type { KeyboardEvent } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import { boardActions, useBoardStore } from '../../state/boardStore';
import { PEDAL_CATALOG } from './catalog';
import { PedalPicker } from './PedalPicker';
import { pedalName } from './registry';
import { useBoardDrag } from './useBoardDrag';
import styles from './PedalBoard.module.css';

/** Space between pedals, in canvas pixels (PedalBoard.module.css, --gap). */
const GAP = 24;

/** The strip control that keeps focus after a change: the handle, or the move button used. */
type Control = 'handle' | 'left' | 'right';

const plural = new Intl.PluralRules('en');

function countText(n: number): string {
  return plural.select(n) === 'one' ? `${n} pedal` : `${n} pedals`;
}

/** Where a pedal sits, for the announcement after it moved or joined. */
function placeText(board: readonly string[], id: string): string {
  const at = board.indexOf(id);
  const where = at === 0 ? 'first' : `after ${pedalName(board[at - 1] ?? '')}`;
  return `${at + 1} of ${board.length}, ${where}`;
}

export function PedalBoard() {
  const pedals = useBoardStore((s) => s.pedals);
  const supported = useBoardStore((s) => s.supported);
  const [picker, setPicker] = useState<'toolbar' | 'empty' | null>(null);
  const [announcement, setAnnouncement] = useState('');
  const toolbarAdd = useRef<HTMLButtonElement>(null);
  const emptyAdd = useRef<HTMLButtonElement>(null);
  const listRef = useRef<HTMLOListElement>(null);
  const scrollerRef = useRef<HTMLDivElement>(null);
  const handles = useRef(new Map<string, HTMLButtonElement>());
  const focusNext = useRef<{ id: string | null; reveal: boolean; control: Control } | null>(null);
  const hintId = useId();

  /**
   * After the board re-renders: focus a strip control of a pedal (the handle when that control
   * has switched off at an end of the board) and bring it into view, or Add pedal when the board
   * is empty.
   */
  const afterRender = (id: string | null, reveal = false, control: Control = 'handle') => {
    focusNext.current = { id, reveal, control };
  };
  useLayoutEffect(() => {
    const next = focusNext.current;
    if (!next) return;
    focusNext.current = null;
    const slot = next.id
      ? listRef.current?.querySelector<HTMLElement>(`[data-pedal="${next.id}"]`)
      : null;
    const wanted = slot?.querySelector<HTMLButtonElement>(`[data-control="${next.control}"]`);
    const target = wanted && !wanted.disabled ? wanted : next.id && handles.current.get(next.id);
    if (target) {
      target.focus({ preventScroll: true });
      // A pedal just added or picked glides into view, unless motion is unwelcome.
      const glide = next.reveal && !window.matchMedia?.('(prefers-reduced-motion: reduce)').matches;
      slot?.scrollIntoView?.({ block: 'nearest', behavior: glide ? 'smooth' : 'auto' });
    } else (toolbarAdd.current ?? emptyAdd.current)?.focus();
  });

  const move = useCallback((id: string, index: number, control: Control = 'handle') => {
    const board = useBoardStore.getState().pedals;
    if (board.indexOf(id) === index) return;
    boardActions.move(id, index);
    const after = useBoardStore.getState().pedals;
    setAnnouncement(`${pedalName(id)} moved to ${placeText(after, id)}.`);
    afterRender(id, false, control);
  }, []);

  const { drag, handleProps } = useBoardDrag({
    listRef,
    scrollerRef,
    gap: GAP,
    onDrop: move,
  });

  const remove = (id: string) => {
    const board = useBoardStore.getState().pedals;
    const at = board.indexOf(id);
    const neighbour = board[at + 1] ?? board[at - 1] ?? null;
    boardActions.remove(id);
    afterRender(neighbour);
  };

  const add = (id: string) => {
    setPicker(null);
    boardActions.add(id);
    setAnnouncement(`Added ${pedalName(id)}, ${placeText(useBoardStore.getState().pedals, id)}.`);
    afterRender(id, true);
  };

  const show = (id: string) => {
    setPicker(null);
    afterRender(id, true);
  };

  const onHandleKey = (id: string) => (e: KeyboardEvent<HTMLButtonElement>) => {
    const at = pedals.indexOf(id);
    const to =
      e.key === 'ArrowLeft' || e.key === 'ArrowUp'
        ? at - 1
        : e.key === 'ArrowRight' || e.key === 'ArrowDown'
          ? at + 1
          : e.key === 'Home'
            ? 0
            : e.key === 'End'
              ? pedals.length - 1
              : null;
    if (to === null) return;
    e.preventDefault();
    if (to >= 0 && to < pedals.length) move(id, to);
  };

  // Forget the handles of pedals that left.
  useEffect(() => {
    for (const id of handles.current.keys()) if (!pedals.includes(id)) handles.current.delete(id);
  }, [pedals]);

  const closePicker = useCallback(() => setPicker(null), []);

  return (
    <div className={styles.section}>
      <div className={styles.toolbar}>
        <p className={styles.flow}>Signal flows left to right, row by row, into the amp.</p>
        <span className={styles.count}>{countText(pedals.length)}</span>
        {supported && (
          <button
            ref={toolbarAdd}
            type="button"
            className={`${buttons.solid} ${buttons.lead}`}
            aria-haspopup="dialog"
            aria-expanded={picker === 'toolbar'}
            onClick={() => setPicker((p) => (p === 'toolbar' ? null : 'toolbar'))}
          >
            <Icon name="plus" size={14} stroke={ICON_STROKE_LABEL} />
            Add pedal
          </button>
        )}
        <PedalPicker
          open={picker === 'toolbar'}
          anchorRef={toolbarAdd}
          board={pedals}
          onAdd={add}
          onShow={show}
          onClose={closePicker}
        />
      </div>

      <div ref={scrollerRef} className={styles.scroller} data-dragging={drag ? true : undefined}>
        {pedals.length === 0 ? (
          <div className={styles.empty}>
            <p className={styles.emptyTitle}>No pedals on the board</p>
            <p className={styles.emptyText}>
              Your guitar goes straight into the amp. Add a pedal to shape it first.
            </p>
            {supported && (
              <div className={styles.emptyAction}>
                <button
                  ref={emptyAdd}
                  type="button"
                  className={`${buttons.nacre} ${buttons.lead}`}
                  aria-haspopup="dialog"
                  aria-expanded={picker === 'empty'}
                  onClick={() => setPicker((p) => (p === 'empty' ? null : 'empty'))}
                >
                  <Icon name="plus" size={14} stroke={ICON_STROKE_LABEL} />
                  Add pedal
                </button>
                <PedalPicker
                  open={picker === 'empty'}
                  anchorRef={emptyAdd}
                  board={pedals}
                  onAdd={add}
                  onShow={show}
                  onClose={closePicker}
                  placement="below-centre"
                />
              </div>
            )}
          </div>
        ) : (
          <ol ref={listRef} className={styles.board} aria-label="Pedalboard, in signal order">
            {pedals.map((id, i) => {
              const entry = PEDAL_CATALOG[id];
              if (!entry) return null;
              const { Panel } = entry;
              const name = pedalName(id);
              const dragging = drag?.id === id;
              return (
                <li
                  key={id}
                  data-pedal={id}
                  className={styles.slot}
                  data-dragging={dragging || undefined}
                  style={dragging ? { translate: `${drag.dx}px ${drag.dy}px` } : undefined}
                >
                  {supported && (
                    <div className={styles.strip}>
                      <button
                        ref={(el) => {
                          if (el) handles.current.set(id, el);
                        }}
                        type="button"
                        className={styles.handle}
                        data-control="handle"
                        aria-label={`Move ${name}`}
                        aria-describedby={hintId}
                        onKeyDown={onHandleKey(id)}
                        {...handleProps(id)}
                      >
                        <Icon name="grip" size={16} />
                      </button>
                      <span className={styles.stripGap} />
                      <button
                        type="button"
                        className={`${buttons.ghost} ${buttons.small} ${buttons.icon} ${styles.tool}`}
                        data-control="left"
                        aria-label={`Move ${name} left`}
                        title="Move left"
                        disabled={i === 0}
                        onClick={() => move(id, i - 1, 'left')}
                      >
                        <Icon name="prev" size={14} />
                      </button>
                      <button
                        type="button"
                        className={`${buttons.ghost} ${buttons.small} ${buttons.icon} ${styles.tool}`}
                        data-control="right"
                        aria-label={`Move ${name} right`}
                        title="Move right"
                        disabled={i === pedals.length - 1}
                        onClick={() => move(id, i + 1, 'right')}
                      >
                        <Icon name="next" size={14} />
                      </button>
                      <button
                        type="button"
                        className={`${buttons.ghost} ${buttons.small} ${buttons.icon} ${styles.tool}`}
                        aria-label={`Remove ${name}`}
                        title="Remove from the board"
                        onClick={() => remove(id)}
                      >
                        <Icon name="close" size={12} />
                      </button>
                    </div>
                  )}
                  <Panel />
                </li>
              );
            })}
            {drag?.bar && (
              <li
                aria-hidden="true"
                className={styles.bar}
                style={{ left: drag.bar.x, top: drag.bar.y, height: drag.bar.height }}
              />
            )}
          </ol>
        )}
      </div>

      <span id={hintId} className="visually-hidden">
        Drag to move it, or use the arrow keys.
      </span>
      <div className="visually-hidden" role="status" aria-live="polite">
        {announcement}
      </div>
    </div>
  );
}
