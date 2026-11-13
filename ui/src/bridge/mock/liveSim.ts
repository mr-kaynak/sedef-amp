// SPDX-License-Identifier: AGPL-3.0-or-later
// The mock's live state (docs/live.md): scenes, bindings, program changes and learn, with the
// plug-in's rules (plugin/src/LiveState.cpp) but on one thread: a switch writes the parameters at
// once. Tests drive the controller with midi() and the audio clock with advance().
import type {
  LiveActivity,
  LiveBinding,
  LiveLearn,
  LiveState,
  LiveSwitchMode,
  ParamSpec,
  ParamValues,
  WrapperType,
} from '../protocol';
import { isGlobalParam, sameValue } from '../../params/presetScope';
import {
  actionFires,
  canRememberInScenes,
  continuousValue,
  isDoubleTap,
  isLearnable,
  isSwitchTarget,
  MAX_SCENE_VALUES,
  NUM_SCENES,
  parseTarget,
  SCENE_SWITCHES,
  SwitchClassifier,
  switchValue,
} from '../../features/live/model';
import type { LooperPress } from './looperSim';

/** A preset's "scenes" object, as the plug-in writes it (§6.4). */
export interface ScenesFile {
  active: number;
  values: string[];
  list: (Record<string, number> | null)[];
}

/** The scenes of the preset in play. The active scene's values are the live parameters. */
interface Bank {
  active: number;
  made: boolean[];
  /** Picked values, in the order picked. */
  values: string[];
  /** Per scene: every scene parameter's value (made scenes only are meaningful). */
  scenes: ParamValues[];
}

export interface LiveHost {
  readonly specs: Record<string, ParamSpec>;
  value(id: string): number;
  /** Writes parameters as the plug-in's tick does, and announces them. */
  write(values: ParamValues): void;
  /** A program change's preset load (not a Recent pick). */
  loadPreset(id: string): boolean;
  stepPreset(delta: number): void;
  /** The preset at a position in library order. */
  presetIdAt(position: number): string | null;
  emit(state: LiveState): void;
  emitActivity(activity: LiveActivity): void;
  /** A footswitch press for the looper (docs/live.md §4.5). */
  looper(press: LooperPress): void;
  /** Scenes count towards "dirty". */
  changed(): void;
}

interface Settings {
  bindings: Omit<LiveBinding, 'index' | 'switch'>[];
  programChannel: number; // 0 any, 1-16
  programs: Map<number, { preset?: string; scene?: number }>; // by program 1-128
}

function clone(bank: Bank): Bank {
  return {
    active: bank.active,
    made: [...bank.made],
    values: [...bank.values],
    scenes: bank.scenes.map((s) => ({ ...s })),
  };
}

export class LiveSim {
  private bank: Bank;
  /** What "dirty" compares against: the bank as loaded or last saved, every value listed. */
  private loadedBank: Bank;
  private settings: Settings = { bindings: [], programChannel: 0, programs: new Map() };
  private learn: (LiveLearn & { classifier?: SwitchClassifier }) | null = null;
  private revision = 0;
  private activity: LiveActivity = { count: 0, status: 0, data1: 0, data2: 0 };
  private inputs: { id: string; name: string; enabled: boolean }[];
  /** Per looper:main binding: when its press waits for a second one (seconds), §4.5. */
  private lastTap = new Map<string, number>();
  /** The audio clock, seconds. */
  clock = 0;

  constructor(
    private readonly host: LiveHost,
    private readonly wrapperType: WrapperType,
    private readonly isStandalone: boolean,
  ) {
    this.bank = this.fromFile(undefined, (id) => host.value(id));
    this.loadedBank = clone(this.bank);
    this.inputs = this.isStandalone
      ? [
          { id: 'mock-floor', name: 'Floor controller', enabled: true },
          { id: 'mock-keys', name: 'USB keyboard', enabled: false },
        ]
      : [];
  }

  // --- The bridge -------------------------------------------------------------------------------

