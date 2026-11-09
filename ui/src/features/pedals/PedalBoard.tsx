// SPDX-License-Identifier: AGPL-3.0-or-later
// The Pedals page: the pedalboard. The signal comes in at the input jack, runs along a cable
// through the pedals in order, row by row, and into the amp. Each pedal carries its own tools
// (a handle, move left and right, remove), shown when the pointer or the keyboard is on it; a
// pedal also drags by its enclosure. Add pedal opens the shelf under the board. Moves and
// additions are announced politely; removals show a notice with Undo.
import { useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState } from 'react';
import type { CSSProperties, KeyboardEvent } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import { useParamValue } from '../../params/useParam';
import { AMP_NAMES } from '../../params/schema';
import { boardActions, useBoardStore } from '../../state/boardStore';
import { uiActions } from '../../state/uiStore';
import { AMPS } from '../amp/amps';
import { PEDAL_CATALOG } from './catalog';
import { PedalScaleContext } from './density';
import { gapAt, layoutBoard, type Arrow, type BoardLayout } from './layout';
import { PedalPicker } from './PedalPicker';
import { addPedal, pedalName } from './registry';
import { useBoardDrag } from './useBoardDrag';
import styles from './PedalBoard.module.css';

/** The tool that keeps focus after a change: the handle, or the move button used. */
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

/** Room under an empty board's cable for what the page says about it. */
const EMPTY_TEXT = 150;

const ARROWS: Record<Arrow['heading'], string> = {
  right: 'M-2 -3.5 1.5 0 -2 3.5',
  left: 'M2 -3.5 -1.5 0 2 3.5',
  down: 'M-3.5 -2 0 1.5 3.5 -2',
};

/** The signal's path: a cable from the input jack to the amp, with its direction marked. */
function Cable({ layout }: { layout: BoardLayout }) {
  return (
    <svg
      className={styles.cable}
      width="100%"
      height={layout.height}
      aria-hidden="true"
      focusable="false"
    >
      <path className={styles.cableJacket} d={layout.cable} />
      <path className={styles.cableCore} d={layout.cable} />
      {layout.arrows.map((a) => (
        <path
          key={`${a.x},${a.y}`}
          className={styles.cableArrow}
          d={ARROWS[a.heading]}
          transform={`translate(${a.x} ${a.y})`}
        />
      ))}
    </svg>
  );
}

/** Where the board's signal goes: the amp, by name, and a way to its page. */
function AmpTerminal({ style }: { style: CSSProperties }) {
  const model = Math.round(useParamValue('ampModel'));
  const capture = useParamValue('ampSource') >= 0.5;
  const amp = AMPS[model];
  const name = capture ? 'Capture' : (AMP_NAMES[model] ?? 'Amp');
  const paint =
    capture || !amp
      ? {}
      : ({
          '--amp-top': amp.finish.plate[0],
          '--amp-bottom': amp.finish.plate[1],
          '--amp-legend': amp.finish.legend,
        } as CSSProperties);
  return (
    <button
      type="button"
      className={`${buttons.press} ${styles.amp}`}
      style={{ ...style, ...paint }}
      aria-label={`Into the amp: ${name}. Open the Amp page`}
      title="Open the Amp page"
      onClick={() => uiActions.setSection('amp')}
    >
      <span className={styles.ampCaption}>Amp</span>
      <span className={styles.ampName}>{name}</span>
      <span className={styles.ampGo} aria-hidden="true">
        <Icon name="next" size={12} />
      </span>
    </button>
  );
}

const box = (r: { x: number; y: number; w: number; h: number }): CSSProperties => ({
  left: r.x,
  top: r.y,
  width: r.w,
  height: r.h,
});

