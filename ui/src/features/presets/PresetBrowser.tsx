// SPDX-License-Identifier: AGPL-3.0-or-later
// The preset browser: a panel over the rig with search, a library switch and one dense list,
// grouped by category. A click or an arrow key auditions a preset while the browser stays open;
// Enter or a double-click keeps it; Esc goes back to the sound from when the browser opened.
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
import { browse, groupByCategory, LIBRARIES, libraryCounts } from './library';
import { LIST_ID, optionId } from './ids';
import { PresetList } from './PresetList';
import { PresetDetail } from './PresetDetail';
import { PresetForm } from './PresetForm';
import { newPreset, presetBrowser, useBrowserStore } from './session';
import { ShortcutList } from './ShortcutList';
import styles from './PresetBrowser.module.css';

export function PresetBrowser() {
  const open = useBrowserStore((s) => s.open);
  return open ? <BrowserPanel /> : null;
}

/** Whose presets: all of them, the starred ones, the ones picked lately, or your own. */
function LibrarySwitch() {
  const state = usePresetStore((s) => s.state);
  const library = useBrowserStore((s) => s.filter.library);
  const counts = libraryCounts(state.items, state.recent);
  const index = LIBRARIES.findIndex((l) => l.id === library);
  const { onKeyDown, radio } = useRadioGroup(LIBRARIES.length, index, (i) => {
    const l = LIBRARIES[i];
    if (l) presetBrowser.setFilter({ library: l.id });
  });
  return (
    <div
      role="radiogroup"
      aria-label="Library"
      className={segmented.segmented}
      onKeyDown={onKeyDown}
    >
      {LIBRARIES.map((l, i) => (
        <button
          key={l.id}
          type="button"
          className={`${nacre.fill} ${segmented.option} ${styles.library}`}
          {...radio(i)}
        >
          {l.label}
          <span className={styles.libraryCount}>{counts[l.id]}</span>
        </button>
      ))}
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
  const [showKeys, setShowKeys] = useState(false);
  const [hoverId, setHoverId] = useState<string | null>(null);
  // The strip under the list is about the highlighted row, which is the playing preset unless a
  // load is on its way; nothing when the sound isn't a preset (Init).
  const detail = state.items.find((p) => p.id === (cursor ?? state.currentId));

  const visible = useMemo(
    () => browse(state.items, filter, state.recent),
    [state.items, state.recent, filter],
  );
  // Recent reads newest first, so it has no category headers.
  const flat = filter.library === 'recent';
  const groups = useMemo(
    () => (flat ? [{ name: '', items: visible }] : groupByCategory(visible)),
    [flat, visible],
  );
  const hovered = hoverId === detail?.id ? undefined : visible.find((p) => p.id === hoverId);
  const filtered = filter.query !== '';

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

  const clearFilters = () => presetBrowser.setFilter({ query: '' });

  const emptyText =
    filter.library === 'favorites' && !filtered
      ? 'Star a preset to keep it here.'
      : filter.library === 'recent' && !filtered
        ? 'Presets you pick show up here.'
        : filter.library === 'user' && !filtered
          ? 'Presets you save appear here.'
          : filter.library === 'all'
            ? 'No presets match.'
            : `No presets match in ${LIBRARIES.find((l) => l.id === filter.library)?.label}.`;
  const activeOption = visible.some((p) => p.id === cursor) && cursor ? optionId(cursor) : '';
  const mod = modKey();

  const back = (
    <div className={styles.hint} id="preset-browser-help">
      <span>
        Click or <kbd>↑</kbd>
        <kbd>↓</kbd> to audition
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
  );

  return (
    <section
      className={styles.browser}
      role="dialog"
      aria-label="Preset browser"
      aria-describedby="preset-browser-help"
    >
      <div className={styles.body} inert={mode !== 'list'}>
        <div className={styles.toolbar}>
          <h2 className={styles.title}>Presets</h2>
          <div className={styles.search}>
            <Icon name="search" size={14} />
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
              placeholder="Search name, style, amp, pedal…"
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
          <LibrarySwitch />
          <span className={styles.count} aria-live="polite">
            {filtered ? (visible.length === 1 ? '1 match' : `${visible.length} matches`) : ''}
          </span>
          <span className={styles.spacer} />
          <button
            type="button"
            className={`${buttons.ghost} ${buttons.small} ${buttons.lead}`}
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
              <Icon name="folder" size={14} />
            </button>
          )}
          <ShortcutList open={showKeys} onToggle={setShowKeys} />
        </div>

        <PresetList
          groups={groups}
          flat={flat}
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
                  Clear search
                </button>
              )}
            </>
          }
          onAudition={(id) => void presetBrowser.audition(id)}
          onKeep={(id) => {
            void presetBrowser.audition(id);
            presetBrowser.keep();
          }}
          onHover={setHoverId}
          onKeyDown={(e) => onListKeys(e, false)}
        />

        <PresetDetail preset={detail} hovered={hovered} back={back} />
      </div>

      {mode !== 'list' && <PresetForm mode={mode} />}
    </section>
  );
}