  state(): LiveState {
    const midiIn =
      this.wrapperType === 'VST3'
        ? 'controllers'
        : this.wrapperType === 'AudioUnit'
          ? 'host'
          : 'full';
    return {
      scenes: {
        active: this.bank.active,
        count: NUM_SCENES,
        made: [...this.bank.made],
        values: [...this.bank.values],
        maxValues: MAX_SCENE_VALUES,
        switches: [...SCENE_SWITCHES],
      },
      bindings: this.settings.bindings.map((b, index) => ({
        ...b,
        index,
        switch: isSwitchTarget(b.target, this.host.specs),
        ...(b.target === 'looper:main' ? { doubleTap: b.doubleTap === true } : {}),
      })),
      maxBindings: 64,
      programs: {
        channel: this.settings.programChannel,
        assigned: [...this.settings.programs.entries()]
          .sort(([a], [b]) => a - b)
          .map(([program, t]) => ({ program, ...t })),
      },
      learn: this.learn
        ? { target: this.learn.target, phase: this.learn.phase, binding: this.learn.binding }
        : null,
      midiIn,
      inputs: this.inputs.length ? this.inputs.map((i) => ({ ...i })) : null,
      revision: ++this.revision,
    };
  }

  selectScene(scene: number): LiveState {
    if (scene >= 0 && scene < NUM_SCENES && scene !== this.bank.active) {
      this.bank.scenes[this.bank.active] = this.liveScene();
      if (!this.bank.made[scene]) this.bank.scenes[scene] = this.liveScene();
      this.bank.made[scene] = true;
      this.bank.active = scene;
      this.host.write({ ...this.bank.scenes[scene], scene });
      this.host.changed();
    }
    return this.state();
  }

  copyScene(scene: number): LiveState {
    if (scene >= 0 && scene < NUM_SCENES && scene !== this.bank.active) {
      this.bank.scenes[scene] = this.liveScene();
      this.bank.made[scene] = true;
      this.host.changed();
    }
    return this.state();
  }

  remember(id: string, remember: boolean): LiveState {
    const spec = this.host.specs[id];
    const has = this.bank.values.includes(id);
    if (spec && canRememberInScenes(spec)) {
      if (remember && !has && this.bank.values.length < MAX_SCENE_VALUES) {
        this.bank.values.push(id);
        for (const s of this.bank.scenes) s[id] = this.host.value(id);
        this.host.changed();
      } else if (!remember && has) {
        this.bank.values = this.bank.values.filter((v) => v !== id);
        for (const s of this.bank.scenes) delete s[id];
        this.host.changed();
      }
    }
    return this.state();
  }

  startLearn(target: string): LiveState {
    const t = parseTarget(target);
    // The looper is the standalone app's (§4.5).
    if (t && (t.kind !== 'looper' || this.isStandalone))
      this.learn = { target, phase: 'waiting', binding: -1 };
    return this.state();
  }

  cancelLearn(): LiveState {
    if (this.learn?.phase === 'listening') this.finish(this.learn.classifier?.mode ?? 'latch');
    this.learn = null;
    return this.state();
  }

  forget(target: string): LiveState {
    this.settings.bindings = this.settings.bindings.filter((b) => b.target !== target);
    return this.state();
  }

  updateBinding(
    index: number,
    changes: Partial<Pick<LiveBinding, 'mode' | 'low' | 'high' | 'doubleTap'>>,
  ) {
    const b = this.settings.bindings[index];
    if (b) {
      if (changes.mode === 'toggle' || changes.mode === 'latch') b.mode = changes.mode;
      const unit = (v: unknown, fallback: number) =>
        typeof v === 'number' && Number.isFinite(v) ? Math.min(1, Math.max(0, v)) : fallback;
      b.low = unit(changes.low, b.low);
      b.high = unit(changes.high, b.high);
      if (typeof changes.doubleTap === 'boolean' && b.target === 'looper:main')
        b.doubleTap = changes.doubleTap;
    }
    return this.state();
  }

  removeBinding(index: number): LiveState {
    this.settings.bindings.splice(index, 1);
    this.learn = null;
    return this.state();
  }

  clearBindings(): LiveState {
    this.settings.bindings = [];
    this.learn = null;
    return this.state();
  }

  setProgram(program: number, target: { preset: string } | { scene: number } | null): LiveState {
    if (program >= 1 && program <= 128) {
      if (target && 'scene' in target && target.scene >= 0 && target.scene < NUM_SCENES)
        this.settings.programs.set(program, { scene: target.scene });
      else if (target && 'preset' in target && target.preset)
        this.settings.programs.set(program, { preset: target.preset });
      else this.settings.programs.delete(program);
    }
    return this.state();
  }

