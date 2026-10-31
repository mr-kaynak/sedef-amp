// SPDX-License-Identifier: AGPL-3.0-or-later
import { useMemo, useState } from 'react';
import type { FormEvent } from 'react';
import { Icon } from '../../controls/Icon';
import buttons from '../../controls/Button.module.css';
import { useAppStore } from '../../state/appStore';
import {
  currentPreset,
  groupPresets,
  presetActions,
  usePresetStore,
  type PresetFilter,
} from '../../state/presetStore';
import styles from './PresetBrowser.module.css';

type Mode = 'list' | 'saveAs' | 'rename' | 'delete';
type Tab = PresetFilter['category'];
const TABS: { id: Tab; label: string }[] = [
  { id: 'Factory', label: 'Factory' },
  { id: 'User', label: 'User' },
  { id: 'Favorites', label: 'Favourites' },
];

export function PresetBrowser({ onDone }: { onDone: () => void }) {
  const state = usePresetStore((s) => s.state);
  const canReveal = useAppStore((s) => s.session.backend === 'juce');
  const current = currentPreset(state);
  const isUser = current?.category === 'User';
  const [tab, setTab] = useState<Tab>(current?.category ?? 'Factory');
  const [query, setQuery] = useState('');
  const [mode, setMode] = useState<Mode>('list');
  const [name, setName] = useState('');
  const [folder, setFolder] = useState('');

  const groups = useMemo(
    () => groupPresets(state.items, { query, category: tab }),
    [state.items, query, tab],
  );
  const first = groups[0]?.items[0];

  const load = (id: string) => {
    void presetActions.load(id);
    onDone();
  };

  const beginSaveAs = () => {
    setName(isUser ? `${state.currentName} copy` : state.currentName);
    setFolder(isUser ? (current?.folder ?? '') : '');
    setMode('saveAs');
  };

  const save = () => {
    if (isUser && current) void presetActions.save(current.name, current.folder);
    else beginSaveAs();
  };

  const submitSaveAs = (e: FormEvent) => {
    e.preventDefault();
    if (!name.trim()) return;
    void presetActions.save(name.trim(), folder.trim());
    setTab('User');
    setMode('list');
  };

  const submitRename = (e: FormEvent) => {
    e.preventDefault();
    if (current && name.trim()) void presetActions.rename(current.id, name.trim());
    setMode('list');
  };

  return (
    <div className={styles.browser}>
      {mode === 'list' && (
        <>
          <div className={styles.search}>
            <Icon name="search" size={14} />
            <input
              data-autofocus
              type="search"
              placeholder="Search presets, folders, tags"
              aria-label="Search presets"
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && first) load(first.id);
              }}
            />
          </div>
          <div className={styles.tabs} role="tablist" aria-label="Preset library">
            {TABS.map((t) => (
              <button
                key={t.id}
                type="button"
                role="tab"
                aria-selected={tab === t.id}
                className={styles.tab}
                onClick={() => setTab(t.id)}
              >
                {t.label}
              </button>
            ))}
          </div>
          <div className={styles.list} role="tabpanel">
            {groups.length === 0 && (
              <p className={styles.empty}>
                {tab === 'Favorites'
                  ? 'Star a preset to keep it here.'
                  : tab === 'User' && !query
                    ? 'Presets you save appear here.'
                    : 'No presets match. Try another word.'}
              </p>
            )}
            {groups.map((g) => (
              <section key={g.folder || '(root)'} className={styles.group}>
                {g.folder && <h3 className={styles.folder}>{g.folder}</h3>}
                <ul>
                  {g.items.map((p) => (
                    <li
                      key={p.id}
                      className={styles.row}
                      data-current={p.id === state.currentId || undefined}
                    >
                      <button type="button" className={styles.load} onClick={() => load(p.id)}>
                        <span className={styles.presetName}>{p.name}</span>
                        {p.tags.length > 0 && (
                          <span className={styles.tags}>{p.tags.join(', ')}</span>
                        )}
                      </button>
                      <button
                        type="button"
                        className={styles.star}
                        aria-pressed={p.favorite}
                        aria-label={
                          p.favorite
                            ? `Remove ${p.name} from favourites`
                            : `Add ${p.name} to favourites`
                        }
                        onClick={() => void presetActions.setFavorite(p.id, !p.favorite)}
                      >
                        <Icon name="star" size={14} filled={p.favorite} />
                      </button>
                    </li>
                  ))}
                </ul>
              </section>
            ))}
          </div>
          <div className={styles.actions}>
            <button type="button" className={`${buttons.solid} ${buttons.small}`} onClick={save}>
              Save
            </button>
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              onClick={beginSaveAs}
            >
              Save as…
            </button>
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              disabled={!isUser}
              title={isUser ? undefined : 'Factory presets cannot be renamed'}
              onClick={() => {
                setName(state.currentName);
                setMode('rename');
              }}
            >
              Rename
            </button>
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              disabled={!isUser}
              title={isUser ? undefined : 'Factory presets cannot be deleted'}
              onClick={() => setMode('delete')}
            >
              Delete
            </button>
            <span className={styles.spacer} />
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
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              onClick={() => {
                void presetActions.init();
                onDone();
              }}
            >
              New
            </button>
          </div>
        </>
      )}

      {mode === 'saveAs' && (
        <form className={styles.form} onSubmit={submitSaveAs}>
          <h3 className={styles.formTitle}>Save as a user preset</h3>
          <label className={styles.field}>
            <span>Name</span>
            <input
              data-autofocus
              value={name}
              onChange={(e) => setName(e.target.value)}
              maxLength={64}
              required
            />
          </label>
          <label className={styles.field}>
            <span>Folder (optional)</span>
            <input
              value={folder}
              onChange={(e) => setFolder(e.target.value)}
              maxLength={64}
              placeholder="e.g. Live"
            />
          </label>
          <p className={styles.hint}>A user preset with the same name is replaced.</p>
          <div className={styles.formActions}>
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              onClick={() => setMode('list')}
            >
              Cancel
            </button>
            <button type="submit" className={`${buttons.nacre} ${buttons.small}`}>
              Save preset
            </button>
          </div>
        </form>
      )}

      {mode === 'rename' && (
        <form className={styles.form} onSubmit={submitRename}>
          <h3 className={styles.formTitle}>Rename preset</h3>
          <label className={styles.field}>
            <span>New name</span>
            <input
              data-autofocus
              value={name}
              onChange={(e) => setName(e.target.value)}
              maxLength={64}
              required
            />
          </label>
          <div className={styles.formActions}>
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              onClick={() => setMode('list')}
            >
              Cancel
            </button>
            <button type="submit" className={`${buttons.nacre} ${buttons.small}`}>
              Rename
            </button>
          </div>
        </form>
      )}

      {mode === 'delete' && current && (
        <div className={styles.form}>
          <h3 className={styles.formTitle}>Delete “{current.name}”?</h3>
          <p className={styles.hint}>
            The preset file is removed from your user folder. This cannot be undone.
          </p>
          <div className={styles.formActions}>
            <button
              data-autofocus
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              onClick={() => setMode('list')}
            >
              Keep it
            </button>
            <button
              type="button"
              className={`${buttons.solid} ${buttons.small} ${styles.danger}`}
              onClick={() => {
                void presetActions.remove(current.id);
                setMode('list');
              }}
            >
              Delete preset
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
