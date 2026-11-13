// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect, useRef, useState } from 'react';
import type { ReactNode } from 'react';
import buttons from '../controls/Button.module.css';
import { Knob } from '../controls/Knob';
import { Segmented } from '../controls/Segmented';
import { NacreStar } from '../features/header/Brand';
import { useParamValue } from '../params/useParam';
import { GateMeter } from './components/GateMeter';
import { IntSelect } from './components/IntSelect';
import { NoteDisplay } from './components/NoteDisplay';
import { PortPicker } from './components/PortPicker';
import { SETUP_GUIDE_URL } from './protocol';
import type { AudioState } from './protocol';
import { midiInvoke, useMidiStore, type Session } from './store';
import styles from './App.module.css';

function context(session: Session): string {
  if (session.isInstrument) return 'Instrument · guitar on the side chain';
  if (session.isStandalone) return 'Standalone';
  return session.wrapperType === 'Browser' ? 'Preview' : `Effect · ${session.wrapperType}`;
}

function Field({
  label,
  hint,
  children,
}: {
  label: string;
  hint?: ReactNode;
  children: ReactNode;
}) {
  return (
    <div className={styles.field}>
      <span className={styles.label}>{label}</span>
      <div className={styles.control}>{children}</div>
      {hint && <p className={styles.hint}>{hint}</p>}
    </div>
  );
}

const ms = (value: number) => `${value.toFixed(1)} ms`;

/** A buffer this long is worth saying something about: it is most of what a note waits. */
const LONG_BUFFER_MS = 6;

/** What Auto has heard of the instrument, and what each fixed range covers. */
function lowNoteHint(choice: number, heard: number): string {
  if (choice === 1) return 'Six strings: down to C♯2.';
  if (choice === 2) return 'Seven strings: down to G♯1.';
  if (choice === 3) return 'Eight strings: down to E1.';
  if (heard === 2) return 'Heard a low B: down to G♯1.';
  if (heard === 3) return 'Heard a low F♯: down to E1.';
  return 'A six-string until it hears a lower string.';
}

/** The audio side of the delay, in the footer; a long buffer in the standalone app gets a way out. */
function AudioFigures({ audio, standalone }: { audio: AudioState; standalone: boolean }) {
  if (audio.blockSize <= 0) return <span>{(audio.sampleRate / 1000).toFixed(1)} kHz</span>;
  const long = standalone && audio.bufferWorstMs > LONG_BUFFER_MS;
  return (
    <>
      <span>{(audio.sampleRate / 1000).toFixed(1)} kHz</span>
      <span className={styles.dotSep} aria-hidden="true" />
      <span data-long={long || undefined} className={styles.buffer}>
        Buffer {audio.blockSize} ({ms(audio.bufferWorstMs)})
      </span>
      {long && (
        <button
          type="button"
          className={styles.inlineLink}
          onClick={() => void midiInvoke('sedef_midi_audioSettings')}
        >
          Every note waits this long: pick a smaller buffer
        </button>
      )}
      {audio.inputLatencyMs >= 0 && (
        <>
          <span className={styles.dotSep} aria-hidden="true" />
          <span>Input {ms(audio.inputLatencyMs)}</span>
        </>
      )}
    </>
  );
}

function PanicButton() {
  const [flash, setFlash] = useState(false);
  const flashTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  // A press just before the window closes must not set state on an unmounted button.
  useEffect(() => () => clearTimeout(flashTimer.current), []);
  return (
    <button
      type="button"
      className={`${buttons.ghost} ${styles.panic}`}
      data-flash={flash || undefined}
      title="Ends every note on all 16 channels"
      onClick={() => {
        void midiInvoke('sedef_midi_panic');
        setFlash(true);
        clearTimeout(flashTimer.current);
        flashTimer.current = setTimeout(() => setFlash(false), 600);
      }}
    >
      Panic
    </button>
  );
}

