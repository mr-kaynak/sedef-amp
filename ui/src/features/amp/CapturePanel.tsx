// SPDX-License-Identifier: AGPL-3.0-or-later
// The amp stage while the amp source is Capture: a Neural Amp Modeler file (docs/dsp/nam.md).
import { useState } from 'react';
import type { DragEvent } from 'react';
import type { CaptureInfo, CaptureState } from '../../bridge/protocol';
import buttons from '../../controls/Button.module.css';
import { Icon } from '../../controls/Icon';
import { Knob } from '../../controls/Knob';
import { Toggle } from '../../controls/Toggle';
import { effectiveCab } from '../../params/schema';
import { formatHz, signed } from '../../params/format';
import { useParamSpec, useParamValue } from '../../params/useParam';
import { useAppStore } from '../../state/appStore';
import { invoke } from '../../state/backendRef';
import { uiActions } from '../../state/uiStore';
import styles from './CapturePanel.module.css';

export const CAPTURE_SITE_URL = 'https://www.tone3000.com';
const MAX_BYTES = 24 * 1024 * 1024;

function apply(state: CaptureState | undefined) {
  if (state) useAppStore.setState({ capture: state });
}

/** The calls behind the panel's buttons and its drop target. */
function useCaptureActions() {
  const [busy, setBusy] = useState(false);
  const [dropError, setDropError] = useState<string | null>(null);
  const run = async (call: () => Promise<CaptureState | undefined>) => {
    setDropError(null);
    setBusy(true);
    try {
      apply(await call());
    } finally {
      setBusy(false);
    }
  };
  return {
    busy,
    dropError,
    load: () => run(() => invoke('sedef_capture_load')),
    loadRecent: (path: string) => run(() => invoke('sedef_capture_loadRecent', path)),
    clear: () => run(() => invoke('sedef_capture_clear')),
    drop: async (file: File) => {
      if (!/\.nam$/i.test(file.name)) {
        setDropError('Drop a .nam file to load a capture.');
        return;
      }
      if (file.size > MAX_BYTES) {
        setDropError('This file is larger than 24 MB, so it isn’t a capture Sedef can play.');
        return;
      }
      const text = await file.text();
      await run(() => invoke('sedef_capture_loadData', file.name, text));
    },
  };
}

function describeModel(info: CaptureInfo): string {
  return info.sampleRate ? `${info.architecture}, ${formatHz(info.sampleRate)}` : info.architecture;
}

function describeKind(info: CaptureInfo): string {
  switch (info.kind) {
    case 'amp':
      return 'Amp only';
    case 'fullRig':
      return 'Full rig, cab and mic included';
    default:
      return 'Not stated in the file';
  }
}

function cabHint(info: CaptureInfo, through: boolean): string {
  if (info.kind === 'fullRig')
    return through
      ? 'This capture already includes a cab, so it plays through two.'
      : 'This capture already includes a cab and mic.';
  if (info.kind === 'amp')
    return through
      ? 'This capture is the amp alone; the cab page shapes it.'
      : 'This capture is the amp alone and sounds harsh without a cab.';
  return 'The file doesn’t say whether it includes a cab.';
}

