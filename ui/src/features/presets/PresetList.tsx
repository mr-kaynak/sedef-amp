// SPDX-License-Identifier: AGPL-3.0-or-later
// The result list: a listbox of one-line rows under category headers. Rows are options, so they
// hold no focusable controls; the star is a mouse shortcut for the Favourite button and the F
// key. What a row leaves out (the note, the author, the cab) is in the strip under the list.
import { useEffect, useRef } from 'react';
import type { KeyboardEvent, MouseEvent, ReactNode } from 'react';
import { Icon } from '../../controls/Icon';
import { Led } from '../../controls/Led';
import type { PresetInfo } from '../../bridge/protocol';
import { presetActions } from '../../state/presetStore';
import { AmpBadge } from './AmpBadge';
import { LIST_ID, optionId } from './ids';
import { chainParts, chainText, type PresetGroup } from './library';
import styles from './PresetList.module.css';

function rowLabel(p: PresetInfo, current: boolean, dirty: boolean): string {
  const where = [p.category, p.folder].filter(Boolean).join(' ');
  return [
    p.name + (current ? (dirty ? ', playing, edited' : ', playing') : ''),
    chainText(p),
    p.description,
    where,
    p.tags.length ? `tags ${p.tags.join(', ')}` : '',
    p.favorite ? 'favourite' : '',
  ]
    .filter(Boolean)
    .join('. ');
}

/** Pedals → amp → effects; the cab is in the strip under the list. */
function Chain({ p }: { p: PresetInfo }) {
  const parts = chainParts(p);
  return (
    <span className={styles.chain}>
      {parts.map((part, i) => (
        <span key={`${part.label}-${i}`} className={styles.link}>
          {i > 0 && <span className={styles.arrow}>→</span>}
          {part.kind === 'amp' ? (
            <AmpBadge p={p} />
          ) : (
            <span className={styles.block}>{part.label}</span>
          )}
        </span>
      ))}
    </span>
  );
}

export interface PresetListProps {
  /** The presets in display order, by category; one group without a header when `flat`. */
  groups: readonly PresetGroup[];
  /** No headers: each row names its category itself (Recent, newest first). */
  flat: boolean;
  cursor: string | null;
  currentId: string | null;
  dirty: boolean;
  /** Shown when the list is empty. */
  empty: ReactNode;
  onAudition: (id: string) => void;
  onKeep: (id: string) => void;
  /** The row under the pointer, or null when it leaves the list. */
  onHover: (id: string | null) => void;
  onKeyDown: (e: KeyboardEvent<HTMLElement>) => void;
}

const headerId = (index: number) => `${LIST_ID}-group-${index}`;

export function PresetList({
  groups,
  flat,
  cursor,
  currentId,
  dirty,
  empty,
  onAudition,
  onKeep,
  onHover,
  onKeyDown,
}: PresetListProps) {
  const listRef = useRef<HTMLDivElement>(null);
  const placed = useRef(false);

  // The playing preset starts in the middle of the list; then the highlighted row is kept in
  // view as the arrows move.
  useEffect(() => {
    if (!cursor) return;
    const row = listRef.current?.querySelector<HTMLElement>(`#${optionId(cursor)}`);
    row?.scrollIntoView?.({ block: placed.current ? 'nearest' : 'center' });
    placed.current = true;
  }, [cursor]);

  const star = (e: MouseEvent, p: PresetInfo) => {
    e.stopPropagation();
    void presetActions.setFavorite(p.id, !p.favorite);
  };

  const count = groups.reduce((n, g) => n + g.items.length, 0);
  const active = groups.some((g) => g.items.some((p) => p.id === cursor)) ? cursor : null;

  const row = (p: PresetInfo) => {
    const current = p.id === currentId;
    return (
      <div
        key={p.id}
        id={optionId(p.id)}
        role="option"
        aria-selected={p.id === active}
        aria-label={rowLabel(p, current, dirty)}
        data-current={current || undefined}
        className={styles.row}
        onClick={() => onAudition(p.id)}
        onDoubleClick={() => onKeep(p.id)}
        onPointerEnter={() => onHover(p.id)}
      >
        <span
          className={styles.star}
          data-on={p.favorite || undefined}
          title={p.favorite ? 'Remove from favourites (F)' : 'Add to favourites (F)'}
          aria-hidden="true"
          onClick={(e) => star(e, p)}
          onDoubleClick={(e) => e.stopPropagation()}
        >
          <Icon name="star" size={13} filled={p.favorite} />
        </span>
        <span className={styles.nameLine}>
          <span className={styles.name}>{p.name}</span>
          {current && (
            <span className={styles.playing}>
              <Led on size={6} />
              Playing
              {dirty && <span className={styles.edited}> · edited</span>}
            </span>
          )}
        </span>
        <Chain p={p} />
        <span className={styles.meta}>
          {p.tags.map((t) => (
            <span key={t} className={styles.tag}>
              {t}
            </span>
          ))}
          {flat && <span className={styles.folder}>{p.folder || 'No category'}</span>}
          {p.category === 'User' && <span className={styles.user}>User</span>}
        </span>
      </div>
    );
  };

  return (
    <div className={styles.scroller} onPointerLeave={() => onHover(null)}>
      <div
        ref={listRef}
        id={LIST_ID}
        role="listbox"
        aria-label="Presets"
        aria-activedescendant={active ? optionId(active) : undefined}
        aria-keyshortcuts="F"
        tabIndex={0}
        className={styles.list}
        onKeyDown={onKeyDown}
      >
        {flat
          ? groups.flatMap((g) => g.items).map(row)
          : groups.map((g, i) => (
              <div key={g.name} role="group" aria-labelledby={headerId(i)} className={styles.group}>
                <div className={styles.header} role="presentation">
                  <span id={headerId(i)} className={styles.headerName}>
                    {g.name || 'No category'}
                  </span>
                  <span className={styles.headerCount} aria-hidden="true">
                    {g.items.length}
                  </span>
                </div>
                {g.items.map(row)}
              </div>
            ))}
      </div>
      {count === 0 && <div className={styles.empty}>{empty}</div>}
    </div>
  );
}
