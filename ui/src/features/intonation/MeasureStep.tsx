// SPDX-License-Identifier: AGPL-3.0-or-later
import { useMemo, useRef } from 'react';
import { useShallow } from 'zustand/react/shallow';
import buttons from '../../controls/Button.module.css';
import { Icon } from '../../controls/Icon';
import { centsBetween, midiLabel } from '../../lib/music';
import { pitch, useLiveFrame } from '../../state/live';
import { TunerGauge } from '../tuner/TunerGauge';
import { stringNumber } from './instruments';
import { adjustmentFor, formatCents, formatMm, IN_TUNE_CENTS, VERDICT_TEXT } from './math';
import { ProgressRing } from './ProgressRing';
import { SaddleDiagram } from './SaddleDiagram';
import {
  intonationActions,
  nextString,
  useIntonationStore,
  useScaleLength,
  useStringNotes,
  type StringStep,
} from './store';
import styles from './Workbench.module.css';

const STEPS: { id: StringStep; label: string }[] = [
  { id: 'tune', label: 'Tune open string' },
  { id: 'fret', label: 'Play 12th fret' },
  { id: 'result', label: 'Adjust saddle' },
];

function StepTrail({ step }: { step: StringStep }) {
  const index = STEPS.findIndex((s) => s.id === step);
  return (
    <ol className={styles.trail} aria-label="Steps for this string">
      {STEPS.map((s, i) => (
        <li
          key={s.id}
          data-state={i < index ? 'done' : i === index ? 'now' : 'next'}
          aria-current={i === index ? 'step' : undefined}
        >
          <span className={styles.trailDot}>
            {i < index ? <Icon name="check" size={10} /> : i + 1}
          </span>
          {s.label}
        </li>
      ))}
    </ol>
  );
}

function HoldBar() {
  const progress = useIntonationStore((s) => s.tuneProgress);
  return (
    <div
      className={styles.hold}
      role="progressbar"
      aria-label="Holding in tune"
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(progress * 100)}
    >
      <div className={styles.holdFill} style={{ transform: `scaleX(${progress})` }} />
    </div>
  );
}

function TuneStep({ midi }: { midi: number }) {
  const target = useMemo(() => ({ kind: 'target' as const, midi }), [midi]);
  return (
    <div className={styles.tune}>
      <p className={styles.instruction}>
        Play the open string and bring it to <strong>{midiLabel(midi)}</strong>. Hold it within one
        cent for a second and the workbench moves on by itself.
      </p>
      <TunerGauge target={target} compact />
      <HoldBar />
    </div>
  );
}

/** Live octave error while the fretted note rings, drawn without re-rendering. */
function LiveOctaveError({ openHz }: { openHz: number }) {
  const ref = useRef<HTMLSpanElement>(null);
  const clock = useRef(0);
  useLiveFrame(pitch, (e, dt) => {
    clock.current += dt;
    if (clock.current < 0.1 || !ref.current) return;
    clock.current = 0;
    ref.current.textContent =
      e?.valid && e.frequencyHz > openHz * 1.5
        ? formatCents(centsBetween(e.frequencyHz, openHz * 2))
        : '—';
  });
  return <span ref={ref}>—</span>;
}

function CaptureRing({ midi, openHz }: { midi: number; openHz: number }) {
  const progress = useIntonationStore((s) => s.captureProgress);
  return (
    <ProgressRing value={progress} label="Capturing the 12th-fret note" size={196}>
      <span className={styles.ringNote}>{midiLabel(midi + 12)}</span>
      <span className={styles.ringLive}>
        <LiveOctaveError openHz={openHz} />
      </span>
      <span className={styles.ringHint}>{progress > 0 ? 'Hold it…' : 'Listening'}</span>
    </ProgressRing>
  );
}