function Facts({ info }: { info: CaptureInfo }) {
  const rows: [string, string][] = [];
  if (info.gear) rows.push(['Gear', info.gear]);
  if (info.author) rows.push(['Captured by', info.author]);
  rows.push(['Model', describeModel(info)]);
  rows.push(['Type', describeKind(info)]);
  return (
    <dl className={styles.facts}>
      {rows.map(([term, value]) => (
        <div key={term} className={styles.fact}>
          <dt>{term}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}

function CabRow({ info }: { info: CaptureInfo }) {
  const through = useParamValue('captureCab') >= 0.5;
  const amp = Math.round(useParamValue('ampModel'));
  const linked = useParamValue('ampCabLink') >= 0.5;
  const cab = effectiveCab(amp, linked ? 1 : 0, Math.round(useParamValue('cabModel')));
  const cabName = useParamSpec('cabModel').choices[cab] ?? '';
  return (
    <div className={styles.cab}>
      <div className={styles.cabToggle}>
        <Toggle param="captureCab" label="Through cab" />
        {through && (
          <button
            type="button"
            className={styles.textButton}
            onClick={() => uiActions.setSection('cab')}
          >
            Open the {cabName} cab
          </button>
        )}
      </div>
      <p className={styles.hint}>{cabHint(info, through)}</p>
    </div>
  );
}

function Interface({ info }: { info: CaptureInfo }) {
  if (info.inputLevelDbu === null) return null;
  return (
    <div className={styles.interface}>
      <Knob param="captureInterfaceLevel" label="Interface" size="sm" />
      <p className={styles.hint}>
        Your interface’s input level at 0 dBFS, set once for every capture. This one was made at{' '}
        {signed(info.inputLevelDbu, 1)} dBu.
      </p>
    </div>
  );
}

function Recent({
  state,
  onPick,
  disabled,
}: {
  state: CaptureState;
  onPick: (path: string) => void;
  disabled: boolean;
}) {
  if (state.recent.length === 0) return null;
  return (
    <section className={styles.recent} aria-labelledby="capture-recent">
      <h4 id="capture-recent" className={styles.recentTitle}>
        Recent captures
      </h4>
      <ul className={styles.recentList}>
        {state.recent.map((r) => (
          <li key={r.path}>
            <button
              type="button"
              className={styles.recentItem}
              aria-current={r.current || undefined}
              disabled={disabled}
              onClick={() => onPick(r.path)}
            >
              <Icon name={r.current ? 'check' : 'file'} size={12} />
              <span className={styles.recentName}>{r.name}</span>
            </button>
          </li>
        ))}
      </ul>
    </section>
  );
}

export function CapturePanel() {
  const state = useAppStore((s) => s.capture);
  const actions = useCaptureActions();
  const [dragging, setDragging] = useState(false);
  const loading = state.loading || actions.busy;
  const info = state.loaded;
  // The plugin's messages say what is wrong; the way forward is always another file.
  const error =
    actions.dropError ?? (state.error ? `${state.error} Choose another file to try again.` : null);

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
    if (file) void actions.drop(file);
  };

  return (
    <section
      className={styles.panel}
      aria-label="Capture"
      data-dragging={dragging || undefined}
      onDragOver={onDragOver}
      onDragLeave={onDragLeave}
      onDrop={onDrop}
    >
      {info ? (
        <div className={styles.loaded}>
          <div className={styles.identity}>
            <p className={styles.eyebrow}>NAM capture</p>
            <h3 className={styles.name}>{info.name}</h3>
            <Facts info={info} />
            <div className={styles.actions}>
              <button
                type="button"
                className={buttons.solid}
                disabled={loading}
                onClick={() => void actions.load()}
              >
                Replace…
              </button>
              <button
                type="button"
                className={buttons.ghost}
                disabled={loading}
                onClick={() => void actions.clear()}
              >
                Clear
              </button>
            </div>
          </div>
          <div className={styles.controls}>
            <div className={styles.trims}>
              <Knob param="captureInput" label="Input" size="lg" />
              <Knob param="captureOutput" label="Output" size="lg" />
            </div>
            <CabRow info={info} />
            <Interface info={info} />
          </div>
        </div>
      ) : (
        <div className={styles.empty}>
          {state.missing ? (
            <>
              <h3 className={styles.emptyTitle}>{state.missing.name} is missing</h3>
              <p className={styles.emptyText}>
                Sedef can’t find {state.missing.fileName}
                {state.missing.path ? ` at ${state.missing.path}` : ''}. Load it again to play it;
                until then the amp models play.
              </p>
            </>
          ) : (
            <>
              <h3 className={styles.emptyTitle}>No capture loaded</h3>
              <p className={styles.emptyText}>
                Load a .nam file to play a captured amp in place of the models.
              </p>
            </>
          )}
          <div className={styles.emptyActions}>
            <button
              type="button"
              className={buttons.nacre}
              disabled={loading}
              onClick={() => void actions.load()}
            >
              {state.missing ? 'Locate file…' : 'Load capture…'}
            </button>
            <span className={styles.dropHint}>or drop a .nam file here</span>
          </div>
          <button
            type="button"
            className={styles.textButton}
            onClick={() => void invoke('sedef_openUrl', CAPTURE_SITE_URL)}
          >
            Find captures on TONE3000
          </button>
        </div>
      )}

      <p className={styles.status} role="status">
        {loading ? 'Loading capture…' : ''}
      </p>
      <p className={styles.error} role="alert">
        {error && !loading ? (
          <>
            <Icon name="info" size={14} />
            {error}
          </>
        ) : null}
      </p>
      <Recent state={state} disabled={loading} onPick={(path) => void actions.loadRecent(path)} />
    </section>
  );
}
