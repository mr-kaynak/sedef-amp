// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import type { KeyboardEvent } from 'react';
import { Icon } from '../../controls/Icon';
import { Toggle } from '../../controls/Toggle';
import { setParam, useParamSpec, useParamValue } from '../../params/useParam';
import { effectiveCab } from '../../params/schema';
import { beginGesture, endGesture } from '../../params/sender';
import { uiActions } from '../../state/uiStore';
import { AMPS } from './amps';
import { AmpHead } from './AmpHead';
import styles from './AmpSection.module.css';

function chooseAmp(index: number) {
  beginGesture('ampModel');
  setParam('ampModel', index);
  endGesture('ampModel');
}

function AmpPicker({ selected }: { selected: number }) {
  const refs = useRef<(HTMLButtonElement | null)[]>([]);
  const onKeyDown = (e: KeyboardEvent) => {
    const delta = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
    if (!delta) return;
    e.preventDefault();
    const next = (selected + delta + AMPS.length) % AMPS.length;
    chooseAmp(next);
    refs.current[next]?.focus();
  };
  return (
    <div className={styles.picker} role="radiogroup" aria-label="Amp" onKeyDown={onKeyDown}>
      {AMPS.map((amp) => (
        <button
          key={amp.name}
          ref={(el) => {
            refs.current[amp.index] = el;
          }}
          type="button"
          role="radio"
          aria-checked={amp.index === selected}
          tabIndex={amp.index === selected ? 0 : -1}
          className={styles.card}
          onClick={() => chooseAmp(amp.index)}
        >
          <span
            className={styles.swatch}
            style={{
              background: `linear-gradient(180deg, ${amp.finish.plate[0]}, ${amp.finish.plate[1]})`,
            }}
            aria-hidden="true"
          />
          <span className={styles.cardText}>
            <span className={styles.cardName}>{amp.name}</span>
            <span className={styles.cardCharacter}>{amp.character}</span>
          </span>
        </button>
      ))}
    </div>
  );
}

function CabLink({ amp }: { amp: number }) {
  const linked = useParamValue('ampCabLink') >= 0.5;
  const cab = effectiveCab(amp, linked ? 1 : 0, Math.round(useParamValue('cabModel')));
  const cabName = useParamSpec('cabModel').choices[cab] ?? '';
  return (
    <div className={styles.link}>
      <Toggle param="ampCabLink" label="Cab follows amp" />
      <span className={styles.linkText}>
        <Icon name={linked ? 'link' : 'unlink'} size={14} />
        {linked ? `${cabName}, matched to this amp` : `${cabName}, chosen on the Cab page`}
      </span>
      <button
        type="button"
        className={styles.linkButton}
        onClick={() => uiActions.setSection('cab')}
      >
        Open cab
      </button>
    </div>
  );
}

export function AmpSection() {
  const selected = Math.round(useParamValue('ampModel'));
  const amp = AMPS[selected] ?? AMPS[1];
  if (!amp) return null;
  return (
    <div className={styles.section}>
      <AmpPicker selected={selected} />
      <div className={styles.stage}>
        <AmpHead amp={amp} />
      </div>
      <CabLink amp={selected} />
    </div>
  );
}
