// SPDX-License-Identifier: AGPL-3.0-or-later
// The strip under the list: what a one-line row leaves out (the note, the author, the cab) for
// the row under the pointer, else the highlighted one, and what you can do with the highlighted
// one.
import type { ReactNode } from 'react';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import { Led } from '../../controls/Led';
import buttons from '../../controls/Button.module.css';
import type { PresetInfo } from '../../bridge/protocol';
import { presetActions, usePresetStore } from '../../state/presetStore';
import { modKey } from '../../app/platform';
import { CAB_NAMES } from './library';
import { deletePreset, presetBrowser, saveCurrent } from './session';
import styles from './PresetDetail.module.css';

export interface PresetDetailProps {
  /** The highlighted row's preset: the one the actions are about. */
  preset: PresetInfo | undefined;
  /** The row under the pointer, when it is another one: only its description shows. */
  hovered?: PresetInfo | undefined;
  /** The way back (Esc), at the start of the action row. */
  back: ReactNode;
}

export function PresetDetail({ preset, hovered, back }: PresetDetailProps) {
  const state = usePresetStore((s) => s.state);
  const mod = modKey();
  const loaded = !preset || preset.id === state.currentId;
  const isUser = loaded && preset?.category === 'User';
  const shown = hovered ?? preset;
  const playing = shown !== undefined && shown.id === state.currentId;

  return (
    <aside className={styles.detail} aria-label="Preset details">
      <div className={styles.info}>
        {shown ? (
          <>
            <div className={styles.titleLine}>
              <h3 className={styles.name}>{shown.name}</h3>
              <p className={styles.facts}>
                <span>{shown.folder || 'No category'}</span>
                <span>{shown.author || shown.category}</span>
                <span>{CAB_NAMES[shown.cab] ?? ''}</span>
              </p>
              <p className={styles.state}>
                <Led on={playing} size={6} />
                {playing ? (
                  <span className={styles.playing}>
                    Playing
                    {state.dirty && <span className={styles.edited}> · edited</span>}
                  </span>
                ) : (
                  <span>{hovered ? 'Click to audition' : 'Loading'}</span>
                )}
              </p>
            </div>
            <p className={styles.description}>
              {shown.description || (shown.category === 'User' ? 'Your preset.' : '')}
            </p>
          </>
        ) : (
          <>
            <div className={styles.titleLine}>
              <h3 className={styles.name}>{state.currentName}</h3>
              <p className={styles.state}>Not saved as a preset</p>
            </div>
            <p className={styles.description}>
              The sound playing now isn't a preset yet. Save it to find it here later.
            </p>
          </>
        )}
      </div>

      <div className={styles.actions}>
        {back}
        <span className={styles.spacer} />
        {preset && (
          <button
            type="button"
            className={`${buttons.ghost} ${buttons.small} ${buttons.lead} ${styles.favourite}`}
            aria-pressed={preset.favorite}
            aria-keyshortcuts="F"
            onClick={() => void presetActions.setFavorite(preset.id, !preset.favorite)}
          >
            <Icon name="star" size={13} filled={preset.favorite} stroke={ICON_STROKE_LABEL} />
            {preset.favorite ? 'Favourite' : 'Add to favourites'}
          </button>
        )}
        {loaded && (
          <>
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
          </>
        )}
        {loaded ? (
          <button
            type="button"
            className={`${buttons.nacre} ${buttons.small} ${styles.primary}`}
            aria-keyshortcuts="Enter"
            onClick={() => presetBrowser.keep()}
          >
            Use &amp; close
            <kbd aria-hidden="true">↵</kbd>
          </button>
        ) : (
          <button
            type="button"
            className={`${buttons.nacre} ${buttons.small} ${styles.primary}`}
            onClick={() => preset && void presetBrowser.audition(preset.id)}
          >
            Load preset
          </button>
        )}
      </div>
    </aside>
  );
}
