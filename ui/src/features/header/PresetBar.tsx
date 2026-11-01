// SPDX-License-Identifier: AGPL-3.0-or-later
// The header's preset bar: previous/next, the current preset (opens the browser) with its dirty
// dot, and A/B compare.
import { Icon } from '../../controls/Icon';
import { useRadioGroup } from '../../controls/useRadioGroup';
import { currentPreset, usePresetStore } from '../../state/presetStore';
import {
  BROWSER_TRIGGER_ID,
  compareCopy,
  compareSwitch,
  presetBrowser,
  stepPreset,
  useBrowserStore,
} from '../presets/session';
import styles from './PresetBar.module.css';

function CompareSwitch() {
  const state = usePresetStore((s) => s.state);
  const { active, other } = state.compare;
  const slots = (['A', 'B'] as const).map((slot) =>
    slot === active
      ? { slot, name: state.currentName, dirty: state.dirty, empty: false }
      : { slot, name: other?.name ?? '', dirty: other?.dirty ?? false, empty: !other },
  );
  const inactive = active === 'A' ? 'B' : 'A';
  const { onKeyDown, radio } = useRadioGroup(2, active === 'A' ? 0 : 1, () => {
    void compareSwitch();
  });

  return (
    <div className={styles.compare}>
      <div
        role="radiogroup"
        aria-label="Compare A/B"
        className={styles.slots}
        onKeyDown={onKeyDown}
      >
        {slots.map((s, i) => {
          const description = s.empty
            ? `empty; switching starts it as a copy of ${active}`
            : `${s.name}${s.dirty ? ', edited' : ''}`;
          return (
            <button
              key={s.slot}
              type="button"
              className={styles.slot}
              data-empty={s.empty || undefined}
              aria-label={`${s.slot}: ${description}`}
              title={`${s.slot}: ${description}`}
              {...radio(i)}
            >
              {s.slot}
              {s.dirty && <span className={styles.slotDirty} aria-hidden="true" />}
            </button>
          );
        })}
      </div>
      <button
        type="button"
        className={styles.copy}
        aria-label={`Copy ${active} to ${inactive}`}
        title={`Copy ${active} to ${inactive}`}
        onClick={() => void compareCopy()}
      >
        <Icon name="copy" size={14} />
      </button>
    </div>
  );
}

export function PresetBar() {
  const state = usePresetStore((s) => s.state);
  const open = useBrowserStore((s) => s.open);
  const preset = currentPreset(state);
  const where = preset ? [preset.category, preset.folder].filter(Boolean).join(' / ') : 'Unsaved';

  return (
    <div className={styles.group}>
      <div className={styles.bar}>
        <button
          type="button"
          className={styles.step}
          aria-label="Previous preset"
          onClick={() => void stepPreset(-1)}
        >
          <Icon name="prev" />
        </button>
        <button
          id={BROWSER_TRIGGER_ID}
          type="button"
          className={styles.current}
          aria-haspopup="dialog"
          aria-expanded={open}
          aria-label={`Preset: ${state.currentName}${state.dirty ? ', modified' : ''}. ${open ? 'Close' : 'Open'} preset browser`}
          onClick={() => presetBrowser.toggle()}
        >
          <span className={styles.where}>{where}</span>
          <span className={styles.nameRow}>
            <span className={styles.name}>{state.currentName}</span>
            {state.dirty && <span className={styles.dirty} title="Modified since it was loaded" />}
          </span>
          <Icon name={open ? 'up' : 'down'} size={14} className={styles.chevron} />
        </button>
        <button
          type="button"
          className={styles.step}
          aria-label="Next preset"
          onClick={() => void stepPreset(1)}
        >
          <Icon name="next" />
        </button>
      </div>
      <CompareSwitch />
    </div>
  );
}
