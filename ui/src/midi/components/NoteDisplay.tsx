// SPDX-License-Identifier: AGPL-3.0-or-later
// The note Sedef MIDI is sending, large enough to read from the guitar. It re-renders only when
// the note changes; the tuning needle draws from telemetry every frame.
import { useRef } from 'react';
import { useLiveFrame, useLiveSelector } from '../../state/live';
import { noteName, spokenNote } from '../notes';
import { midiTelemetry } from '../store';
import styles from './NoteDisplay.module.css';

export function NoteDisplay({ channel, className }: { channel: number; className?: string }) {
  const note = useLiveSelector(midiTelemetry, (t) => t?.note ?? -1);
  const velocity = useLiveSelector(midiTelemetry, (t) => (t && t.note >= 0 ? t.velocity : 0));
  const last = useLiveSelector(midiTelemetry, (t) => t?.lastNote ?? -1);
  const shown = note >= 0 ? note : last;
  const sounding = note >= 0;

  const needle = useRef<HTMLDivElement>(null);
  const smoothed = useRef(0);
  useLiveFrame(midiTelemetry, (t, dt) => {
    const el = needle.current;
    if (!el) return;
    const cents = t && t.note >= 0 && t.pitch > 0 ? 100 * (t.pitch - t.note) : 0;
    // A short glide, so vibrato reads as motion rather than flicker.
    smoothed.current += (cents - smoothed.current) * Math.min(1, dt * 18);
    const x = Math.max(-50, Math.min(50, smoothed.current));
    el.style.transform = `translateX(${x}%)`;
    el.style.opacity = t && t.note >= 0 ? '1' : '0';
  });

  const { name, octave } = shown >= 0 ? noteName(shown) : { name: '–', octave: null };
  return (
    <section
      className={[styles.panel, className].filter(Boolean).join(' ')}
      aria-label="Note being sent"
      data-sounding={sounding || undefined}
    >
      <div className={styles.readout}>
        <span className={styles.note} aria-hidden="true">
          <span className={styles.name}>{name}</span>
          {octave !== null && <span className={styles.octave}>{octave}</span>}
        </span>
        <span className={styles.srOnly}>
          {sounding
            ? `${spokenNote(shown)}, velocity ${velocity}`
            : shown >= 0
              ? `No note. Last: ${spokenNote(shown)}`
              : 'No note yet'}
        </span>
      </div>
      <div className={styles.tune} aria-hidden="true">
        <div className={styles.scale}>
          <span className={styles.centre} />
          <div ref={needle} className={styles.needle} />
        </div>
        <div className={styles.scaleLabels}>
          <span>−50</span>
          <span>cents</span>
          <span>+50</span>
        </div>
      </div>
      <dl className={styles.facts}>
        <div>
          <dt>Velocity</dt>
          <dd>{sounding ? velocity : '–'}</dd>
        </div>
        <div>
          <dt>Channel</dt>
          <dd>{channel}</dd>
        </div>
        <div>
          <dt>MIDI note</dt>
          <dd>{shown >= 0 ? shown : '–'}</dd>
        </div>
      </dl>
    </section>
  );
}
