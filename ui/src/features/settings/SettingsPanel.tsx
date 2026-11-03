// SPDX-License-Identifier: AGPL-3.0-or-later
import { Segmented } from '../../controls/Segmented';
import { useRadioGroup } from '../../controls/useRadioGroup';
import { useAppStore } from '../../state/appStore';
import { invoke } from '../../state/backendRef';
import { SCALES, uiActions, useUiStore } from '../../state/uiStore';
import { NacreStar } from '../header/Brand';
import { UpdatesSettings } from '../updates/UpdatesSettings';
import styles from './SettingsPanel.module.css';

const LICENSE_URL = 'https://www.gnu.org/licenses/agpl-3.0.html';

export function SettingsPanel() {
  const scale = useUiStore((s) => s.scale);
  const session = useAppStore((s) => s.session);

  const scales = useRadioGroup(
    SCALES.length,
    SCALES.findIndex((s) => s === scale),
    (index) => {
      const value = SCALES[index] ?? 1;
      uiActions.setScale(value);
      void invoke('sedef_ui_setScale', value);
    },
  );

  return (
    <div className={styles.panel}>
      <section className={styles.section}>
        <h3 className={styles.heading}>Window size</h3>
        <div
          className={styles.scales}
          role="radiogroup"
          aria-label="Window size"
          onKeyDown={scales.onKeyDown}
        >
          {SCALES.map((s, i) => (
            <button key={s} type="button" className={styles.scale} {...scales.radio(i)}>
              {Math.round(s * 100)}%
            </button>
          ))}
        </div>
      </section>
      <section className={styles.section}>
        <h3 className={styles.heading}>Stereo input</h3>
        <Segmented param="stereoMode" labels={['Sum to mono', 'Two channels']} />
        <p className={styles.note}>
          Applies when the host feeds a stereo signal. Sum to mono plays a guitar on either input at
          full level. Two channels runs a full rig per side.
        </p>
      </section>
      <UpdatesSettings className={styles.section} />
      <section className={`${styles.section} ${styles.about}`}>
        <NacreStar size={36} />
        <div>
          <p className={styles.product}>Sedef Amp {session.appVersion}</p>
          <p className={styles.note}>
            {session.wrapperType === 'Browser'
              ? 'Browser preview, simulated audio'
              : session.wrapperType}
            {', '}
            {(session.sampleRate / 1000).toFixed(1).replace(/\.0$/, '')} kHz,{' '}
            {session.latencySamples} samples latency
          </p>
          <p className={styles.note}>
            Free software under the GNU AGPL, version 3 or later.{' '}
            <button
              type="button"
              className={styles.link}
              onClick={() => void invoke('sedef_openUrl', LICENSE_URL)}
            >
              Read the licence
            </button>
          </p>
        </div>
      </section>
    </div>
  );
}
