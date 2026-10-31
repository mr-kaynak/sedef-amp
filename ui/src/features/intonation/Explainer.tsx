// SPDX-License-Identifier: AGPL-3.0-or-later
import styles from './Workbench.module.css';

export function Explainer({ open = false }: { open?: boolean }) {
  return (
    <details className={styles.explainer} open={open}>
      <summary>Why this works</summary>
      <p>
        The 12th fret sits halfway along the string, so the note there should be exactly one octave
        above the open string. If it comes out sharp, the fretted half is too short: move the saddle
        back, away from the neck. If it is flat, move the saddle forward.
      </p>
      <p>
        How far follows from the scale length L and the error c in cents: Δs = L × (2
        <sup>c/1200</sup> − 1). Pressing a string stretches it a little, so treat the number as a
        first move, then retune and measure again.
      </p>
    </details>
  );
}
