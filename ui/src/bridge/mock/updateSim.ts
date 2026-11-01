// SPDX-License-Identifier: AGPL-3.0-or-later
// The mock backend's update checker (plugin/src/update/UpdateService.cpp): scripted scenarios for
// the browser preview (?update=...) and for tests, which advance checks and downloads with step().
import type { ReleaseInfo, UpdateErrorCode, UpdateState } from '../protocol';

export const UPDATE_SCENARIOS = [
  'current',
  'available',
  'skipped',
  'error',
  'noinstaller',
  'mismatch',
  'unsupported',
] as const;
export type UpdateScenario = (typeof UPDATE_SCENARIOS)[number];

export function isUpdateScenario(value: unknown): value is UpdateScenario {
  return typeof value === 'string' && (UPDATE_SCENARIOS as readonly string[]).includes(value);
}

type Supported = Extract<UpdateState, { supported: true }>;

const RELEASES_URL = 'https://github.com/mr-kaynak/sedef-amp/releases';
const HOUR = 60 * 60 * 1000;
const DOWNLOAD_STEPS = 8;

const NOTES = `Presets you can find again, and updates that come to you.

### Added

- **Preset browser:** search by name, tag or author, with folders and favourites at the top.
- **Updates:** Sedef checks for a new version once a day and can download, verify and open the
  installer for you. Turn it off in Settings. See [how it works](https://github.com/mr-kaynak/sedef-amp/blob/main/docs/updates.md).
- Installers for every OS: \`Sedef-Amp-macOS.dmg\`, \`Sedef-Amp-Windows-Setup.exe\` and a Linux archive.

### Fixed

- The tuner no longer reads sharp on the first pluck after a long silence.
- Drift's tape drive keeps its level when you change the head pattern.

**Full changelog:** https://github.com/mr-kaynak/sedef-amp/compare/v0.1.0...v0.2.0`;

function release(version: string, withInstaller: boolean): ReleaseInfo {
  return {
    version,
    tag: `v${version}`,
    name: `Sedef Amp ${version}`,
    notes: NOTES,
    url: `${RELEASES_URL}/tag/v${version}`,
    publishedAt: '2026-11-20T10:00:00Z',
    installer: withInstaller ? { name: 'Sedef-Amp-macOS.dmg', size: 48_600_000 } : null,
    hasChecksums: true,
  };
}

export interface UpdateSimOptions {
  scenario?: UpdateScenario | undefined;
  currentVersion: string;
  /** Run timers (browser). Tests advance with step(). */
  autoStart?: boolean | undefined;
  /** localStorage key for the two settings; null keeps them in memory. */
  storageKey?: string | null | undefined;
  emit: (state: UpdateState) => void;
}

export class UpdateSimulator {
  private readonly scenario: UpdateScenario;
  private state: Supported;
  private timer: ReturnType<typeof setInterval> | null = null;
  private stepsLeft = 0;

  constructor(private readonly options: UpdateSimOptions) {
    this.scenario = options.scenario ?? 'current';
    const s = this.scenario;
    const latest = s === 'current' ? release('0.1.0', true) : release('0.2.0', s !== 'noinstaller');
    this.state = {
      supported: true,
      currentVersion: options.currentVersion,
      platform: 'macOS',
      autoCheck: true,
      autoCheckAllowed: true,
      skippedVersion: s === 'skipped' ? '0.2.0' : '',
      lastChecked: s === 'error' ? null : Date.now() - 3 * HOUR,
      phase: s === 'error' ? 'error' : 'idle',
      latest: s === 'error' ? null : latest,
      updateAvailable: s !== 'current' && s !== 'error',
      error: s === 'error' ? { code: 'network', detail: 'could not connect' } : null,
      progress: null,
      download: null,
      releasesUrl: RELEASES_URL,
    };
    this.loadSettings();
  }

  // --- Native functions -------------------------------------------------------------------------

  getState(): UpdateState {
    return this.snapshot();
  }

  check(): UpdateState {
    if (this.busy()) return this.snapshot();
    this.update({ phase: 'checking', error: null });
    this.stepsLeft = 1;
    this.run(900);
    return this.snapshot();
  }

  setAutoCheck(enabled: boolean): UpdateState {
    this.update({ autoCheck: enabled });
    this.saveSettings();
    return this.snapshot();
  }

