// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect, useMemo, useRef, useState } from 'react';
import { useShallow } from 'zustand/react/shallow';
import buttons from '../../controls/Button.module.css';
import { Icon } from '../../controls/Icon';
import { centsBetween, midiLabel } from '../../lib/music';
import { pitch, useLiveFrame, useLiveSelector } from '../../state/live';
import { TunerGauge } from '../tuner/TunerGauge';
import { stringNumber } from './instruments';
import {
  adjustmentFor,
  formatCents,
  formatMm,
  IN_TUNE_CENTS,
  isOctaveOf,
  VERDICT_TEXT,
  verdictFor,
} from './math';
import { TUNE_TOLERANCE_CENTS } from './controller';
import { ProgressRing } from './ProgressRing';
import { SaddleDiagram } from './SaddleDiagram';
import {
  intonationActions,
  nextString,
  stringNotes,
  useIntonationStore,
  useScaleLength,
  useStringNotes,
  type PluckStatus,
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

function HoldBar({ progress }: { progress: number }) {
  return (
    <div
      className={styles.hold}
      role="progressbar"
      aria-label="Measuring the open string"
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(progress * 100)}
    >
      <div className={styles.holdFill} style={{ transform: `scaleX(${progress})` }} />
    </div>
  );
}

/** "7.2 cents flat, tune up a little" */
function offBy(cents: number): string {
  const amount = `${Math.abs(cents).toFixed(1)} cents ${cents < 0 ? 'flat' : 'sharp'}`;
  const way = cents < 0 ? 'up' : 'down';
  return `${amount}, tune ${way}${Math.abs(cents) <= 15 ? ' a little' : ''}`;
}

/** While nothing is being measured: what the tuner hears, as guidance towards the target. */
function ListeningText({ midi, missed }: { midi: number; missed: boolean }) {
  const heard = useLiveSelector(pitch, (e) => (e?.valid ? e.midiNote : null));
  if (heard === null) return <>Pluck the open string and let it ring.</>;
  if (heard === midi) return <>{missed ? 'Pluck it again to measure.' : 'Listening…'}</>;
  // A note or two away is this string out of tune; further is probably another string.
  if (Math.abs(heard - midi) > 2) {
    return (
      <>
        That&apos;s {midiLabel(heard)}, not {midiLabel(midi)}.
      </>
    );
  }
  return (
    <>
      That&apos;s {midiLabel(heard)}. Tune {heard < midi ? 'up' : 'down'} to {midiLabel(midi)}.
    </>
  );
}

function TuneProgress({ midi }: { midi: number }) {
  const { pluck, progress, close, offer } = useIntonationStore(
    useShallow((s) => ({
      pluck: s.pluck,
      progress: s.tuneProgress,
      close: s.tuneClose,
      offer: s.tuneOffer,
    })),
  );
  // The Announcer speaks for this; the guidance below follows every pluck and stays quiet.
  const measuring = pluck === 'measuring';
  const announced = offer
    ? `${offBy(offer.cents)}.`
    : measuring
      ? close
        ? 'Good enough, measuring…'
        : 'Measuring…'
      : '';
  const state = offer ? 'offer' : measuring ? (close ? 'close' : 'off') : pluck;
  return (
    <div className={styles.tuneStatus} data-state={state}>
      {!offer && <HoldBar progress={progress} />}
      <p className={styles.tuneText}>{announced}</p>
      {offer ? (
        <>
          <button
            type="button"
            className={buttons.solid}
            onClick={() => intonationActions.acceptTuning()}
          >
            Use this tuning
          </button>
          <p className={styles.small}>Or tune it and pluck again.</p>
        </>
      ) : (
        !measuring && (
          <p className={styles.tuneText}>
            <ListeningText midi={midi} missed={pluck === 'missed'} />
          </p>
        )
      )}
    </div>
  );
}

