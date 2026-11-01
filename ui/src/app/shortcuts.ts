// SPDX-License-Identifier: AGPL-3.0-or-later
// The keyboard shortcuts, as the shortcut list shows them. useShortcuts.ts and the preset browser
// implement them; keep the three in step.
import { modKey } from './platform';

export interface Shortcut {
  /** Key caps; alternatives are separate entries in `keys`, shown with "or". */
  keys: string[];
  label: string;
}

export function shortcuts(): { title: string; items: Shortcut[] }[] {
  const mod = modKey().symbol;
  return [
    {
      title: 'Presets',
      items: [
        { keys: ['P'], label: 'Open or close the preset browser' },
        { keys: ['/', `${mod}F`], label: 'Search presets' },
        { keys: ['[', ']'], label: 'Previous or next preset' },
        { keys: ['A', 'B'], label: 'Compare slot A or B' },
        { keys: [`${mod}S`], label: 'Save (Save as for factory presets)' },
        { keys: [`⇧${mod}S`], label: 'Save as' },
        { keys: [`${mod}Z`], label: 'Undo a load or delete while its notice shows' },
      ],
    },
    {
      title: 'In the browser',
      items: [
        { keys: ['↑', '↓'], label: 'Audition the previous or next preset' },
        { keys: ['↵'], label: 'Keep it and close' },
        { keys: ['Esc'], label: 'Close and go back to where you started' },
        { keys: ['F'], label: 'Favourite (in the list)' },
      ],
    },
    {
      title: 'Rig',
      items: [{ keys: ['1–5'], label: 'Pedals, Amp, EQ, Cab, FX' }],
    },
  ];
}
