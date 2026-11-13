// SPDX-License-Identifier: AGPL-3.0-or-later
import buttons from '../../controls/Button.module.css';
import nacre from '../../controls/NacreFill.module.css';
import { ChoiceSelect } from '../../controls/ChoiceSelect';
import { IconSwap } from '../../controls/IconSwap';
import { Knob } from '../../controls/Knob';
import { Segmented } from '../../controls/Segmented';
import { setParam, useParamValue } from '../../params/useParam';
import { telemetry, useLiveSelector } from '../../state/live';
import { useUiStore } from '../../state/uiStore';
import { setMetronomePattern, tapTempo } from './actions';
import { DrumsControls } from './DrumsControls';
import {
  cycleStep,
  STEP_ACCENT,
  STEP_NORMAL,
  STEP_OFF,
  STEP_SOFT,
  stepsFor,
} from '../../lib/metronome';
import styles from './MetronomePanel.module.css';

const STEP_NAMES: Record<number, string> = {
  [STEP_OFF]: 'off',
  [STEP_NORMAL]: 'normal',
  [STEP_ACCENT]: 'accent',
  [STEP_SOFT]: 'soft',
};

export function MetronomePanel() {
  const playing = useParamValue('metronomeEnabled') >= 0.5;
  const drums = Math.round(useParamValue('metronomeMode')) === 1;
  const bpm = useParamValue('tempo');
  const signature = Math.round(useParamValue('metronomeSignature'));
  const steps = useUiStore((s) => s.metronomeSteps);
  const count = stepsFor(signature);
  const currentStep = useLiveSelector(telemetry, (t) => (playing ? (t?.metronomeStep ?? -1) : -1));

  const toggleStep = (i: number) => {
    const next = [...steps];
    next[i] = cycleStep(next[i] ?? STEP_NORMAL);
    setMetronomePattern(next);
  };

  return (
    <div className={styles.panel}>
      <div className={styles.top}>
        <button
          type="button"
          className={`${buttons.press} ${nacre.fill} ${styles.play}`}
          aria-pressed={playing}
          aria-label={
            drums
              ? playing
                ? 'Stop drums'
                : 'Start drums'
              : playing
                ? 'Stop metronome'
                : 'Start metronome'
          }
          onClick={() => setParam('metronomeEnabled', playing ? 0 : 1)}
        >
          <IconSwap active={playing} icon="play" activeIcon="stop" size={18} filled />
        </button>
        <div className={styles.tempo}>
          <span className={styles.bpm}>{Math.round(bpm)}</span>
          <span className={styles.unit}>bpm</span>
        </div>
        <Knob param="tempo" size="sm" label="Tempo" hideValue />
        <button
          type="button"
          className={`${buttons.tap} ${styles.tap}`}
          onClick={() => void tapTempo()}
        >
          Tap
        </button>
      </div>

      <Segmented param="metronomeMode" ariaLabel="Click or drums" className={styles.mode} />

      {drums ? (
        <DrumsControls currentStep={currentStep} />
      ) : (
        <>
          <div className={styles.pattern}>
            <div
              className={styles.grid}
              style={{ gridTemplateColumns: `repeat(${count}, 1fr)` }}
              role="group"
              aria-label="Beat pattern"
              aria-describedby="metronome-pattern-hint"
            >
              {Array.from({ length: count }, (_, i) => {
                const state = steps[i] ?? STEP_NORMAL;
                return (
                  <button
                    key={i}
                    type="button"
                    className={`${buttons.press} ${styles.step}`}
                    data-state={STEP_NAMES[state]}
                    data-now={i === currentStep || undefined}
                    aria-label={`Beat ${i + 1}: ${STEP_NAMES[state]}`}
                    onClick={() => toggleStep(i)}
                  >
                    <span className={styles.bar} />
                    <span className={styles.index}>{i + 1}</span>
                  </button>
                );
              })}
            </div>
            <p id="metronome-pattern-hint" className={styles.hint}>
              Click a beat to change it: normal, accent, soft, off.
            </p>
          </div>

          <div className={styles.row}>
            <label className={styles.field}>
              <span>Signature</span>
              <ChoiceSelect param="metronomeSignature" />
            </label>
            <label className={styles.field}>
              <span>Sound</span>
              <ChoiceSelect param="metronomeSound" />
            </label>
            <Knob param="metronomeVolume" size="sm" label="Volume" />
          </div>
        </>
      )}
    </div>
  );
}
