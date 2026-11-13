// SPDX-License-Identifier: AGPL-3.0-or-later
// The Recorder panel (docs/recorder.md): Record, one big button that records a take as a DI and
// an output file, with the time, the level of both and where they went; Re-amp, a recorded DI
// or any dropped or opened file played into the rig as if it were the guitar.
import { useEffect, useRef, useState } from 'react';
import type { DragEvent, KeyboardEvent } from 'react';
import type { ReampState, RecorderState } from '../../bridge/protocol';
import buttons from '../../controls/Button.module.css';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import { Led } from '../../controls/Led';
import nacre from '../../controls/NacreFill.module.css';
import segmented from '../../controls/Segmented.module.css';
import toggle from '../../controls/Toggle.module.css';
import { useRadioGroup } from '../../controls/useRadioGroup';
import { telemetry, useLiveFrame, useLiveSelector } from '../../state/live';
import { meterPosition } from '../header/meterScale';
import {
  importFile,
  loadFile,
  openFile,
  playReamp,
  reampTake,
  refreshFiles,
  reveal,
  setReampLoop,
  setReampMix,
  toggleRecording,
} from './actions';
import { channels, clock, percent, rate } from './format';
import { onRecorderChange, recorderUi, useRecorderStore, type RecorderTab } from './store';
import styles from './RecorderPanel.module.css';

const TABS: { id: RecorderTab; label: string }[] = [
  { id: 'record', label: 'Record' },
  { id: 'reamp', label: 'Re-amp' },
];

const RELEASE_DB_PER_S = 26;

/** The level of what goes into each file, drawn every frame from telemetry. */
function TakeMeters() {
  const bars = useRef<(HTMLDivElement | null)[]>([]);
  const level = useRef([-90, -90]);
  useLiveFrame(telemetry, (t, dt) => {
    const dbs = [t?.recorder?.diDb ?? -90, t?.recorder?.outputDb ?? -90];
    dbs.forEach((db, i) => {
      level.current[i] = Math.max(db, (level.current[i] ?? -90) - RELEASE_DB_PER_S * dt);
      const bar = bars.current[i];
      if (bar) {
        const p = meterPosition(level.current[i] ?? -90);
        bar.style.transform = `scaleX(${p})`;
        bar.dataset.hot = (level.current[i] ?? -90) >= -1 ? 'true' : 'false';
      }
    });
  });
  return (
    <div className={styles.meters} aria-hidden="true">
      {['DI', 'Output'].map((name, i) => (
        <div key={name} className={styles.meterRow}>
          <span className={styles.meterName}>{name}</span>
          <div className={styles.meterTrack}>
            <div
              ref={(el) => {
                bars.current[i] = el;
              }}
              className={styles.meterBar}
            />
          </div>
        </div>
      ))}
    </div>
  );
}

function statusTitle(r: RecorderState): string {
  switch (r.status) {
    case 'starting':
      return 'Starting…';
    case 'recording':
      return 'Recording';
    case 'saving':
      return 'Saving…';
    default:
      return 'Ready to record';
  }
}

/** What the last take's end means, when it is worth saying. */
function endNote(r: RecorderState): string | null {
  switch (r.take?.ended) {
    case 'sizeLimit':
      return 'The take stopped at the WAV size limit of 4 GB. Start another to go on.';
    case 'audioStopped':
      return 'The take ended when the audio device stopped or restarted.';
    case 'empty':
      return 'Nothing was recorded: it stopped before any audio arrived.';
    default:
      return null;
  }
}