function FretStep({ midi, openHz }: { midi: number; openHz: number }) {
  return (
    <div className={styles.fret}>
      <CaptureRing midi={midi} openHz={openHz} />
      <div className={styles.fretText}>
        <p className={styles.instruction}>
          Fret the string at the <strong>12th fret</strong> and let it ring. Press as you normally
          would, right behind the fret, and pluck with the same strength as before.
        </p>
        <p className={styles.small}>
          The open string read {openHz.toFixed(2)} Hz. The ring fills once the reading is steady; a
          new pluck starts it again.
        </p>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small}`}
          onClick={() => intonationActions.remeasure()}
        >
          Retune the open string
        </button>
      </div>
    </div>
  );
}

function ResultStep({ index }: { index: number }) {
  const notes = useStringNotes();
  const L = useScaleLength();
  const results = useIntonationStore((s) => s.results);
  const toSummary = useIntonationStore((s) => nextString(s) === null);
  const result = results[index];
  if (!result) return null;
  const a = adjustmentFor(L, result.cents);
  const offsets = notes.map((_, i) => {
    const r = results[i];
    return r ? adjustmentFor(L, r.cents).offsetMm : null;
  });
  const labels = notes.map((n, i) => `${stringNumber(i, notes.length)} ${midiLabel(n)}`);

  return (
    <div className={styles.result} data-verdict={a.verdict}>
      <div className={styles.verdictCol}>
        <p className={styles.errorLabel}>Octave error at the 12th fret</p>
        <p className={styles.error} aria-label={`${formatCents(result.cents)} cents`}>
          <span className={styles.errorSign}>
            {result.cents >= 0.05 ? '+' : result.cents <= -0.05 ? '\u2212' : ''}
          </span>
          <span className={styles.errorValue}>{Math.abs(result.cents).toFixed(1)}</span>
          <span className={styles.errorUnit}>
            cents
            <br />
            {Math.abs(result.cents) < 0.05 ? 'exact' : result.cents > 0 ? 'sharp' : 'flat'}
          </span>
        </p>
        <p className={styles.verdict}>{VERDICT_TEXT[a.verdict]}</p>
        {a.verdict !== 'in-tune' ? (
          <p className={styles.amount}>
            About <strong>{formatMm(Math.abs(a.offsetMm)).replace('+', '')}</strong>
            {a.verdict === 'move-back' ? ' back' : ' forward'}, on a {L} mm scale.
          </p>
        ) : (
          <p className={styles.amount}>Within ±{IN_TUNE_CENTS} cents of a perfect octave.</p>
        )}
        <p className={styles.small}>
          {result.frettedHz.toFixed(2)} Hz against an open {result.openHz.toFixed(2)} Hz, reading
          steady {result.stdCents < 0.01 ? 'within' : 'to'} ±
          {Math.max(0.01, result.stdCents).toFixed(2)} cents.
        </p>
        <div className={styles.resultActions}>
          <button
            type="button"
            className={buttons.solid}
            onClick={() => intonationActions.remeasure()}
          >
            <Icon name="refresh" size={14} />
            Measure again
          </button>
          <button type="button" className={buttons.nacre} onClick={() => intonationActions.next()}>
            {toSummary ? 'See summary' : 'Next string'}
          </button>
        </div>
        {a.verdict !== 'in-tune' && (
          <p className={styles.tip}>
            Slacken the string before you turn the saddle screw, then retune and measure again.
            Small moves are easier to judge than one big one.
          </p>
        )}
      </div>
      <SaddleDiagram offsets={offsets} labels={labels} highlight={index} />
    </div>
  );
}

export function MeasureStep() {
  const { current, step, openHz, fromSummary } = useIntonationStore(
    useShallow((s) => ({
      current: s.current,
      step: s.step,
      openHz: s.openHz,
      fromSummary: s.fromSummary,
    })),
  );
  const notes = useStringNotes();
  const midi = notes[current];
  if (midi === undefined) return null;

  return (
    <div className={styles.measure}>
      <header className={styles.measureHead}>
        <h3 className={styles.stringTitle}>
          String {stringNumber(current, notes.length)}
          <span className={styles.stringNote}>{midiLabel(midi)}</span>
        </h3>
        <StepTrail step={step} />
        {fromSummary && (
          <button
            type="button"
            className={`${buttons.ghost} ${buttons.small}`}
            onClick={() => intonationActions.showSummary()}
          >
            Back to summary
          </button>
        )}
      </header>
      {step === 'tune' && <TuneStep midi={midi} />}
      {step === 'fret' && openHz !== null && <FretStep midi={midi} openHz={openHz} />}
      {step === 'result' && <ResultStep index={current} />}
    </div>
  );
}
