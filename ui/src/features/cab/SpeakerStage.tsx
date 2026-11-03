// SPDX-License-Identifier: AGPL-3.0-or-later
// Front view of the miked speaker. A mic marker goes anywhere over the cone: drag it, or click the
// cone to bring the selected mic there. Scroll over it, or drag it with Alt (Option) held, to move
// it closer or further away; the dashed ring grows with distance. Turned off axis, the capsule
// leans towards the dust cap, so a stem shows the mic body leaning outwards.
import { useCallback, useEffect, useId, useRef, useState } from 'react';
import type { KeyboardEvent, PointerEvent } from 'react';
import { wheelPixels } from '../../controls/useValueControl';
import { getValue } from '../../params/paramStore';
import { beginGesture, endGesture } from '../../params/sender';
import { setParam, useParam, useParamSpec, useParamValue } from '../../params/useParam';
import type { ParamHandle } from '../../params/useParam';
import { C, clockLabel, CONE_R, markerPoint, placementAt, SIZE } from './placement';
import styles from './SpeakerStage.module.css';

const CAP_R = 34;
const DOT_R = 11;
const RAD = Math.PI / 180;
/** SVG units of vertical Alt-drag for the whole distance range. */
const DISTANCE_DRAG = 140;
/** Shift divides drag sensitivity by four. */
const FINE_DRAG = 0.25;
/** Arrow-key steps in cone radii, and in distance (0..1). */
const KEY_STEP = 0.05;
const KEY_FINE_STEP = 0.01;
const PLACEMENT_KEYS = new Set([
  'ArrowLeft',
  'ArrowRight',
  'ArrowUp',
  'ArrowDown',
  'PageUp',
  'PageDown',
  'Home',
  'End',
  'Delete',
  'Backspace',
]);
/** The wheel moves distance like a knob: 2 % of the range per 100 px notch, a fifth with shift. */
const WHEEL_PER_PX = 0.02 / 100;
const WHEEL_FINE = 0.2;
const WHEEL_IDLE_MS = 400;
/** The marker's stem at the largest off-axis angle, beyond the dot. */
const STEM_MAX = 30;
const MAX_OFF_AXIS = 60;

type MicId = 'A' | 'B';
const MICS: MicId[] = ['A', 'B'];

const clamp01 = (v: number) => Math.min(1, Math.max(0, v));

interface MicState {
  mic: MicId;
  position: ParamHandle;
  azimuth: ParamHandle;
  distance: ParamHandle;
  offAxis: ParamHandle;
  enabled: boolean;
  /** Plays a custom IR on this cab: the file holds its own position and distance. */
  ir: boolean;
}

function useMic(mic: MicId, cab: number): MicState {
  const typeId = `mic${mic}Cab${cab + 1}Type`;
  const type = Math.round(useParamValue(typeId));
  const choices = useParamSpec(typeId).choices;
  return {
    mic,
    position: useParam(`mic${mic}Position`),
    azimuth: useParam(`mic${mic}Azimuth`),
    distance: useParam(`mic${mic}Distance`),
    offAxis: useParam(`mic${mic}OffAxis`),
    enabled: useParamValue(`mic${mic}Enabled`) >= 0.5,
    ir: choices[type] === 'Custom IR',
  };
}

const ids = (mic: MicId) => ({
  position: `mic${mic}Position`,
  azimuth: `mic${mic}Azimuth`,
  distance: `mic${mic}Distance`,
  offAxis: `mic${mic}OffAxis`,
});

/** Moves a mic to the point under (x, y), clamped to the cone. Call inside a gesture. */
function moveTo(mic: MicId, x: number, y: number) {
  const id = ids(mic);
  const p = placementAt(x, y, getValue(id.azimuth));
  setParam(id.position, p.position);
  setParam(id.azimuth, p.azimuth);
}

/** One edit as one host gesture per parameter it touches. */
function edit(paramIds: string[], change: () => void) {
  paramIds.forEach(beginGesture);
  change();
  paramIds.forEach(endGesture);
}

interface Drag {
  mic: MicId;
  pointerId: number;
  mode: 'move' | 'distance';
  ids: string[];
  lastX: number;
  lastY: number;
  /** SVG units per screen pixel (the canvas is scaled). */
  scale: number;
  /** Where the marker would be without the cone's edge: following it keeps the grab point. */
  x: number;
  y: number;
  distance: number;
}

interface Wheel {
  mic: MicId;
  timer?: ReturnType<typeof setTimeout>;
}

/**
 * The other speakers, for the 2x12 and 4x12: a small baffle with the miked speaker outlined and
 * both mics on it, so moving a marker towards a neighbour reads as such.
 */
