// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing live (docs/live.md): the arithmetic and names the UI and the mock share. The rules
// mirror engine/include/sedef/live (LiveMap.h, SceneBank.h) and plugin/src/LiveSettings.cpp.
import type { LiveActivity, LiveBinding, LiveSwitchMode, ParamSpec } from '../../bridge/protocol';
import { fromNormalised } from '../../params/normalise';
import { isGlobalParam } from '../../params/presetScope';
import { PEDALS } from '../pedals/registry';

/** Scenes per preset (§6.1). */
export const NUM_SCENES = 8;
/** Values the player may keep per scene (§7.3). */
export const MAX_SCENE_VALUES = 16;
/** The MIDI 1.0 split for switch controllers: 0-63 off, 64-127 on. */
export const SWITCH_THRESHOLD = 64;
/** How long learn watches a footswitch for its release (§4.4), seconds. */
export const LEARN_WINDOW_S = 1.5;

/** The switches every scene holds: each pedal's on/off, the EQ banks, Doubler, Drift, Fathom. */
export const SCENE_SWITCHES: readonly string[] = [
  ...PEDALS.map((p) => p.enable),
  'eqLagoonEnabled',
  'eqReefEnabled',
  'eqAbyssEnabled',
  'doublerEnabled',
  'driftEnabled',
  'fathomEnabled',
];

/** A continuous parameter of the preset: what can be kept per scene (§6.2). */
export function canRememberInScenes(spec: ParamSpec): boolean {
  return spec.kind === 'float' && !isGlobalParam(spec.id);
}

/** The looper's footswitch actions (§4.5), standalone app only. */
export type LooperTarget = 'main' | 'stop' | 'undo' | 'clear';

export type ParsedTarget =
  | { kind: 'param'; id: string }
  | { kind: 'scene'; scene: number }
  | { kind: 'sceneNext' | 'scenePrevious' | 'presetNext' | 'presetPrevious' }
  | { kind: 'looper'; action: LooperTarget };

/** The looper's targets, in the order the MIDI panel offers them, with their names there. */
export const LOOPER_TARGETS: readonly { target: string; action: LooperTarget; short: string }[] = [
  { target: 'looper:main', action: 'main', short: 'Record, play' },
  { target: 'looper:stop', action: 'stop', short: 'Stop' },
  { target: 'looper:undo', action: 'undo', short: 'Undo, redo' },
  { target: 'looper:clear', action: 'clear', short: 'Clear' },
];

/** A second press of the looper's main switch this soon after the first stops it (§4.5), s. */
export const DOUBLE_TAP_S = 0.4;

/** The second press of a double tap: the first (-1: none waiting) less than the window before. */
export function isDoubleTap(last: number, now: number): boolean {
  return last >= 0 && now - last < DOUBLE_TAP_S;
}

export function paramTarget(id: string): string {
  return `param:${id}`;
}

export function sceneTarget(scene: number): string {
  return `scene:${scene + 1}`;
}

export function parseTarget(target: string): ParsedTarget | null {
  if (target.startsWith('param:')) {
    const id = target.slice(6);
    if (id === 'presetNext') return { kind: 'presetNext' };
    if (id === 'presetPrevious') return { kind: 'presetPrevious' };
    return id ? { kind: 'param', id } : null;
  }
  const looper = LOOPER_TARGETS.find((t) => t.target === target);
  if (looper) return { kind: 'looper', action: looper.action };
  switch (target) {
    case 'scene:next':
      return { kind: 'sceneNext' };
    case 'scene:previous':
      return { kind: 'scenePrevious' };
    case 'preset:next':
      return { kind: 'presetNext' };
    case 'preset:previous':
      return { kind: 'presetPrevious' };
  }
  const m = /^scene:([1-8])$/.exec(target);
  return m ? { kind: 'scene', scene: Number(m[1]) - 1 } : null;
}

const LOOPER_LABELS: Record<LooperTarget, string> = {
  main: 'Looper: record, play, overdub',
  stop: 'Looper: stop',
  undo: 'Looper: undo, redo',
  clear: 'Looper: clear',
};

