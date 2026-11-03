// SPDX-License-Identifier: AGPL-3.0-or-later
// The preset browser: a panel over the rig with a library rail, tag chips, search and a list of
// rich rows. A click or an arrow key auditions a preset while the browser stays open; Enter or a
// double-click keeps it; Esc goes back to the sound from when the browser opened.
import { useEffect, useMemo, useRef, useState } from 'react';
import type { KeyboardEvent } from 'react';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import buttons from '../../controls/Button.module.css';
import nacre from '../../controls/NacreFill.module.css';
import segmented from '../../controls/Segmented.module.css';
import { useRadioGroup } from '../../controls/useRadioGroup';
import { useAppStore } from '../../state/appStore';
import { presetActions, usePresetStore } from '../../state/presetStore';
import { useUiStore } from '../../state/uiStore';
import { modKey } from '../../app/platform';
import {
  browse,
  categoryFacets,
  LIBRARIES,
  libraryCounts,
  SORTS,
  tagFacets,
  type Library,
} from './library';
import { LIST_ID, optionId } from './ids';
import { PresetList } from './PresetList';
import { PresetDetail } from './PresetDetail';
import { PresetForm } from './PresetForm';
import { newPreset, presetBrowser, useBrowserStore } from './session';
import { ShortcutList } from './ShortcutList';
import styles from './PresetBrowser.module.css';

const SHOWN_TAGS = 9;

export function PresetBrowser() {
  const open = useBrowserStore((s) => s.open);
  return open ? <BrowserPanel /> : null;
}

function LibraryRail() {
  const state = usePresetStore((s) => s.state);
  const filter = useBrowserStore((s) => s.filter);
  const counts = libraryCounts(state.items, state.recent);
  const categories = categoryFacets(state.items, filter.library, state.recent);

  const pickLibrary = (library: Library) => {
    const keepCategory =
      filter.category === null ||
      categoryFacets(state.items, library, state.recent).some((c) => c.name === filter.category);
    presetBrowser.setFilter({
      library,
      category: keepCategory ? filter.category : null,
      // Recent reads best newest first; the sort can still be changed.
      ...(library === 'recent' ? { sort: 'recent' as const } : {}),
    });
  };

  const libraryIndex = LIBRARIES.findIndex((l) => l.id === filter.library);
  const libraries = useRadioGroup(LIBRARIES.length, libraryIndex, (i) => {
    const l = LIBRARIES[i];
    if (l) pickLibrary(l.id);
  });

  const categoryOptions = [null, ...categories.map((c) => c.name)];
  const categoryIndex = Math.max(0, categoryOptions.indexOf(filter.category));
  const cats = useRadioGroup(categoryOptions.length, categoryIndex, (i) =>
    presetBrowser.setFilter({ category: categoryOptions[i] ?? null }),
  );

  return (
    <nav className={styles.rail} aria-label="Preset library">
      <h2 className={styles.title}>Presets</h2>
      <h3 className={styles.railHeading} id="preset-rail-library">
        Library
      </h3>
      <div role="radiogroup" aria-labelledby="preset-rail-library" onKeyDown={libraries.onKeyDown}>
        {LIBRARIES.map((l, i) => (
          <button key={l.id} type="button" className={styles.railItem} {...libraries.radio(i)}>
            <Icon name={RAIL_ICONS[l.id]} size={14} className={styles.railIcon} />
            <span className={styles.railLabel}>{l.label}</span>
            <span className={styles.railCount}>{counts[l.id]}</span>
          </button>
        ))}
      </div>
      <h3 className={styles.railHeading} id="preset-rail-categories">
        Categories
      </h3>
      <div role="radiogroup" aria-labelledby="preset-rail-categories" onKeyDown={cats.onKeyDown}>
        {categoryOptions.map((name, i) => (
          <button
            key={name ?? '(all)'}
            type="button"
            className={styles.railItem}
            data-category
            {...cats.radio(i)}
          >
            <span className={styles.railLabel}>{name ?? 'All categories'}</span>
            <span className={styles.railCount}>
              {name === null ? '' : categories.find((c) => c.name === name)?.count}
            </span>
          </button>
        ))}
      </div>
    </nav>
  );
}

