// SPDX-License-Identifier: AGPL-3.0-or-later
// Needle and strobe display. Everything that moves is drawn from the pitch channel in
// requestAnimationFrame through refs; React re-renders only when the note or the in-tune state
// changes. Screen readers get a settled reading ("E 2, in tune") rather than every pluck.
import { useRef } from 'react';
import { noteOf, splitAccidental } from '../../lib/music';
import { pitch, useLiveFrame, useLiveSelector } from '../../state/live';
import { centsFor, IN_TUNE_CENTS, spokenReading, type GaugeTarget } from './cents';
import { formatCents } from '../intonation/math';
import styles from './TunerGauge.module.css';

const SWEEP_DEG = 52;
/** A reading is announced once it has held this long, and at most this often. */
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
  const motion = useRef({ angle: 0, strobe: 0, textClock: 0, visible: 0 });
  const speech = useRef({ heard: '', heardFor: 0, announced: '', sinceAnnounced: Infinity });

  const note = useLiveSelector(pitch, (e) => {
    if (target.kind === 'target') return target.midi;
    return e?.valid ? e.midiNote : null;
  });
  const state = useLiveSelector(pitch, (e) => {
    const c = centsFor(e, target);
    if (c === null) return 'idle';
    if (Math.abs(c) <= inTuneCents) return 'in-tune';
    return c > 0 ? 'sharp' : 'flat';
  });

  useLiveFrame(pitch, (e, dt) => {
    const m = motion.current;
    const cents = centsFor(e, target);
    const clamped = cents === null ? 0 : Math.max(-50, Math.min(50, cents));
    const targetAngle = (clamped / 50) * SWEEP_DEG;
    // Critically damped approach: smooth but never overshooting, like a well-damped meter.
    m.angle += (targetAngle - m.angle) * (1 - Math.exp(-dt * 16));
    m.visible += ((cents === null ? 0.25 : 1) - m.visible) * (1 - Math.exp(-dt * 10));
    needleRef.current?.setAttribute('transform', `rotate(${m.angle.toFixed(2)} ${CX} ${CY})`);
    if (needleRef.current) needleRef.current.style.opacity = m.visible.toFixed(2);

    // The strobe drifts at a speed proportional to the error and stands still when in tune.
    m.strobe = (m.strobe + (cents ?? 0) * dt * 6) % 24;
    strobeRef.current?.style.setProperty('--offset', `${m.strobe.toFixed(2)}px`);

    m.textClock += dt;
    if (m.textClock > 0.08) {
      m.textClock = 0;
      if (centsRef.current)
        centsRef.current.textContent = cents === null ? '—' : formatCents(cents);
      if (hzRef.current)
        hzRef.current.textContent = e?.valid ? `${e.frequencyHz.toFixed(2)} Hz` : '';
    }

    const s = speech.current;
    const heard =
      cents === null || !e
        ? ''
        : spokenReading(target.kind === 'target' ? target.midi : e.midiNote, cents, inTuneCents);
    s.heardFor = heard === s.heard ? s.heardFor + dt : 0;
    s.heard = heard;
    s.sinceAnnounced += dt;
    if (
      heard &&
      heard !== s.announced &&
      s.heardFor >= ANNOUNCE_SETTLE_SECONDS &&
      s.sinceAnnounced >= ANNOUNCE_GAP_SECONDS &&
      announceRef.current
    ) {
      announceRef.current.textContent = heard;
      s.announced = heard;
      s.sinceAnnounced = 0;
    }
  });

  const label = note === null ? null : noteOf(note);
  const [letter, accidental] = label ? splitAccidental(label.note) : ['', ''];

  return (
    <div className={styles.gauge} data-state={state} data-compact={compact || undefined}>
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
      </div>
      <div ref={strobeRef} className={styles.strobe} aria-hidden="true" />
      <span ref={announceRef} className="visually-hidden" aria-live="polite" />
    </div>
  );
}
