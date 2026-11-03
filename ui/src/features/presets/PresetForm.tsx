// SPDX-License-Identifier: AGPL-3.0-or-later
// Save as and Rename, as a sheet over the browser's list. Save as takes a name, a category (an
// existing folder or a new one), tags with suggestions and a one-line note. Clashes show inline
// before anything is written.
import { useEffect, useId, useMemo, useRef, useState } from 'react';
import type { FormEvent, KeyboardEvent } from 'react';
import { Icon } from '../../controls/Icon';
import buttons from '../../controls/Button.module.css';
import {
  cleanTags,
  MAX_DESCRIPTION,
  MAX_TAGS,
  renameClashes,
  saveTarget,
} from '../../lib/presetFiles';
import { currentPreset, presetActions, usePresetStore } from '../../state/presetStore';
import { showToast } from '../../state/toastStore';
import { allCategories, allTags } from './library';
import { presetBrowser, sendPresetCommand, type BrowserMode } from './session';
import styles from './PresetForm.module.css';

const NEW_CATEGORY = '__new-category__';

export function PresetForm({ mode }: { mode: Exclude<BrowserMode, 'list'> }) {
  const ref = useRef<HTMLDivElement>(null);
  // Focus moves into the sheet so it never falls to the page, where digit keys switch sections.
  useEffect(() => {
    ref.current?.querySelector<HTMLElement>('[data-autofocus]')?.focus({ preventScroll: true });
  }, [mode]);
  return (
    <div className={styles.scrim}>
      <div ref={ref} className={styles.sheet} role="group" aria-labelledby="preset-form-title">
        {mode === 'saveAs' ? <SaveAsForm /> : <RenameForm />}
      </div>
    </div>
  );
}

