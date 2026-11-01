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
    expect(hello.schema).toHaveLength(137);
    expect(Object.keys(hello.values)).toHaveLength(137);
    expect(hello.presets.currentName).toBe('Nacre');
    expect(hello.presets.currentId).toBe('factory:nacre');
    expect(hello.presets.items.filter((p) => p.category === 'Factory')).toHaveLength(13);
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
    await mock.call('sedef_setParam', 'lagoonVolume', 0.45); // Nacre's stored value
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
    expect(state.currentName).toBe('Riptide');
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
