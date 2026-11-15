// SPDX-License-Identifier: AGPL-3.0-or-later
// Times, rates and channel counts as the recorder panel writes and says them.

/** 0:42, 12:05, 1:02:09; with tenths 0:42.3. */
export function clock(seconds: number, tenths = false): string {
  const s = Math.max(0, seconds);
  const whole = tenths ? Math.floor(s * 10) / 10 : Math.floor(s);
  const h = Math.floor(whole / 3600);
  const m = Math.floor((whole % 3600) / 60);
  const sec = whole % 60;
  const secText = tenths
    ? sec.toFixed(1).padStart(4, '0')
    : String(Math.floor(sec)).padStart(2, '0');
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${secText}` : `${m}:${secText}`;
}

/** "1 minute 23 seconds", "45 seconds", "2 hours 3 minutes": for screen readers. */
export function spoken(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const part = (n: number, one: string) => `${n} ${one}${n === 1 ? '' : 's'}`;
  if (h > 0) return m > 0 ? `${part(h, 'hour')} ${part(m, 'minute')}` : part(h, 'hour');
  if (m > 0) return sec > 0 ? `${part(m, 'minute')} ${part(sec, 'second')}` : part(m, 'minute');
  return part(sec, 'second');
}

/** 44.1 kHz, 48 kHz, 96 kHz. */
export function rate(hz: number): string {
  const k = hz / 1000;
  return `${Number.isInteger(k) ? k : k.toFixed(1)} kHz`;
}

export function channels(n: number): string {
  return n === 1 ? 'mono' : n === 2 ? 'stereo' : `${n} channels`;
}

/** A fraction as a whole percentage, written like the rest of the app ("40%"). */
export function percent(fraction: number): string {
  return `${Math.round(fraction * 100)}%`;
}