function SaveAsForm() {
  const state = usePresetStore((s) => s.state);
  const current = currentPreset(state);
  const isUser = current?.category === 'User';
  const categories = useMemo(() => allCategories(state.items), [state.items]);
  const suggestions = useMemo(() => allTags(state.items), [state.items]);
  const ids = useId();

  // Starts from what is playing: a factory preset saves under its own name and category.
  const [name, setName] = useState(() =>
    current ? (isUser ? `${current.name} copy` : current.name) : '',
  );
  const [category, setCategory] = useState(current?.folder ?? '');
  const [newCategory, setNewCategory] = useState('');
  const [tags, setTags] = useState<string[]>(() => current?.tags ?? []);
  const [tagText, setTagText] = useState('');
  const [description, setDescription] = useState(current?.description ?? '');
  const newCategoryRef = useRef<HTMLInputElement>(null);
  const tagRef = useRef<HTMLInputElement>(null);

  const creating = category === NEW_CATEGORY;
  const folder = (creating ? newCategory : category).trim();
  const trimmed = name.trim();
  const target = trimmed ? saveTarget(state.items, trimmed, folder) : undefined;
  const factoryTwin =
    !target &&
    state.items.some(
      (p) => p.category === 'Factory' && p.name.toLowerCase() === trimmed.toLowerCase(),
    );
  const invalid = !trimmed || (creating && !folder);

  useEffect(() => {
    if (creating) newCategoryRef.current?.focus();
  }, [creating]);

  const addTags = (text: string) => {
    const next = cleanTags([...tags, ...text.split(',')]);
    setTags(next);
    setTagText('');
  };

  const onTagKeys = (e: KeyboardEvent<HTMLInputElement>) => {
    if ((e.key === 'Enter' || e.key === ',') && tagText.trim()) {
      e.preventDefault();
      addTags(tagText);
    } else if (e.key === 'Backspace' && !tagText && tags.length) {
      setTags(tags.slice(0, -1));
    }
  };

  const typed = tagText.trim().toLowerCase();
  const offered = suggestions
    .filter((t) => !tags.includes(t) && (!typed || t.startsWith(typed)))
    .slice(0, 8);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    if (invalid) return;
    const allTagsNow = tagText.trim() ? cleanTags([...tags, ...tagText.split(',')]) : tags;
    await sendPresetCommand(() =>
      presetActions.save(trimmed, folder, { tags: allTagsNow, description }),
    );
    await presetBrowser.rebase();
    presetBrowser.endForm();
    showToast(`Saved ${trimmed} to ${['User', folder].filter(Boolean).join(' / ')}`);
  };

  return (
    <form className={styles.form} onSubmit={(e) => void submit(e)} noValidate>
      <h3 id="preset-form-title" className={styles.title}>
        Save as a user preset
      </h3>
      <p className={styles.lede}>
        {current ? (
          <>
            The sound now playing{state.dirty ? ', with your edits,' : ''} becomes a preset of your
            own. {current.category === 'Factory' && 'The factory preset stays as it is.'}
          </>
        ) : (
          'The sound now playing becomes a preset of your own.'
        )}
      </p>

      <label className={styles.field}>
        <span className={styles.label}>Name</span>
        <input
          data-autofocus
          value={name}
          maxLength={64}
          required
          aria-invalid={!trimmed || undefined}
          aria-describedby={target || factoryTwin ? `${ids}-name` : undefined}
          onChange={(e) => setName(e.target.value)}
        />
      </label>
      {/* Always in the page, so screen readers announce what appears in it. */}
      <div id={`${ids}-name`} className={styles.status} role="status">
        {target && (
          <p className={styles.warn}>
            <Icon name="info" size={14} />
            <span>
              “{target.name}” is already in {['User', target.folder].filter(Boolean).join(' / ')}.
              Saving replaces it.
            </span>
          </p>
        )}
        {factoryTwin && (
          <p className={styles.note}>
            A factory preset has this name too. Yours is saved under User.
          </p>
        )}
      </div>

      <div className={styles.row}>
        <label className={styles.field}>
          <span className={styles.label}>Category</span>
          <select value={category} onChange={(e) => setCategory(e.target.value)}>
            <option value="">No category</option>
            {categories.map((c) => (
              <option key={c} value={c}>
                {c}
              </option>
            ))}
            <option value={NEW_CATEGORY}>New category…</option>
          </select>
        </label>
        {creating && (
          <label className={styles.field}>
            <span className={styles.label}>New category</span>
            <input
              ref={newCategoryRef}
              value={newCategory}
              maxLength={40}
              placeholder="e.g. Live set"
              aria-invalid={!folder || undefined}
              onChange={(e) => setNewCategory(e.target.value)}
            />
          </label>
        )}
      </div>

      <div className={styles.field}>
        <span className={styles.label} id={`${ids}-tags`}>
          Tags
        </span>
        <div
          className={styles.tagBox}
          onClick={() => tagRef.current?.focus()}
          aria-labelledby={`${ids}-tags`}
          role="group"
        >
          {tags.map((t) => (
            <span key={t} className={styles.tag}>
              {t}
              <button
                type="button"
                aria-label={`Remove tag ${t}`}
                onClick={() => setTags(tags.filter((x) => x !== t))}
              >
                <Icon name="close" size={10} />
              </button>
            </span>
          ))}
          {tags.length < MAX_TAGS && (
            <input
              ref={tagRef}
              value={tagText}
              aria-label="Add a tag"
              placeholder={tags.length ? '' : 'Type a tag, then Enter'}
              maxLength={24}
              onChange={(e) => setTagText(e.target.value)}
              onKeyDown={onTagKeys}
            />
          )}
        </div>
        {offered.length > 0 && tags.length < MAX_TAGS && (
          <div className={styles.suggestions} role="group" aria-label="Suggested tags">
            {offered.map((t) => (
              <button
                key={t}
                type="button"
                className={`${buttons.press} ${styles.suggestion}`}
                onClick={() => {
                  addTags(t);
                  tagRef.current?.focus();
                }}
              >
                <Icon name="plus" size={10} />
                {t}
              </button>
            ))}
          </div>
        )}
      </div>

      <label className={styles.field}>
        <span className={styles.labelRow}>
          <span className={styles.label}>Note</span>
          <span className={styles.counter}>
            {description.length}/{MAX_DESCRIPTION}
          </span>
        </span>
        <input
          value={description}
          maxLength={MAX_DESCRIPTION}
          placeholder="What it is for, in a line (optional)"
          onChange={(e) => setDescription(e.target.value)}
        />
      </label>

      <div className={styles.actions}>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small}`}
          onClick={() => presetBrowser.endForm()}
        >
          Cancel
        </button>
        <button
          type="submit"
          className={`${buttons.nacre} ${buttons.small}`}
          disabled={invalid}
          aria-describedby={target ? `${ids}-name` : undefined}
        >
          {target ? `Replace “${target.name}”` : 'Save preset'}
        </button>
      </div>
    </form>
  );
}

function RenameForm() {
  const state = usePresetStore((s) => s.state);
  const current = currentPreset(state);
  const [name, setName] = useState(current?.name ?? '');
  const clash = !!current && renameClashes(state.items, current.id, name);

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    if (clash || !current || !name.trim()) return;
    await sendPresetCommand(() => presetActions.rename(current.id, name.trim()));
    const now = usePresetStore.getState().state;
    if (now.currentId && now.currentId !== current.id)
      presetBrowser.renamed(current.id, now.currentId, now.currentName);
    await presetBrowser.rebase();
    presetBrowser.endForm();
  };

  return (
    <form className={styles.form} onSubmit={(e) => void submit(e)} noValidate>
      <h3 id="preset-form-title" className={styles.title}>
        Rename preset
      </h3>
      <label className={styles.field}>
        <span className={styles.label}>New name</span>
        <input
          data-autofocus
          value={name}
          onChange={(e) => setName(e.target.value)}
          maxLength={64}
          required
          aria-invalid={clash || !name.trim() || undefined}
          aria-describedby={clash ? 'preset-rename-error' : undefined}
        />
      </label>
      {clash && (
        <p id="preset-rename-error" className={styles.error} role="alert">
          Another preset in this folder is already called “{name.trim()}”. Choose another name.
        </p>
      )}
      <div className={styles.actions}>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small}`}
          onClick={() => presetBrowser.endForm()}
        >
          Cancel
        </button>
        <button
          type="submit"
          className={`${buttons.nacre} ${buttons.small}`}
          disabled={!name.trim()}
        >
          Rename
        </button>
      </div>
    </form>
  );
}
