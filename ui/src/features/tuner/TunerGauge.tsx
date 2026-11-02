// SPDX-License-Identifier: AGPL-3.0-or-later
// Needle and strobe display. The engine's display fields are already calm (docs/dsp/tuner.md,
// "Display"); readout.ts decides on every pitch event what the gauge says (note, in tune, the
// numbers), and the needle and strobe follow in requestAnimationFrame through refs. React
// re-renders only when the note, the state or the hold changes. Screen readers hear a settled
// state ("E 2, in tune") once it has held for a moment, never more often than every 2 s.
import { useEffect, useRef, useState } from 'react';
import { noteOf, splitAccidental } from '../../lib/music';
import { getValue } from '../../params/paramStore';
import { pitch, useLiveFrame } from '../../state/live';
import { IN_TUNE_CENTS, spokenState, type GaugeTarget } from './cents';
import { NeedleSpring, needleOpacity } from './needle';
import { readoutInput, TunerReadout, type TuneState } from './readout';
import styles from './TunerGauge.module.css';

const SWEEP_DEG = 52;
/** A state is announced once it has held this long, and at most this often. */
const ANNOUNCE_SETTLE_SECONDS = 0.6;
const ANNOUNCE_GAP_SECONDS = 2;
const W = 560;
const H = 250;
const CX = W / 2;
const CY = 262;
const R = 228;

function tickPath(cents: number, inner: number, outer: number): string {
  const a = ((cents / 50) * SWEEP_DEG - 90) * (Math.PI / 180);
  const x0 = CX + Math.cos(a) * inner;
  const y0 = CY + Math.sin(a) * inner;
  const x1 = CX + Math.cos(a) * outer;
  const y1 = CY + Math.sin(a) * outer;
  return `M${x0.toFixed(1)} ${y0.toFixed(1)}L${x1.toFixed(1)} ${y1.toFixed(1)}`;
}

const TICKS = Array.from({ length: 21 }, (_, i) => (i - 10) * 5);
const CHROMATIC: GaugeTarget = { kind: 'chromatic' };

/** Arc of the in-tune zone, ±cents around the centre of the dial. */
function zonePath(cents: number): string {
  const point = (c: number) => {
    const a = ((c / 50) * SWEEP_DEG - 90) * (Math.PI / 180);
    return `${(CX + Math.cos(a) * R).toFixed(1)} ${(CY + Math.sin(a) * R).toFixed(1)}`;
  };
  return `M${point(-cents)} A ${R} ${R} 0 0 1 ${point(cents)}`;
}

/** Seconds on the clock the intonation workbench also times its holds with. */
const now = () => performance.now() / 1000;

/** What the tuner tells the player to do, beside the colour of the state. */
const HINT: Record<TuneState, string> = {
  idle: '',
  listening: '',
  'in-tune': 'In tune',
  sharp: 'Tune down',
  flat: 'Tune up',
};

interface View {
  note: number | null;
  tune: TuneState;
  held: boolean;
}

