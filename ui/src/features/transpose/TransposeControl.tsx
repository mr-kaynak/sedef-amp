// SPDX-License-Identifier: AGPL-3.0-or-later
// Transpose at the right end of the section tabs, on every page: the whole rig plays lower or
// higher without retuning (docs/dsp/transpose.md). A semitone per step, the latency it adds
// while it shifts, and the way back to 0.
import buttons from '../../controls/Button.module.css';
import { describeTranspose, transposeLatencyMs } from '../../lib/transpose';
import { useParam } from '../../params/useParam';
import styles from './TransposeControl.module.css';

export function TransposeControl() {
  const p = useParam('transpose');
  const st = Math.round(p.value);
  const set = (value: number) => {
    p.begin();
    p.set(value);
    p.end();
  };

  // The stepper sits at the right end, so what appears while it shifts (to its left) never
  // moves the buttons under the pointer.
  return (
    <div
      className={styles.transpose}
      role="group"
      aria-labelledby="transpose-label"
      data-on={st !== 0 || undefined}
    >
      {st !== 0 && (
        <>
          <span className={styles.latency}>Adds {transposeLatencyMs(st)} ms latency</span>
          <button
            type="button"
            className={`${buttons.link} ${styles.reset}`}
            onClick={() => set(0)}
          >
            Back to 0
          </button>
        </>
      )}
      <span id="transpose-label" className={styles.label}>
        Transpose
      </span>
      <span id="transpose-help" className="visually-hidden">
        Plays the whole rig lower or higher without retuning. Shifting down adds 3 ms of latency,
        shifting up 10 ms.
      </span>
      <div className={`${buttons.stepper} ${styles.stepper}`}>
        <button
          type="button"
          aria-label="Transpose down a semitone"
          aria-describedby="transpose-help"
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
          aria-describedby="transpose-help"
          onClick={() => set(st + 1)}
          disabled={st >= p.spec.max}
        >
          +
        </button>
      </div>
    </div>
  );
}
