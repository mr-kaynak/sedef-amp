// SPDX-License-Identifier: AGPL-3.0-or-later
// Front view of the miked speaker. Each mic marker sits on its own guide line: drag sideways to
// move it from the dust cap (centre) to the cone edge, up and down to change its distance (the
// ring grows as the mic moves away from the grille).
import { useRef, useState } from 'react';
import type { KeyboardEvent, PointerEvent } from 'react';
import { useParam } from '../../params/useParam';
import styles from './SpeakerStage.module.css';

const SIZE = 400;
const C = SIZE / 2;
const CONE_R = 168;
const CAP_R = 34;
const LANE_OFFSET = 52;

function MicMarker({ mic, lane }: { mic: 'A' | 'B'; lane: number }) {
  const position = useParam(`mic${mic}Position`);
  const distance = useParam(`mic${mic}Distance`);
  const enabled = useParam(`mic${mic}Enabled`).value >= 0.5;
  const drag = useRef<{ x: number; y: number; scale: number; pos: number; dist: number } | null>(
    null,
  );
  const [dragging, setDragging] = useState(false);

  const x = C + position.value * CONE_R;
  const y = C + lane;
  const ring = 16 + distance.value * 34;

  const onPointerDown = (e: PointerEvent<SVGGElement>) => {
    if (e.button !== 0) return;
    e.preventDefault();
    (e.currentTarget as SVGGElement).focus();
    e.currentTarget.setPointerCapture?.(e.pointerId);
    // Convert screen pixels to SVG units (the canvas may be scaled).
    const svg = e.currentTarget.ownerSVGElement;
    const rect = svg?.getBoundingClientRect();
    const scale = rect && rect.width ? SIZE / rect.width : 1;
    drag.current = { x: e.clientX, y: e.clientY, scale, pos: position.value, dist: distance.value };
    position.begin();
    distance.begin();
    setDragging(true);
  };

  const onPointerMove = (e: PointerEvent<SVGGElement>) => {
    const d = drag.current;
    if (!d) return;
    const dx = (e.clientX - d.x) * d.scale;
    const dy = (e.clientY - d.y) * d.scale;
    d.x = e.clientX;
    d.y = e.clientY;
    const fine = e.shiftKey ? 0.25 : 1;
    // Accumulate locally: several pointer events can arrive before React re-renders.
    d.pos = Math.min(1, Math.max(0, d.pos + (dx / CONE_R) * fine));
    d.dist = Math.min(1, Math.max(0, d.dist - (dy / 140) * fine));
    position.set(d.pos);
    distance.set(d.dist);
  };

  const onPointerUp = (e: PointerEvent<SVGGElement>) => {
    if (!drag.current) return;
    drag.current = null;
    e.currentTarget.releasePointerCapture?.(e.pointerId);
    position.end();
    distance.end();
    setDragging(false);
  };

  const onKeyDown = (e: KeyboardEvent<SVGGElement>) => {
    const step = e.shiftKey ? 0.01 : 0.05;
    const target =
      e.key === 'ArrowLeft' || e.key === 'ArrowRight'
        ? position
        : e.key === 'ArrowUp' || e.key === 'ArrowDown'
          ? distance
          : null;
    if (!target) return;
    e.preventDefault();
    const sign = e.key === 'ArrowRight' || e.key === 'ArrowUp' ? 1 : -1;
    target.begin();
    target.set(target.value + sign * step);
    target.end();
  };

  return (
    <g
      className={styles.marker}
      data-mic={mic}
      data-off={!enabled || undefined}
      data-dragging={dragging || undefined}
      tabIndex={0}
      role="slider"
      aria-roledescription="mic placement"
      aria-label={`Mic ${mic} placement. Left and right move across the cone, up and down change distance.`}
      aria-valuemin={0}
      aria-valuemax={1}
      aria-valuenow={Number(position.value.toFixed(2))}
      aria-valuetext={`Position ${position.display}, distance ${distance.display}`}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerUp}
      onKeyDown={onKeyDown}
      onDoubleClick={() => {
        position.reset();
        distance.reset();
      }}
    >
      <line className={styles.lane} x1={C} x2={C + CONE_R} y1={y} y2={y} />
      <circle className={styles.ring} cx={x} cy={y} r={ring} />
      <circle className={styles.hit} cx={x} cy={y} r={Math.max(18, ring)} />
      <circle className={styles.dot} cx={x} cy={y} r={11} />
      <text className={styles.letter} x={x} y={y + 4}>
        {mic}
      </text>
    </g>
  );
}

export function SpeakerStage({ cabLabel }: { cabLabel: string }) {
  const bolts = Array.from({ length: 8 }, (_, i) => {
    const a = (i / 8) * Math.PI * 2;
    return (
      <circle
        key={i}
        className={styles.bolt}
        cx={C + Math.cos(a) * 186}
        cy={C + Math.sin(a) * 186}
        r={4}
      />
    );
  });
  return (
    <div className={styles.stage}>
      <svg className={styles.svg} viewBox={`0 0 ${SIZE} ${SIZE}`} width={SIZE} height={SIZE}>
        <defs>
          <radialGradient id="cone" cx="0.5" cy="0.5" r="0.5">
            <stop offset="0.18" stopColor="#1b2a33" />
            <stop offset="0.7" stopColor="#101b22" />
            <stop offset="0.93" stopColor="#0c151b" />
            <stop offset="1" stopColor="#1f2e37" />
          </radialGradient>
          <radialGradient id="cap" cx="0.42" cy="0.38" r="0.7">
            <stop offset="0" stopColor="#3a4c57" />
            <stop offset="1" stopColor="#141f26" />
          </radialGradient>
        </defs>
        <circle className={styles.frame} cx={C} cy={C} r={196} />
        {bolts}
        <circle className={styles.surround} cx={C} cy={C} r={CONE_R + 8} />
        <circle cx={C} cy={C} r={CONE_R} fill="url(#cone)" />
        {[0.35, 0.55, 0.75].map((f) => (
          <circle key={f} className={styles.rib} cx={C} cy={C} r={CONE_R * f + 20} />
        ))}
        <circle cx={C} cy={C} r={CAP_R} fill="url(#cap)" className={styles.cap} />
        <text className={styles.scaleText} x={C} y={C - LANE_OFFSET - 24}>
          centre
        </text>
        <text className={styles.scaleText} x={C + CONE_R - 6} y={C - LANE_OFFSET - 24}>
          edge
        </text>
        <MicMarker mic="A" lane={-LANE_OFFSET} />
        <MicMarker mic="B" lane={LANE_OFFSET} />
      </svg>
      <p className={styles.caption}>
        {cabLabel}. Drag a mic sideways to move it across the cone, up or down to change its
        distance.
      </p>
    </div>
  );
}
