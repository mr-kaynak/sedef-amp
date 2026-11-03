// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect } from 'react';
import { useShallow } from 'zustand/react/shallow';
import buttons from '../../controls/Button.module.css';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import { midiLabel } from '../../lib/music';
import { uiActions } from '../../state/uiStore';
import { startIntonationSession } from './controller';
import { Explainer } from './Explainer';
import { stringNumber } from './instruments';
import { adjustmentFor, formatCents } from './math';
import { MeasureStep } from './MeasureStep';
import { SetupStep } from './SetupStep';
import { useIntonationStore, useScaleLength, useStringNotes, type Phase } from './store';
import { SummaryStep } from './SummaryStep';
import styles from './Workbench.module.css';

const PHASES: { id: Phase; label: string }[] = [
  { id: 'setup', label: 'Instrument' },
  { id: 'measure', label: 'Measure' },
  { id: 'summary', label: 'Summary' },
];

function PhaseTrail({ phase }: { phase: Phase }) {
  const index = PHASES.findIndex((p) => p.id === phase);
  return (
    <ol className={styles.phases} aria-label="Workbench steps">
      {PHASES.map((p, i) => (
        <li
          key={p.id}
          data-state={i < index ? 'done' : i === index ? 'now' : 'next'}
          aria-current={i === index ? 'step' : undefined}
        >
          <span className={styles.phaseNum}>{i + 1}</span>
          {p.label}
        </li>
      ))}
    </ol>
  );
}

function StringRail() {
  const s = useIntonationStore(
    useShallow((st) => ({ phase: st.phase, current: st.current, results: st.results })),
  );
  const notes = useStringNotes();
  const L = useScaleLength();
  return (
    <ul className={styles.rail} aria-label="Strings">
      {notes.map((midi, i) => {
        const r = s.results[i];
        const verdict = r ? adjustmentFor(L, r.cents).verdict : null;
        const current = s.phase === 'measure' && s.current === i;
        return (
          <li key={i} data-current={current || undefined} data-verdict={verdict ?? undefined}>
            <span className={styles.railNum}>{stringNumber(i, notes.length)}</span>
            <span className={styles.railNote}>{midiLabel(midi)}</span>
            <span className={styles.railStatus}>
              {r ? formatCents(r.cents) : current ? 'now' : ''}
            </span>
          </li>
        );
      })}
    </ul>
  );
}

export function IntonationWorkbench() {
  const phase = useIntonationStore((s) => s.phase);

  // Pitch analysis runs only while strings are being measured.
  useEffect(() => (phase === 'measure' ? startIntonationSession() : undefined), [phase]);

  return (
    <section className={styles.workbench} aria-label="Intonation workbench">
      <header className={styles.head}>
        <h2 className={styles.title}>Intonation</h2>
        <PhaseTrail phase={phase} />
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small} ${buttons.lead}`}
          onClick={() => uiActions.openOverlay(null)}
        >
          <Icon name="close" size={14} stroke={ICON_STROKE_LABEL} />
          Close
        </button>
      </header>
      <div className={styles.body}>
        <aside className={styles.side}>
          <StringRail />
          <Explainer open={phase === 'setup'} />
        </aside>
        <div className={styles.main}>
          {phase === 'setup' && <SetupStep />}
          {phase === 'measure' && <MeasureStep />}
          {phase === 'summary' && <SummaryStep />}
        </div>
      </div>
    </section>
  );
}
