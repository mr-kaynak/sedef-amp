// SPDX-License-Identifier: AGPL-3.0-or-later
// Transpose under the input meter: the whole rig plays lower or higher without retuning
// (docs/dsp/transpose.md). The chip shows the setting from every page; its panel steps it a
// semitone at a time, goes back to 0 and says what latency it adds.
import { useCallback, useRef } from 'react';
import buttons from '../../controls/Button.module.css';
import { Popover } from '../../controls/Popover';
import { describeTranspose, transposeLatencyMs } from '../../lib/transpose';
import { signed } from '../../params/format';
import { useParam } from '../../params/useParam';
import { uiActions, useUiStore } from '../../state/uiStore';
import styles from './TransposeControl.module.css';

export function TransposeControl() {
  const p = useParam('transpose');
  const open = useUiStore((s) => s.popover === 'transpose');
  const chipRef = useRef<HTMLButtonElement>(null);
  const close = useCallback(() => uiActions.setPopover(null), []);
  const st = Math.round(p.value);
  const on = st !== 0;

  return (
    <div className={styles.anchor}>
      <button
        ref={chipRef}
        type="button"
        className={`${buttons.press} ${styles.chip}`}
        data-on={on || undefined}
        aria-label={`Transpose, ${describeTranspose(st)}`}
        aria-haspopup="dialog"
        aria-expanded={open}
        title="Play lower or higher without retuning"
        onClick={() => uiActions.togglePopover('transpose')}
      >
        Transpose
        {/* The semitones without their unit, as a keyboard's transpose reads; the panel says
            "st". (With it, −12 st would reach the Output label.) */}
        {on && <span className={styles.value}>{signed(st, 0)}</span>}
      </button>
      <Popover
        open={open}
        onClose={close}
        anchorRef={chipRef}
        label="Transpose"
        className={styles.popover}
      >
        <TransposePanel />
      </Popover>
    </div>
  );
}

function TransposePanel() {
  const p = useParam('transpose');
  const st = Math.round(p.value);
  const latency = transposeLatencyMs(st);
  const set = (value: number) => {
    p.begin();
    p.set(value);
    p.end();
  };

  return (
    <div className={styles.panel}>
      <h3 className={styles.heading}>Transpose</h3>
      <div className={styles.row}>
        <div className={`${buttons.stepper} ${styles.stepper}`}>
          <button
            type="button"
            aria-label="Transpose down a semitone"
            onClick={() => set(st - 1)}
            disabled={st <= p.spec.min}
          >
            −
          </button>
          {/* Read out as words: "st" would be spelled out or read as "street". */}
          <output aria-live="polite">
            <span aria-hidden="true">{p.display}</span>
            <span className="visually-hidden">{describeTranspose(st)}</span>
          </output>
          <button
            type="button"
            aria-label="Transpose up a semitone"
            onClick={() => set(st + 1)}
            disabled={st >= p.spec.max}
          >
            +
          </button>
        </div>
        {st !== 0 && (
          <button
            type="button"
            className={`${buttons.link} ${styles.reset}`}
            onClick={() => set(0)}
          >
            Back to 0
          </button>
        )}
      </div>
      <p className={styles.note}>
        {st === 0
          ? 'Plays the whole rig lower or higher without retuning, up to an octave either way. ' +
            'Shifting down adds 3 ms of latency, shifting up 10 ms.'
          : `Adds ${latency} ms of latency while it shifts ${st < 0 ? 'down' : 'up'}.`}
      </p>
      <p className={styles.note}>The tuner still hears your strings.</p>
    </div>
  );
}