  setProgramChannel(channel: number): LiveState {
    this.settings.programChannel = Math.min(16, Math.max(0, Math.round(channel)));
    return this.state();
  }

  setInput(id: string, enabled: boolean): LiveState {
    const input = this.inputs.find((i) => i.id === id);
    if (input) input.enabled = enabled;
    return this.state();
  }

  // --- The controller and the audio clock (tests, demos) ---------------------------------------

  /** One control or program change, at the audio clock (or `time`, seconds). */
  midi(status: number, data1: number, data2 = 0, time = this.clock): void {
    this.clock = Math.max(this.clock, time);
    this.activity = { count: this.activity.count + 1, status, data1, data2 };
    this.host.emitActivity({ ...this.activity });
    const kind = status & 0xf0;
    const channel = (status & 0x0f) + 1;
    if (this.learn && this.learn.phase !== 'done') {
      this.learnFrom(kind, channel, data1, data2, time);
      return;
    }
    if (kind === 0xb0) {
      for (const b of [...this.settings.bindings])
        if (b.channel === channel && b.controller === data1) this.act(b, data2, time);
    } else if (kind === 0xc0) {
      const ch = this.settings.programChannel;
      if (ch !== 0 && ch !== channel) return;
      const assigned = this.settings.programs.get(data1 + 1);
      if (assigned?.scene !== undefined) this.switchFromMidi(assigned.scene);
      else {
        const id = assigned?.preset ?? this.host.presetIdAt(data1);
        if (id) this.host.loadPreset(id);
      }
    }
  }

  /** Moves the audio clock on: a learn watching for a release ends once its window has passed. */
  advance(seconds: number): void {
    this.clock += seconds;
    if (this.learn?.phase === 'listening') {
      const mode = this.learn.classifier?.elapsed(this.clock);
      if (mode) this.finish(mode);
    }
  }

  // --- Presets, snapshots, "dirty" --------------------------------------------------------------

  /** The scenes a preset file holds; `base` is its "parameters" (the active scene's sound). */
  fromFile(file: ScenesFile | undefined, base: (id: string) => number): Bank {
    const values = (file?.values ?? []).filter((id, i, all) => {
      const spec = this.host.specs[id];
      return spec && canRememberInScenes(spec) && all.indexOf(id) === i;
    });
    const picked = values.slice(0, MAX_SCENE_VALUES);
    const ids = [...SCENE_SWITCHES, ...picked];
    const baseScene = Object.fromEntries(ids.map((id) => [id, base(id)]));
    const active =
      typeof file?.active === 'number' ? Math.min(NUM_SCENES - 1, Math.max(0, file.active)) : 0;
    const made = Array.from({ length: NUM_SCENES }, (_, s) => s === active);
    const scenes = Array.from({ length: NUM_SCENES }, (_, s) => {
      const entry = file?.list[s];
      const scene = { ...baseScene };
      if (entry && typeof entry === 'object') {
        made[s] = true;
        for (const [id, v] of Object.entries(entry)) {
          const spec = this.host.specs[id];
          if (spec && id in scene && typeof v === 'number')
            scene[id] =
              spec.kind === 'bool' ? (v >= 0.5 ? 1 : 0) : Math.min(spec.max, Math.max(spec.min, v));
        }
      }
      return scene;
    });
    // "parameters" are the active scene's sound, whatever its entry says.
    scenes[active] = { ...baseScene };
    return { active, made, values: picked, scenes };
  }

  /** The "scenes" object for a preset file: only what differs from the live sound. */
  toFile(): ScenesFile | undefined {
    if (
      this.bank.values.length === 0 &&
      this.bank.made.every((m, s) => !m || s === this.bank.active)
    )
      return undefined;
    const live = this.liveScene();
    const list = this.bank.scenes.map((scene, s) => {
      if (!this.bank.made[s]) return null;
      if (s === this.bank.active) return {};
      return Object.fromEntries(Object.entries(scene).filter(([id, v]) => v !== live[id]));
    });
    while (list.length && list[list.length - 1] === null) list.pop();
    return { active: this.bank.active, values: [...this.bank.values], list };
  }