  skip(version: string): UpdateState {
    this.update({ skippedVersion: version });
    this.saveSettings();
    return this.snapshot();
  }

  download(): UpdateState {
    const latest = this.state.latest;
    if (this.busy() || !latest) return this.snapshot();
    if (!latest.installer) {
      this.fail('noInstaller', `${latest.tag} has no Sedef-Amp-macOS.dmg`);
      return this.snapshot();
    }
    this.update({
      phase: 'downloading',
      error: null,
      download: null,
      progress: { received: 0, total: latest.installer.size },
    });
    this.stepsLeft = DOWNLOAD_STEPS + 1; // the last step verifies
    this.run(140);
    return this.snapshot();
  }

  cancel(): UpdateState {
    if (this.state.phase === 'downloading' || this.state.phase === 'verifying') {
      this.stop();
      this.update({ phase: 'idle', progress: null });
    }
    return this.snapshot();
  }

  reveal(): UpdateState {
    return this.snapshot();
  }

  // --- Test helpers ---------------------------------------------------------------------------

  /** Advances a running check or download by one step. */
  step(): void {
    if (this.stepsLeft <= 0) return;
    this.stepsLeft -= 1;
    const { phase, progress, latest } = this.state;
    if (phase === 'checking') {
      if (this.scenario === 'error') this.fail('network', 'could not connect');
      else this.update({ phase: 'idle', lastChecked: Date.now() });
    } else if (phase === 'downloading' && progress) {
      const received = Math.min(
        progress.total,
        progress.received + Math.ceil(progress.total / DOWNLOAD_STEPS),
      );
      this.update({
        phase: received >= progress.total ? 'verifying' : 'downloading',
        progress: { ...progress, received },
      });
    } else if (phase === 'verifying') {
      if (this.scenario === 'mismatch') this.fail('checksumMismatch', 'expected 3f…, got 9a…');
      else
        this.update({
          phase: 'done',
          progress: null,
          download: { fileName: latest?.installer?.name ?? 'installer', opened: true },
        });
    }
    if (this.stepsLeft <= 0) this.stop();
  }

  dispose(): void {
    this.stop();
  }

  // --- Internals --------------------------------------------------------------------------------

  private busy(): boolean {
    const p = this.state.phase;
    return p === 'checking' || p === 'downloading' || p === 'verifying';
  }

  private fail(code: UpdateErrorCode, detail: string): void {
    this.stepsLeft = 0;
    this.update({ phase: 'error', error: { code, detail }, progress: null });
  }

  private update(patch: Partial<Supported>): void {
    this.state = { ...this.state, ...patch };
    this.options.emit(this.snapshot());
  }

  private snapshot(): UpdateState {
    if (this.scenario === 'unsupported')
      return { supported: false, currentVersion: this.options.currentVersion };
    const s = this.state;
    return {
      ...s,
      latest: s.latest
        ? { ...s.latest, installer: s.latest.installer && { ...s.latest.installer } }
        : null,
      error: s.error && { ...s.error },
      progress: s.progress && { ...s.progress },
      download: s.download && { ...s.download },
    };
  }

  private run(intervalMs: number): void {
    if (!this.options.autoStart) return;
    this.stop();
    this.timer = setInterval(() => this.step(), intervalMs);
  }

  private stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  private loadSettings(): void {
    const key = this.options.storageKey;
    if (!key) return;
    try {
      const saved = JSON.parse(window.localStorage.getItem(key) ?? 'null') as unknown;
      if (typeof saved !== 'object' || saved === null) return;
      const { autoCheck, skippedVersion } = saved as Record<string, unknown>;
      if (typeof autoCheck === 'boolean') this.state.autoCheck = autoCheck;
      if (typeof skippedVersion === 'string') this.state.skippedVersion = skippedVersion;
    } catch {
      // Storage can be unavailable (private mode); the settings then last for the page.
    }
  }

  private saveSettings(): void {
    const key = this.options.storageKey;
    if (!key) return;
    try {
      const { autoCheck, skippedVersion } = this.state;
      window.localStorage.setItem(key, JSON.stringify({ autoCheck, skippedVersion }));
    } catch {
      // See loadSettings.
    }
  }
}
