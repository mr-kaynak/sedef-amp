// SPDX-License-Identifier: AGPL-3.0-or-later
// Words and numbers for the Updates panel: error messages in plain language, install advice per
// OS, sizes and dates.
import type {
  InstallStep,
  ManualReason,
  UpdateErrorCode,
  UpdatePlatform,
} from '../../bridge/protocol';
import type { SupportedUpdateState } from './updateStore';

export function errorMessage(code: UpdateErrorCode, platform: UpdatePlatform): string {
  switch (code) {
    case 'network':
      return "Sedef couldn't reach GitHub. Check your internet connection and try again.";
    case 'rateLimited':
      return 'GitHub is limiting requests from your network right now. Try again in an hour.';
    case 'badResponse':
      return "GitHub's answer didn't make sense to Sedef. Try again later, or get the update from the releases page.";
    case 'noInstaller':
      return `This release has no installer for ${platform} on this computer. Download it from the releases page instead.`;
    case 'noChecksum':
      return "This release doesn't publish a checksum for its installer, so Sedef won't install it. Download it from the releases page instead.";
    case 'checksumMismatch':
      return "The download didn't match its published checksum, so Sedef deleted it. Try again, or download from the releases page.";
    case 'incomplete':
      return 'The download stopped before it finished. Try again.';
    case 'disk':
      return "Sedef couldn't save the installer. Check that your disk has free space and try again.";
    case 'openFailed':
      return "The installer is downloaded and verified, but Sedef couldn't open it. You'll find it in your Downloads folder.";
    case 'badPackage':
      return "The downloaded installer doesn't look like Sedef's, so Sedef didn't install it. Try again, or use Install manually.";
    case 'elevationFailed':
      return ELEVATION_FAILED[platform];
    case 'installFailed':
      return 'The installer reported a problem and stopped. Try again, or use Install manually.';
    case 'unverified':
      return "The installer finished, but the installed files aren't the new version. Try again, or use Install manually.";
  }
}

const ELEVATION_FAILED: Record<UpdatePlatform, string> = {
  macOS: "Sedef couldn't ask for your password here. Use Install manually instead.",
  Windows: "Windows didn't start the installer. Use Install manually instead.",
  Linux:
    "Sedef couldn't ask for your password: no authentication agent is running. Use Install manually instead.",
};

/** Shown next to "Update now": what happens, and who asks for what. */
export function beforeInstall(state: SupportedUpdateState): string {
  switch (state.platform) {
    case 'macOS':
      return 'Sedef downloads the update, checks it and installs it over the formats you have. macOS asks for your password first.';
    case 'Windows':
      return state.standalone
        ? 'Sedef downloads the update and checks it. Once Windows gives permission, it installs when you quit Sedef, and Sedef opens again.'
        : `Sedef downloads the update and checks it. Once Windows gives permission, it installs when you close ${state.host}.`;
    case 'Linux':
      return 'Sedef downloads the update, checks it and installs it over your current copy. For a copy installed for every user, your system asks for your password.';
  }
}

/** What Install manually does, on this OS. */
export function installManuallyNote(platform: UpdatePlatform): string {
  return MANUAL_ACTION[platform];
}

/** Why only Install manually is offered, then what it does. */
export function manualOnly(reason: ManualReason | null, state: SupportedUpdateState): string {
  const why =
    reason === 'elsewhere'
      ? "This copy of Sedef runs from a folder the installer doesn't update, so Sedef can't install the update for you."
      : reason === 'sandboxed'
        ? `Plug-ins can't ask for your password in ${state.host}, so Sedef can't install the update for you.`
        : reason === 'noElevation'
          ? "Sedef is installed for every user, and there's no way to ask for your password here (pkexec isn't available), so Sedef can't install the update for you."
          : "Sedef can't install updates for you on this system.";
  return `${why} ${MANUAL_ACTION[state.platform]}`;
}

const MANUAL_ACTION: Record<UpdatePlatform, string> = {
  macOS: 'Install manually saves the disk image to your Downloads folder and opens it.',
  Windows: 'Install manually saves the installer to your Downloads folder and starts it.',
  Linux: 'Install manually saves the archive to your Downloads folder and shows it.',
};