  /** A preset loaded (or Init): its scenes become the live and the loaded bank. */
  select(file: ScenesFile | undefined): void {
    this.bank = this.fromFile(file, (id) => this.host.value(id));
    this.loadedBank = clone(this.bank);
    this.host.write({ scene: this.bank.active });
    this.host.emit(this.state());
  }

  /** The saved state is the new reference. */
  saved(): void {
    this.bank.scenes[this.bank.active] = this.liveScene();
    this.loadedBank = clone(this.bank);
  }

  /** Snapshots (browser, Undo) and A/B carry the scenes exactly. */
  capture(): { bank: Bank; loaded: Bank } {
    const bank = clone(this.bank);
    bank.scenes[bank.active] = this.liveScene();
    return { bank, loaded: clone(this.loadedBank) };
  }

  restore(snapshot: { bank: Bank; loaded: Bank }): void {
    this.bank = clone(snapshot.bank);
    this.loadedBank = clone(snapshot.loaded);
    this.host.write({ scene: this.bank.active });
    this.host.emit(this.state());
  }

  /** The parameter check of "dirty" leaves these to the scenes. */
  /** The live sound becomes the scene in use as loaded (the demo's overrides are part of it). */
  adoptLive(): void {
    this.bank.scenes[this.bank.active] = this.liveScene();
    this.loadedBank = clone(this.bank);
  }

  owns(id: string): boolean {
    return SCENE_SWITCHES.includes(id) || this.bank.values.includes(id);
  }

  /** PresetManager's "dirty" for the scenes (LiveState::differs, §6.4). */
  differs(): boolean {
    const now = this.bank;
    const ref = this.loadedBank;
    if (now.values.length !== ref.values.length || now.values.some((v) => !ref.values.includes(v)))
      return true;
    if (ref.made.some((m, s) => m && !now.made[s])) return true;
    const live = this.liveScene();
    for (let s = 0; s < NUM_SCENES; ++s) {
      if (!now.made[s]) continue;
      const scene = s === now.active ? live : now.scenes[s];
      const reference = ref.made[s] ? ref.scenes[s] : ref.scenes[ref.active];
      for (const [id, v] of Object.entries(scene ?? {})) {
        const spec = this.host.specs[id];
        if (spec && !sameValue(v, reference?.[id] ?? spec.default, spec.min, spec.max)) return true;
      }
    }
    return false;
  }

  // --- Internals ------------------------------------------------------------------------------

  private liveScene(): ParamValues {
    return Object.fromEntries(
      [...SCENE_SWITCHES, ...this.bank.values].map((id) => [id, this.host.value(id)]),
    );
  }

  private switchFromMidi(scene: number): void {
    if (scene !== this.bank.active) this.selectScene(scene);
    this.host.emit(this.state());
  }

  private act(b: Settings['bindings'][number], value: number, time: number): void {
    const t = parseTarget(b.target);
    if (!t) return;
    switch (t.kind) {
      case 'param': {
        const spec = this.host.specs[t.id];
        if (!spec) return;
        if (t.id === 'scene') {
          this.switchFromMidi(continuousValue(spec, b.low, b.high, value));
          return;
        }
        if (spec.kind === 'bool') {
          const on = switchValue(b.mode, this.host.value(t.id) >= 0.5, value);
          if (on !== null) this.host.write({ [t.id]: on ? 1 : 0 });
        } else this.host.write({ [t.id]: continuousValue(spec, b.low, b.high, value) });
        if (!isGlobalParam(t.id)) this.host.changed();
        return;
      }
      case 'scene':
        if (actionFires(b.mode, value)) this.switchFromMidi(t.scene);
        return;
      case 'sceneNext':
      case 'scenePrevious':
        if (actionFires(b.mode, value))
          this.switchFromMidi(
            (this.bank.active + (t.kind === 'sceneNext' ? 1 : NUM_SCENES - 1)) % NUM_SCENES,
          );
        return;
      case 'presetNext':
      case 'presetPrevious':
        if (actionFires(b.mode, value)) this.host.stepPreset(t.kind === 'presetNext' ? 1 : -1);
        return;
      case 'looper': {
        if (!actionFires(b.mode, value)) return;
        if (t.action !== 'main') return this.host.looper(t.action);
        // A second press less than the window after the first stops (§4.5).
        if (b.doubleTap && isDoubleTap(this.lastTap.get(b.target) ?? -1, time)) {
          this.lastTap.delete(b.target);
          return this.host.looper('doubleTap');
        }
        if (b.doubleTap) this.lastTap.set(b.target, time);
        return this.host.looper('main');
      }
    }
  }

