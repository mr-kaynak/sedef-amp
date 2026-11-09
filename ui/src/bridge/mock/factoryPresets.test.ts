// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { bundledSchema } from '../../params/schema';
import { isGlobalParam, slugify } from '../../params/presetScope';
import { PEDALS } from '../../features/pedals/registry';
import { DEFAULT_PRESET_ID, FACTORY_PRESETS, parsePresetFile } from './factoryPresets';

/** The presets 0.5.0 shipped: sessions, favourites and Recent name them by these ids. */
const IDS_0_5_0 = [
  'benthic bioluminescence brine coral-fuzz glass-pocket harbor-lights holdfast mariana',
  'morning-ritual nacre riptide sea-lantern silk-road soft-focus thermocline tidewater undertow',
]
  .join(' ')
  .split(' ');

const CATEGORIES = ['Clean', 'Crunch', 'High Gain', 'Lead', 'Ambient'];

describe('factory presets', () => {
  it('bundles every preset in presets/factory with plugin-style ids', () => {
    expect(FACTORY_PRESETS.length).toBeGreaterThanOrEqual(40);
    const ids = FACTORY_PRESETS.map((p) => p.info.id);
    expect(new Set(ids).size).toBe(ids.length);
    expect(ids).toContain(DEFAULT_PRESET_ID);
    expect(FACTORY_PRESETS.every((p) => p.info.category === 'Factory' && p.info.folder)).toBe(true);
    const names = FACTORY_PRESETS.map((p) => p.info.name);
    expect(new Set(names).size).toBe(names.length);
  });

  it('keeps the ids of the presets 0.5.0 shipped, whatever they are called now', () => {
    const ids = new Set(FACTORY_PRESETS.map((p) => p.info.id));
    for (const id of IDS_0_5_0) expect(ids.has(`factory:${id}`), id).toBe(true);
    // The id is the file's own "id", which is also its file name; the name is free to change.
    const files = import.meta.glob<string>('../../../../presets/factory/*.sedefpreset', {
      query: '?raw',
      import: 'default',
      eager: true,
    });
    for (const [path, text] of Object.entries(files)) {
      const stem = path.slice(path.lastIndexOf('/') + 1).replace('.sedefpreset', '');
      expect((JSON.parse(text) as { id?: unknown }).id, path).toBe(stem);
      expect(parsePresetFile(text)?.info.id).toBe(`factory:${stem}`);
    }
    const renamed = parsePresetFile(
      '{"format":"sedef-preset","version":1,"id":"nacre","name":"Chorus Clean","parameters":{}}',
    );
    expect(renamed?.info).toMatchObject({ id: 'factory:nacre', name: 'Chorus Clean' });
  });

  it('covers every category, amp and pedal', () => {
    for (const category of CATEGORIES) {
      const count = FACTORY_PRESETS.filter((p) => p.info.folder === category).length;
      expect(count, category).toBeGreaterThanOrEqual(5);
    }
    for (const amp of [0, 1, 2])
      expect(FACTORY_PRESETS.filter((p) => p.info.amp === amp).length).toBeGreaterThanOrEqual(8);
    for (const pedal of PEDALS) {
      if (pedal.id === 'gate') continue;
      const users = FACTORY_PRESETS.filter((p) => p.info.blocks.includes(pedal.id));
      expect(users.length, pedal.id).toBeGreaterThanOrEqual(2);
    }
    // One octave pedal at a time (docs/dsp/trench.md).
    for (const p of FACTORY_PRESETS)
      expect(p.info.blocks.includes('trench') && p.info.blocks.includes('urchin')).toBe(false);
  });

  it('carries a board of exactly the pedals it switches on', () => {
    for (const p of FACTORY_PRESETS) {
      const on = PEDALS.filter(
        (pedal) => (p.values[pedal.enable] ?? (pedal.id === 'gate' ? 1 : 0)) >= 0.5,
      );
      expect([...(p.board ?? [])].sort(), p.info.name).toEqual(on.map((x) => x.id).sort());
    }
  });

  it('have a category, tags from the shared vocabulary and a one-line description', () => {
    // presets/README.md, "Categories and tags".
    const vocabulary = new Set(
      [
        'compressor overdrive boost distortion fuzz octave chorus tremolo vibrato delay slapback',
        'plate hall wah phaser flanger vibe doubler',
        'rhythm solo swell drone pad',
        'blues rock metal funk neo-soul surf rockabilly garage jazz country doom',
      ]
        .join(' ')
        .split(' ')
        .concat('edge of breakup', 'envelope filter'),
    );
    for (const { info } of FACTORY_PRESETS) {
      expect(CATEGORIES, info.name).toContain(info.folder);
      expect(info.tags.length, info.name).toBeGreaterThan(0);
      for (const tag of info.tags) expect(vocabulary.has(tag), `${info.name}: ${tag}`).toBe(true);
      expect(info.description, info.name).toMatch(/^[^\n]{20,140}$/);
      // The name says the sound: short enough for one line of the browser.
      expect(info.name.length, info.name).toBeLessThanOrEqual(24);
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
