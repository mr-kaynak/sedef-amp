// SPDX-License-Identifier: AGPL-3.0-or-later
// The mock backend's update checker (plugin/src/update/UpdateService.cpp): scripted scenarios for
// the browser preview (?update=...) and for tests, which advance checks, downloads and installs
// with step().
import type {
  InstallStep,
  ReleaseInfo,
  UpdateErrorCode,
  UpdatePlatform,
  UpdateState,
} from '../protocol';

export const UPDATE_SCENARIOS = [
  'current',
  'available',
  'skipped',
  'error',
  'noinstaller',
  'mismatch',
  'unsupported',
  /** The password dialog is cancelled: back to ready. */
  'declined',
  /** The OS's installer fails: Install manually. */
  'installfail',
  /** This copy runs from a folder the installer doesn't update: Install manually only. */
  'elsewhere',
  /** Another window or DAW installed it: restart to use it. */
  'installed',
  /** The standalone app, which restarts itself after installing. */
  'standalone',
  /** Windows: Setup waits for the host to exit. */
  'windows',
  'scheduled',
  /** Linux, in your home folder: no password. */
  'linux',
] as const;
export type UpdateScenario = (typeof UPDATE_SCENARIOS)[number];

export function isUpdateScenario(value: unknown): value is UpdateScenario {
  return typeof value === 'string' && (UPDATE_SCENARIOS as readonly string[]).includes(value);
}

type Supported = Extract<UpdateState, { supported: true }>;

const RELEASES_URL = 'https://github.com/mr-kaynak/sedef-amp/releases';
const HOUR = 60 * 60 * 1000;
const DOWNLOAD_STEPS = 8;

const INSTALLERS: Record<UpdatePlatform, string> = {
  macOS: 'Sedef-Amp-macOS.dmg',
  Windows: 'Sedef-Amp-Windows-Setup.exe',
  Linux: 'Sedef-Amp-Linux-x86_64.tar.gz',
};

/** What each OS's installer goes through (plugin/src/update/Installer.cpp). */
const INSTALL_STEPS: Record<UpdatePlatform, InstallStep[]> = {
  macOS: ['preparing', 'permission', 'finishing'],
  Windows: ['preparing', 'permission'], // then Setup waits for the host
  Linux: ['preparing', 'installing', 'finishing'],
};

/** How long each phase takes in the browser preview. */
const DELAY_MS: Record<string, number> = {
  checking: 900,
  downloading: 140,
  verifying: 500,
  preparing: 700,
  permission: 2400,
  installing: 1200,
  finishing: 600,
};

const NOTES = `Updates that install themselves, and a calmer tuner.

### Added

- **Update now:** Sedef downloads the new version, checks it and installs it through your system's own installer. You only enter your password. See [how it works](https://github.com/mr-kaynak/sedef-amp/blob/main/docs/updates.md).
- Installers for every OS: \`Sedef-Amp-macOS.dmg\`, \`Sedef-Amp-Windows-Setup.exe\` and a Linux archive.

### Fixed

- The tuner no longer reads sharp on the first pluck after a long silence.
- Drift's tape drive keeps its level when you change the head pattern.

**Full changelog:** https://github.com/mr-kaynak/sedef-amp/compare/v0.1.0...v0.2.0`;

function platformOf(scenario: UpdateScenario): UpdatePlatform {
  if (scenario === 'windows' || scenario === 'scheduled') return 'Windows';
  if (scenario === 'linux') return 'Linux';
  return 'macOS';
}

function initialPhase(scenario: UpdateScenario): Supported['phase'] {
  if (scenario === 'error') return 'error';
  if (scenario === 'installed') return 'installed';
  if (scenario === 'scheduled') return 'scheduled';
  return 'idle';
}

