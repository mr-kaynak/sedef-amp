// SPDX-License-Identifier: AGPL-3.0-or-later
import { useAppStore } from '../state/appStore';
import { telemetry, useLiveSelector } from '../state/live';
import styles from './StatusBar.module.css';

export function StatusBar() {
  const session = useAppStore((s) => s.session);
  const cpu = useLiveSelector(telemetry, (t) => (t ? Math.round(t.cpu * 100) : null));
  const latencyMs = session.sampleRate ? (session.latencySamples / session.sampleRate) * 1000 : 0;
  return (
    <footer className={styles.status}>
      <span>{(session.sampleRate / 1000).toFixed(1).replace(/\.0$/, '')} kHz</span>
      <span>{latencyMs.toFixed(1)} ms latency</span>
      {session.layout && <span>{session.layout}</span>}
      {session.backend === 'mock' && (
        <span className={styles.sim}>Simulated audio, browser preview</span>
      )}
      <span className={styles.spacer} />
      <span aria-label="Audio processing load">DSP {cpu === null ? '—' : `${cpu}%`}</span>
    </footer>
  );
}
