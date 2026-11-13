// SPDX-License-Identifier: AGPL-3.0-or-later
// The Pedals page: the pedalboard. The signal comes in at the Guitar jack, runs along a cable
// through the pedals in front of the amp, into the amp (a block in the chain), out through the
// pedals after it and on to the Out jack, row by row (docs/board.md §5). Each pedal carries its
// own tools (a handle, move left and right, remove), shown when the pointer or the keyboard is on
// it; a pedal also drags by its enclosure. Only modulation crosses the amp. Add pedal opens the
// shelf under the board. Moves and additions are announced politely; removals show a notice with
// Undo.
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
import { PedalCopyContext } from './copy';
import { PedalScaleContext } from './density';
import { gapAt, layoutBoard, type Arrow, type BoardLayout } from './layout';
import { PedalPicker, type AddWhere } from './PedalPicker';
import {
  addAfterAmp,
  addPedal,
  AMP,
  baseId,
  copyNumber,
  movePedal,
  pedalMeta,
  pedalName,
  positionText,
  sameBoard,
  sideOf,
  withAmp,
} from './registry';
import { useBoardDrag } from './useBoardDrag';
import styles from './PedalBoard.module.css';

/** The tool that keeps focus after a change: the handle, or the move button used. */
type Control = 'handle' | 'left' | 'right';

const plural = new Intl.PluralRules('en');

function countText(n: number): string {
  return plural.select(n) === 'one' ? `${n} pedal` : `${n} pedals`;
}

/** Room under an empty board's cable for what the page says about it. */
const EMPTY_TEXT = 150;

const ARROWS: Record<Arrow['heading'], string> = {
  right: 'M-2 -3.5 1.5 0 -2 3.5',
  left: 'M2 -3.5 -1.5 0 2 3.5',
  down: 'M-3.5 -2 0 1.5 3.5 -2',
};

/** The signal's path: a cable from the Guitar jack to Out, with its direction marked. */
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

