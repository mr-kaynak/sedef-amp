// SPDX-License-Identifier: AGPL-3.0-or-later
// What the browser shows about a preset's sound, computed from its stored values with defaults
// applied. Mirrors PresetManager::describe in plugin/src/PresetManager.cpp.
import type { ParamSpec, PresetBlock, PresetInfo } from '../protocol';
import { snapValue } from '../../params/normalise';
import { AMP_NAMES } from '../../params/schema';

type Described = Pick<PresetInfo, 'amp' | 'cab' | 'blocks'>;

const EQ_BANDS = [1, 2, 3, 4, 5, 6, 7, 8, 9];

export function describePreset(
  stored: Record<string, number>,
  specs: Record<string, ParamSpec>,
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

  const blocks: PresetBlock[] = [];
  const add = (id: string, block: PresetBlock) => {
    if (on(id)) blocks.push(block);
  };
  add('gateEnabled', 'gate');
  add('trenchEnabled', 'trench');
  add('tideEnabled', 'tide');
  add('urchinEnabled', 'urchin');
  add('pearlEnabled', 'pearl');
  add('kelpEnabled', 'kelp');
  add('coralEnabled', 'coral');
  add('rippleEnabled', 'ripple');
  if (eqShapes) add(`${bank}Enabled`, 'eq');
  add('doublerEnabled', 'doubler');
  add('driftEnabled', 'drift');
  add('fathomEnabled', 'fathom');
  return { amp, cab, blocks };
}
