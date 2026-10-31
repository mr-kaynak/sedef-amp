// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect } from 'react';
import { Icon } from '../../controls/Icon';
import { Toggle } from '../../controls/Toggle';
import buttons from '../../controls/Button.module.css';
import { setParam, useParamValue } from '../../params/useParam';
import { acquirePitchAnalysis } from '../../state/pitchLease';
import { uiActions } from '../../state/uiStore';
import { TunerGauge } from './TunerGauge';
import styles from './TunerOverlay.module.css';

function ReferenceControl() {
  const ref = useParamValue('tunerReference');
  const nudge = (delta: number) => setParam('tunerReference', Math.round(ref) + delta);
  return (
    <div className={styles.reference}>
      <span className={styles.label}>Reference A4</span>
      <div className={styles.stepper}>
        <button
          type="button"
          aria-label="Lower reference by 1 Hz"
          onClick={() => nudge(-1)}
          disabled={ref <= 420}
        >
          −
        </button>
        <output aria-live="polite">{Math.round(ref)} Hz</output>
        <button
          type="button"
          aria-label="Raise reference by 1 Hz"
          onClick={() => nudge(1)}
          disabled={ref >= 460}
        >
          +
        </button>
      </div>
      {Math.round(ref) !== 440 && (
        <button
          type="button"
          className={styles.resetRef}
          onClick={() => setParam('tunerReference', 440)}
        >
          Back to 440 Hz
        </button>
      )}
    </div>
  );
}

export function TunerOverlay() {
  useEffect(() => acquirePitchAnalysis(), []);
  return (
    <section className={styles.overlay} aria-label="Tuner">
      <header className={styles.head}>
        <h2 className={styles.title}>Tuner</h2>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small}`}
          onClick={() => uiActions.openOverlay(null)}
        >
          <Icon name="close" size={14} />
          Close
        </button>
      </header>
      <div className={styles.body}>
        <TunerGauge />
      </div>
      <footer className={styles.foot}>
        <ReferenceControl />
        <Toggle param="tunerMute" label="Mute output while tuning" />
      </footer>
    </section>
  );
}
