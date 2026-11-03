// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { bundledSchema } from '../../params/schema';
import { isGlobalParam, slugify } from '../../params/presetScope';
import { DEFAULT_PRESET_ID, FACTORY_PRESETS, parsePresetFile } from './factoryPresets';

describe('factory presets', () => {
  it('bundles every preset in presets/factory with plugin-style ids', () => {
    expect(FACTORY_PRESETS).toHaveLength(14);
    const ids = FACTORY_PRESETS.map((p) => p.info.id);
    expect(new Set(ids).size).toBe(ids.length);
    expect(ids).toContain(DEFAULT_PRESET_ID);
    expect(ids).toContain('factory:coral-fuzz');
    expect(FACTORY_PRESETS.every((p) => p.info.category === 'Factory' && p.info.folder)).toBe(true);
  });

  it('have a category, tags from the shared vocabulary and a one-line description', () => {
    // presets/README.md, "Categories and tags".
    const categories = ['Clean', 'Crunch', 'High Gain', 'Lead', 'Ambient'];
    const vocabulary = new Set(
      [
        'compressor overdrive boost distortion chorus tremolo vibrato delay slapback plate hall',
        'wah phaser flanger vibe',
        'rhythm solo swell drone blues rock metal funk neo-soul surf rockabilly garage',
      ]
        .join(' ')
        .split(' ')
        .concat('edge of breakup', 'envelope filter'),
    );
    for (const { info } of FACTORY_PRESETS) {
      expect(categories, info.name).toContain(info.folder);
      expect(info.tags.length, info.name).toBeGreaterThan(0);
      for (const tag of info.tags) expect(vocabulary.has(tag), `${info.name}: ${tag}`).toBe(true);
      expect(info.description, info.name).toMatch(/^[^\n]{20,140}$/);
    }
  });

  it('only stores known, non-global parameters', () => {
    const known = new Set(bundledSchema.map((s) => s.id));
    for (const p of FACTORY_PRESETS) {
      for (const id of Object.keys(p.values)) {
        expect(known.has(id), `${p.info.name}: ${id}`).toBe(true);
        expect(isGlobalParam(id)).toBe(false);
      }
    }
  });

  it('rejects files that are not presets and drops global values', () => {
    expect(parsePresetFile('not json')).toBeNull();
    expect(parsePresetFile('{"format":"other","name":"x","parameters":{}}')).toBeNull();
    const p = parsePresetFile(
      '{"format":"sedef-preset","version":1,"name":"Test Me","parameters":{"inputGain":6,"reefGain":0.3}}',
    );
    expect(p?.info.id).toBe('factory:test-me');
    expect(p?.values).toEqual({ reefGain: 0.3 });
  });
});

describe('slugify (mirrors PresetManager::slugify)', () => {
  it('lowercases, drops other characters, trims, and turns spaces into dashes', () => {
    expect(slugify('Coral Fuzz')).toBe('coral-fuzz');
    expect(slugify("Rock 'n' Roll!")).toBe('rock-n-roll');
    expect(slugify('  Late-Night  Set ')).toBe('late-night--set');
    expect(slugify('Ümlaut 2')).toBe('mlaut-2');
  });
});
