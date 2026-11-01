// SPDX-License-Identifier: AGPL-3.0-or-later
// The result list: a listbox of rows. Rows are options, so they hold no focusable controls; the
// star is a mouse shortcut for the detail pane's Favourite button and the F key.
import { useEffect, useRef } from 'react';
import type { KeyboardEvent, MouseEvent, ReactNode } from 'react';
import { Icon } from '../../controls/Icon';
import { Led } from '../../controls/Led';
import type { PresetInfo } from '../../bridge/protocol';
import { presetActions } from '../../state/presetStore';
import { AmpBadge } from './AmpBadge';
import { LIST_ID, optionId } from './ids';
import { chainParts, chainText } from './library';
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

/** Pedals → amp → effects; the cab is in the detail pane. */
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
  items: readonly PresetInfo[];
  cursor: string | null;
  currentId: string | null;
  dirty: boolean;
  /** Shown when the list is empty. */
  empty: ReactNode;
  onAudition: (id: string) => void;
  onKeep: (id: string) => void;
  onKeyDown: (e: KeyboardEvent<HTMLElement>) => void;
}

export function PresetList({
  items,
  cursor,
  currentId,
  dirty,
  empty,
  onAudition,
  onKeep,
  onKeyDown,
}: PresetListProps) {
  const listRef = useRef<HTMLUListElement>(null);
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

  const active = items.some((p) => p.id === cursor) ? cursor : null;

  return (
    <div className={styles.scroller}>
      <ul
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
        {items.map((p) => {
          const current = p.id === currentId;
          return (
            <li
              key={p.id}
              id={optionId(p.id)}
              role="option"
              aria-selected={p.id === active}
              aria-label={rowLabel(p, current, dirty)}
              data-current={current || undefined}
              className={styles.row}
              onClick={() => onAudition(p.id)}
              onDoubleClick={() => onKeep(p.id)}
            >
              <span
                className={styles.star}
                data-on={p.favorite || undefined}
                title={p.favorite ? 'Remove from favourites (F)' : 'Add to favourites (F)'}
                aria-hidden="true"
                onClick={(e) => star(e, p)}
                onDoubleClick={(e) => e.stopPropagation()}
              >
                <Icon name="star" size={14} filled={p.favorite} />
              </span>
              <span className={styles.main}>
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
                <span className={styles.description}>
                  {p.description || (p.category === 'User' ? 'Your preset' : '')}
                </span>
              </span>
              <span className={styles.sound}>
                <Chain p={p} />
                {p.tags.length > 0 && (
                  <span className={styles.tags}>
                    {p.tags.map((t) => (
                      <span key={t} className={styles.tag}>
                        {t}
                      </span>
                    ))}
                  </span>
                )}
              </span>
              <span className={styles.meta}>
                <span className={styles.folder}>{p.folder || 'No category'}</span>
                <span className={styles.source} data-user={p.category === 'User' || undefined}>
                  {p.category}
                </span>
              </span>
            </li>
          );
        })}
      </ul>
      {items.length === 0 && <div className={styles.empty}>{empty}</div>}
    </div>
  );
}