  private learnFrom(kind: number, channel: number, data1: number, data2: number, time: number) {
    const l = this.learn;
    if (!l) return;
    if (l.phase === 'waiting') {
      if (kind !== 0xb0 || !isLearnable(data1)) return;
      const binding = {
        target: l.target,
        channel,
        controller: data1,
        mode: 'toggle' as LiveSwitchMode,
        low: 0,
        high: 1,
      };
      const at = this.settings.bindings.findIndex((b) => b.target === l.target);
      if (at >= 0) this.settings.bindings[at] = binding;
      else if (this.settings.bindings.length < 64) this.settings.bindings.push(binding);
      else return;
      l.binding = at >= 0 ? at : this.settings.bindings.length - 1;
      if (!isSwitchTarget(l.target, this.host.specs)) {
        this.finish(null);
        return;
      }
      l.classifier = new SwitchClassifier();
      l.phase = 'listening';
      const mode = l.classifier.first(data2, time);
      if (mode) this.finish(mode);
      else this.host.emit(this.state());
      return;
    }
    const b = this.settings.bindings[l.binding];
    if (kind === 0xb0 && b && b.channel === channel && b.controller === data1) {
      const mode = l.classifier?.next(data2, time);
      if (mode) this.finish(mode);
    }
  }

  private finish(mode: LiveSwitchMode | null): void {
    const l = this.learn;
    if (!l) return;
    const b = this.settings.bindings[l.binding];
    if (b && mode) b.mode = mode;
    l.phase = 'done';
    this.host.emit(this.state());
  }

  /** A demo setup for screenshots and the browser preview. */
  seedDemo(): void {
    this.settings.bindings = [
      { target: 'param:pearlEnabled', channel: 1, controller: 20, mode: 'toggle', low: 0, high: 1 },
      { target: 'param:driftEnabled', channel: 1, controller: 21, mode: 'toggle', low: 0, high: 1 },
      { target: 'param:conchPosition', channel: 1, controller: 11, mode: 'latch', low: 0, high: 1 },
      {
        target: 'param:outputGain',
        channel: 1,
        controller: 7,
        mode: 'latch',
        low: 0.35,
        high: 0.8,
      },
      { target: 'scene:next', channel: 1, controller: 24, mode: 'toggle', low: 0, high: 1 },
      { target: 'preset:next', channel: 1, controller: 25, mode: 'latch', low: 0, high: 1 },
      {
        target: 'looper:main',
        channel: 1,
        controller: 26,
        mode: 'toggle',
        low: 0,
        high: 1,
        doubleTap: true,
      },
    ];
    this.settings.programs.set(1, { preset: 'factory:nacre' });
    this.settings.programs.set(8, { scene: 2 });
    // A preset saved with three scenes: the verse as loaded, a chorus with Drift and more delay,
    // a solo with Pearl, Drift and a longer reverb.
    const v = (id: string) => this.host.value(id);
    this.bank.values = ['driftMix', 'fathomMix'];
    const base = this.liveScene();
    this.bank.scenes = this.bank.scenes.map(() => ({ ...base }));
    this.bank.scenes[1] = { ...base, driftEnabled: 1, driftMix: Math.min(1, v('driftMix') + 0.15) };
    this.bank.scenes[2] = {
      ...base,
      pearlEnabled: 1,
      driftEnabled: 1,
      fathomEnabled: 1,
      fathomMix: Math.min(1, v('fathomMix') + 0.1),
    };
    this.bank.made = this.bank.made.map((_, i) => i < 3);
    this.loadedBank = clone(this.bank);
    this.activity = { count: 42, status: 0xb0, data1: 11, data2: 87 };
    this.host.emitActivity({ ...this.activity });
  }

  /** The last message, for the hello-time activity (screenshots). */
  lastActivity(): LiveActivity {
    return { ...this.activity };
  }
}