function BaffleMap({ cab, mics }: { cab: number; mics: MicState[] }) {
  if (cab === 0) return null;
  const r = 10;
  const pad = 5;
  // Centre spacing in cone radii (engine: 0.34 m for the 2x12, 0.33 x 0.32 m for the 4x12).
  const dx = r * (cab === 1 ? 2.83 : 2.75);
  const dy = cab === 2 ? r * 2.67 : 0;
  const w = dx + 2 * (r + pad);
  const h = dy + 2 * (r + pad);
  // The miked speaker: left of the 2x12, bottom-left of the 4x12.
  const mx = pad + r;
  const my = h - pad - r;
  const speakers =
    cab === 1
      ? [
          [mx, my],
          [mx + dx, my],
        ]
      : [
          [mx, my],
          [mx + dx, my],
          [mx, my - dy],
          [mx + dx, my - dy],
        ];
  return (
    <g className={styles.map} transform={`translate(${SIZE - w} 0)`} aria-hidden="true">
      <rect className={styles.mapBox} x={0.5} y={0.5} width={w - 1} height={h - 1} rx={4} />
      {speakers.map(([x, y], i) => (
        <circle
          key={i}
          className={i === 0 ? styles.mapMiked : styles.mapSpeaker}
          cx={x}
          cy={y}
          r={r}
        />
      ))}
      {mics.map((m) => {
        const a = m.azimuth.value * RAD;
        return (
          <circle
            key={m.mic}
            className={styles.mapMic}
            data-mic={m.mic}
            data-off={!m.enabled || m.ir || undefined}
            cx={mx + m.position.value * r * Math.sin(a)}
            cy={my - m.position.value * r * Math.cos(a)}
            r={2.6}
          />
        );
      })}
    </g>
  );
}