export function MidiApp() {
  const status = useMidiStore((s) => s.status);
  const message = useMidiStore((s) => s.message);
  const session = useMidiStore((s) => s.session);
  const audio = useMidiStore((s) => s.audio);
  const lowNote = useParamValue('lowNote');
  const mode = useParamValue('mode');
  const timing = useParamValue('timing');
  const channel = useParamValue('channel');

  if (status === 'connecting') return <div className={styles.loading} aria-busy="true" />;
  if (status === 'error' || !session)
    return (
      <div className={styles.failed} role="alert">
        {message ??
          'Sedef MIDI could not start its interface. Close this window and open it again; if it stays empty, reinstall Sedef MIDI.'}
      </div>
    );

  const bend = Math.round(mode) === 1;
  const steady = Math.round(timing) === 1;
  const audioLabel = session.isInstrument ? 'Side chain' : 'Guitar audio';
  return (
    <div className={styles.app}>
      <header className={styles.header}>
        <div className={styles.brand}>
          <NacreStar size={26} />
          <span className={styles.wordmark}>Sedef</span>
          <span className={styles.product}>midi</span>
        </div>
        <span className={styles.context}>{context(session)}</span>
        {session.isStandalone && (
          <button
            type="button"
            className={buttons.ghost}
            onClick={() => void midiInvoke('sedef_midi_audioSettings')}
          >
            Audio input…
          </button>
        )}
        <PanicButton />
      </header>

      {message && (
        <p className={styles.alert} role="alert">
          {message}
        </p>
      )}

      <main className={styles.main}>
        <div className={styles.meter}>
          <GateMeter height={300} />
        </div>
        <NoteDisplay channel={Math.round(channel)} className={styles.note} />
        <section className={`${styles.group} ${styles.velocity}`} aria-labelledby="velocity-legend">
          <h2 id="velocity-legend" className={styles.legend}>
            Velocity
          </h2>
          <div className={styles.velocityRow}>
            <Knob param="dynamics" size="md" label="Sensitivity" />
            <Field
              label="Curve"
              hint="Soft: a light touch already plays loud. Hard: loud notes need a hard pluck or pick."
            >
              <Segmented param="curve" size="md" />
            </Field>
          </div>
        </section>

        <div className={styles.controls}>
          <section className={styles.group} aria-labelledby="notes-legend">
            <h2 id="notes-legend" className={styles.legend}>
              Notes
            </h2>
            <Field label="Octave">
              <Segmented param="octave" size="md" labels={['−2', '−1', '0', '+1']} />
            </Field>
            <Field label="Mode">
              <Segmented param="mode" size="md" labels={['Notes', 'Notes + Bend']} />
            </Field>
            <Field
              label="Bend range"
              hint={bend ? 'Set your instrument to the same range.' : 'Used in Notes + Bend.'}
            >
              <IntSelect
                param="bendRange"
                ariaLabel="Bend range"
                className={styles.narrow}
                disabled={!bend}
                format={(v) => `${v} st`}
              />
            </Field>
            <Field
              label="Timing"
              hint={
                steady
                  ? `Every note ${Math.round(session.steadyLagMs)} ms after you play it. Set the track delay to −${Math.round(session.steadyLagMs)} ms.`
                  : 'Each note as soon as it is sure. Low strings take longer.'
              }
            >
              <Segmented param="timing" size="md" />
            </Field>
            <Field
              label="Lowest note"
              hint={
                <>
                  {lowNoteHint(Math.round(lowNote), audio?.lowNote ?? 1)}
                  {Math.round(lowNote) === 0 && (audio?.lowNote ?? 1) > 1 && (
                    <>
                      {' '}
                      <button
                        type="button"
                        className={styles.inlineLink}
                        onClick={() => void midiInvoke('sedef_midi_forgetLowNote')}
                      >
                        Forget
                      </button>
                    </>
                  )}
                </>
              }
            >
              <Segmented
                param="lowNote"
                size="md"
                ariaLabel="Lowest note"
                labels={['Auto', 'E2', 'B1', 'F♯1']}
              />
            </Field>
          </section>

          <section className={styles.group} aria-labelledby="output-legend">
            <h2 id="output-legend" className={styles.legend}>
              Output
            </h2>
            <Field label="Channel">
              <IntSelect param="channel" ariaLabel="MIDI channel" className={styles.narrow} />
            </Field>
            <Field label={audioLabel}>
              <Segmented param="audio" size="md" ariaLabel={audioLabel} />
            </Field>
            <Field label={session.isStandalone ? 'Send to' : 'Also send to'}>
              <PortPicker standalone={session.isStandalone} />
            </Field>
          </section>
        </div>
      </main>

      <footer className={styles.footer}>
        {audio ? (
          <AudioFigures audio={audio} standalone={session.isStandalone} />
        ) : (
          <span>{(session.sampleRate / 1000).toFixed(1)} kHz</span>
        )}
        <span className={styles.dotSep} aria-hidden="true" />
        <span>Sedef MIDI {session.appVersion}</span>
        <button
          type="button"
          className={styles.link}
          onClick={() => void midiInvoke('sedef_openUrl', SETUP_GUIDE_URL)}
        >
          Setup guide
        </button>
      </footer>
    </div>
  );
}
