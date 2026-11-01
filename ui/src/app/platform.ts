// SPDX-License-Identifier: AGPL-3.0-or-later
// The command key as shown and as announced: ⌘ on macOS, Ctrl elsewhere.

export function isMac(): boolean {
  if (typeof navigator === 'undefined') return false;
  return /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);
}

export function modKey(): { symbol: string; aria: 'Meta' | 'Control' } {
  return isMac() ? { symbol: '⌘', aria: 'Meta' } : { symbol: 'Ctrl+', aria: 'Control' };
}