export function SpeakerStage({ cab, cabLabel }: { cab: number; cabLabel: string }) {
  const a = useMic('A', cab);
  const b = useMic('B', cab);
  const byId = { A: a, B: b };
  const [selected, setSelected] = useState<MicId>('A');
  const [dragging, setDragging] = useState<MicId | null>(null);
  const svgRef = useRef<SVGSVGElement | null>(null);
  const markerRefs = useRef<Record<MicId, SVGGElement | null>>({ A: null, B: null });
  const drag = useRef<Drag | null>(null);
  const wheel = useRef<Wheel | null>(null);
  const hintId = useId();

  // Read by the native wheel listener, which outlives renders.
  const live = useRef({ selected, irA: a.ir, irB: b.ir });
  useEffect(() => {
    live.current = { selected, irA: a.ir, irB: b.ir };
  });

  const endDrag = useCallback(() => {
    const d = drag.current;
    if (!d) return;
    drag.current = null;
    d.ids.forEach(endGesture);
    setDragging(null);
  }, []);

  const endWheel = useCallback(() => {
    const w = wheel.current;
    if (!w) return;
    clearTimeout(w.timer);
    wheel.current = null;
    endGesture(ids(w.mic).distance);
  }, []);

  // A section switch can unmount the stage mid-edit: close the host gestures it opened.
  useEffect(
    () => () => {
      endDrag();
      endWheel();
    },
    [endDrag, endWheel],
  );

  // The wheel changes the distance of the mic under the pointer, or over the cone of the selected
  // one (the other one if the selected mic plays an IR, as a click on the cone does). Non-passive,
  // so nothing scrolls underneath. A drag owns its mic: the wheel waits for it.
  useEffect(() => {
    const svg = svgRef.current;
    if (!svg) return;
    const onWheel = (e: WheelEvent) => {
      const target = e.target instanceof Element ? e.target.closest('[data-wheel]') : null;
      if (!target) return;
      const px = wheelPixels(e);
      if (px === 0) return;
      const isIr = (m: MicId) => (m === 'A' ? live.current.irA : live.current.irB);
      const selected = live.current.selected;
      const other: MicId = selected === 'A' ? 'B' : 'A';
      const mic =
        (target.getAttribute('data-mic') as MicId | null) ?? (isIr(selected) ? other : selected);
      if (isIr(mic)) return;
      e.preventDefault();
      if (drag.current) return;
      if (wheel.current && wheel.current.mic !== mic) endWheel();
      if (!wheel.current) {
        wheel.current = { mic };
        beginGesture(ids(mic).distance);
      }
      clearTimeout(wheel.current.timer);
      const id = ids(mic).distance;
      setParam(id, clamp01(getValue(id) + px * WHEEL_PER_PX * (e.shiftKey ? WHEEL_FINE : 1)));
      wheel.current.timer = setTimeout(endWheel, WHEEL_IDLE_MS);
    };
    svg.addEventListener('wheel', onWheel, { passive: false });
    return () => svg.removeEventListener('wheel', onWheel);
  }, [endWheel]);

  const startDrag = (
    m: MicState,
    e: PointerEvent<Element>,
    mode: Drag['mode'],
    from: { x: number; y: number },
  ) => {
    const svg = svgRef.current;
    const rect = svg?.getBoundingClientRect();
    const id = ids(m.mic);
    const paramIds = mode === 'move' ? [id.position, id.azimuth] : [id.distance];
    drag.current = {
      mic: m.mic,
      pointerId: e.pointerId,
      mode,
      ids: paramIds,
      lastX: e.clientX,
      lastY: e.clientY,
      scale: rect && rect.width ? SIZE / rect.width : 1,
      x: from.x,
      y: from.y,
      distance: m.distance.value,
    };
    // The drag takes over from a wheel turn still open: begin first, so a shared parameter's host
    // gesture runs on without a gap (gestures are counted per parameter).
    paramIds.forEach(beginGesture);
    endWheel();
    svg?.setPointerCapture?.(e.pointerId);
    setSelected(m.mic);
    setDragging(m.mic);
  };

  const onMarkerDown = (m: MicState) => (e: PointerEvent<SVGGElement>) => {
    if (e.button !== 0 || drag.current || m.ir) return;
    e.preventDefault();
    e.stopPropagation();
    e.currentTarget.focus();
    startDrag(m, e, e.altKey ? 'distance' : 'move', markerPoint(m.position.value, m.azimuth.value));
  };

  // Clicking the cone brings the selected mic there (and keeps dragging it).
  const onConeDown = (e: PointerEvent<SVGCircleElement>) => {
    if (e.button !== 0 || drag.current) return;
    const m = byId[selected].ir ? byId[selected === 'A' ? 'B' : 'A'] : byId[selected];
    if (m.ir) return;
    const rect = svgRef.current?.getBoundingClientRect();
    if (!rect || !rect.width) return;
    e.preventDefault();
    const scale = SIZE / rect.width;
    const at = { x: (e.clientX - rect.left) * scale, y: (e.clientY - rect.top) * scale };
    startDrag(m, e, 'move', at);
    moveTo(m.mic, at.x, at.y);
    markerRefs.current[m.mic]?.focus();
  };

  const onPointerMove = (e: PointerEvent<SVGSVGElement>) => {
    const d = drag.current;
    if (!d || d.pointerId !== e.pointerId) return;
    const fine = e.shiftKey ? FINE_DRAG : 1;
    const dx = (e.clientX - d.lastX) * d.scale * fine;
    const dy = (e.clientY - d.lastY) * d.scale * fine;
    d.lastX = e.clientX;
    d.lastY = e.clientY;
    // Accumulate locally: several pointer events can arrive before React re-renders.
    if (d.mode === 'move') {
      d.x += dx;
      d.y += dy;
      moveTo(d.mic, d.x, d.y);
    } else {
      d.distance = clamp01(d.distance - dy / DISTANCE_DRAG);
      setParam(ids(d.mic).distance, d.distance);
    }
  };

  const onPointerUp = (e: PointerEvent<SVGSVGElement>) => {
    if (drag.current?.pointerId !== e.pointerId) return;
    endDrag();
    e.currentTarget.releasePointerCapture?.(e.pointerId);
  };

  const resetPlacement = (m: MicState) => {
    m.position.reset();
    m.azimuth.reset();
    m.distance.reset();
    m.offAxis.reset();
  };

  const onKeyDown = (m: MicState) => (e: KeyboardEvent<SVGGElement>) => {
    if (!PLACEMENT_KEYS.has(e.key)) return;
    e.preventDefault();
    e.stopPropagation();
    // A drag owns its mic until it ends.
    if (drag.current) return;
    const id = ids(m.mic);
    const step = e.shiftKey ? KEY_FINE_STEP : KEY_STEP;
    const nudge = (sx: number, sy: number) =>
      edit([id.position, id.azimuth], () => {
        const p = markerPoint(getValue(id.position), getValue(id.azimuth));
        moveTo(m.mic, p.x + sx * step * CONE_R, p.y + sy * step * CONE_R);
      });
    switch (e.key) {
      case 'ArrowLeft':
        nudge(-1, 0);
        break;
      case 'ArrowRight':
        nudge(1, 0);
        break;
      case 'ArrowUp':
        nudge(0, -1);
        break;
      case 'ArrowDown':
        nudge(0, 1);
        break;
      case 'PageUp':
      case 'PageDown': {
        const sign = e.key === 'PageUp' ? 1 : -1;
        edit([id.distance], () =>
          setParam(id.distance, clamp01(getValue(id.distance) + sign * step)),
        );
        break;
      }
      case 'Home':
        edit([id.position], () => setParam(id.position, 0));
        break;
      case 'End':
        edit([id.position], () => setParam(id.position, 1));
        break;
      case 'Delete':
      case 'Backspace':
        resetPlacement(m);
        break;
    }
  };

  // The selected (or dragged) mic draws on top.
  const order = MICS.filter((m) => m !== selected).concat(selected);
  const irMics = MICS.filter((m) => byId[m].ir);
  const modifier =
    typeof navigator !== 'undefined' && /Mac|iPhone|iPad/.test(navigator.platform)
      ? 'Option'
      : 'Alt';

  const bolts = Array.from({ length: 8 }, (_, i) => {
    const angle = (i / 8) * Math.PI * 2;
    return (
      <circle
        key={i}
        className={styles.bolt}
        cx={C + Math.cos(angle) * 186}
        cy={C + Math.sin(angle) * 186}
        r={4}
      />
    );
  });

  return (
    <div className={styles.stage}>
      <svg
        ref={svgRef}
        className={styles.svg}
        viewBox={`0 0 ${SIZE} ${SIZE}`}
        width={SIZE}
        height={SIZE}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onPointerCancel={onPointerUp}
        onLostPointerCapture={(e) => {
          if (drag.current?.pointerId === e.pointerId) endDrag();
        }}
      >
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
        {/* Hit surface over the cone and surround: click to place the selected mic. */}
        <circle
          className={styles.pad}
          data-wheel=""
          data-dragging={dragging || undefined}
          cx={C}
          cy={C}
          r={CONE_R + 8}
          onPointerDown={onConeDown}
        />
        <BaffleMap cab={cab} mics={[a, b]} />
        {order.map((id) => {
          const m = byId[id];
          const { x, y } = markerPoint(m.position.value, m.azimuth.value);
          const ring = 16 + m.distance.value * 34;
          const angle = m.offAxis.value;
          const az = m.azimuth.value * RAD;
          const ux = Math.sin(az);
          const uy = -Math.cos(az);
          const stem = DOT_R + (STEM_MAX * Math.sin(angle * RAD)) / Math.sin(MAX_OFF_AXIS * RAD);
          const turned = Math.round(angle) > 0;
          const placement = `${clockLabel(m.azimuth.value)}, position ${m.position.display}, distance ${m.distance.display}, ${turned ? `${m.offAxis.display} off axis` : 'on axis'}`;
          return (
            <g
              key={id}
              ref={(el) => {
                markerRefs.current[id] = el;
              }}
              className={styles.marker}
              data-mic={id}
              data-wheel={m.ir ? undefined : ''}
              data-off={!m.enabled || undefined}
              data-ir={m.ir || undefined}
              data-selected={(selected === id && !m.ir) || undefined}
              data-dragging={dragging === id || undefined}
              tabIndex={m.ir ? -1 : 0}
              role="slider"
              aria-roledescription="mic placement"
              aria-label={`Mic ${id} placement`}
              aria-describedby={hintId}
              aria-disabled={m.ir || undefined}
              aria-valuemin={0}
              aria-valuemax={1}
              aria-valuenow={Number(m.position.value.toFixed(2))}
              aria-valuetext={m.ir ? 'Set by the impulse response' : placement}
              onPointerDown={onMarkerDown(m)}
              onKeyDown={m.ir ? undefined : onKeyDown(m)}
              onFocus={() => {
                if (!m.ir) setSelected(id);
              }}
              onDoubleClick={() => {
                if (!m.ir && !drag.current) resetPlacement(m);
              }}
            >
              {turned && (
                <line
                  className={styles.stem}
                  x1={x + ux * DOT_R}
                  y1={y + uy * DOT_R}
                  x2={x + ux * stem}
                  y2={y + uy * stem}
                />
              )}
              {!m.ir && <circle className={styles.ring} cx={x} cy={y} r={ring} />}
              <circle className={styles.hit} cx={x} cy={y} r={Math.max(18, ring)} />
              <circle className={styles.dot} cx={x} cy={y} r={DOT_R} />
              <text className={styles.letter} x={x} y={y + 4}>
                {id}
              </text>
              {turned && (
                <text
                  className={styles.angle}
                  x={x + ux * (stem + 11)}
                  y={y + uy * (stem + 11) + 3.5}
                >
                  {m.offAxis.display}
                </text>
              )}
            </g>
          );
        })}
      </svg>
      <p className={styles.caption}>
        {cabLabel}. Drag a mic anywhere over the cone, or click the cone to bring the selected one
        there. Scroll over a mic, or {modifier}-drag it, to move it closer or further away.
        {cab > 0 && ' Further back, it hears the other speakers too.'}
        {irMics.length === 2
          ? ' Both mics play impulse responses, which hold their own placement.'
          : irMics.length === 1
            ? ` Mic ${irMics[0]} plays an impulse response, which holds its own placement.`
            : null}
      </p>
      <p id={hintId} className="visually-hidden">
        Arrow keys move the mic over the cone, with Shift for small steps. Page Up and Page Down
        move it away from or towards the grille. Home and End take it to the dust cap or the cone
        edge. Delete resets it.
      </p>
    </div>
  );
}
