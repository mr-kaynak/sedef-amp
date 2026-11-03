// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import type { KeyboardEvent } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon } from '../../controls/Icon';
import { Toggle } from '../../controls/Toggle';
import { setParam, useParamSpec, useParamValue } from '../../params/useParam';
import { effectiveCab } from '../../params/schema';
import { beginGesture, endGesture } from '../../params/sender';
import { useAppStore } from '../../state/appStore';
import { uiActions } from '../../state/uiStore';
import { AMPS } from './amps';
import { AmpHead } from './AmpHead';
import { CapturePanel } from './CapturePanel';
import styles from './AmpSection.module.css';

/** The picker's index for the Capture card, after the three amps. */
const CAPTURE = AMPS.length;

function set(id: string, value: number) {
  beginGesture(id);
  setParam(id, value);
  endGesture(id);
}

function choose(index: number, source: number) {
  if (index === CAPTURE) {
    if (source !== 1) set('ampSource', 1);
    return;
  }
  if (source !== 0) set('ampSource', 0);
  set('ampModel', index);
}

function AmpPicker({ selected, withCapture }: { selected: number; withCapture: boolean }) {
  const refs = useRef<(HTMLButtonElement | null)[]>([]);
  const source = Math.round(useParamValue('ampSource'));
  const capture = useAppStore((s) => s.capture.loaded);
  const count = withCapture ? AMPS.length + 1 : AMPS.length;
  const onKeyDown = (e: KeyboardEvent) => {
    const delta = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
    if (!delta) return;
    e.preventDefault();
    const next = (selected + delta + count) % count;
    choose(next, source);
    refs.current[next]?.focus();
  };
  return (
    <div
      className={styles.picker}
      data-count={count}
      role="radiogroup"
      aria-label="Amp"
      onKeyDown={onKeyDown}
    >
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
          onClick={() => choose(amp.index, source)}
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
      {withCapture && (
        <button
          ref={(el) => {
            refs.current[CAPTURE] = el;
          }}
          type="button"
          role="radio"
          aria-checked={selected === CAPTURE}
          tabIndex={selected === CAPTURE ? 0 : -1}
          className={styles.card}
          onClick={() => choose(CAPTURE, source)}
        >
          <span className={`${styles.swatch} ${styles.captureSwatch}`} aria-hidden="true" />
          <span className={styles.cardText}>
            <span className={styles.cardName}>Capture</span>
            <span className={styles.cardCharacter} title={capture?.name}>
              {capture ? capture.name : 'Your own .nam file of a real amp'}
            </span>
          </span>
        </button>
      )}
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
        className={`${buttons.press} ${styles.linkButton}`}
        onClick={() => uiActions.setSection('cab')}
      >
        Open cab
      </button>
    </div>
  );
}

export function AmpSection() {
  const model = Math.round(useParamValue('ampModel'));
  const withCapture = useAppStore((s) => s.capturesSupported);
  const source = Math.round(useParamValue('ampSource'));
  const capture = withCapture && source === 1;
  const amp = AMPS[model] ?? AMPS[1];
  if (!amp) return null;
  return (
    <div className={styles.section}>
      <AmpPicker selected={capture ? CAPTURE : model} withCapture={withCapture} />
      <div className={styles.stage}>{capture ? <CapturePanel /> : <AmpHead amp={amp} />}</div>
      {!capture && <CabLink amp={model} />}
    </div>
  );
}
