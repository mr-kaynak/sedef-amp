// SPDX-License-Identifier: AGPL-3.0-or-later
// Words and numbers for the Updates panel: error messages in plain language, install advice per
// OS, sizes and dates.
import type { UpdateErrorCode, UpdatePlatform } from '../../bridge/protocol';

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
      return "This release doesn't publish a checksum for its installer, so Sedef won't open it. Download it from the releases page instead.";
    case 'checksumMismatch':
      return "The download didn't match its published checksum, so Sedef deleted it. Try again, or download from the releases page.";
    case 'incomplete':
      return 'The download stopped before it finished. Try again.';
    case 'disk':
      return "Sedef couldn't save the installer in your Downloads folder. Check that there's free space and try again.";
    case 'openFailed':
      return "The installer is downloaded and verified, but Sedef couldn't open it. You'll find it in your Downloads folder.";
  }
}

/** Shown next to "Download & install". */
export const BEFORE_INSTALL: Record<UpdatePlatform, string> = {
  macOS:
    'Sedef downloads the disk image, checks it and opens it. Quit your DAW before you run the installer, and restart it afterwards.',
  Windows:
    'Sedef downloads the installer, checks it and starts it. Quit your DAW before you click Install (Windows keeps loaded plug-ins locked), and restart it afterwards.',
  Linux:
    'Sedef downloads the archive, checks it and shows it in your file manager. Quit your DAW before you install, and restart it afterwards.',
};

/** Shown once the installer is open. */
export const AFTER_DOWNLOAD: Record<UpdatePlatform, string> = {
  macOS:
    'The disk image is open. Quit your DAW, run the installer inside it, then start your DAW again.',
  Windows:
    "The installer is running. Quit your DAW before you click Install: Windows can't replace plug-ins your DAW has loaded. Start your DAW again afterwards.",
  Linux:
    'The archive is in your file manager. Quit your DAW, install the plug-ins as its README describes, then start your DAW again.',
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
