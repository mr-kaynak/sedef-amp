// SPDX-License-Identifier: AGPL-3.0-or-later
import buttons from '../../controls/Button.module.css';
import { Segmented } from '../../controls/Segmented';
import nacre from '../../controls/NacreFill.module.css';
import segmented from '../../controls/Segmented.module.css';
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
      {session.isStandalone && (
        <section className={styles.section}>
          <h3 className={styles.heading}>Audio</h3>
          <div className={styles.row}>
            <p className={styles.rowText}>Your audio interface</p>
            <button
              type="button"
              className={`${buttons.solid} ${buttons.small}`}
              aria-haspopup="dialog"
              onClick={() => {
                uiActions.setPopover(null);
                void invoke('sedef_audioSettings');
              }}
            >
              Audio settings…
            </button>
          </div>
          <p className={styles.note}>Its input, sample rate and buffer size.</p>
        </section>
      )}
      <section className={styles.section}>
        <h3 className={styles.heading}>Window size</h3>
        <div
          className={`${segmented.segmented} ${styles.scales}`}
          role="radiogroup"
          aria-label="Window size"
          onKeyDown={scales.onKeyDown}
        >
          {SCALES.map((s, i) => (
            <button
              key={s}
              type="button"
              className={`${nacre.fill} ${segmented.option}`}
              {...scales.radio(i)}
            >
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
      <section className={styles.section}>
        <h3 className={styles.heading}>Latency</h3>
        <Segmented
          param="oversampling"
          labels={['Low latency', 'Linear phase']}
          ariaLabel="Latency"
        />
        <p className={styles.note}>
          Linear phase keeps every frequency in time for mixing and adds about 5 ms at 48 kHz.
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
              className={buttons.link}
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
