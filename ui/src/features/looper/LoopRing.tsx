// SPDX-License-Identifier: AGPL-3.0-or-later
// The ring that fills as the loop plays: the loop's position, or the current bar of a take. Drawn
// in requestAnimationFrame from telemetry, gliding between its 30 Hz frames; with reduced motion
// it moves in 24 steps a turn instead of gliding.
import { useRef } from 'react';
import { stepBeats, stepsFor } from '../../lib/metronome';
import { useParamValue } from '../../params/useParam';
import { telemetry, useLiveFrame } from '../../state/live';
import { IDLE, ringProgress } from './model';

function reducedMotion(): boolean {
  return (
    typeof window !== 'undefined' &&
    !!window.matchMedia?.('(prefers-reduced-motion: reduce)').matches
  );
}

export function LoopRing({
  size,
  stroke,
  rate,
  className,
}: {
  size: number;
  stroke: number;
  rate: number;
  className?: string;
}) {
  const progressRef = useRef<SVGCircleElement>(null);
  const bpm = useParamValue('tempo');
  const signature = Math.round(useParamValue('metronomeSignature'));
  const clickOn = useParamValue('metronomeEnabled') >= 0.5;
  const bar = clickOn
    ? ((rate * 60 * stepBeats(signature)) / Math.max(1, bpm)) * stepsFor(signature)
    : null;
  const frame = useRef({ version: -1, at: 0 });
  const r = (size - stroke) / 2;
  const circumference = 2 * Math.PI * r;

  useLiveFrame(telemetry, (e) => {
    const el = progressRef.current;
    if (!el) return;
    const l = e?.looper ?? IDLE;
    const now = typeof performance !== 'undefined' ? performance.now() : 0;
    const version = telemetry.getVersion();
    if (version !== frame.current.version) frame.current = { version, at: now };
    const reduced = reducedMotion();
    let { position, length } = l;
    if (!reduced) {
      // Where the audio is now: the frame's value plus the time since it came (at most 100 ms).
      const ahead = Math.min(0.1, Math.max(0, (now - frame.current.at) / 1000)) * rate;
      if ((l.state === 'playing' || l.state === 'overdubbing') && length > 0)
        position = (position + ahead) % length;
      else if (l.state === 'recording' || l.state === 'closing') length += ahead;
    }
    let p = ringProgress({ ...l, position, length }, bar);
    if (reduced) p = Math.floor(p * 24) / 24;
    el.style.strokeDashoffset = String(circumference * (1 - p));
    // A round cap would leave a dot at nothing.
    el.style.opacity = p > 0 ? '1' : '0';
  });

  return (
    <svg
      className={className}
      width={size}
      height={size}
      viewBox={`0 0 ${size} ${size}`}
      aria-hidden="true"
      focusable="false"
    >
      <circle
        data-part="track"
        cx={size / 2}
        cy={size / 2}
        r={r}
        fill="none"
        strokeWidth={stroke}
      />
      <circle
        ref={progressRef}
        data-part="progress"
        cx={size / 2}
        cy={size / 2}
        r={r}
        fill="none"
        strokeWidth={stroke}
        strokeLinecap="round"
        strokeDasharray={circumference}
        strokeDashoffset={circumference}
        transform={`rotate(-90 ${size / 2} ${size / 2})`}
        style={{ opacity: 0 }}
      />
    </svg>
  );
}