/** The amp in the chain: by name, in its colours, and a way to its page. */
function AmpTerminal() {
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
      style={paint}
      aria-label={`The amp: ${name}. Open the Amp page`}
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

/** What the shelf points at: a pedal, and which of its buttons. */
interface Considered {
  id: string;
  where: AddWhere;
}

export function PedalBoard() {
  const pedals = useBoardStore((s) => s.pedals);
  const supported = useBoardStore((s) => s.supported);
  const [picker, setPicker] = useState<'toolbar' | 'empty' | null>(null);
  /** The pedal the shelf is pointing at: where it would land, or where it is. */
  const [considered, setConsidered] = useState<Considered | null>(null);
  const [announcement, setAnnouncement] = useState('');
  const toolbarAdd = useRef<HTMLButtonElement>(null);
  const emptyAdd = useRef<HTMLButtonElement>(null);
  const listRef = useRef<HTMLOListElement>(null);
  const scrollerRef = useRef<HTMLDivElement>(null);
  const handles = useRef(new Map<string, HTMLButtonElement>());
  const focusNext = useRef<{ id: string | null; reveal: boolean; control: Control } | null>(null);
  const hintId = useId();

  const shown = useMemo(
    () => pedals.filter((t) => t === AMP || PEDAL_CATALOG[baseId(t)] !== undefined),
    [pedals],
  );
  const pedalCount = shown.filter((t) => t !== AMP).length;
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
    if (sameBoard(movePedal(board, id, index), board)) return;
    boardActions.move(id, index);
    const after = useBoardStore.getState().pedals;
    setAnnouncement(`${pedalName(id)} moved to ${positionText(after, id)}.`);
    afterRender(id, false, control);
  }, []);

  const rowOf = useCallback(
    (el: HTMLElement) => {
      const row = layout.rows[Number(el.dataset.row)];
      return row && { top: row.top, height: row.height };
    },
    [layout],
  );

  /** A pedal that can't stand after the amp lands in front of it however far it is dragged. */
  const clamp = useCallback((id: string, index: number) => {
    if (pedalMeta(id)?.afterAmp) return index;
    const amp = withAmp(useBoardStore.getState().pedals)
      .filter((t) => t !== id)
      .indexOf(AMP);
    return Math.min(index, amp);
  }, []);

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
    clamp,
    onDrop: drop,
  });

  const remove = (id: string) => {
    const board = withAmp(useBoardStore.getState().pedals).filter((t) => t !== AMP);
    const at = board.indexOf(id);
    const neighbour = board[at + 1] ?? board[at - 1] ?? null;
    boardActions.remove(id);
    afterRender(neighbour);
  };

  // The shelf stays open after a pedal is added, so several can go on in one visit; focus stays
  // on the pedal's card, which now says it is on the board.
  const add = (id: string, where: AddWhere) => {
    const before = useBoardStore.getState().pedals;
    const after = where === 'post' ? boardActions.addAfterAmp(id) : boardActions.add(id);
    const added = after.find((t) => t !== AMP && !before.includes(t));
    if (added) setAnnouncement(`Added ${pedalName(added)}, ${positionText(after, added)}.`);
  };

  const closePicker = useCallback(() => {
    setPicker(null);
    setConsidered(null);
  }, []);

  const show = (id: string) => {
    closePicker();
    afterRender(pedals.includes(id) ? id : `${id}#2`, true);
  };

  /** Where the arrow keys and the move buttons take a pedal: its place in the drawn chain. */
  const placeOf = (id: string) => {
    const drawn = withAmp(pedals);
    const at = drawn.indexOf(id);
    const rest = drawn.filter((t) => t !== id);
    const amp = rest.indexOf(AMP);
    const after = sideOf(pedals, id) === 'post';
    const crosses = pedalMeta(id)?.afterAmp ?? false;
    return {
      at,
      first: after ? amp + 1 : 0,
      last: after ? rest.length : amp,
      canLeft: at > 0,
      canRight: at < drawn.length - 1 && (crosses || drawn[at + 1] !== AMP),
    };
  };

  const onHandleKey = (id: string) => (e: KeyboardEvent<HTMLButtonElement>) => {
    const place = placeOf(id);
    const to =
      e.key === 'ArrowLeft' || e.key === 'ArrowUp'
        ? place.at - 1
        : e.key === 'ArrowRight' || e.key === 'ArrowDown'
          ? place.at + 1
          : e.key === 'Home'
            ? place.first
            : e.key === 'End'
              ? place.last
              : null;
    if (to === null) return;
    e.preventDefault();
    if (to >= 0) move(id, to);
  };

  // Forget the handles of pedals that left.
  useEffect(() => {
    for (const id of handles.current.keys()) if (!pedals.includes(id)) handles.current.delete(id);
  }, [pedals]);

  // Where the pedal the shelf points at would land: a bar in that gap, brought into view. Copies
  // already on the board are ringed where they stand.
  const marked = considered ? shown.filter((t) => t !== AMP && baseId(t) === considered.id) : [];
  const landing = useMemo(() => {
    if (!considered) return null;
    const next =
      considered.where === 'post'
        ? addAfterAmp(shown, considered.id)
        : addPedal(shown, considered.id);
    const added = next.find((t) => t !== AMP && !shown.includes(t));
    return added ? gapAt(layout, withAmp(next).indexOf(added)) : null;
  }, [considered, layout, shown]);
  const landingRef = useRef<HTMLLIElement>(null);
  const landingKey = landing ? `${landing.x},${landing.y}` : '';
  useEffect(() => {
    if (landingKey) landingRef.current?.scrollIntoView?.({ block: 'nearest' });
  }, [landingKey]);
  const markedKey = marked.join(',');
  useEffect(() => {
    if (!markedKey || landingKey) return;
    listRef.current
      ?.querySelector<HTMLElement>(`[data-pedal="${markedKey.split(',')[0]}"]`)
      ?.scrollIntoView?.({ block: 'nearest' });
  }, [markedKey, landingKey]);

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
        <span className={styles.count}>
          {drag?.clamped ? (
            <span className={styles.note}>Only modulation goes after the amp</span>
          ) : (
            countText(pedalCount)
          )}
        </span>
        {supported && addButton('toolbar')}
      </div>

      <div ref={scrollerRef} className={styles.scroller} data-dragging={drag ? true : undefined}>
        <div
          className={styles.stage}
          style={{ height: layout.height + (pedalCount === 0 ? EMPTY_TEXT : 0) }}
        >
          {layout.afterAmp.map((band) => (
            <div
              key={`${band.x},${band.y}`}
              className={styles.afterAmp}
              style={box(band)}
              aria-hidden="true"
            />
          ))}
          {layout.afterAmpLabel && (
            <span
              className={styles.afterAmpLabel}
              style={box(layout.afterAmpLabel)}
              aria-hidden="true"
            >
              After the amp
            </span>
          )}
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
              {layout.items.map((placed) => {
                const { id } = placed;
                if (id === AMP)
                  return (
                    <li
                      key={AMP}
                      data-pedal={AMP}
                      data-row={placed.row}
                      className={`${styles.slot} ${styles.ampSlot}`}
                      style={box(placed)}
                    >
                      <AmpTerminal />
                    </li>
                  );
                const base = baseId(id);
                const { Panel } = PEDAL_CATALOG[base]!;
                const name = pedalName(id);
                const dragging = drag?.id === id;
                const place = placeOf(id);
                const drawn = withAmp(pedals);
                // The move buttons say so when they take the pedal across the amp.
                const crossesLeft = drawn[place.at - 1] === AMP;
                const crossesRight = place.canRight && drawn[place.at + 1] === AMP;
                return (
                  <li
                    key={id}
                    data-pedal={id}
                    data-row={placed.row}
                    className={styles.slot}
                    data-dragging={dragging || undefined}
                    data-dropped={dropped === id || undefined}
                    data-marked={marked.includes(id) || undefined}
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
                          aria-label={
                            crossesLeft ? `Move ${name} in front of the amp` : `Move ${name} left`
                          }
                          title={crossesLeft ? 'Move in front of the amp' : 'Move left'}
                          disabled={!place.canLeft}
                          onClick={() => move(id, place.at - 1, 'left')}
                        >
                          <Icon name="prev" size={12} />
                        </button>
                        <button
                          type="button"
                          className={`${buttons.press} ${styles.tool}`}
                          data-control="right"
                          aria-label={
                            crossesRight ? `Move ${name} after the amp` : `Move ${name} right`
                          }
                          title={crossesRight ? 'Move after the amp' : 'Move right'}
                          disabled={!place.canRight}
                          onClick={() => move(id, place.at + 1, 'right')}
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
                    <PedalCopyContext value={{ pedal: base, copy: copyNumber(id) }}>
                      <Panel />
                    </PedalCopyContext>
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

          <div className={styles.input} style={box(layout.output)}>
            <span className={styles.jack} aria-hidden="true" />
            <span className={styles.inputLabel}>Out</span>
          </div>

          {pedalCount === 0 && (
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
        onConsider={(id, where) => setConsidered(id ? { id, where } : null)}
        onClose={closePicker}
        returnFocusTo={picker === 'empty' ? emptyAdd : toolbarAdd}
      />

      <span id={hintId} className="visually-hidden">
        Drag to move it, or use the arrow keys. Modulation can go after the amp.
      </span>
      <div className="visually-hidden" role="status" aria-live="polite">
        {announcement}
      </div>
    </div>
  );
}
