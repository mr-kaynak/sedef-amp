// SPDX-License-Identifier: AGPL-3.0-or-later
// The real factory presets (presets/factory/*.sedefpreset), bundled at build time so the browser
// mock offers exactly what the plugin ships. Ids follow PresetManager: "factory:<slug>".
import type { PresetInfo } from '../protocol';
import { isGlobalParam, slugify } from '../../params/presetScope';
import { bundledSchema, indexSchema } from '../../params/schema';
import { describePreset } from './describePreset';
import { sanitiseBoard } from '../../features/pedals/registry';

export interface MockPreset {
  info: PresetInfo;
  /** Stored parameters in real units; anything missing is the schema default. */
  values: Record<string, number>;
  /** Its pedalboard; absent in presets saved before the board (they get legacyBoardFor). */
  board?: string[];
}

interface PresetFile {
  format?: unknown;
  name?: unknown;
  folder?: unknown;
  author?: unknown;
  tags?: unknown;
  description?: unknown;
  parameters?: unknown;
  board?: unknown;
}

const SPECS = indexSchema(bundledSchema);

/** PresetInfo for stored values: metadata from the file, amp/cab/blocks computed. */
export function presetInfo(
  base: Omit<PresetInfo, 'amp' | 'cab' | 'blocks'>,
  values: Record<string, number>,
  board?: readonly string[],
): PresetInfo {
  return { ...base, ...describePreset(values, SPECS, board) };
}

/** Parses a .sedefpreset file; null when it is not a valid preset. */
export function parsePresetFile(json: string): MockPreset | null {
  let file: PresetFile;
  try {
    file = JSON.parse(json) as PresetFile;
  } catch {
    return null;
  }
  if (file.format !== 'sedef-preset' || typeof file.name !== 'string') return null;
  if (!file.parameters || typeof file.parameters !== 'object') return null;
  const values: Record<string, number> = {};
  for (const [id, value] of Object.entries(file.parameters as Record<string, unknown>)) {
    if (typeof value === 'number' && Number.isFinite(value) && !isGlobalParam(id))
      values[id] = value;
  }
  const board = Array.isArray(file.board) ? sanitiseBoard(file.board) : undefined;
  return {
    info: presetInfo(
      {
        id: `factory:${slugify(file.name)}`,
        name: file.name,
        category: 'Factory',
        folder: typeof file.folder === 'string' ? file.folder : '',
        author: typeof file.author === 'string' ? file.author : '',
        tags: Array.isArray(file.tags)
          ? file.tags.filter((t): t is string => typeof t === 'string')
          : [],
        favorite: false,
        description: typeof file.description === 'string' ? file.description : '',
      },
      values,
      board,
    ),
    values,
    ...(board ? { board } : {}),
  };
}

const files = import.meta.glob<string>('../../../../presets/factory/*.sedefpreset', {
  query: '?raw',
  import: 'default',
  eager: true,
});

export const FACTORY_PRESETS: MockPreset[] = Object.values(files)
  .map(parsePresetFile)
  .filter((p): p is MockPreset => p !== null);

export const DEFAULT_PRESET_ID = 'factory:nacre';

const SUNDAY = { ampModel: 1, reefGain: 0.46, driftEnabled: 1, driftMix: 0.2, fathomEnabled: 1 };

/** One user preset so the User and Favourites views have something to show. */
export const USER_PRESETS: MockPreset[] = [
  {
    info: presetInfo(
      {
        id: 'user:Live/Sunday set.sedefpreset',
        name: 'Sunday set',
        category: 'User',
        folder: 'Live',
        author: 'You',
        tags: ['worship', 'delay'],
        favorite: true,
        description: 'Warm Reef crunch with a quarter-note echo for the band.',
      },
      SUNDAY,
    ),
    values: SUNDAY,
  },
];
