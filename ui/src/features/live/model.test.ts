// SPDX-License-Identifier: AGPL-3.0-or-later
// The live arithmetic the UI and the mock share; the same cases as engine/tests/live
// (LiveMapTests.cpp), so the browser preview behaves like the plug-in.
import { describe, expect, it } from 'vitest';
import { bundledSchema, indexSchema } from '../../params/schema';
import { toNormalised } from '../../params/normalise';
import {
  actionFires,
  activityLabel,
  canRememberInScenes,
  continuousValue,
  DOUBLE_TAP_S,
  isDoubleTap,
  isLearnable,
  isSwitchTarget,
  LOOPER_TARGETS,
  parseTarget,
  rangeLabel,
  sceneTarget,
  SCENE_SWITCHES,
  sourceLabel,
  SwitchClassifier,
  switchValue,
  targetLabel,
} from './model';

const specs = indexSchema(bundledSchema);
const spec = (id: string) => {
  const s = specs[id];
  if (!s) throw new Error(id);
  return s;
};

describe('continuous controllers', () => {
  it('map 0 and 127 exactly onto the ends of the range', () => {
    const gain = spec('outputGain');
    expect(continuousValue(gain, 0, 1, 0)).toBe(-24);
    expect(continuousValue(gain, 0, 1, 127)).toBe(24);
  });

  it('are linear in the knob’s travel, through its skew', () => {
    const mix = spec('conchPosition');
    for (let v = 0; v <= 127; v += 1) expect(continuousValue(mix, 0, 1, v)).toBeCloseTo(v / 127, 6);
    // Ripple's speed is skewed: the controller moves the knob evenly, not the hertz.
    const speed = spec('rippleSpeed');
    expect(toNormalised(speed, continuousValue(speed, 0, 1, 64))).toBeCloseTo(64 / 127, 5);
  });

  it('keep to a minimum and maximum, and turn the other way when inverted', () => {
    const mix = spec('conchPosition');
    expect(continuousValue(mix, 0.25, 0.75, 0)).toBe(0.25);
    expect(continuousValue(mix, 0.25, 0.75, 127)).toBe(0.75);
    expect(continuousValue(mix, 1, 0, 0)).toBe(1);
    expect(continuousValue(mix, 1, 0, 127)).toBe(0);
    expect(continuousValue(mix, 0.8, 0.2, 127)).toBe(0.2);
  });

  it('clamp out-of-range data', () => {
    const mix = spec('conchPosition');
    expect(continuousValue(mix, 0, 1, 200)).toBe(1);
    expect(continuousValue(mix, 0, 1, -5)).toBe(0);
  });

  it('step through choices', () => {
    const amp = spec('ampModel');
    expect([0, 31, 32, 95, 96, 127].map((v) => continuousValue(amp, 0, 1, v))).toEqual([
      0, 0, 1, 1, 2, 2,
    ]);
  });
});

describe('switches', () => {
  it('Toggle flips on each press and ignores the release', () => {
    expect(switchValue('toggle', false, 127)).toBe(true);
    expect(switchValue('toggle', true, 127)).toBe(false);
    expect(switchValue('toggle', true, 64)).toBe(false);
    expect(switchValue('toggle', true, 0)).toBeNull();
    expect(switchValue('toggle', false, 63)).toBeNull();
  });

  it('Latch follows the switch: 64 and up is on', () => {
    expect(switchValue('latch', false, 64)).toBe(true);
    expect(switchValue('latch', true, 63)).toBe(false);
    expect(switchValue('latch', true, 127)).toBe(true);
  });

  it('scene and preset actions fire on presses (Toggle) or every message (Latch)', () => {
    expect(actionFires('toggle', 127)).toBe(true);
    expect(actionFires('toggle', 0)).toBe(false);
    expect(actionFires('latch', 0)).toBe(true);
    expect(actionFires('latch', 127)).toBe(true);
  });
});

describe('learn', () => {
  it('skips bank select, data entry, (N)RPN and channel mode messages', () => {
    const skipped = [
      0,
      6,
      32,
      38,
      98,
      99,
      100,
      101,
      ...Array.from({ length: 8 }, (_, i) => 120 + i),
    ];
    for (let cc = 0; cc < 128; cc += 1) expect(isLearnable(cc)).toBe(!skipped.includes(cc));
  });

  it('tells a momentary footswitch from a latching one by its release', () => {
    const momentary = new SwitchClassifier();
    expect(momentary.first(127, 10)).toBeNull();
    expect(momentary.next(0, 10.2)).toBe('toggle');

    const latching = new SwitchClassifier();
    expect(latching.first(127, 10)).toBeNull();
    expect(latching.elapsed(11.5)).toBeNull();
    expect(latching.elapsed(11.6)).toBe('latch');

    // A release after the window is the latching switch turned off again.
    const late = new SwitchClassifier();
    late.first(127, 0);
    expect(late.next(0, 2)).toBe('latch');

    // A pedal, a knob, or a switch that presses with 0 is Latch at once.
    expect(new SwitchClassifier().first(64, 0)).toBe('latch');
    expect(new SwitchClassifier().first(0, 0)).toBe('latch');
  });
});