function release(version: string, installer: string | null): ReleaseInfo {
  return {
    version,
    tag: `v${version}`,
    name: `Sedef Amp ${version}`,
    notes: NOTES,
    url: `${RELEASES_URL}/tag/v${version}`,
    publishedAt: '2026-11-20T10:00:00Z',
    installer: installer ? { name: installer, size: 48_600_000 } : null,
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
  private timer: ReturnType<typeof setTimeout> | null = null;
  /** The running download ends in Install manually rather than the OS's installer. */
  private manual = false;
  /** Where a check returns to: a restart that waits stays waiting. */
  private afterCheck: Supported['phase'] = 'idle';

  constructor(private readonly options: UpdateSimOptions) {
    this.scenario = options.scenario ?? 'current';
    const s = this.scenario;
    const platform = platformOf(s);
    const latest =
      s === 'current'
        ? release('0.1.0', INSTALLERS[platform])
        : release('0.2.0', s === 'noinstaller' ? null : INSTALLERS[platform]);
    const standalone = s === 'standalone';
    this.state = {
      supported: true,
      currentVersion: options.currentVersion,
      platform,
      autoCheck: true,
      autoCheckAllowed: true,
      skippedVersion: s === 'skipped' ? '0.2.0' : '',
      lastChecked: s === 'error' ? null : Date.now() - 3 * HOUR,
      phase: initialPhase(s),
      latest: s === 'error' ? null : latest,
      updateAvailable: s !== 'current' && s !== 'error',
      error: s === 'error' ? { code: 'network', detail: 'could not connect' } : null,
      progress: null,
      install: null,
      installedVersion: s === 'installed' || s === 'scheduled' ? '0.2.0' : null,
      host: standalone ? 'Sedef' : 'your DAW',
      standalone,
      canRestart: false,
      autoInstall:
        s === 'elsewhere'
          ? { available: false, reason: 'elsewhere' }
          : { available: true, reason: null },
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
    const { phase } = this.state;
    this.afterCheck = phase === 'installed' || phase === 'scheduled' ? phase : 'idle';
    this.update({ phase: 'checking', error: null });
    this.run();
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

  install(): UpdateState {
    if (!this.state.autoInstall.available) return this.installManually();
    const { phase } = this.state;
    if (phase === 'installed' || phase === 'scheduled') return this.snapshot();
    return this.startDownload(false);
  }

  installManually(): UpdateState {
    return this.startDownload(true);
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

  /** The standalone app quits and opens the new version: here, the page simply runs it. */
  restart(): UpdateState {
    const { canRestart, installedVersion } = this.state;
    if (canRestart && installedVersion)
      this.update({
        phase: 'idle',
        currentVersion: installedVersion,
        installedVersion: null,
        updateAvailable: false,
        canRestart: false,
      });
    return this.snapshot();
  }

  // --- Test helpers ---------------------------------------------------------------------------

  /** Advances a running check, download or install by one step. */
  step(): void {
    const { phase, progress, latest, platform, install } = this.state;
    if (phase === 'checking') {
      if (this.scenario === 'error') this.fail('network', 'could not connect');
      else this.update({ phase: this.afterCheck, lastChecked: Date.now() });
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
      else if (this.manual)
        this.update({
          phase: 'done',
          progress: null,
          download: { fileName: latest?.installer?.name ?? 'installer', opened: true },
        });
      else this.update({ phase: 'installing', progress: null, install: { step: 'preparing' } });
    } else if (phase === 'installing' && install) {
      this.stepInstall(install.step, platform, latest);
    }
  }

  dispose(): void {
    this.stop();
  }

  // --- Internals --------------------------------------------------------------------------------

  private stepInstall(step: InstallStep, platform: UpdatePlatform, latest: ReleaseInfo | null) {
    if (step === 'permission' && this.scenario === 'declined') {
      this.update({ phase: 'idle', install: null }); // not an error: ready to try again
      return;
    }
    if (step === 'permission' && this.scenario === 'installfail') {
      this.fail('installFailed', 'installer: The install failed.');
      return;
    }
    const steps = INSTALL_STEPS[platform];
    const next = steps[steps.indexOf(step) + 1];
    if (next) {
      this.update({ install: { step: next } });
      return;
    }
    this.update({
      phase: platform === 'Windows' ? 'scheduled' : 'installed',
      install: null,
      installedVersion: latest?.version ?? null,
      canRestart: this.state.standalone,
    });
  }

  private busy(): boolean {
    const p = this.state.phase;
    return p === 'checking' || p === 'downloading' || p === 'verifying' || p === 'installing';
  }

  private startDownload(manual: boolean): UpdateState {
    const latest = this.state.latest;
    if (this.busy() || !latest) return this.snapshot();
    if (!latest.installer) {
      this.fail('noInstaller', `${latest.tag} has no ${INSTALLERS[this.state.platform]}`);
      return this.snapshot();
    }
    this.manual = manual;
    this.update({
      phase: 'downloading',
      error: null,
      download: null,
      progress: { received: 0, total: latest.installer.size },
    });
    this.run();
    return this.snapshot();
  }

  private fail(code: UpdateErrorCode, detail: string): void {
    this.update({ phase: 'error', error: { code, detail }, progress: null, install: null });
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
      install: s.install && { ...s.install },
      autoInstall: { ...s.autoInstall },
      download: s.download && { ...s.download },
    };
  }

  /** In the browser, steps on a timer until the job is done. */
  private run(): void {
    if (!this.options.autoStart) return;
    this.stop();
    const { phase, install } = this.state;
    const key = phase === 'installing' ? (install?.step ?? '') : phase;
    this.timer = setTimeout(() => {
      this.timer = null;
      this.step();
      if (this.busy()) this.run();
    }, DELAY_MS[key] ?? 500);
  }

  private stop(): void {
    if (this.timer) clearTimeout(this.timer);
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
