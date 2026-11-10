// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, describe, expect, it, vi } from 'vitest';
import { MockBackend } from './mockBackend';
import { PROTOCOL_VERSION, type PitchEvent, type PresetState } from './protocol';
import { flush } from '../test/harness';

let mock: MockBackend;
afterEach(() => mock?.dispose());

describe('MockBackend', () => {
  it('answers the handshake with the real schema and a loaded preset', async () => {
    mock = new MockBackend();
    const hello = await mock.hello();
    expect(hello.protocol).toBe(PROTOCOL_VERSION);
    expect(hello.schema).toHaveLength(203);
    expect(Object.keys(hello.values)).toHaveLength(203);
    expect(hello.presets.currentName).toBe('Glassy Chorus');
    expect(hello.presets.currentId).toBe('factory:nacre');
    expect(
      hello.presets.items.filter((p) => p.category === 'Factory').length,
    ).toBeGreaterThanOrEqual(40);
    expect(hello.presets.dirty).toBe(false);
    expect(hello.ir.slots).toHaveLength(6);
  });

  it('rejects a mismatched protocol', async () => {
    mock = new MockBackend();
    await expect(mock.call('sedef_hello', 99)).rejects.toThrow();
  });

  it('follows the plugin preset rules: globals never dirty, reverting clears dirty', async () => {
    mock = new MockBackend();
    await mock.call('sedef_setParam', 'inputGain', 6);
    await mock.call('sedef_setParam', 'tempo', 90);
    expect((await mock.call('sedef_presets_list')).dirty).toBe(false);
    await mock.call('sedef_setParam', 'lagoonVolume', 0.8);
    expect((await mock.call('sedef_presets_list')).dirty).toBe(true);
    await mock.call('sedef_setParam', 'lagoonVolume', 0.45); // Glassy Chorus's stored value
    expect((await mock.call('sedef_presets_list')).dirty).toBe(false);
  });

  it('keeps global parameters when loading a preset', async () => {
    mock = new MockBackend();
    await mock.call('sedef_setParam', 'outputGain', -6);
    await mock.call('sedef_presets_load', 'factory:brine');
    expect(mock.value('outputGain')).toBe(-6);
    expect(mock.value('reefVoice')).toBe(2);
    // Missing parameters fall back to the schema default.
    expect(mock.value('lagoonVolume')).toBe(0.5);
  });

  it('applies demo overrides as part of the initial preset', async () => {
    mock = new MockBackend({ overrides: { pearlEnabled: 1, pearlGain: 0.8 } });
    const hello = await mock.hello();
    expect(hello.values.pearlGain).toBe(0.8);
    expect(hello.presets.dirty).toBe(false);
  });

  it('stores values in real units, snapped and clamped', async () => {
    mock = new MockBackend();
    await mock.call('sedef_setParam', 'transpose', 3.4);
    await mock.call('sedef_setParam', 'inputGain', 99);
    expect(mock.value('transpose')).toBe(3);
    expect(mock.value('inputGain')).toBe(24);
  });

  it('marks the preset dirty on tone changes only', async () => {
    mock = new MockBackend();
    const presets: PresetState[] = [];
    mock.on('sedef:presets', (s) => presets.push(s));
    await mock.call('sedef_setParam', 'tunerReference', 442);
    await flush();
    expect(presets).toHaveLength(0);
    await mock.call('sedef_setParam', 'reefGain', 0.9);
    await flush();
    expect(presets.at(-1)?.dirty).toBe(true);
  });

  it('loads, steps, saves, renames, favourites and deletes presets', async () => {
    mock = new MockBackend();
    const params = vi.fn();
    mock.on('sedef:params', params);

    let state = await mock.call('sedef_presets_load', 'factory:riptide');
    expect(state.currentName).toBe('Singing Echo');
    expect(mock.value('ampModel')).toBe(2);
    await flush();
    expect(params).toHaveBeenCalled();

    state = await mock.call('sedef_presets_save', 'My lead', 'Gigs');
    const saved = state.items.find((p) => p.name === 'My lead');
    expect(saved).toMatchObject({ category: 'User', folder: 'Gigs' });
    expect(state.currentId).toBe(saved?.id);
    expect(state.dirty).toBe(false);

    // Renaming renames the file, so the id follows, and never lands on another preset's file.
    state = await mock.call('sedef_presets_rename', saved!.id, 'Big lead');
    expect(state.currentName).toBe('Big lead');
    const renamed = 'user:Gigs/Big lead.sedefpreset';
    expect(state.currentId).toBe(renamed);
    state = await mock.call('sedef_presets_save', 'Rhythm', 'Gigs');
    const rhythm = state.items.find((p) => p.name === 'Rhythm')!;
    state = await mock.call('sedef_presets_rename', rhythm.id, 'big LEAD');
    expect(state.items.find((p) => p.id === rhythm.id)?.name).toBe('Rhythm');

    state = await mock.call('sedef_presets_setFavorite', renamed, true);
    expect(state.items.find((p) => p.id === renamed)?.favorite).toBe(true);

    state = await mock.call('sedef_presets_delete', renamed);
    expect(state.items.find((p) => p.id === renamed)).toBeUndefined();

    // Factory presets cannot be deleted.
    state = await mock.call('sedef_presets_delete', 'factory:glass-pocket');
    expect(state.items.some((p) => p.id === 'factory:glass-pocket')).toBe(true);
  });

  it('describes every preset: amp, cab and the switched-on chain with defaults applied', async () => {
    mock = new MockBackend();
    const { items } = await mock.call('sedef_presets_list');
    const nacre = items.find((p) => p.id === 'factory:nacre');
    expect(nacre).toMatchObject({ amp: 0, cab: 0 });
    expect(nacre?.blocks).toEqual(['gate', 'tide', 'ripple', 'fathom']);
    expect(nacre?.description).toMatch(/clean/i);
    const mariana = items.find((p) => p.id === 'factory:mariana');
    expect(mariana).toMatchObject({ amp: 2, cab: 2, folder: 'High Gain' });

    // An EQ counts only when the amp's bank is on and shapes the sound.
    await mock.call('sedef_presets_load', 'factory:brine');
    await mock.call('sedef_setParam', 'eqReefBand5', 3);
    let state = await mock.call('sedef_presets_save', 'Mids', '');
    expect(state.items.find((p) => p.name === 'Mids')?.blocks).toContain('eq');
    await mock.call('sedef_setParam', 'eqReefEnabled', 0);
    state = await mock.call('sedef_presets_save', 'Mids', '');
    expect(state.items.find((p) => p.name === 'Mids')?.blocks).not.toContain('eq');
  });

  it('restores a snapshot exactly, edits and dirty state included', async () => {
    mock = new MockBackend();
    await mock.call('sedef_setParam', 'lagoonVolume', 0.81);
    await mock.call('sedef_setParam', 'rippleSpeed', 1.234567);
    const before = (await mock.hello()).values;
    await mock.call('sedef_presets_snapshot', 'browse');
    await mock.call('sedef_presets_load', 'factory:mariana', true);
    await mock.call('sedef_presets_load', 'factory:undertow', true);
    await mock.call('sedef_setParam', 'inputGain', 4); // global: not part of a preset
    const state = await mock.call('sedef_presets_restore', 'browse');
    const after = (await mock.hello()).values;
    expect({ ...after, inputGain: before.inputGain }).toEqual(before);
    expect(after.inputGain).toBe(4);
    expect(state).toMatchObject({
      currentId: 'factory:nacre',
      currentName: 'Glassy Chorus',
      dirty: true,
    });
  });

  it('counts picks, not auditions, as Recent', async () => {
    mock = new MockBackend();
    let state = await mock.call('sedef_presets_load', 'factory:riptide', true);
    expect(state.recent).toEqual([]);
    state = await mock.call('sedef_presets_step', 1);
    expect(state.recent).toEqual([]);
    state = await mock.call('sedef_presets_markRecent', 'factory:riptide');
    expect(state.recent).toEqual(['factory:riptide']);
    await mock.call('sedef_presets_load', 'factory:brine');
    state = await mock.call('sedef_presets_load', 'factory:riptide');
    expect(state.recent).toEqual(['factory:riptide', 'factory:brine']);
  });

  it('compares A and B, each with its own preset and edits', async () => {
    mock = new MockBackend();
    let state = await mock.call('sedef_presets_compare', 'switch');
    // B starts as a copy of A.
    expect(state.compare).toEqual({ active: 'B', other: { name: 'Glassy Chorus', dirty: false } });
    expect(state.currentName).toBe('Glassy Chorus');
    await mock.call('sedef_presets_load', 'factory:riptide');
    await mock.call('sedef_setParam', 'abyssGain', 0.9);
    state = await mock.call('sedef_presets_compare', 'switch');
    expect(state).toMatchObject({ currentName: 'Glassy Chorus', dirty: false });
    expect(state.compare).toEqual({
      active: 'A',
      other: { name: 'Singing Echo', dirty: true },
    });
    expect(mock.value('ampModel')).toBe(0);
    state = await mock.call('sedef_presets_compare', 'switch');
    expect(state).toMatchObject({ currentName: 'Singing Echo', dirty: true });
    expect(mock.value('abyssGain')).toBe(0.9);
    // Copy puts the live slot into the other one.
    state = await mock.call('sedef_presets_compare', 'copy');
    expect(state.compare).toEqual({
      active: 'B',
      other: { name: 'Singing Echo', dirty: true },
    });
  });

  it('saves details, keeps them on a plain overwrite, and undoes a delete', async () => {
    mock = new MockBackend();
    let state = await mock.call('sedef_presets_save', 'Encore', 'Live', {
      tags: [' Big ', 'big', 'LEAD', ''],
      description: 'Line one\nline two',
    });
    const id = 'user:Live/Encore.sedefpreset';
    expect(state.items.find((p) => p.id === id)).toMatchObject({
      tags: ['big', 'lead'],
      description: 'Line one line two',
    });
    expect(state.recent[0]).toBe(id);
    await mock.call('sedef_setParam', 'lagoonVolume', 0.2);
    state = await mock.call('sedef_presets_save', 'encore', 'Live');
    expect(
      state.items.filter((p) => p.folder === 'Live' && p.name.toLowerCase() === 'encore'),
    ).toHaveLength(1);
    expect(state.items.find((p) => p.id === id)?.tags).toEqual(['big', 'lead']);

    state = await mock.call('sedef_presets_delete', id);
    expect(state.items.some((p) => p.id === id)).toBe(false);
    expect(state.currentId).toBeNull();
    state = await mock.call('sedef_presets_undoDelete');
    expect(state.items.some((p) => p.id === id)).toBe(true);
    expect(state.currentId).toBe(id);
  });

  it('steps through presets and wraps around', async () => {
    mock = new MockBackend({ initialPresetId: null });
    const first = await mock.call('sedef_presets_step', 1);
    expect(first.currentId).toBe(first.items[0]?.id);
    const last = await mock.call('sedef_presets_step', -1);
    expect(last.currentId).toBe(last.items.at(-1)?.id);
  });

  it('loads and clears custom IRs per mic and cab', async () => {
    mock = new MockBackend();
    let ir = await mock.call('sedef_ir_load', 1, 2);
    expect(ir.slots.find((s) => s.mic === 1 && s.cab === 2)?.name).toBeTruthy();
    expect(ir.slots.filter((s) => s.name)).toHaveLength(1);
    ir = await mock.call('sedef_ir_clear', 1, 2);
    expect(ir.slots.every((s) => s.name === null)).toBe(true);
  });

  it('emits pitch only while analysis is active', async () => {
    mock = new MockBackend();
    const events: PitchEvent[] = [];
    mock.on('sedef:pitch', (e) => events.push(e));
    mock.tickPitch();
    expect(events).toHaveLength(0);
    await mock.call('sedef_tuner_setActive', true);
    for (let i = 0; i < 50; i += 1) mock.tickPitch();
    expect(events).toHaveLength(50);
    expect(events.some((e) => e.valid && e.note === 'E' && e.octave === 2)).toBe(true);
  });

  it('simulates a 12th-fret capture with the configured error', async () => {
    mock = new MockBackend({ intonationErrors: [4], tuningOffsets: [0] });
    await mock.call('sedef_tuner_setActive', true);
    mock.simulator.setIntent({ kind: 'fret', midi: 52, stringIndex: 0 });
    let last: PitchEvent | null = null;
    mock.on('sedef:pitch', (e) => (last = e));
    for (let i = 0; i < 120; i += 1) mock.tickPitch();
    const e = last as PitchEvent | null;
    expect(e?.stable).toBe(true);
    const target = 440 * Math.pow(2, (52 - 69) / 12);
    // 4 cents, less the slow flat sag of a note 1.3 s after the pluck.
    const cents = 1200 * Math.log2((e?.stableHz ?? 0) / target);
    expect(cents).toBeGreaterThan(3);
    expect(cents).toBeLessThan(4.2);
  });

  it('produces telemetry including the effective Drift time', () => {
    mock = new MockBackend();
    const t = vi.fn();
    mock.on('sedef:telemetry', t);
    mock.tickTelemetry();
    const event = t.mock.calls[0]?.[0];
    expect(event.inputDb).toHaveLength(1);
    expect(event.outputDb).toHaveLength(2);
    // Default sync is Host at the mock's 120 bpm, note 1/4.
    expect(event.driftTimeMs).toBeCloseTo(500);
    expect(event.metronomeStep).toBe(-1);
  });

  it('keeps UI state and taps tempo', async () => {
    mock = new MockBackend();
    await mock.call('sedef_ui_setState', { section: 'fx' });
    expect((await mock.hello()).uiState).toEqual({ section: 'fx' });
    vi.useFakeTimers();
    try {
      await mock.call('sedef_metronome_tap');
      vi.advanceTimersByTime(500);
      const bpm = await mock.call('sedef_metronome_tap');
      expect(bpm).toBeCloseTo(120, 0);
      expect(mock.value('tempo')).toBeCloseTo(120, 0);
    } finally {
      vi.useRealTimers();
    }
  });
});