describe('targets and labels', () => {
  it('parse every target form and reject the rest', () => {
    expect(parseTarget('param:pearlGain')).toEqual({ kind: 'param', id: 'pearlGain' });
    expect(parseTarget('param:presetNext')).toEqual({ kind: 'presetNext' });
    expect(parseTarget(sceneTarget(0))).toEqual({ kind: 'scene', scene: 0 });
    expect(parseTarget('scene:8')).toEqual({ kind: 'scene', scene: 7 });
    expect(parseTarget('scene:next')).toEqual({ kind: 'sceneNext' });
    expect(parseTarget('preset:previous')).toEqual({ kind: 'presetPrevious' });
    for (const bad of ['scene:0', 'scene:9', 'param:', 'pedal:pearl', ''])
      expect(parseTarget(bad)).toBeNull();
  });

  it('name targets, sources, ranges and activity in plain words', () => {
    expect(targetLabel('param:pearlGain', specs)).toBe('Pearl Gain');
    expect(targetLabel('scene:3', specs)).toBe('Scene 3');
    expect(targetLabel('preset:next', specs)).toBe('Next preset');
    expect(sourceLabel({ controller: 20, channel: 1 })).toBe('CC 20 · ch 1');
    expect(rangeLabel({ low: 1, high: 0 })).toBe('100–0 %');
    expect(activityLabel({ count: 1, status: 0xb3, data1: 11, data2: 64 })).toBe(
      'CC 11 = 64 · ch 4',
    );
    expect(activityLabel({ count: 1, status: 0xc0, data1: 4, data2: 0 })).toBe('Program 5 · ch 1');
    expect(activityLabel({ count: 1, status: 0x90, data1: 60, data2: 100 })).toBeNull();
  });

  it('know which targets are switches and what scenes can keep', () => {
    expect(isSwitchTarget('param:pearlEnabled', specs)).toBe(true);
    expect(isSwitchTarget('param:pearlGain', specs)).toBe(false);
    expect(isSwitchTarget('scene:next', specs)).toBe(true);
    expect(canRememberInScenes(spec('pearlGain'))).toBe(true);
    // Global (the same in every preset) and stepped parameters stay out of scenes.
    expect(canRememberInScenes(spec('outputGain'))).toBe(false);
    expect(canRememberInScenes(spec('ampModel'))).toBe(false);
    // Transpose adds latency while it shifts: a scene must never change it (docs/live.md §6.2).
    expect(canRememberInScenes(spec('transpose'))).toBe(false);
    expect(SCENE_SWITCHES).not.toContain('transpose');
    expect(canRememberInScenes(spec('pearlEnabled'))).toBe(false);
  });

  it('list the 22 switches every scene holds, as the engine does', () => {
    expect(SCENE_SWITCHES).toHaveLength(22);
    expect(new Set(SCENE_SWITCHES).size).toBe(22);
    for (const id of SCENE_SWITCHES) expect(spec(id).kind).toBe('bool');
  });
});

describe('the looper’s footswitches (docs/live.md §4.5)', () => {
  it('are switch targets with names of their own', () => {
    expect(LOOPER_TARGETS.map((t) => t.target)).toEqual([
      'looper:main',
      'looper:stop',
      'looper:undo',
      'looper:clear',
    ]);
    expect(parseTarget('looper:main')).toEqual({ kind: 'looper', action: 'main' });
    expect(parseTarget('looper:pause')).toBeNull();
    expect(targetLabel('looper:main', specs)).toBe('Looper: record, play, overdub');
    expect(targetLabel('looper:undo', specs)).toBe('Looper: undo, redo');
    for (const t of LOOPER_TARGETS) expect(isSwitchTarget(t.target, specs)).toBe(true);
  });

  it('read a double tap when the second press comes less than 0.4 s after the first', () => {
    expect(DOUBLE_TAP_S).toBe(0.4);
    expect(isDoubleTap(0, 0.399)).toBe(true);
    expect(isDoubleTap(0, 0.4)).toBe(false);
    expect(isDoubleTap(-1, 0.1)).toBe(false); // nothing waits
  });
});
