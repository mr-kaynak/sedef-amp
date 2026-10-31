// SPDX-License-Identifier: AGPL-3.0-or-later
import { useState } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon } from '../../controls/Icon';
import { midiLabel } from '../../lib/music';
import { stringNumber } from './instruments';
import { adjustmentFor, formatCents, formatMm, VERDICT_TEXT } from './math';
import { buildReport, copyText, instrumentName, scaleLabel } from './report';
import { SaddleDiagram } from './SaddleDiagram';
import { intonationActions, scaleLengthMm, stringNotes, useIntonationStore } from './store';
import styles from './Workbench.module.css';

export function SummaryStep() {
  const s = useIntonationStore();
  const notes = stringNotes(s);
  const L = scaleLengthMm(s);
  const [copied, setCopied] = useState<'idle' | 'done' | 'failed'>('idle');
  const measured = s.results.filter(Boolean).length;
  const inTune = s.results.filter(
    (r) => r && adjustmentFor(L, r.cents).verdict === 'in-tune',
  ).length;

  const copy = async () => {
    const ok = await copyText(buildReport(s, s.results));
    setCopied(ok ? 'done' : 'failed');
  };

  return (
    <div className={styles.summary}>
      <div className={styles.summaryMain}>
        <p className={styles.summaryLead}>
          {measured === 0
            ? 'No strings measured yet.'
            : `${inTune} of ${notes.length} strings in tune.`}{' '}
          <span className={styles.small}>
            {instrumentName(s)}, scale {scaleLabel(s)}
          </span>
        </p>
        <table className={styles.table}>
          <thead>
            <tr>
              <th scope="col">String</th>
              <th scope="col">Error</th>
              <th scope="col">What to do</th>
              <th scope="col">Move</th>
              <th scope="col">
                <span className="visually-hidden">Re-measure</span>
              </th>
            </tr>
          </thead>
          <tbody>
            {notes.map((midi, i) => {
              const r = s.results[i];
              const a = r ? adjustmentFor(L, r.cents) : null;
              return (
                <tr key={i} data-verdict={a?.verdict ?? 'none'}>
                  <th scope="row">
                    {stringNumber(i, notes.length)}{' '}
                    <span className={styles.tableNote}>{midiLabel(midi)}</span>
                  </th>
                  <td className={styles.num}>{r ? formatCents(r.cents) : '—'}</td>
                  <td>{a ? VERDICT_TEXT[a.verdict] : 'Not measured'}</td>
                  <td className={styles.num}>
                    {a && a.verdict !== 'in-tune'
                      ? formatMm(Math.abs(a.offsetMm)).replace('+', '')
                      : ''}
                  </td>
                  <td>
                    <button
                      type="button"
                      className={`${buttons.ghost} ${buttons.small}`}
                      onClick={() => intonationActions.goToString(i)}
                    >
                      {r ? 'Re-measure' : 'Measure'}
                    </button>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
        <div className={styles.resultActions}>
          <button type="button" className={buttons.solid} onClick={() => void copy()}>
            <Icon name={copied === 'done' ? 'check' : 'copy'} size={14} />
            {copied === 'done' ? 'Copied' : 'Copy as text'}
          </button>
          <button type="button" className={buttons.ghost} onClick={() => intonationActions.start()}>
            Measure all again
          </button>
          <button
            type="button"
            className={buttons.ghost}
            onClick={() => intonationActions.backToSetup()}
          >
            Change instrument
          </button>
          {copied === 'failed' && (
            <span className={styles.small} role="status">
              The clipboard is not available here. Select the table and copy it instead.
            </span>
          )}
        </div>
      </div>
      <SaddleDiagram
        offsets={notes.map((_, i) => {
          const r = s.results[i];
          return r ? adjustmentFor(L, r.cents).offsetMm : null;
        })}
        labels={notes.map((n, i) => `${stringNumber(i, notes.length)} ${midiLabel(n)}`)}
        highlight={null}
      />
    </div>
  );
}
