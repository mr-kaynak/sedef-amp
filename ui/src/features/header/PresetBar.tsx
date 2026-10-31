// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback, useRef } from 'react';
import { Icon } from '../../controls/Icon';
import { Popover } from '../../controls/Popover';
import { currentPreset, presetActions, usePresetStore } from '../../state/presetStore';
import { uiActions, useUiStore } from '../../state/uiStore';
import { PresetBrowser } from './PresetBrowser';
import styles from './PresetBar.module.css';

export function PresetBar() {
  const state = usePresetStore((s) => s.state);
  const open = useUiStore((s) => s.popover === 'presets');
  const triggerRef = useRef<HTMLButtonElement>(null);
  const close = useCallback(() => uiActions.setPopover(null), []);
  const preset = currentPreset(state);
  const where = preset ? [preset.category, preset.folder].filter(Boolean).join(' / ') : 'Unsaved';

  return (
    <div className={styles.bar}>
      <button
        type="button"
        className={styles.step}
        aria-label="Previous preset"
        onClick={() => void presetActions.step(-1)}
      >
        <Icon name="prev" />
      </button>
      <button
        ref={triggerRef}
        type="button"
        className={styles.current}
        aria-haspopup="dialog"
        aria-expanded={open}
        aria-label={`Preset: ${state.currentName}${state.dirty ? ', modified' : ''}. Open preset browser`}
        onClick={() => uiActions.togglePopover('presets')}
      >
        <span className={styles.where}>{where}</span>
        <span className={styles.nameRow}>
          <span className={styles.name}>{state.currentName}</span>
          {state.dirty && <span className={styles.dirty} title="Modified since it was loaded" />}
        </span>
        <Icon name="down" size={14} className={styles.chevron} />
      </button>
      <button
        type="button"
        className={styles.step}
        aria-label="Next preset"
        onClick={() => void presetActions.step(1)}
      >
        <Icon name="next" />
      </button>
      <Popover
        open={open}
        onClose={close}
        anchorRef={triggerRef}
        label="Presets"
        className={styles.popover}
      >
        <PresetBrowser onDone={close} />
      </Popover>
    </div>
  );
}