function TuneStep({ midi }: { midi: number }) {
  const target = useMemo(() => ({ kind: 'target' as const, midi }), [midi]);
  return (
    <div className={styles.tune}>
      <p className={styles.instruction}>
        Play the open string and tune it to <strong>{midiLabel(midi)}</strong>. Within{' '}
        {TUNE_TOLERANCE_CENTS} cents is plenty: the 12th fret is checked against what the string
        actually plays, so it doesn&apos;t have to be spot on.
      </p>
      <TunerGauge target={target} compact inTuneCents={TUNE_TOLERANCE_CENTS} />
      <TuneProgress midi={midi} />
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

const RING_HINT: Record<PluckStatus, string> = {
  listening: 'Listening',
  measuring: 'Hold it…',
  missed: 'Pluck again',
};

function CaptureRing({ midi, openHz }: { midi: number; openHz: number }) {
  const progress = useIntonationStore((s) => s.captureProgress);
  const pluck = useIntonationStore((s) => s.pluck);
  return (
    <ProgressRing value={progress} label="Capturing the 12th-fret note" size={196}>
      <span className={styles.ringNote}>{midiLabel(midi + 12)}</span>
      <span className={styles.ringLive}>
        <LiveOctaveError openHz={openHz} />
      </span>
      <span className={styles.ringHint}>{RING_HINT[pluck]}</span>
    </ProgressRing>
  );
}

/** When the tuner hears something other than the octave of the open string, says what. */
function FretGuidance({ midi, openHz }: { midi: number; openHz: number }) {
  const heard = useLiveSelector(pitch, (e) =>
    e?.valid && !isOctaveOf(e.frequencyHz, openHz) ? e.midiNote : null,
  );
  if (heard === null) return null;
  return (
    <p className={styles.fretHint}>
      That&apos;s {midiLabel(heard)}; play the 12th fret of the {midiLabel(midi)} string.
    </p>
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
        <FretGuidance midi={midi} openHz={openHz} />
        <p className={styles.small}>
          The open string read {openHz.toFixed(2)} Hz. The ring fills over a second while the note
          rings; if the reading wavers, pluck again.
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

type IntonationState = ReturnType<typeof useIntonationStore.getState>;

/** "String 6, play 12th fret." On the result, the error and what to do. */
function stepAnnouncement(s: IntonationState): string {
  const label = STEPS.find((x) => x.id === s.step)?.label ?? '';
  const head = `String ${stringNumber(s.current, stringNotes(s).length)}, ${label.toLowerCase()}.`;
  const r = s.step === 'result' ? s.results[s.current] : null;
  if (!r) return head;
  const amount = `${Math.abs(r.cents).toFixed(1)} cents ${r.cents < 0 ? 'flat' : 'sharp'}`;
  return `${head} ${amount}. ${VERDICT_TEXT[verdictFor(r.cents)]}.`;
}

/** What the current pluck is doing, as the step shows it; empty while just listening. */
function pluckAnnouncement(s: IntonationState): string {
  if (s.step === 'tune') {
    if (s.tuneOffer)
      return `${offBy(s.tuneOffer.cents)}. Use this tuning, or tune and pluck again.`;
    if (s.pluck === 'measuring') return s.tuneClose ? 'Good enough, measuring.' : 'Measuring.';
    if (s.pluck === 'missed') return 'Pluck it again to measure.';
  } else if (s.step === 'fret') {
    if (s.pluck === 'measuring') return 'Hold it, measuring.';
    if (s.pluck === 'missed') return 'Pluck again.';
  }
  return '';
}

/**
 * One polite live region for the whole measure phase, so a screen reader hears the workbench move
 * on by itself (the steps' own text comes and goes with them): the string and step when they
 * change, then what each pluck is doing.
 */
function Announcer() {
  const [message, setMessage] = useState('');
  useEffect(() => {
    let step = '';
    let pluck = '';
    const update = (s: IntonationState) => {
      const key = `${s.current}/${s.step}`;
      const now = pluckAnnouncement(s);
      if (key !== step) {
        step = key;
        pluck = now;
        setMessage([stepAnnouncement(s), now].filter(Boolean).join(' '));
      } else if (now !== pluck) {
        pluck = now;
        if (now) setMessage(now);
      }
    };
    update(useIntonationStore.getState());
    return useIntonationStore.subscribe(update);
  }, []);
  return (
    <p className="visually-hidden" role="status">
      {message}
    </p>
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
      <Announcer />
    </div>
  );
}
