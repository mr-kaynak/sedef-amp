// SPDX-License-Identifier: AGPL-3.0-or-later
import { transposeLatencyMs } from '../lib/transpose';
import { useParamValue } from '../params/useParam';
import { useAppStore } from '../state/appStore';
import { clock } from '../features/recorder/format';
import { telemetry, useLiveSelector } from '../state/live';
import styles from './StatusBar.module.css';

/** A take recording or a file re-amping (docs/recorder.md): always in sight, the preset browser
 *  open or not. */
function RecorderStatus() {
  const recording = useLiveSelector(telemetry, (t) =>
    t?.recorder && t.recorder.status !== 'idle' ? Math.floor(t.recorder.seconds) : null,
  );
  const reamp = useLiveSelector(telemetry, (t) =>
    t?.reamp?.playing ? `${clock(t.reamp.seconds)} / ${clock(t.reamp.length)}` : null,
  );
  return (
    <>
      {recording !== null && (
        <span className={styles.rec}>
          <span className={styles.recDot} aria-hidden="true" />
          Recording {clock(recording)}
        </span>
      )}
      {reamp !== null && <span className={styles.reamp}>Re-amping {reamp}</span>}
    </>
  );
}

export function StatusBar() {
  const session = useAppStore((s) => s.session);
  const cpu = useLiveSelector(telemetry, (t) => (t ? Math.round(t.cpu * 100) : null));
  const latencyMs = session.sampleRate ? (session.latencySamples / session.sampleRate) * 1000 : 0;
  // Transpose's share, while it shifts: the one setting that changes the latency as you play.
  const transposeMs = transposeLatencyMs(useParamValue('transpose'));
  return (
    <footer className={styles.status}>
      <span>{(session.sampleRate / 1000).toFixed(1).replace(/\.0$/, '')} kHz</span>
      <span>
        {latencyMs.toFixed(1)} ms latency
        {transposeMs > 0 && ` (Transpose adds ${transposeMs} ms)`}
      </span>
      {session.layout && <span>{session.layout}</span>}
      {session.wrapperType === 'Browser' && (
        <span className={styles.sim}>Simulated audio, browser preview</span>
      )}
      <span className={styles.spacer} />
      <RecorderStatus />
      <span aria-label="Audio processing load">DSP {cpu === null ? '—' : `${cpu}%`}</span>
    </footer>
  );
}