export function PedalBoard() {
  const pedals = useBoardStore((s) => s.pedals);
  const supported = useBoardStore((s) => s.supported);
  const [picker, setPicker] = useState<'toolbar' | 'empty' | null>(null);
  /** The pedal the shelf is pointing at: where it would land, or where it is. */
  const [considered, setConsidered] = useState<string | null>(null);
  const [announcement, setAnnouncement] = useState('');
  const toolbarAdd = useRef<HTMLButtonElement>(null);
  const emptyAdd = useRef<HTMLButtonElement>(null);
  const listRef = useRef<HTMLOListElement>(null);
  const scrollerRef = useRef<HTMLDivElement>(null);
  const handles = useRef(new Map<string, HTMLButtonElement>());
  const focusNext = useRef<{ id: string | null; reveal: boolean; control: Control } | null>(null);
  const hintId = useId();

  const shown = useMemo(() => pedals.filter((id) => PEDAL_CATALOG[id]), [pedals]);
  const layout = useMemo(() => layoutBoard(shown, picker === null), [shown, picker]);

  /**
   * After the board re-renders: focus a tool of a pedal (the handle when that tool has switched
   * off at an end of the board) and bring it into view, or Add pedal when the board is empty.
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
      // A pedal just picked glides into view, unless motion is unwelcome.
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

  const rowOf = useCallback(
    (el: HTMLElement) => {
      const row = layout.rows[Number(el.dataset.row)];
      return row && { top: row.top, height: row.height };
    },
    [layout],
  );

  // A pedal let go lands where it was dropped: it does not glide there from its old place, as
  // the pedals making room for it do.
  const [dropped, setDropped] = useState<string | null>(null);
  const drop = useCallback(
    (id: string, index: number) => {
      setDropped(id);
      move(id, index);
    },
    [move],
  );
  useEffect(() => {
    if (!dropped) return;
    const frame = requestAnimationFrame(() => setDropped(null));
    return () => cancelAnimationFrame(frame);
  }, [dropped]);

  const { drag, handleProps, bodyProps } = useBoardDrag({
    listRef,
    scrollerRef,
    gap: layout.gap,
    rowOf,
    onDrop: drop,
  });

  const remove = (id: string) => {
    const board = useBoardStore.getState().pedals;
    const at = board.indexOf(id);
    const neighbour = board[at + 1] ?? board[at - 1] ?? null;
    boardActions.remove(id);
    afterRender(neighbour);
  };

  // The shelf stays open after a pedal is added, so several can go on in one visit; focus stays
  // on the pedal's card, which now says it is on the board.
  const add = (id: string) => {
    boardActions.add(id);
    setAnnouncement(`Added ${pedalName(id)}, ${placeText(useBoardStore.getState().pedals, id)}.`);
  };

  const closePicker = useCallback(() => {
    setPicker(null);
    setConsidered(null);
  }, []);

  const show = (id: string) => {
    closePicker();
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

  // Where the pedal the shelf points at would land: a bar in that gap, brought into view. A pedal
  // on the board already is ringed where it stands.
  const onBoard = considered !== null && shown.includes(considered);
  const landing =
    considered && !onBoard ? gapAt(layout, addPedal(shown, considered).indexOf(considered)) : null;
  const landingRef = useRef<HTMLLIElement>(null);
  const landingKey = landing ? `${landing.x},${landing.y}` : '';
  useEffect(() => {
    if (landingKey) landingRef.current?.scrollIntoView?.({ block: 'nearest' });
  }, [landingKey]);
  useEffect(() => {
    if (!considered || !onBoard) return;
    listRef.current
      ?.querySelector<HTMLElement>(`[data-pedal="${considered}"]`)
      ?.scrollIntoView?.({ block: 'nearest' });
  }, [considered, onBoard]);

  const addButton = (where: 'toolbar' | 'empty') => (
    <button
      ref={where === 'toolbar' ? toolbarAdd : emptyAdd}
      type="button"
      className={`${where === 'toolbar' ? buttons.solid : buttons.nacre} ${buttons.lead}`}
      aria-haspopup="dialog"
      aria-expanded={picker === where}
      onClick={() => {
        setConsidered(null);
        setPicker((p) => (p === where ? null : where));
      }}
    >
      <Icon name="plus" size={14} stroke={ICON_STROKE_LABEL} />
      Add pedal
    </button>
  );

  return (
    <div className={styles.section}>
      <div className={styles.toolbar}>
        <span className={styles.count}>{countText(pedals.length)}</span>
        {supported && addButton('toolbar')}
      </div>

      <div ref={scrollerRef} className={styles.scroller} data-dragging={drag ? true : undefined}>
        <div
          className={styles.stage}
          style={{ height: layout.height + (pedals.length === 0 ? EMPTY_TEXT : 0) }}
        >
          <Cable layout={layout} />
          <div className={styles.input} style={box(layout.input)}>
            <span className={styles.jack} aria-hidden="true" />
            <span className={styles.inputLabel}>Guitar</span>
          </div>

          <PedalScaleContext value={layout.scale}>
            <ol
              ref={listRef}
              className={styles.board}
              style={{ '--u': `${layout.scale}px` } as CSSProperties}
              aria-label="Pedalboard, in signal order"
            >
              {layout.pedals.map((placed, i) => {
                const { id } = placed;
                const { Panel } = PEDAL_CATALOG[id]!;
                const name = pedalName(id);
                const dragging = drag?.id === id;
                return (
                  <li
                    key={id}
                    data-pedal={id}
                    data-row={placed.row}
                    className={styles.slot}
                    data-dragging={dragging || undefined}
                    data-dropped={dropped === id || undefined}
                    data-marked={considered === id || undefined}
                    data-editable={supported || undefined}
                    style={{
                      ...box(placed),
                      ...(dragging ? { translate: `${drag.dx}px ${drag.dy}px` } : {}),
                    }}
                    {...(supported ? bodyProps(id) : {})}
                  >
                    {supported && (
                      <div className={styles.tools}>
                        <button
                          ref={(el) => {
                            if (el) handles.current.set(id, el);
                          }}
                          type="button"
                          className={`${buttons.press} ${styles.tool} ${styles.handle}`}
                          data-control="handle"
                          aria-label={`Move ${name}`}
                          aria-describedby={hintId}
                          onKeyDown={onHandleKey(id)}
                          {...handleProps(id)}
                        >
                          <Icon name="grip" size={14} />
                        </button>
                        <span className={styles.toolsGap} />
                        <button
                          type="button"
                          className={`${buttons.press} ${styles.tool}`}
                          data-control="left"
                          aria-label={`Move ${name} left`}
                          title="Move left"
                          disabled={i === 0}
                          onClick={() => move(id, i - 1, 'left')}
                        >
                          <Icon name="prev" size={12} />
                        </button>
                        <button
                          type="button"
                          className={`${buttons.press} ${styles.tool}`}
                          data-control="right"
                          aria-label={`Move ${name} right`}
                          title="Move right"
                          disabled={i === layout.pedals.length - 1}
                          onClick={() => move(id, i + 1, 'right')}
                        >
                          <Icon name="next" size={12} />
                        </button>
                        <button
                          type="button"
                          className={`${buttons.press} ${styles.tool}`}
                          aria-label={`Remove ${name}`}
                          title="Remove from the board"
                          onClick={() => remove(id)}
                        >
                          <Icon name="close" size={11} />
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
              {landing && !drag && (
                <li
                  ref={landingRef}
                  aria-hidden="true"
                  className={styles.bar}
                  data-landing
                  style={{ left: landing.x, top: landing.y, height: landing.h }}
                />
              )}
            </ol>
          </PedalScaleContext>

          <AmpTerminal style={box(layout.output)} />

          {pedals.length === 0 && (
            <div className={styles.empty} style={{ top: layout.height }}>
              <p className={styles.emptyTitle}>No pedals on the board</p>
              <p className={styles.emptyText}>
                Your guitar goes straight into the amp. Add a pedal to shape it first.
              </p>
              {supported && <div className={styles.emptyAction}>{addButton('empty')}</div>}
            </div>
          )}
        </div>
      </div>

      <PedalPicker
        open={picker !== null}
        board={shown}
        onAdd={add}
        onShow={show}
        onConsider={setConsidered}
        onClose={closePicker}
        returnFocusTo={picker === 'empty' ? emptyAdd : toolbarAdd}
      />

      <span id={hintId} className="visually-hidden">
        Drag to move it, or use the arrow keys.
      </span>
      <div className="visually-hidden" role="status" aria-live="polite">
        {announcement}
      </div>
    </div>
  );
}
