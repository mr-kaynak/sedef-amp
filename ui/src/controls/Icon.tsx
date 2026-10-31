// SPDX-License-Identifier: AGPL-3.0-or-later
// Inline stroke icons on a 16px grid. Kept in one place so line weight stays consistent.

const PATHS = {
  prev: 'M10 3.5 5.5 8 10 12.5',
  next: 'M6 3.5 10.5 8 6 12.5',
  down: 'M4 6.5 8 10.5l4-4',
  close: 'M4 4l8 8M12 4l-8 8',
  search: 'M7 12A5 5 0 1 0 7 2a5 5 0 0 0 0 10zM10.6 10.6 14 14',
  star: 'M8 1.8l1.9 3.9 4.3.6-3.1 3 .7 4.3L8 11.6l-3.8 2 .7-4.3-3.1-3 4.3-.6z',
  fork: 'M5.5 1.5v6a2.5 2.5 0 0 0 5 0v-6M8 10v4.5M6 14.5h4',
  ruler: 'M1.5 10.5 10.5 1.5l4 4-9 9zM4 8l1.5 1.5M6 6l2 2M8 4l1.5 1.5',
  metronome: 'M5.5 1.5h5l3 13h-11zM8 11 11.5 3.5M4.2 11h7.6',
  sliders:
    'M2 4h7M12 4h2M2 12h2M7 12h7M9 2.5v3M12 2.5v3M4 10.5v3M7 10.5v3M2 8h4M9 8h5M6 6.5v3M9 6.5v3',
  play: 'M5 3.2v9.6L12.5 8z',
  stop: 'M4.5 4.5h7v7h-7z',
  link: 'M6.8 9.2l2.4-2.4M7.5 4.6l1.1-1.1a2.6 2.6 0 0 1 3.7 3.7l-1.1 1.1M8.5 11.4l-1.1 1.1a2.6 2.6 0 0 1-3.7-3.7l1.1-1.1',
  unlink:
    'M7.5 4.6l1.1-1.1a2.6 2.6 0 0 1 3.7 3.7l-1.1 1.1M8.5 11.4l-1.1 1.1a2.6 2.6 0 0 1-3.7-3.7l1.1-1.1M2 2l12 12',
  folder: 'M1.5 4.5v8h13v-7H8L6.5 4z',
  plus: 'M8 3v10M3 8h10',
  copy: 'M5.5 5.5h8v8h-8zM10.5 5.5v-3h-8v8h3',
  check: 'M3 8.5l3.2 3L13 4.5',
  refresh: 'M13 8a5 5 0 1 1-1.5-3.6M13 2.5v2.5h-2.5',
  file: 'M4 1.5h5.5L12.5 4.5v10H4zM9.5 1.5v3h3',
  info: 'M8 14.5a6.5 6.5 0 1 0 0-13 6.5 6.5 0 0 0 0 13zM8 7.2V11M8 4.8v.4',
} as const;

export type IconName = keyof typeof PATHS;

export function Icon({
  name,
  size = 16,
  filled = false,
  className,
}: {
  name: IconName;
  size?: number;
  filled?: boolean;
  className?: string;
}) {
  return (
    <svg
      className={className}
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill={filled ? 'currentColor' : 'none'}
      stroke="currentColor"
      strokeWidth={1.4}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      focusable="false"
    >
      <path d={PATHS[name]} />
    </svg>
  );
}
