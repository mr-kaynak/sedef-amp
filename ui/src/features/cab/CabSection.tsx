// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef } from 'react';
import type { KeyboardEvent } from 'react';
import { Toggle } from '../../controls/Toggle';
import { setParam, useParamSpec, useParamValue } from '../../params/useParam';
import { AMP_NAMES, effectiveCab } from '../../params/schema';
import { beginGesture, endGesture } from '../../params/sender';
import { MicStrip } from './MicStrip';
import { SpeakerStage } from './SpeakerStage';
import styles from './CabSection.module.css';

const SPEAKERS = [1, 2, 4];

/** A cabinet silhouette with its speakers; the miked one is highlighted. */
function CabGlyph({ speakers }: { speakers: number }) {
  const cols = speakers === 1 ? 1 : 2;
  const rows = speakers === 4 ? 2 : 1;
  const w = 64;
  const h = rows === 2 ? 58 : 34;
  const r = 11;
  return (
    <svg width={w} height={h} viewBox={`0 0 ${w} ${h}`} aria-hidden="true" className={styles.glyph}>
      <rect x={0.5} y={0.5} width={w - 1} height={h - 1} rx={4} className={styles.box} />
      {Array.from({ length: speakers }, (_, i) => {
        const cx = cols === 1 ? w / 2 : i % 2 === 0 ? w / 2 - 15 : w / 2 + 15;
        const cy = rows === 1 ? h / 2 : i < 2 ? h / 2 - 13 : h / 2 + 13;
        return (
          <circle
            key={i}
            cx={cx}
            cy={cy}
            r={r}
            className={i === 0 ? styles.miked : styles.speaker}
          />
        );
      })}
    </svg>
  );
}

function selectCab(index: number, linked: boolean, amp: number) {
  if (linked && index !== amp) {
    beginGesture('ampCabLink');
    setParam('ampCabLink', 0);
    endGesture('ampCabLink');
  }
  beginGesture('cabModel');
  setParam('cabModel', index);
  endGesture('cabModel');
}

export function CabSection() {
  const amp = Math.round(useParamValue('ampModel'));
  const linked = useParamValue('ampCabLink') >= 0.5;
  const cab = effectiveCab(amp, linked ? 1 : 0, Math.round(useParamValue('cabModel')));
  const names = useParamSpec('cabModel').choices;
  const refs = useRef<(HTMLButtonElement | null)[]>([]);

  const onKeyDown = (e: KeyboardEvent) => {
    const delta =
      e.key === 'ArrowDown' || e.key === 'ArrowRight'
        ? 1
        : e.key === 'ArrowUp' || e.key === 'ArrowLeft'
          ? -1
          : 0;
    if (!delta) return;
    e.preventDefault();
    const next = (cab + delta + names.length) % names.length;
    selectCab(next, linked, amp);
    refs.current[next]?.focus();
  };

  return (
    <div className={styles.section}>
      <aside className={styles.cabs}>
        <h2 className={styles.heading}>Cabinet</h2>
        <div role="radiogroup" aria-label="Cabinet" className={styles.list} onKeyDown={onKeyDown}>
          {names.map((name, i) => (
            <button
              key={name}
              ref={(el) => {
                refs.current[i] = el;
              }}
              type="button"
              role="radio"
              aria-checked={i === cab}
              tabIndex={i === cab ? 0 : -1}
              className={styles.cab}
              onClick={() => selectCab(i, linked, amp)}
            >
              <CabGlyph speakers={SPEAKERS[i] ?? 1} />
              <span className={styles.cabText}>
                <span className={styles.cabName}>{name}</span>
                {linked && i === cab && (
                  <span className={styles.cabNote}>Matched to {AMP_NAMES[amp]}</span>
                )}
              </span>
            </button>
          ))}
        </div>
        <div className={styles.link}>
          <Toggle param="ampCabLink" label="Cab follows amp" />
          <p className={styles.note}>
            {linked
              ? 'Changing the amp brings its matching cab. Pick another cab to break the link.'
              : 'The cab stays put when you change amps.'}
          </p>
        </div>
      </aside>
      <SpeakerStage cabLabel={names[cab] ?? ''} />
      <div className={styles.mics}>
        <MicStrip mic="A" cab={cab} />
        <MicStrip mic="B" cab={cab} />
      </div>
    </div>
  );
}