function RecordTab({ r }: { r: RecorderState }) {
  const active = r.status !== 'idle';
  const tenths = useLiveSelector(telemetry, (t) => Math.floor((t?.recorder?.seconds ?? 0) * 10));
  const dropouts = useLiveSelector(telemetry, (t) => t?.recorder?.dropouts ?? 0);
  const take = r.take;
  const last = !active && take && take.ended !== 'empty' && take.ended !== '' ? take : null;
  const note = active ? null : endNote(r);
  const lost = active ? dropouts : (last?.dropouts ?? 0);

  return (
    <div className={styles.tab} data-tone={active ? 'record' : undefined}>
      <div className={styles.top}>
        <button
          type="button"
          className={`${buttons.press} ${styles.main}`}
          data-action={active ? 'stop' : 'record'}
          data-autofocus
          aria-label={active ? 'Stop recording' : 'Start recording'}
          aria-describedby="recorder-state"
          aria-disabled={r.status === 'saving' || undefined}
          onClick={() => {
            if (r.status !== 'saving') void toggleRecording();
          }}
        >
          <Icon name={active ? 'stop' : 'record'} size={26} filled stroke={ICON_STROKE_LABEL} />
        </button>
        <div className={styles.state} id="recorder-state">
          <p className={styles.title}>
            <span className={styles.lamp} aria-hidden="true" />
            {statusTitle(r)}
          </p>
          <p className={styles.time}>
            {active ? clock(tenths / 10, true) : last ? `Last take ${clock(last.seconds)}` : ''}
          </p>
          <TakeMeters />
        </div>
      </div>

      {/* Not a live region: the header button says a dropout aloud once, while recording. */}
      {lost > 0 && (
        <p className={styles.warn}>
          {lost === 1 ? '1 dropout' : `${lost} dropouts`}
          {last ? `: ${last.droppedSeconds.toFixed(2)} s of this take is silence.` : '.'} The disk
          could not keep up; the files keep their length and stay aligned.
        </p>
      )}
      {r.error && (
        <p className={styles.error} role="alert">
          {r.error}
        </p>
      )}
      {note && <p className={styles.note}>{note}</p>}

      <dl className={styles.facts}>
        <div className={styles.fact}>
          <dt>Saves to</dt>
          <dd className={styles.path}>{r.folder}</dd>
          <button
            type="button"
            className={`${buttons.ghost} ${buttons.small} ${buttons.lead}`}
            onClick={() => reveal()}
          >
            <Icon name="folder" size={12} />
            Show folder
          </button>
        </div>
        {last && (
          <div className={styles.fact}>
            <dt>Last take</dt>
            <dd>
              <span className={styles.takeName}>{last.name}</span>
              <span className={styles.sub}>DI and Output, {clock(last.seconds)}</span>
            </dd>
            <div className={styles.factActions}>
              <button
                type="button"
                className={`${buttons.ghost} ${buttons.small}`}
                aria-label={`Show the take ${last.name} in its folder`}
                onClick={() => reveal(last.diPath)}
              >
                Show
              </button>
              <button
                type="button"
                className={`${buttons.ghost} ${buttons.small}`}
                aria-label={`Re-amp the take ${last.name}`}
                onClick={() => void reampTake(last.diPath)}
              >
                Re-amp
              </button>
            </div>
          </div>
        )}
      </dl>

      <p className={styles.hint}>
        Two WAV files that line up sample for sample: your guitar dry (DI) and the sound you hear
        (Output, the click included when it plays).
      </p>
      <p className={styles.keys}>
        <kbd>R</kbd> start or stop a take
      </p>
    </div>
  );
}

/** Where the file plays, drawn every frame. */
function PlayPosition() {
  const fill = useRef<HTMLDivElement>(null);
  const text = useLiveSelector(telemetry, (t) =>
    t?.reamp ? `${clock(t.reamp.seconds)} / ${clock(t.reamp.length)}` : '',
  );
  useLiveFrame(telemetry, (t) => {
    const r = t?.reamp;
    if (fill.current)
      fill.current.style.transform = `scaleX(${r && r.length > 0 ? Math.min(1, r.seconds / r.length) : 0})`;
  });
  return (
    <div className={styles.position}>
      <div className={styles.positionTrack} aria-hidden="true">
        <div ref={fill} className={styles.positionFill} />
      </div>
      <span className={styles.positionText}>{text}</span>
    </div>
  );
}

function reampTitle(a: ReampState, playing: boolean): string {
  if (playing) return 'Re-amping';
  switch (a.status) {
    case 'loading':
      return 'Loading…';
    case 'converting':
      return 'Converting…';
    case 'ready':
      return 'Ready';
    case 'error':
      return 'Can’t play this';
    default:
      return 'No file yet';
  }
}