const RAIL_ICONS = {
  all: 'list',
  favorites: 'star',
  recent: 'clock',
  factory: 'spark',
  user: 'user',
} as const;

function SortSwitch() {
  const sort = useBrowserStore((s) => s.filter.sort);
  const index = SORTS.findIndex((s) => s.id === sort);
  const { onKeyDown, radio } = useRadioGroup(SORTS.length, index, (i) => {
    const s = SORTS[i];
    if (s) presetBrowser.setFilter({ sort: s.id });
  });
  return (
    <div className={styles.sort}>
      <span className={styles.sortLabel} id="preset-sort-label">
        Sort
      </span>
      <div
        role="radiogroup"
        aria-labelledby="preset-sort-label"
        className={`${segmented.segmented} ${segmented.sm}`}
        onKeyDown={onKeyDown}
      >
        {SORTS.map((s, i) => (
          <button
            key={s.id}
            type="button"
            className={`${nacre.fill} ${segmented.option}`}
            {...radio(i)}
          >
            {s.label}
          </button>
        ))}
      </div>
    </div>
  );
}

function BrowserPanel() {
  const state = usePresetStore((s) => s.state);
  const mode = useBrowserStore((s) => s.mode);
  const origin = useBrowserStore((s) => s.origin);
  const auditioned = useBrowserStore((s) => s.auditioned);
  const filter = useBrowserStore((s) => s.filter);
  const cursor = useBrowserStore((s) => s.cursor);
  const searchFocus = useBrowserStore((s) => s.searchFocus);
  const canReveal = useAppStore((s) => s.session.backend === 'juce');
  const searchRef = useRef<HTMLInputElement>(null);
  const [allTags, setAllTags] = useState(false);
  const [showKeys, setShowKeys] = useState(false);
  // The detail pane shows the highlighted row, which is the playing preset unless a load is on
  // its way; nothing when the sound isn't a preset (Init).
  const detail = state.items.find((p) => p.id === (cursor ?? state.currentId));

  // Library, category and search narrow the list first; the chips offer what is left.
  const scoped = useMemo(
    () => browse(state.items, { ...filter, tags: [] }, state.recent),
    [state.items, state.recent, filter],
  );
  const visible = useMemo(
    () => scoped.filter((p) => filter.tags.every((t) => p.tags.includes(t))),
    [scoped, filter.tags],
  );
  const tags = useMemo(() => {
    const facets = tagFacets(scoped);
    const chosen = filter.tags.filter((t) => !facets.some((f) => f.name === t));
    return [...chosen.map((name) => ({ name, count: 0 })), ...facets];
  }, [scoped, filter.tags]);
  const shownTags = allTags
    ? tags
    : tags.filter((t, i) => i < SHOWN_TAGS || filter.tags.includes(t.name));
  const filtered = filter.query !== '' || filter.tags.length > 0 || filter.category !== null;

  // The highlighted row follows the playing preset when it changes from elsewhere (A/B, host).
  useEffect(() => {
    presetBrowser.syncCursor();
  }, [state.currentId]);

  useEffect(() => {
    if (mode === 'list') searchRef.current?.focus({ preventScroll: true });
  }, [mode, searchFocus]);

  // Esc: the shortcut list closes, a form goes back to the list, the browser goes back to the
  // sound it opened on. Capture phase, so it runs before the global shortcuts; an open popover
  // gets the key first.
  useEffect(() => {
    const onKeyDown = (e: globalThis.KeyboardEvent) => {
      if (e.key !== 'Escape' || e.defaultPrevented || useUiStore.getState().popover) return;
      e.preventDefault();
      e.stopPropagation();
      if (showKeys) setShowKeys(false);
      else if (useBrowserStore.getState().mode !== 'list') presetBrowser.endForm();
      else presetBrowser.cancel();
    };
    document.addEventListener('keydown', onKeyDown, true);
    return () => document.removeEventListener('keydown', onKeyDown, true);
  }, [showKeys]);

  const move = (to: 'next' | 'prev' | 'first' | 'last') => {
    if (!visible.length) return;
    const at = visible.findIndex((p) => p.id === cursor);
    const index =
      to === 'first'
        ? 0
        : to === 'last'
          ? visible.length - 1
          : at < 0
            ? to === 'next'
              ? 0
              : visible.length - 1
            : Math.min(visible.length - 1, Math.max(0, at + (to === 'next' ? 1 : -1)));
    const target = visible[index];
    if (target) void presetBrowser.audition(target.id);
  };

  const keepCursor = () => {
    // The highlighted row, or the best match when the playing preset is filtered out. Auditioning
    // the row that is already on its way (or playing) sends nothing.
    const target = visible.find((p) => p.id === cursor) ?? visible[0];
    if (target) void presetBrowser.audition(target.id);
    presetBrowser.keep();
  };

  const onListKeys = (e: KeyboardEvent<HTMLElement>, inSearch: boolean) => {
    if (e.altKey || e.metaKey || e.ctrlKey) return;
    switch (e.key) {
      case 'ArrowDown':
        move('next');
        break;
      case 'ArrowUp':
        move('prev');
        break;
      case 'PageDown':
        move('last');
        break;
      case 'PageUp':
        move('first');
        break;
      case 'Home':
      case 'End':
        if (inSearch) return;
        move(e.key === 'Home' ? 'first' : 'last');
        break;
      case 'Enter':
        keepCursor();
        break;
      case 'f':
      case 'F': {
        if (inSearch) return;
        const target = visible.find((p) => p.id === cursor);
        if (target) void presetActions.setFavorite(target.id, !target.favorite);
        break;
      }
      default:
        return;
    }
    e.preventDefault();
  };

  const toggleTag = (tag: string) =>
    presetBrowser.setFilter({
      tags: filter.tags.includes(tag)
        ? filter.tags.filter((t) => t !== tag)
        : [...filter.tags, tag],
    });

  const clearFilters = () => presetBrowser.setFilter({ query: '', tags: [], category: null });

  const emptyText =
    filter.library === 'favorites' && !filtered
      ? 'Star a preset to keep it here.'
      : filter.library === 'recent' && !filtered
        ? 'Presets you pick show up here.'
        : filter.library === 'user' && !filtered
          ? 'Presets you save appear here.'
          : 'No presets match.';
  const activeOption = visible.some((p) => p.id === cursor) && cursor ? optionId(cursor) : '';
  const mod = modKey();

  return (
    <section
      className={styles.browser}
      role="dialog"
      aria-label="Preset browser"
      aria-describedby="preset-browser-help"
    >
      <div className={styles.body} inert={mode !== 'list'}>
        <LibraryRail />

        <div className={styles.results}>
          <div className={styles.searchRow}>
            <div className={styles.search}>
              <Icon name="search" size={15} />
              <input
                ref={searchRef}
                type="search"
                role="combobox"
                data-command-keys
                aria-label="Search presets"
                aria-controls={LIST_ID}
                aria-expanded="true"
                aria-autocomplete="list"
                aria-activedescendant={activeOption || undefined}
                aria-keyshortcuts={`/ ${mod.aria}+F`}
                placeholder="Search name, tag, style…   ↑↓ audition · ↵ keep"
                value={filter.query}
                onChange={(e) => presetBrowser.setFilter({ query: e.target.value })}
                onKeyDown={(e) => onListKeys(e, true)}
              />
              {filter.query && (
                <button
                  type="button"
                  className={`${buttons.press} ${styles.clearSearch}`}
                  aria-label="Clear search"
                  onClick={() => {
                    presetBrowser.setFilter({ query: '' });
                    searchRef.current?.focus();
                  }}
                >
                  <Icon name="close" size={12} />
                </button>
              )}
              <kbd className={styles.searchKey} aria-hidden="true">
                /
              </kbd>
            </div>
            <SortSwitch />
          </div>
          {tags.length > 0 && (
            <div className={styles.chips} role="group" aria-label="Filter by tag">
              {shownTags.map((t) => (
                <button
                  key={t.name}
                  type="button"
                  className={`${buttons.press} ${nacre.fill} ${styles.chip}`}
                  aria-pressed={filter.tags.includes(t.name)}
                  onClick={() => toggleTag(t.name)}
                >
                  {t.name}
                </button>
              ))}
              {tags.length > shownTags.length || allTags ? (
                <button
                  type="button"
                  className={`${buttons.press} ${styles.moreChips}`}
                  aria-expanded={allTags}
                  onClick={() => setAllTags(!allTags)}
                >
                  {allTags ? 'Fewer tags' : `${tags.length - shownTags.length} more`}
                </button>
              ) : null}
            </div>
          )}
          <div className={styles.resultHead}>
            <span className={styles.count} aria-live="polite">
              {visible.length === 1 ? '1 preset' : `${visible.length} presets`}
            </span>
            {filtered && visible.length > 0 && (
              <button
                type="button"
                className={`${buttons.link} ${styles.clear}`}
                onClick={clearFilters}
              >
                Clear filters
              </button>
            )}
          </div>
          <PresetList
            items={visible}
            cursor={cursor}
            currentId={state.currentId}
            dirty={state.dirty}
            empty={
              <>
                <span>{emptyText}</span>
                {filtered && (
                  <button
                    type="button"
                    className={`${buttons.ghost} ${buttons.small}`}
                    onClick={clearFilters}
                  >
                    Clear filters
                  </button>
                )}
              </>
            }
            onAudition={(id) => void presetBrowser.audition(id)}
            onKeep={(id) => {
              void presetBrowser.audition(id);
              presetBrowser.keep();
            }}
            onKeyDown={(e) => onListKeys(e, false)}
          />
        </div>

        <PresetDetail preset={detail} />
      </div>

      <footer className={styles.foot} inert={mode !== 'list'}>
        <div className={styles.hint} id="preset-browser-help">
          <span>
            Click or <kbd>↑</kbd>
            <kbd>↓</kbd> to audition — the browser stays open.
          </span>
          <button
            type="button"
            className={`${buttons.press} ${styles.back}`}
            data-armed={auditioned || undefined}
            aria-label={`Back to ${origin?.name ?? state.currentName}${origin?.dirty ? ' with your edits' : ''}`}
            aria-keyshortcuts="Escape"
            onClick={() => presetBrowser.cancel()}
          >
            <kbd>Esc</kbd>
            <span>
              returns to <strong>{origin?.name ?? state.currentName}</strong>
              {origin?.dirty && <span className={styles.edited}> with your edits</span>}
            </span>
          </button>
        </div>
        <span className={styles.spacer} />
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small}`}
          title="Start from the default sound"
          onClick={() => void newPreset()}
        >
          <Icon name="plus" size={12} stroke={ICON_STROKE_LABEL} />
          New
        </button>
        {canReveal && (
          <button
            type="button"
            className={`${buttons.ghost} ${buttons.small} ${buttons.icon}`}
            aria-label="Show the preset folder"
            title="Show the preset folder"
            onClick={() => void presetActions.reveal()}
          >
            <Icon name="folder" size={14} stroke={ICON_STROKE_LABEL} />
          </button>
        )}
        <ShortcutList open={showKeys} onToggle={setShowKeys} />
      </footer>

      {mode !== 'list' && <PresetForm mode={mode} />}
    </section>
  );
}