/** "Pearl", "Conch Position", "Scene 3", "Next scene", "Looper: stop". */
export function targetLabel(target: string, specs: Record<string, ParamSpec>): string {
  const t = parseTarget(target);
  if (!t) return target;
  switch (t.kind) {
    case 'param':
      return specs[t.id]?.name ?? t.id;
    case 'scene':
      return `Scene ${t.scene + 1}`;
    case 'sceneNext':
      return 'Next scene';
    case 'scenePrevious':
      return 'Previous scene';
    case 'presetNext':
      return 'Next preset';
    case 'presetPrevious':
      return 'Previous preset';
    case 'looper':
      return LOOPER_LABELS[t.action];
  }
}

/** A switch-like target: a footswitch's mode matters. */
export function isSwitchTarget(target: string, specs: Record<string, ParamSpec>): boolean {
  const t = parseTarget(target);
  if (!t) return false;
  return t.kind !== 'param' || specs[t.id]?.kind === 'bool';
}

/** Controllers learn takes: not mode messages, bank select, data entry or (N)RPN (§4.1). */
export function isLearnable(controller: number): boolean {
  return (
    controller > 0 &&
    controller < 120 &&
    controller !== 6 &&
    controller !== 32 &&
    controller !== 38 &&
    !(controller >= 98 && controller <= 101)
  );
}

/** n = low + (high - low) * value / 127, through the parameter's skew (§4.3). */
export function continuousValue(spec: ParamSpec, low: number, high: number, value: number): number {
  const t = Math.min(127, Math.max(0, value)) / 127;
  // Exact at both ends, like std::lerp.
  const n = t === 1 ? high : low + (high - low) * t;
  return fromNormalised(spec, Math.min(1, Math.max(0, n)));
}

/** Toggle flips on presses (>= 64) and ignores releases; Latch follows (§4.4). */
export function switchValue(mode: LiveSwitchMode, current: boolean, value: number): boolean | null {
  const pressed = value >= SWITCH_THRESHOLD;
  if (mode === 'latch') return pressed;
  return pressed ? !current : null;
}

/** Scene and preset actions: Toggle on presses, Latch on every message. */
export function actionFires(mode: LiveSwitchMode, value: number): boolean {
  return mode === 'latch' || value >= SWITCH_THRESHOLD;
}

/**
 * The learn classifier (§4.4), in seconds of audio: a pedal or knob, or a press that sends 0, is
 * Latch at once; 127 then a release within the window is a momentary switch (Toggle); 127 and
 * nothing else for the whole window is a latching one.
 */
export class SwitchClassifier {
  private start = 0;
  private decidedMode: LiveSwitchMode | null = null;

  first(value: number, time: number): LiveSwitchMode | null {
    this.start = time;
    if (value !== 127) this.decidedMode = 'latch';
    return this.decidedMode;
  }

  next(value: number, time: number): LiveSwitchMode | null {
    if (this.decidedMode) return this.decidedMode;
    if (time - this.start > LEARN_WINDOW_S) this.decidedMode = 'latch';
    else if (value < SWITCH_THRESHOLD) this.decidedMode = 'toggle';
    return this.decidedMode;
  }

  elapsed(now: number): LiveSwitchMode | null {
    if (!this.decidedMode && now - this.start > LEARN_WINDOW_S) this.decidedMode = 'latch';
    return this.decidedMode;
  }

  get mode(): LiveSwitchMode | null {
    return this.decidedMode;
  }
}

/** "CC 20 · ch 1" */
export function sourceLabel(b: Pick<LiveBinding, 'controller' | 'channel'>): string {
  return `CC ${b.controller} · ch ${b.channel}`;
}

/** "CC 11 = 64 · ch 1", "Program 5 · ch 1", or null for anything else. */
export function activityLabel(a: LiveActivity): string | null {
  const kind = a.status & 0xf0;
  const channel = (a.status & 0x0f) + 1;
  if (kind === 0xb0) return `CC ${a.data1} = ${a.data2} · ch ${channel}`;
  if (kind === 0xc0) return `Program ${a.data1 + 1} · ch ${channel}`;
  return null;
}

/** A binding's range as percentages of the knob's travel: "0–100 %", "100–0 %". */
export function rangeLabel(b: Pick<LiveBinding, 'low' | 'high'>): string {
  return `${Math.round(b.low * 100)}–${Math.round(b.high * 100)} %`;
}
