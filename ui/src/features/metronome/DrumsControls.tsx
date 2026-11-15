// SPDX-License-Identifier: AGPL-3.0-or-later
// The metronome panel in Drums mode (docs/drums.md, section 8): the grooves written for the
// current meter, how busy they play, how often they fill, and how they sit under the guitar.
import { useId } from 'react';
import { ChoiceSelect } from '../../controls/ChoiceSelect';
import { Knob } from '../../controls/Knob';
import { Segmented } from '../../controls/Segmented';
import { useRadioGroup } from '../../controls/useRadioGroup';
import { FILL_LABELS, grooveFor, groovesFor } from '../../lib/drums';
import { stepsFor } from '../../lib/metronome';
import { setParam, useParamValue } from '../../params/useParam';
import styles from './DrumsControls.module.css';

export function DrumsControls({ currentStep }: { currentStep: number }) {
  const id = useId();
  const signature = Math.round(useParamValue('metronomeSignature'));
  const groove = useParamValue('drumsGroove');
  const list = groovesFor(signature);
  // The groove that plays: the chosen one if it is in this meter, otherwise the meter's first.
  const playing = grooveFor(groove, signature);
  const selected = list.findIndex((g) => g.index === playing);
  const { onKeyDown, radio } = useRadioGroup(list.length, selected, (i) => {
    const g = list[i];
    if (g) setParam('drumsGroove', g.index);
  });
  const count = stepsFor(signature);

  return (
    <div className={styles.drums}>
      <div className={styles.meter}>
        <label className={styles.field}>
          <span>Signature</span>
          <ChoiceSelect param="metronomeSignature" />
        </label>
        <div className={styles.beats} aria-hidden="true">
          {Array.from({ length: count }, (_, i) => (
            <span key={i} className={styles.beat} data-now={i === currentStep || undefined} />
          ))}
        </div>
      </div>

      <div role="radiogroup" aria-label="Groove" className={styles.grooves} onKeyDown={onKeyDown}>
        {list.map((g, i) => (
          <button
            key={g.index}
            type="button"
            className={styles.groove}
            aria-labelledby={`${id}-name-${i}`}
            aria-describedby={`${id}-feel-${i}`}
            {...radio(i)}
          >
            <span id={`${id}-name-${i}`} className={styles.name}>
              {g.name}
            </span>
            <span id={`${id}-feel-${i}`} className={styles.feel}>
              {g.feel}
            </span>
          </button>
        ))}
      </div>

      <div className={styles.setting}>
        <span className={styles.label} aria-hidden="true">
          Intensity
        </span>
        <Segmented
          param="drumsIntensity"
          ariaLabel="Intensity"
          ariaDescribedBy={`${id}-intensity`}
          className={styles.segmented}
        />
      </div>
      <p id={`${id}-intensity`} className={styles.hint}>
        1 plays the bare groove, 5 the busiest.
      </p>
      <div className={styles.setting}>
        <span className={styles.label} aria-hidden="true">
          Fill every
        </span>
        <Segmented
          param="drumsFills"
          labels={FILL_LABELS}
          ariaLabel="Fill every"
          className={styles.segmented}
        />
      </div>

      <div className={styles.knobs}>
        <Knob param="drumsLevel" size="sm" label="Level" />
        <Knob param="drumsTone" size="sm" label="Tone" />
      </div>
    </div>
  );
}