/** The installing step's progress label and the line under it. */
export function installStep(
  step: InstallStep,
  platform: UpdatePlatform,
  version: string,
): { label: string; note: string } {
  const keepOpen = 'Keep this window open until it finishes.';
  switch (step) {
    case 'preparing':
      return {
        label: `Preparing Sedef Amp ${version}…`,
        note: 'Checking the download once more before it installs.',
      };
    case 'permission':
      if (platform === 'Windows')
        return {
          label: 'Waiting for permission…',
          note: 'Choose Yes when Windows asks whether the installer may make changes.',
        };
      return {
        label: 'Waiting for your password…',
        note: `Enter your password when ${platform === 'macOS' ? 'macOS' : 'your system'} asks. Sedef Amp ${version} installs right after. ${keepOpen}`,
      };
    case 'installing':
      return { label: `Installing Sedef Amp ${version}…`, note: keepOpen };
    case 'finishing':
      return {
        label: 'Checking the installed files…',
        note: `Making sure every format has version ${version}.`,
      };
  }
}

/** Installed, or (Windows) waiting for the host to exit: the headline and what's left to do. */
export function restartText(state: SupportedUpdateState): {
  headline: string;
  status: string;
  note: string;
} {
  const version = state.installedVersion ?? '';
  if (state.phase === 'scheduled')
    return {
      headline: `Sedef Amp ${version} is ready to install`,
      status: state.standalone
        ? 'It installs when you quit Sedef, and Sedef opens again.'
        : `It installs when you close ${state.host}.`,
      note: 'If another program still has Sedef open, Windows finishes the update when you restart your computer.',
    };
  return {
    headline: `Sedef Amp ${version} is installed`,
    status: `Restart ${state.host} to use it.`,
    note: state.standalone
      ? 'Your DAWs load the new version the next time they start.'
      : 'Any other DAW with Sedef open keeps the old version until you restart it too.',
  };
}

/**
 * "Install manually", once the installer is open. The copy carries the OS's downloaded-file mark,
 * so the OS asks before the installer runs, and stops an unsigned one
 * (docs/releasing.md#signing-status).
 */
export const AFTER_DOWNLOAD: Record<UpdatePlatform, string> = {
  macOS:
    "The disk image is open. Quit your DAW and run the installer inside it. If macOS says it can't open the installer, go to System Settings → Privacy & Security and click Open Anyway. Then start your DAW again.",
  Windows:
    "The installer is starting. If Windows says it protected your PC, click More info, then Run anyway. Save your project and quit your DAW before you click Install; if it's still open, the installer offers to close it, or finishes the update when you restart Windows. Start your DAW again afterwards.",
  Linux:
    'The archive is in your file manager. Quit your DAW, extract it and run install.sh (its README has the details), then start your DAW again.',
};

export function formatBytes(bytes: number): string {
  if (bytes >= 1e6) return `${(bytes / 1e6).toFixed(1)} MB`;
  if (bytes >= 1e3) return `${Math.round(bytes / 1e3)} kB`;
  return `${bytes} B`;
}

/** "20 Nov 2026"; empty for a missing or broken date. */
export function formatDate(iso: string): string {
  const t = Date.parse(iso);
  if (!iso || Number.isNaN(t)) return '';
  return new Intl.DateTimeFormat('en-GB', {
    day: 'numeric',
    month: 'short',
    year: 'numeric',
  }).format(t);
}

/** "just now", "5 minutes ago", "3 hours ago", "yesterday", then a date. */
export function formatSince(time: number | null, now: number = Date.now()): string {
  if (time === null) return 'never';
  const minutes = Math.floor((now - time) / 60_000);
  if (minutes < 1) return 'just now';
  if (minutes < 60) return minutes === 1 ? '1 minute ago' : `${minutes} minutes ago`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return hours === 1 ? '1 hour ago' : `${hours} hours ago`;
  if (hours < 48) return 'yesterday';
  return formatDate(new Date(time).toISOString());
}
