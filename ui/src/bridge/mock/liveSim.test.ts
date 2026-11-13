// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser preview's live layer (docs/live.md) against the same rules the plug-in's tests pin
// (plugin/tests/LiveStateTests.cpp): learn, scenes and their "made" state, dirty, program
// changes, presets and snapshots carrying scenes.
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { MockBackend } from '../mockBackend';
import type { LiveState } from '../protocol';

const CC = 0xb0;
const PC = 0xc0;

describe('The mock live layer', () => {
  let mock: MockBackend;
  const state = () => mock.call('sedef_live_get');
  const dirty = async () => (await mock.call('sedef_presets_list')).dirty;

  beforeEach(() => {
    mock = new MockBackend({ autoStart: false, persistKey: null });
  });
  afterEach(() => mock.dispose());

  describe('learn', () => {
    it('binds a momentary footswitch as Toggle: press, release within the window', async () => {
      await mock.call('sedef_live_learn', 'param:pearlEnabled');
      expect((await state()).learn).toMatchObject({ phase: 'waiting' });
      mock.midi(CC, 20, 127, 1);
      expect((await state()).learn).toMatchObject({ phase: 'listening', binding: 0 });
      mock.midi(CC, 20, 0, 1.2);
      const s = await state();
      expect(s.learn).toMatchObject({ phase: 'done', binding: 0 });
      expect(s.bindings).toEqual([
        expect.objectContaining({ target: 'param:pearlEnabled', controller: 20, channel: 1 }),
      ]);
      expect(s.bindings[0]).toMatchObject({ switch: true, mode: 'toggle' });

      // Done: the switch plays; each press flips Pearl, releases do nothing.
      await mock.call('sedef_live_cancelLearn');
      const before = mock.value('pearlEnabled');
      mock.midi(CC, 20, 127);
      expect(mock.value('pearlEnabled')).toBe(1 - before);
      mock.midi(CC, 20, 0);
      expect(mock.value('pearlEnabled')).toBe(1 - before);
      mock.midi(CC, 20, 127);
      expect(mock.value('pearlEnabled')).toBe(before);
    });

    it('binds a latching footswitch as Latch once the window passes without a release', async () => {
      await mock.call('sedef_live_learn', 'param:driftEnabled');
      mock.midi(CC, 21, 127, 5);
      mock.live.advance(1.4);
      expect((await state()).learn?.phase).toBe('listening');
      mock.live.advance(0.2);
      const s = await state();
      expect(s.learn?.phase).toBe('done');
      expect(s.bindings[0]).toMatchObject({ controller: 21, mode: 'latch' });
      await mock.call('sedef_live_cancelLearn');
      mock.midi(CC, 21, 0);
      expect(mock.value('driftEnabled')).toBe(0);
      mock.midi(CC, 21, 127);
      expect(mock.value('driftEnabled')).toBe(1);
    });

    it('binds a pedal to a knob over its whole travel, and moves a re-learned target', async () => {
      await mock.call('sedef_live_learn', 'param:conchPosition');
      mock.midi(CC, 11, 40);
      let s = await state();
      expect(s.learn?.phase).toBe('done');
      expect(s.bindings[0]).toMatchObject({ switch: false, low: 0, high: 1 });
      mock.midi(CC, 11, 127);
      expect(mock.value('conchPosition')).toBe(1);

      // The same target learned again takes the new controller; the old one is free.
      await mock.call('sedef_live_learn', 'param:conchPosition');
      mock.midi(CC, 7, 0);
      s = await state();
      expect(s.bindings).toHaveLength(1);
      expect(s.bindings[0]).toMatchObject({ controller: 7 });
    });

    it('ignores bank select and channel mode messages while waiting', async () => {
      await mock.call('sedef_live_learn', 'param:pearlGain');
      mock.midi(CC, 0, 1);
      mock.midi(CC, 121, 0);
      mock.midi(CC, 123, 0);
      expect((await state()).learn?.phase).toBe('waiting');
      expect((await state()).bindings).toHaveLength(0);
    });

    it('edits, removes and clears bindings', async () => {
      await mock.call('sedef_live_learn', 'param:conchPosition');
      mock.midi(CC, 11, 0);
      await mock.call('sedef_live_learn', 'scene:next');
      mock.midi(CC, 24, 0);
      let s = await mock.call('sedef_live_updateBinding', 0, { low: 0.75, high: 0.25 });
      expect(s.bindings[0]).toMatchObject({ low: 0.75, high: 0.25 });
      mock.midi(CC, 11, 127);
      expect(mock.value('conchPosition')).toBe(0.25);
      s = await mock.call('sedef_live_removeBinding', 0);
      expect(s.bindings.map((b) => b.target)).toEqual(['scene:next']);
      s = await mock.call('sedef_live_clearBindings');
      expect(s.bindings).toEqual([]);
    });
  });

  describe('scenes', () => {
    it('start with only the scene in use made; a new scene is a copy of the one left', async () => {
      let s = await state();
      expect(s.scenes.made).toEqual([true, false, false, false, false, false, false, false]);
      mock.hostSet({ pearlEnabled: 1 });
      s = await mock.call('sedef_live_selectScene', 1);
      expect(s.scenes.active).toBe(1);
      expect(s.scenes.made.slice(0, 3)).toEqual([true, true, false]);
      expect(mock.value('pearlEnabled')).toBe(1);
      expect(mock.value('scene')).toBe(1);

      // Change scene 2 and go back: scene 1 is as it was left.
      mock.hostSet({ pearlEnabled: 0 });
      await mock.call('sedef_live_selectScene', 0);
      expect(mock.value('pearlEnabled')).toBe(1);
      await mock.call('sedef_live_selectScene', 1);
      expect(mock.value('pearlEnabled')).toBe(0);
    });

    it('keep picked knobs per scene, and drop them on request', async () => {
      await mock.call('sedef_live_remember', 'pearlGain', true);
      mock.hostSet({ pearlGain: 0.2 });
      await mock.call('sedef_live_selectScene', 1);
      mock.hostSet({ pearlGain: 0.8 });
      await mock.call('sedef_live_selectScene', 0);
      expect(mock.value('pearlGain')).toBe(0.2);
      await mock.call('sedef_live_selectScene', 1);
      expect(mock.value('pearlGain')).toBe(0.8);

      // Global and stepped parameters cannot be kept.
      await mock.call('sedef_live_remember', 'outputGain', true);
      let s = await mock.call('sedef_live_remember', 'ampModel', true);
      expect(s.scenes.values).toEqual(['pearlGain']);
      s = await mock.call('sedef_live_remember', 'pearlGain', false);
      expect(s.scenes.values).toEqual([]);
    });

    it('switch from footswitches, scene steps and the Scene parameter', async () => {
      await mock.call('sedef_live_learn', 'scene:3');
      mock.midi(CC, 30, 127, 0);
      mock.midi(CC, 30, 0, 0.1);
      await mock.call('sedef_live_learn', 'scene:next');
      mock.midi(CC, 31, 127, 1);
      mock.midi(CC, 31, 0, 1.1);
      await mock.call('sedef_live_cancelLearn');

      mock.midi(CC, 30, 127);
      expect((await state()).scenes.active).toBe(2);
      mock.midi(CC, 31, 127);
      expect((await state()).scenes.active).toBe(3);
      // The release of a momentary switch does nothing.
      mock.midi(CC, 31, 0);
      expect((await state()).scenes.active).toBe(3);
    });

    it('copy the scene in use into another', async () => {
      mock.hostSet({ driftEnabled: 1 });
      const s = await mock.call('sedef_live_copyScene', 5);
      expect(s.scenes.made[5]).toBe(true);
      mock.hostSet({ driftEnabled: 0 });
      await mock.call('sedef_live_selectScene', 5);
      expect(mock.value('driftEnabled')).toBe(1);
    });

    it('mark a preset modified when they change, and not for visiting an untouched copy', async () => {
      expect(await dirty()).toBe(false);
      // Visiting a scene that was not saved makes an untouched copy: nothing to save.
      await mock.call('sedef_live_selectScene', 2);
      expect(await dirty()).toBe(false);
      await mock.call('sedef_live_selectScene', 0);
      expect(await dirty()).toBe(false);
      // A switch changed in it is a change.
      await mock.call('sedef_live_selectScene', 2);
      mock.hostSet({ pearlEnabled: 1 - mock.value('pearlEnabled') });
      await mock.call('sedef_live_selectScene', 0);
      expect(await dirty()).toBe(true);
    });
  });

  describe('program changes', () => {
    it('load presets in library order unless assigned, on the chosen channel', async () => {
      const items = (await mock.call('sedef_presets_list')).items;
      mock.midi(PC, 2, 0);
      expect((await mock.call('sedef_presets_list')).currentId).toBe(items[2]?.id);
      // Past the end of the library: nothing happens.
      mock.midi(PC, 127, 0);
      expect((await mock.call('sedef_presets_list')).currentId).toBe(items[2]?.id);

      await mock.call('sedef_live_setProgram', 3, { preset: items[0]!.id });
      mock.midi(PC, 2, 0);
      expect((await mock.call('sedef_presets_list')).currentId).toBe(items[0]?.id);

      await mock.call('sedef_live_setProgram', 4, { scene: 6 });
      mock.midi(PC, 3, 0);
      expect((await state()).scenes.active).toBe(6);

      const s: LiveState = await mock.call('sedef_live_setProgramChannel', 2);
      expect(s.programs.channel).toBe(2);
      mock.midi(PC, 5, 0); // channel 1: ignored
      expect((await mock.call('sedef_presets_list')).currentId).toBe(items[0]?.id);
      mock.midi(PC | 1, 5, 0);
      expect((await mock.call('sedef_presets_list')).currentId).toBe(items[5]?.id);

      // Unassigned: back to library order.
      await mock.call('sedef_live_setProgramChannel', 0);
      await mock.call('sedef_live_setProgram', 3, null);
      mock.midi(PC, 2, 0);
      expect((await mock.call('sedef_presets_list')).currentId).toBe(items[2]?.id);
    });
  });

  describe('presets and snapshots', () => {
    it('save scenes with a preset and bring them back when it loads', async () => {
      await mock.call('sedef_live_remember', 'pearlGain', true);
      mock.hostSet({ pearlGain: 0.3 });
      await mock.call('sedef_live_selectScene', 1);
      mock.hostSet({ pearlGain: 0.7, pearlEnabled: 1 });
      const saved = await mock.call('sedef_presets_save', 'Two scenes', '');
      const id = saved.currentId!;
      expect(saved.dirty).toBe(false);

      await mock.call('sedef_presets_init');
      let s = await state();
      expect(s.scenes.values).toEqual([]);
      expect(s.scenes.made).toEqual([true, false, false, false, false, false, false, false]);

      await mock.call('sedef_presets_load', id, false);
      s = await state();
      expect(s.scenes.active).toBe(1);
      expect(s.scenes.values).toEqual(['pearlGain']);
      expect(s.scenes.made.slice(0, 3)).toEqual([true, true, false]);
      expect(mock.value('pearlGain')).toBe(0.7);
      await mock.call('sedef_live_selectScene', 0);
      expect(mock.value('pearlGain')).toBe(0.3);
      expect(await dirty()).toBe(false);
    });

    it('carry scenes in snapshots', async () => {
      await mock.call('sedef_live_selectScene', 4);
      await mock.call('sedef_presets_snapshot', 'undo');
      await mock.call('sedef_live_selectScene', 0);
      await mock.call('sedef_presets_restore', 'undo');
      expect((await state()).scenes.active).toBe(4);
    });
  });
});