export function TunerGauge({
  target = CHROMATIC,
  compact = false,
  inTuneCents = IN_TUNE_CENTS,
}: {
  target?: GaugeTarget;
  compact?: boolean;
  /**
   * Readings within this many cents show as in tune: the band on the dial, the colour and what a
   * screen reader hears. The tuner keeps its precision default; the intonation workbench passes
   * the tolerance it accepts for the open string.
   */
  inTuneCents?: number;
}) {
  const needleRef = useRef<SVGGElement>(null);
  const centsRef = useRef<HTMLSpanElement>(null);
  const hzRef = useRef<HTMLSpanElement>(null);
  const strobeRef = useRef<HTMLDivElement>(null);
  const announceRef = useRef<HTMLSpanElement>(null);
  const machine = useRef<TunerReadout | null>(null);
  const motion = useRef({ spring: new NeedleSpring(), strobe: 0, opacity: 0.25, heldAt: 0 });
  const [view, setView] = useState<View>({ note: null, tune: 'idle', held: false });
  const targetMidi = target.kind === 'target' ? target.midi : null;

  // Every pitch event: decide what the gauge says.
  useEffect(() => {
    const gaugeTarget: GaugeTarget =
      targetMidi === null ? CHROMATIC : { kind: 'target', midi: targetMidi };
    const readout = new TunerReadout(inTuneCents);
    machine.current = readout;
    const speech = { heard: '', heardAt: 0, announced: '', announcedAt: -Infinity };
    const handle = () => {
      const t = now();
      const input = readoutInput(pitch.latest(), gaugeTarget, getValue('tunerReference'));
      const wasHeld = readout.current.held;
      const r = readout.update(input, t);
      if (r.held && !wasHeld) motion.current.heldAt = t;
      if (centsRef.current) centsRef.current.textContent = r.centsText;
      if (hzRef.current) hzRef.current.textContent = r.hzText;
      setView((v) =>
        v.note === r.note && v.tune === r.tune && v.held === r.held
          ? v
          : { note: r.note, tune: r.tune, held: r.held },
      );

      const heard =
        r.note === null || r.held || r.tune === 'idle' || r.tune === 'listening'
          ? ''
          : spokenState(r.note, r.tune);
      if (heard !== speech.heard) {
        speech.heard = heard;
        speech.heardAt = t;
      }
      if (
        heard &&
        heard !== speech.announced &&
        t - speech.heardAt >= ANNOUNCE_SETTLE_SECONDS &&
        t - speech.announcedAt >= ANNOUNCE_GAP_SECONDS &&
        announceRef.current
      ) {
        announceRef.current.textContent = heard;
        speech.announced = heard;
        speech.announcedAt = t;
      }
    };
    handle();
    return pitch.subscribe(handle);
  }, [targetMidi, inTuneCents]);

  // Every frame: move the needle and the strobe.
  useLiveFrame(pitch, (_e, dt) => {
    const m = motion.current;
    const r = machine.current?.current;
    const cents = r?.cents ?? null;
    const shown = cents !== null;
    const clamped = shown ? Math.max(-50, Math.min(50, cents)) : 0;
    const angle = (m.spring.step(clamped, dt) / 50) * SWEEP_DEG;
    const opacity = needleOpacity(shown, r?.held ? now() - m.heldAt : null);
    m.opacity += (opacity - m.opacity) * (1 - Math.exp(-dt * 10));
    needleRef.current?.setAttribute('transform', `rotate(${angle.toFixed(2)} ${CX} ${CY})`);
    if (needleRef.current) needleRef.current.style.opacity = m.opacity.toFixed(2);

    // The strobe drifts at a speed proportional to the error, stands still in tune and stops
    // when the note ends.
    const drift = shown && !r?.held ? m.spring.position : 0;
    m.strobe = (m.strobe + drift * dt * 6) % 24;
    strobeRef.current?.style.setProperty('--offset', `${m.strobe.toFixed(2)}px`);
  });

  const label = view.note === null ? null : noteOf(view.note);
  const [letter, accidental] = label ? splitAccidental(label.note) : ['', ''];

  return (
    <div
      className={styles.gauge}
      data-state={view.tune}
      data-held={view.held || undefined}
      data-compact={compact || undefined}
    >
      <svg
        className={styles.dial}
        viewBox={`0 0 ${W} ${H}`}
        width={W}
        height={H}
        aria-hidden="true"
      >
        <path className={styles.zone} d={zonePath(inTuneCents)} />
        {TICKS.map((c) => (
          <path
            key={c}
            className={c === 0 ? styles.tickZero : c % 25 === 0 ? styles.tickMajor : styles.tick}
            d={tickPath(c, c % 25 === 0 ? R - 22 : R - 12, R)}
          />
        ))}
        <text
          className={styles.scaleLabel}
          x={CX - R * Math.sin((SWEEP_DEG * Math.PI) / 180) - 8}
          y={CY - R * Math.cos((SWEEP_DEG * Math.PI) / 180) + 4}
          textAnchor="end"
        >
          −50
        </text>
        <text
          className={styles.scaleLabel}
          x={CX + R * Math.sin((SWEEP_DEG * Math.PI) / 180) + 8}
          y={CY - R * Math.cos((SWEEP_DEG * Math.PI) / 180) + 4}
        >
          +50
        </text>
        {/* Only the outer part of the needle is drawn so it never crosses the note name. */}
        <g ref={needleRef} className={styles.needle}>
          <line x1={CX} y1={CY - R + 66} x2={CX} y2={CY - R - 8} />
          <circle cx={CX} cy={CY - R + 66} r={3.5} />
        </g>
      </svg>
      <div className={styles.readout}>
        <div className={styles.note}>
          {label ? (
            <>
              <span className={styles.letter}>{letter}</span>
              {accidental && <span className={styles.accidental}>{accidental}</span>}
              <span className={styles.octave}>{label.octave}</span>
            </>
          ) : (
            <span className={styles.waiting}>Play a string</span>
          )}
        </div>
        <div className={styles.numbers}>
          <span ref={centsRef} className={styles.cents}>
            —
          </span>
          <span ref={hzRef} className={styles.hz} />
        </div>
        {!compact && <p className={styles.hint}>{HINT[view.tune]}</p>}
      </div>
      <div ref={strobeRef} className={styles.strobe} aria-hidden="true" />
      <span ref={announceRef} className="visually-hidden" role="status" />
    </div>
  );
}