function MixChoice({ mix }: { mix: boolean }) {
  const options = ['Muted', 'Mixed in'];
  const { onKeyDown, radio } = useRadioGroup(options.length, mix ? 1 : 0, (i) => {
    void setReampMix(i === 1);
  });
  return (
    <div className={styles.mix}>
      <span className={styles.mixLabel} id="reamp-mix-label">
        Your guitar
      </span>
      <div
        role="radiogroup"
        aria-labelledby="reamp-mix-label"
        className={segmented.segmented}
        onKeyDown={onKeyDown}
      >
        {options.map((label, i) => (
          <button
            key={label}
            type="button"
            className={`${nacre.fill} ${segmented.option}`}
            {...radio(i)}
          >
            {label}
          </button>
        ))}
      </div>
    </div>
  );
}

function ReampTab({ a }: { a: ReampState }) {
  const files = useRecorderStore((s) => s.files);
  const importing = useRecorderStore((s) => s.importing);
  const dropError = useRecorderStore((s) => s.dropError);
  const playing = useLiveSelector(telemetry, (t) => t?.reamp?.playing ?? a.playing) || a.playing;
  const gaps = useLiveSelector(telemetry, (t) => (t?.reamp?.playing ? t.reamp.gaps : 0));
  const [dragging, setDragging] = useState(false);
  const ready = a.status === 'ready' && a.file !== null;
  // A new pick cancels a conversion still running; only a drop on its way in holds the list.
  const busy = importing !== null;
  const error = dropError ?? (a.status === 'error' ? a.error : null);
  const said = importing
    ? `Copying ${importing.name}`
    : a.status === 'converting'
      ? 'Converting the file for this device'
      : ready
        ? `${a.file?.name ?? 'The file'} is ready to play`
        : '';

  // The list, now and whenever a take finishes saving (recording while re-amping, say).
  useEffect(() => {
    void refreshFiles();
    return onRecorderChange((previous) => {
      const now = useRecorderStore.getState().state?.recorder.status;
      if (previous && previous.recorder.status !== 'idle' && now === 'idle') void refreshFiles();
    });
  }, []);

  const hasFiles = (e: DragEvent) => Array.from(e.dataTransfer.types).includes('Files');
  const onDragOver = (e: DragEvent) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
    setDragging(true);
  };
  const onDragLeave = (e: DragEvent) => {
    if (!e.currentTarget.contains(e.relatedTarget as Node | null)) setDragging(false);
  };
  const onDrop = (e: DragEvent) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    setDragging(false);
    const file = e.dataTransfer.files[0];
    if (file) void importFile(file);
  };

  return (
    <div
      className={styles.tab}
      data-tone={playing ? 'play' : undefined}
      data-dragging={dragging || undefined}
      onDragOver={onDragOver}
      onDragLeave={onDragLeave}
      onDrop={onDrop}
    >
      <div className={styles.top}>
        <button
          type="button"
          className={`${buttons.press} ${styles.main}`}
          data-action={playing ? 'stop' : 'play'}
          data-autofocus
          aria-label={playing ? 'Stop re-amping' : 'Play the file through the rig'}
          aria-describedby="reamp-state"
          aria-disabled={(!ready && !playing) || undefined}
          onClick={() => {
            if (ready || playing) void playReamp(!playing);
          }}
        >
          <Icon name={playing ? 'stop' : 'play'} size={26} filled stroke={ICON_STROKE_LABEL} />
        </button>
        <div className={styles.state} id="reamp-state">
          <p className={styles.title}>
            <span className={styles.lamp} aria-hidden="true" />
            {reampTitle(a, playing)}
          </p>
          {a.file && (
            <p className={styles.fileLine}>
              <span className={styles.fileName}>{a.file.name}</span>
              <span className={styles.sub}>
                {clock(a.file.seconds)}, {channels(a.file.channels)}, {rate(a.file.sampleRate)}
                {a.file.converted ? ', converted for this device' : ''}
              </span>
            </p>
          )}
          {a.status === 'converting' ? (
            <div className={styles.position}>
              <div
                className={styles.positionTrack}
                role="progressbar"
                aria-label="Converting"
                aria-valuemin={0}
                aria-valuemax={100}
                aria-valuenow={Math.round(a.progress * 100)}
              >
                <div
                  className={styles.positionFill}
                  style={{ transform: `scaleX(${a.progress})` }}
                />
              </div>
              <span className={styles.positionText}>{percent(a.progress)}</span>
            </div>
          ) : (
            ready && <PlayPosition />
          )}
        </div>
      </div>

      <div className={styles.options}>
        <button
          type="button"
          role="switch"
          aria-checked={a.loop}
          className={`${buttons.press} ${toggle.chip}`}
          data-on={a.loop || undefined}
          onClick={() => void setReampLoop(!a.loop)}
        >
          <Led on={a.loop} size={6} />
          <span>Loop</span>
        </button>
        <MixChoice mix={a.mix} />
      </div>

      {gaps > 0 && (
        <p className={styles.warn}>
          The disk could not keep up: the file paused {gaps === 1 ? 'once' : `${gaps} times`} and
          went on where it stopped.
        </p>
      )}
      {importing && (
        <p className={styles.note}>
          Copying {importing.name}… {percent(importing.progress)}
        </p>
      )}
      {/* Rendered before its text changes, so the change is said; the percentages are not. */}
      <span className="visually-hidden" role="status">
        {said}
      </span>
      {error && (
        <p className={styles.error} role="alert">
          {error}
        </p>
      )}

      <section className={styles.files} aria-labelledby="reamp-files">
        <h4 id="reamp-files" className={styles.filesTitle}>
          Takes and files
        </h4>
        {files.length === 0 ? (
          <p className={styles.empty}>Record a take, open a file or drop one here.</p>
        ) : (
          <ul className={styles.fileList}>
            {files.map((f) => {
              const current = a.file?.path === f.path;
              return (
                <li key={f.path}>
                  <button
                    type="button"
                    className={styles.fileItem}
                    aria-current={current || undefined}
                    disabled={busy}
                    onClick={() => void loadFile(f.path)}
                  >
                    <Icon
                      name={current ? 'check' : f.kind === 'take' ? 'record' : 'file'}
                      size={12}
                    />
                    <span className={styles.fileItemName}>{f.name}</span>
                    <span className={styles.fileItemTime}>{clock(f.seconds)}</span>
                  </button>
                </li>
              );
            })}
          </ul>
        )}
      </section>

      <div className={styles.openRow}>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small} ${buttons.lead}`}
          disabled={busy}
          onClick={() => void openFile()}
        >
          <Icon name="folder" size={12} />
          Open file…
        </button>
        <span className={styles.dropHint}>or drop a WAV, AIFF or FLAC file here</span>
      </div>
      <p className={styles.hint}>
        The file plays into the rig as if it were your guitar. Change the tone while it plays;
        record to keep the new sound.
      </p>
    </div>
  );
}

export function RecorderPanel() {
  const state = useRecorderStore((s) => s.state);
  const tab = useRecorderStore((s) => s.tab);
  const tabRefs = useRef<(HTMLButtonElement | null)[]>([]);
  if (!state) return null;

  const onTabKey = (e: KeyboardEvent<HTMLDivElement>) => {
    const index = TABS.findIndex((t) => t.id === tab);
    const moves: Record<string, number> = {
      ArrowRight: (index + 1) % TABS.length,
      ArrowLeft: (index - 1 + TABS.length) % TABS.length,
      Home: 0,
      End: TABS.length - 1,
    };
    const next = moves[e.key];
    if (next === undefined) return;
    e.preventDefault();
    recorderUi.setTab(TABS[next]!.id);
    tabRefs.current[next]?.focus();
  };

  return (
    <div className={styles.panel}>
      <div
        role="tablist"
        aria-label="Recorder"
        className={`${segmented.segmented} ${styles.tabs}`}
        onKeyDown={onTabKey}
      >
        {TABS.map((t, i) => (
          <button
            key={t.id}
            ref={(el) => {
              tabRefs.current[i] = el;
            }}
            type="button"
            role="tab"
            id={`recorder-tab-${t.id}`}
            aria-selected={tab === t.id}
            aria-controls={tab === t.id ? `recorder-panel-${t.id}` : undefined}
            tabIndex={tab === t.id ? 0 : -1}
            className={`${nacre.fill} ${segmented.option} ${styles.tabButton}`}
            onClick={() => recorderUi.setTab(t.id)}
          >
            {t.label}
          </button>
        ))}
      </div>
      <div role="tabpanel" id={`recorder-panel-${tab}`} aria-labelledby={`recorder-tab-${tab}`}>
        {tab === 'record' ? <RecordTab r={state.recorder} /> : <ReampTab a={state.reamp} />}
      </div>
    </div>
  );
}
