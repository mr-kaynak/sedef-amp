// SPDX-License-Identifier: AGPL-3.0-or-later
import buttons from '../../controls/Button.module.css';
import { midiLabel } from '../../lib/music';
import {
  CUSTOM_INSTRUMENT_ID,
  CUSTOM_NOTE_RANGE,
  CUSTOM_SCALE_ID,
  INSTRUMENTS,
  MAX_CUSTOM_SCALE_MM,
  MIN_CUSTOM_SCALE_MM,
  SCALE_LENGTHS,
  stringNumber,
  tuningLabel,
} from './instruments';
import { intonationActions, stringNotes, useIntonationStore } from './store';
import styles from './Workbench.module.css';

const NOTE_OPTIONS = Array.from(
  { length: CUSTOM_NOTE_RANGE.max - CUSTOM_NOTE_RANGE.min + 1 },
  (_, i) => CUSTOM_NOTE_RANGE.min + i,
);

function CustomTuning() {
  const notes = useIntonationStore((s) => s.customNotes);
  return (
    <div className={styles.custom}>
      <div className={styles.customHead}>
        <span>Strings</span>
        <div className={styles.countStepper}>
          <button
            type="button"
            aria-label="Remove a string"
            disabled={notes.length <= 4}
            onClick={() => intonationActions.setCustomStringCount(notes.length - 1)}
          >
            −
          </button>
          <output>{notes.length}</output>
          <button
            type="button"
            aria-label="Add a string"
            disabled={notes.length >= 8}
            onClick={() => intonationActions.setCustomStringCount(notes.length + 1)}
          >
            +
          </button>
        </div>
      </div>
      <div className={styles.noteRow}>
        {notes.map((midi, i) => (
          <label key={i} className={styles.notePick}>
            <span>{stringNumber(i, notes.length)}</span>
            <select
              aria-label={`String ${stringNumber(i, notes.length)} open note`}
              value={midi}
              onChange={(e) => intonationActions.setCustomNote(i, Number(e.target.value))}
            >
              {NOTE_OPTIONS.map((n) => (
                <option key={n} value={n}>
                  {midiLabel(n)}
                </option>
              ))}
            </select>
          </label>
        ))}
      </div>
      <p className={styles.small}>Lowest string first. String 1 is the thinnest.</p>
    </div>
  );
}

export function SetupStep() {
  const s = useIntonationStore();
  const notes = stringNotes(s);

  return (
    <div className={styles.setup}>
      <section className={styles.block}>
        <h3 className={styles.blockTitle}>Instrument and tuning</h3>
        <div className={styles.cards} role="radiogroup" aria-label="Instrument">
          {[
            ...INSTRUMENTS,
            { id: CUSTOM_INSTRUMENT_ID, name: 'Custom tuning', notes: s.customNotes },
          ].map((inst) => (
            <button
              key={inst.id}
              type="button"
              role="radio"
              aria-checked={s.instrumentId === inst.id}
              className={styles.card}
              onClick={() => intonationActions.chooseInstrument(inst.id)}
            >
              <span className={styles.cardName}>{inst.name}</span>
              <span className={styles.cardMeta}>{tuningLabel(inst.notes)}</span>
            </button>
          ))}
        </div>
        {s.instrumentId === CUSTOM_INSTRUMENT_ID && <CustomTuning />}
      </section>

      <section className={styles.block}>
        <h3 className={styles.blockTitle}>Scale length</h3>
        <p className={styles.small}>
          Nut to 12th fret, doubled. Your maker lists it in the specifications.
        </p>
        <div className={styles.chips} role="radiogroup" aria-label="Scale length">
          {SCALE_LENGTHS.map((l) => (
            <button
              key={l.id}
              type="button"
              role="radio"
              aria-checked={s.scaleId === l.id}
              className={styles.chip}
              onClick={() => intonationActions.chooseScale(l.id)}
            >
              <span>{l.label}</span>
              <span className={styles.chipMeta}>{l.mm} mm</span>
            </button>
          ))}
          <button
            type="button"
            role="radio"
            aria-checked={s.scaleId === CUSTOM_SCALE_ID}
            className={styles.chip}
            onClick={() => intonationActions.chooseScale(CUSTOM_SCALE_ID)}
          >
            <span>Custom</span>
            <span className={styles.chipMeta}>in mm</span>
          </button>
        </div>
        {s.scaleId === CUSTOM_SCALE_ID && (
          <label className={styles.mmField}>
            <span>Scale length</span>
            <input
              type="number"
              min={MIN_CUSTOM_SCALE_MM}
              max={MAX_CUSTOM_SCALE_MM}
              step={0.5}
              value={s.customScaleMm}
              onChange={(e) => intonationActions.setCustomScaleMm(Number(e.target.value))}
            />
            <span>mm</span>
          </label>
        )}
      </section>

      <div className={styles.startRow}>
        <p className={styles.small}>
          Fit fresh strings if you can, tune with your usual picking strength, and use the neck
          pickup. You will measure {notes.length} strings, about 20 seconds each.
        </p>
        <button type="button" className={buttons.nacre} onClick={() => intonationActions.start()}>
          Start measuring
        </button>
      </div>
    </div>
  );
}
