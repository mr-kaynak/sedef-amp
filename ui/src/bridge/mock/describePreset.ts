// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows about a preset's sound, computed from its stored values with defaults
// applied. Mirrors PresetManager::describe in plugin/src/PresetManager.cpp.
import type { ParamSpec, PresetBlock, PresetInfo } from '../protocol';
import { snapValue } from '../../params/normalise';
import { AMP_NAMES } from '../../params/schema';
import { legacyBoardFor, pedalMeta } from '../../features/pedals/registry';

type Described = Pick<PresetInfo, 'amp' | 'cab' | 'blocks'>;

const EQ_BANDS = [1, 2, 3, 4, 5, 6, 7, 8, 9];

/** `pedals`: the preset's board; without one, the board it gets (legacyBoardFor). */
export function describePreset(
  stored: Record<string, number>,
  specs: Record<string, ParamSpec>,
  pedals?: readonly string[],
): Described {
  const value = (id: string) => {
    const spec = specs[id];
    if (!spec) return 0;
    const raw = stored[id];
    return raw === undefined ? spec.default : snapValue(spec, raw);
  };
  const on = (id: string) => value(id) >= 0.5;

  const amp = Math.round(value('ampModel'));
  const cab = on('ampCabLink') ? amp : Math.round(value('cabModel'));
  const bank = `eq${AMP_NAMES[amp] ?? 'Reef'}`;
  const eqShapes =
    value(`${bank}LowCut`) > 20.5 ||
    value(`${bank}HighCut`) < 19990 ||
    EQ_BANDS.some((b) => Math.abs(value(`${bank}Band${b}`)) > 0.05);

  // The pedals on its board that it switches on, in the board's order, then the post blocks.
  const board = pedals ?? legacyBoardFor(on);
  const blocks: PresetBlock[] = board.filter((id) => {
    const meta = pedalMeta(id);
    return meta !== undefined && on(meta.enable);
  });
  const add = (id: string, block: PresetBlock) => {
    if (on(id)) blocks.push(block);
  };
  if (eqShapes) add(`${bank}Enabled`, 'eq');
  add('doublerEnabled', 'doubler');
  add('driftEnabled', 'drift');
  add('fathomEnabled', 'fathom');
  return { amp, cab, blocks };
}
