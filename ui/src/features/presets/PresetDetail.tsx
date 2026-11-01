// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser's right pane: the highlighted preset in full (category and author, the name in the
// display face, its note, the chain by stage, tags) and what you can do with it.
import { Icon } from '../../controls/Icon';
import { Led } from '../../controls/Led';
import buttons from '../../controls/Button.module.css';
import type { PresetInfo } from '../../bridge/protocol';
import { presetActions, usePresetStore } from '../../state/presetStore';
import { modKey } from '../../app/platform';
import { AmpBadge } from './AmpBadge';
import { chainGroups } from './library';
import { deletePreset, presetBrowser, saveCurrent, useBrowserStore } from './session';
import styles from './PresetDetail.module.css';

function Chain({ p }: { p: PresetInfo }) {
  const g = chainGroups(p);
  const list = (items: string[]) => (items.length ? items.join(' → ') : 'None');
  return (
    <dl className={styles.chain}>
      <dt>Pedals</dt>
      <dd>{list(g.pedals)}</dd>
      <dt>Amp</dt>
      <dd>
        <AmpBadge p={p} />
      </dd>
      <dt>Cab</dt>
      <dd>{g.cab}</dd>
      <dt>FX</dt>
      <dd>{list(g.fx)}</dd>
    </dl>
  );
}

export function PresetDetail({ preset }: { preset: PresetInfo | undefined }) {
  const state = usePresetStore((s) => s.state);
  const filterTags = useBrowserStore((s) => s.filter.tags);
  const mod = modKey();
  const loaded = !preset || preset.id === state.currentId;
  const isUser = loaded && preset?.category === 'User';

  const toggleTag = (tag: string) =>
    presetBrowser.setFilter({
      tags: filterTags.includes(tag) ? filterTags.filter((t) => t !== tag) : [...filterTags, tag],
    });

  return (
    <aside className={styles.detail} aria-label="Preset details">
      <div className={styles.scroll}>
        {preset ? (
          <>
            <p className={styles.kicker}>
              {[preset.folder || 'No category', preset.author || preset.category].join(' · ')}
            </p>
            <h3 className={styles.name}>{preset.name}</h3>
            <p className={styles.state}>
              <Led on={loaded} size={6} />
              {loaded ? (
                <span>
                  Playing
                  {state.dirty && <span className={styles.edited}> · edited</span>}
                </span>
              ) : (
                <span>Not loaded</span>
              )}
              <span className={styles.source}>{preset.category}</span>
            </p>
            {preset.description && <p className={styles.description}>{preset.description}</p>}
            <Chain p={preset} />
            {preset.tags.length > 0 && (
              <div className={styles.tags} role="group" aria-label="Tags (filter by tag)">
                {preset.tags.map((t) => (
                  <button
                    key={t}
                    type="button"
                    className={styles.tag}
                    aria-pressed={filterTags.includes(t)}
                    onClick={() => toggleTag(t)}
                  >
                    {t}
                  </button>
                ))}
              </div>
            )}
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small} ${styles.favourite}`}
              aria-pressed={preset.favorite}
              aria-keyshortcuts="F"
              onClick={() => void presetActions.setFavorite(preset.id, !preset.favorite)}
            >
              <Icon name="star" size={13} filled={preset.favorite} />
              {preset.favorite ? 'Favourite' : 'Add to favourites'}
            </button>
          </>
        ) : (
          <>
            <p className={styles.kicker}>Not saved as a preset</p>
            <h3 className={styles.name}>{state.currentName}</h3>
            <p className={styles.description}>
              The sound playing now isn't a preset yet. Save it to find it here later.
            </p>
          </>
        )}
      </div>

      <div className={styles.actions}>
        {loaded ? (
          <button
            type="button"
            className={`${buttons.nacre} ${styles.primary}`}
            aria-keyshortcuts="Enter"
            onClick={() => presetBrowser.keep()}
          >
            Use &amp; close
            <kbd aria-hidden="true">↵</kbd>
          </button>
        ) : (
          <button
            type="button"
            className={`${buttons.nacre} ${styles.primary}`}
            onClick={() => preset && void presetBrowser.audition(preset.id)}
          >
            Load preset
          </button>
        )}
        {loaded && (
          <div className={styles.row}>
            <button
              type="button"
              className={`${state.dirty ? buttons.solid : buttons.ghost} ${buttons.small}`}
              aria-keyshortcuts={`${mod.aria}+S`}
              title={
                isUser
                  ? `Save ${state.currentName} (${mod.symbol}S)`
                  : `Save a copy (${mod.symbol}S)`
              }
              onClick={() => void saveCurrent()}
            >
              Save
            </button>
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              aria-keyshortcuts={`${mod.aria}+Shift+S`}
              onClick={() => presetBrowser.open('saveAs')}
            >
              Save as…
            </button>
            {isUser && (
              <>
                <button
                  type="button"
                  className={`${buttons.ghost} ${buttons.small}`}
                  onClick={() => presetBrowser.open('rename')}
                >
                  Rename
                </button>
                <button
                  type="button"
                  className={`${buttons.ghost} ${buttons.small} ${styles.delete}`}
                  title="Delete it (you can undo)"
                  onClick={() => preset && void deletePreset(preset.id)}
                >
                  Delete
                </button>
              </>
            )}
          </div>
        )}
      </div>
    </aside>
  );
}
