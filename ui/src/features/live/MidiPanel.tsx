// SPDX-License-Identifier: AGPL-3.0-or-later
// The MIDI panel (docs/live.md §3-§6): what arrives, the learned controls, program changes and
// what scenes keep. Learning itself starts from a control's menu (ControlMenu) or from the
// action buttons here; the banner (LearnBanner) follows it.
import { useId, useRef, useState } from 'react';
import type { KeyboardEvent } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon } from '../../controls/Icon';
import nacre from '../../controls/NacreFill.module.css';
import segmented from '../../controls/Segmented.module.css';
import select from '../../controls/ChoiceSelect.module.css';
import { useRadioGroup } from '../../controls/useRadioGroup';
import type { LiveBinding, LiveState, ParamSpec } from '../../bridge/protocol';
import { useParamStore } from '../../params/paramStore';
import { usePresetStore } from '../../state/presetStore';
import { bindingFor, liveActions, useLiveStore } from '../../state/liveStore';
import { activityLabel, NUM_SCENES, rangeLabel, sourceLabel, targetLabel } from './model';
import styles from './MidiPanel.module.css';

type Tab = 'controls' | 'programs' | 'scenes';
const TABS: readonly { id: Tab; label: string }[] = [
  { id: 'controls', label: 'Controls' },
  { id: 'programs', label: 'Programs' },
  { id: 'scenes', label: 'Scenes' },
];

/**
 * After a row goes, focus what took its place: the same button on the next row, else on the one
 * before, else `fallback` (a selector in the same tab panel). Without it focus would fall to the
 * page and Tab would start over.
 */
function refocusAfter(from: HTMLElement, done: Promise<void>, index: number, fallback: string) {
  const scope = from.closest<HTMLElement>('[role="tabpanel"]');
  const selector = `[data-row-action="${from.dataset.rowAction ?? ''}"]`;
  void done.then(() =>
    setTimeout(() => {
      const rows = scope?.querySelectorAll<HTMLElement>(selector) ?? [];
      const next = rows[index] ?? rows[index - 1] ?? scope?.querySelector<HTMLElement>(fallback);
      next?.focus();
    }),
  );
}

/** The last tab the player looked at, for this window. */
let lastTab: Tab = 'controls';
function rememberTab(tab: Tab): void {
  lastTab = tab;
}

export function MidiPanel({ onClose }: { onClose: () => void }) {
  const state = useLiveStore((s) => s.state);
  const [tab, setTab] = useState<Tab>(lastTab);
  const refs = useRef<(HTMLButtonElement | null)[]>([]);
  const ids = useId();
  if (!state) return null;

  const choose = (t: Tab) => {
    rememberTab(t);
    setTab(t);
  };
  const onKeyDown = (e: KeyboardEvent) => {
    const index = TABS.findIndex((t) => t.id === tab);
    const next =
      e.key === 'ArrowRight'
        ? (index + 1) % TABS.length
        : e.key === 'ArrowLeft'
          ? (index - 1 + TABS.length) % TABS.length
          : e.key === 'Home'
            ? 0
            : e.key === 'End'
              ? TABS.length - 1
              : null;
    if (next === null) return;
    e.preventDefault();
    const t = TABS[next];
    if (!t) return;
    choose(t.id);
    refs.current[next]?.focus();
  };

  return (
    <div className={styles.panel}>
      <header className={styles.head}>
        <h3 className={styles.title}>MIDI</h3>
        <Activity />
      </header>
      <FormatNote state={state} />
      {state.inputs && <Inputs state={state} />}
      <div
        role="tablist"
        aria-label="MIDI settings"
        className={`${segmented.segmented} ${styles.tabs}`}
        onKeyDown={onKeyDown}
      >
        {TABS.map((t, i) => (
          <button
            key={t.id}
            ref={(el) => {
              refs.current[i] = el;
            }}
            type="button"
            role="tab"
            id={`${ids}-${t.id}`}
            aria-selected={tab === t.id}
            aria-controls={`${ids}-panel`}
            tabIndex={tab === t.id ? 0 : -1}
            className={`${nacre.fill} ${segmented.option} ${styles.tab}`}
            onClick={() => choose(t.id)}
          >
            {t.label}
          </button>
        ))}
      </div>
      <div
        role="tabpanel"
        id={`${ids}-panel`}
        aria-labelledby={`${ids}-${tab}`}
        className={styles.body}
      >
        {tab === 'controls' && <Controls state={state} onClose={onClose} />}
        {tab === 'programs' && <Programs state={state} />}
        {tab === 'scenes' && <Scenes state={state} />}
      </div>
    </div>
  );
}

function Activity() {
  const activity = useLiveStore((s) => s.activity);
  const label = activity && activity.count > 0 ? activityLabel(activity) : null;
  return (
    <p className={styles.activity}>
      {label ? (
        <>
          <span className={styles.quiet}>Last received</span> {label}
        </>
      ) : (
        <span className={styles.quiet}>Nothing received yet</span>
      )}
    </p>
  );
}

function FormatNote({ state }: { state: LiveState }) {
  if (state.midiIn === 'full') return null;
  return (
    <p className={styles.formatNote}>
      <Icon name="info" size={14} className={styles.formatIcon} />
      {state.midiIn === 'controllers'
        ? 'The VST3 version receives controllers but no program changes. Use the CLAP version or the standalone app to change presets by program change.'
        : 'What arrives depends on the host. Logic Pro and GarageBand send no MIDI to effects: assign their controller setup to Sedef Amp’s parameters instead (Scene is one).'}
    </p>
  );
}

function Inputs({ state }: { state: LiveState }) {
  const inputs = state.inputs ?? [];
  return (
    <fieldset className={styles.inputs}>
      <legend className={styles.label}>Inputs</legend>
      {inputs.length === 0 ? (
        <p className={styles.note}>No MIDI inputs found. Plug in a controller.</p>
      ) : (
        inputs.map((input) => (
          <label key={input.id} className={styles.check}>
            <input
              type="checkbox"
              checked={input.enabled}
              onChange={(e) => void liveActions.setInput(input.id, e.target.checked)}
            />
            {input.name}
          </label>
        ))
      )}
    </fieldset>
  );
}

// ---------------------------------------------------------------------------------------------
// Controls

const ACTIONS: readonly { target: string; label: string }[] = [
  { target: 'scene:previous', label: 'Previous scene' },
  { target: 'scene:next', label: 'Next scene' },
  { target: 'preset:previous', label: 'Previous preset' },
  { target: 'preset:next', label: 'Next preset' },
];

function Controls({ state, onClose }: { state: LiveState; onClose: () => void }) {
  const specs = useParamStore((s) => s.specs);
  const [confirming, setConfirming] = useState(false);
  const bindings = state.bindings;
  const learning = state.learn?.target;

  return (
    <>
      <div className={styles.listHead}>
        <p className={styles.count}>
          {bindings.length} of {state.maxBindings} learned
        </p>
        {bindings.length > 0 &&
          (confirming ? (
            <span
              className={styles.confirm}
              role="group"
              aria-label={`Forget all ${bindings.length} learned controls?`}
            >
              <span aria-hidden="true">Forget all {bindings.length}?</span>
              <button
                type="button"
                className={`${buttons.solid} ${buttons.small}`}
                data-row-action="clear"
                onClick={(e) => {
                  refocusAfter(e.currentTarget, liveActions.clearBindings(), -1, '[data-learn]');
                  setConfirming(false);
                }}
              >
                Forget all
              </button>
              <button
                type="button"
                className={`${buttons.ghost} ${buttons.small}`}
                autoFocus
                onClick={() => setConfirming(false)}
              >
                Keep
              </button>
            </span>
          ) : (
            <button
              type="button"
              className={`${buttons.ghost} ${buttons.small}`}
              onClick={() => setConfirming(true)}
            >
              Forget all
            </button>
          ))}
      </div>
      {bindings.length === 0 ? (
        <p className={styles.empty}>
          Right-click a knob, switch or scene (or focus it and press Shift+F10), choose{' '}
          <b>Learn MIDI</b>, then move a pedal or press a footswitch.
        </p>
      ) : (
        <ul className={styles.list} aria-label="Learned controls">
          {bindings.map((b) => (
            <BindingRow key={`${b.index}:${b.target}`} binding={b} specs={specs} />
          ))}
        </ul>
      )}
      <div className={styles.actions}>
        <p className={styles.label} id="midi-actions-label">
          Learn a footswitch for
        </p>
        <div className={styles.actionGrid} role="group" aria-labelledby="midi-actions-label">
          {ACTIONS.map((a) => {
            const bound = bindingFor(state, a.target);
            const waiting = learning === a.target;
            return (
              <button
                key={a.target}
                type="button"
                className={`${buttons.ghost} ${buttons.small} ${styles.action}`}
                aria-pressed={waiting}
                data-learn={a.target}
                onClick={() => {
                  if (waiting) void liveActions.cancelLearn();
                  else {
                    void liveActions.learn(a.target);
                    // The banner takes over; the panel would cover what the player looks at.
                    onClose();
                  }
                }}
              >
                <span>{a.label}</span>
                <span className={styles.quiet}>{bound ? sourceLabel(bound) : 'Learn'}</span>
              </button>
            );
          })}
        </div>
      </div>
    </>
  );
}

function BindingRow({
  binding: b,
  specs,
}: {
  binding: LiveBinding;
  specs: Record<string, ParamSpec>;
}) {
  const name = targetLabel(b.target, specs);
  const source = sourceLabel(b);
  return (
    <li className={styles.row}>
      <div className={styles.what}>
        <span className={styles.target}>{name}</span>
        <span className={styles.source}>{source}</span>
      </div>
      {b.switch ? <ModeChoice binding={b} name={name} /> : <Range binding={b} name={name} />}
      <button
        type="button"
        className={`${buttons.ghost} ${buttons.small} ${buttons.icon}`}
        aria-label={`Forget ${name} (${source})`}
        title="Forget"
        data-row-action="forget"
        onClick={(e) =>
          refocusAfter(e.currentTarget, liveActions.removeBinding(b.index), b.index, '[data-learn]')
        }
      >
        <Icon name="close" size={12} />
      </button>
    </li>
  );
}

const MODES = [
  { mode: 'toggle', label: 'Toggle', hint: 'Each press switches it' },
  { mode: 'latch', label: 'Latch', hint: 'Follows the switch: down is on' },
] as const;

function ModeChoice({ binding: b, name }: { binding: LiveBinding; name: string }) {
  const { onKeyDown, radio } = useRadioGroup(
    MODES.length,
    MODES.findIndex((m) => m.mode === b.mode),
    (i) => void liveActions.updateBinding(b.index, { mode: MODES[i]?.mode ?? 'toggle' }),
  );
  return (
    <div
      role="radiogroup"
      aria-label={`${name} footswitch`}
      className={`${segmented.segmented} ${segmented.sm}`}
      onKeyDown={onKeyDown}
    >
      {MODES.map((m, i) => (
        <button
          key={m.mode}
          type="button"
          className={`${nacre.fill} ${segmented.option}`}
          title={m.hint}
          {...radio(i)}
        >
          {m.label}
        </button>
      ))}
    </div>
  );
}

function Range({ binding: b, name }: { binding: LiveBinding; name: string }) {
  const inverted = b.low > b.high;
  return (
    <div className={styles.range} role="group" aria-label={`${name} range, ${rangeLabel(b)}`}>
      <Percent
        value={b.low}
        label={`${name} at controller 0, percent of its travel`}
        onCommit={(low) => void liveActions.updateBinding(b.index, { low })}
      />
      <span aria-hidden="true">–</span>
      <Percent
        value={b.high}
        label={`${name} at controller 127, percent of its travel`}
        onCommit={(high) => void liveActions.updateBinding(b.index, { high })}
      />
      <span className={styles.quiet} aria-hidden="true">
        %
      </span>
      <button
        type="button"
        className={`${buttons.ghost} ${buttons.small} ${styles.invert}`}
        aria-pressed={inverted}
        title="Swap the ends: the knob turns down as the pedal goes up"
        onClick={() => void liveActions.updateBinding(b.index, { low: b.high, high: b.low })}
      >
        Invert
      </button>
    </div>
  );
}

/** 0-100 for a 0-1 value. Commits on Enter or leaving the field; Escape puts it back. */
function Percent({
  value,
  label,
  onCommit,
}: {
  value: number;
  label: string;
  onCommit: (value: number) => void;
}) {
  const shown = Math.round(value * 100);
  const commit = (input: HTMLInputElement) => {
    const n = Number(input.value);
    if (input.value.trim() === '' || !Number.isFinite(n)) {
      input.value = String(shown);
      return;
    }
    const clamped = Math.min(100, Math.max(0, Math.round(n)));
    input.value = String(clamped);
    if (clamped !== shown) onCommit(clamped / 100);
  };
  return (
    <input
      // A new value from the plug-in redraws the field with it.
      key={shown}
      type="number"
      inputMode="numeric"
      min={0}
      max={100}
      step={1}
      defaultValue={shown}
      aria-label={label}
      className={styles.percent}
      onBlur={(e) => commit(e.currentTarget)}
      onKeyDown={(e) => {
        if (e.key === 'Enter') commit(e.currentTarget);
        if (e.key === 'Escape' && e.currentTarget.value !== String(shown)) {
          e.stopPropagation();
          e.currentTarget.value = String(shown);
        }
      }}
    />
  );
}

// ---------------------------------------------------------------------------------------------
// Programs

function Programs({ state }: { state: LiveState }) {
  const presets = usePresetStore((s) => s.state.items);
  const names = new Map(presets.map((p) => [p.id, p.name]));
  const [program, setProgram] = useState(1);
  const [target, setTarget] = useState('scene:0');
  const assigned = state.programs.assigned;

  const assign = () => {
    if (target.startsWith('scene:'))
      void liveActions.setProgram(program, { scene: Number(target.slice(6)) });
    else if (target.startsWith('preset:'))
      void liveActions.setProgram(program, { preset: target.slice(7) });
  };

  return (
    <>
      <label className={styles.field}>
        <span className={styles.label}>Listen on</span>
        <span className={select.wrap}>
          <select
            className={select.select}
            value={state.programs.channel}
            onChange={(e) => void liveActions.setProgramChannel(Number(e.target.value))}
          >
            <option value={0}>Any channel</option>
            {Array.from({ length: 16 }, (_, i) => (
              <option key={i} value={i + 1}>
                Channel {i + 1}
              </option>
            ))}
          </select>
          <Icon name="down" size={14} className={select.chevron} />
        </span>
      </label>
      <p className={styles.note}>
        Programs not assigned below load presets in library order: program 1 loads the first preset,
        program 2 the second, and so on.
      </p>
      {assigned.length > 0 && (
        <ul className={styles.list} aria-label="Assigned programs">
          {assigned.map((a) => {
            const what =
              a.scene !== undefined
                ? `Scene ${a.scene + 1}`
                : (names.get(a.preset ?? '') ?? 'A missing preset');
            return (
              <li key={a.program} className={styles.row}>
                <div className={styles.what}>
                  <span className={styles.target}>Program {a.program}</span>
                  <span className={styles.source}>{what}</span>
                </div>
                <button
                  type="button"
                  className={`${buttons.ghost} ${buttons.small} ${buttons.icon}`}
                  aria-label={`Unassign program ${a.program} (${what})`}
                  title="Back to library order"
                  data-row-action="unassign"
                  onClick={(e) =>
                    refocusAfter(
                      e.currentTarget,
                      liveActions.setProgram(a.program, null),
                      assigned.indexOf(a),
                      'input',
                    )
                  }
                >
                  <Icon name="close" size={12} />
                </button>
              </li>
            );
          })}
        </ul>
      )}
      <form
        className={styles.assign}
        aria-label="Assign a program"
        onSubmit={(e) => {
          e.preventDefault();
          assign();
        }}
      >
        <label className={styles.programField}>
          <span className={styles.label}>Program</span>
          <input
            type="number"
            inputMode="numeric"
            min={1}
            max={128}
            step={1}
            value={program}
            className={styles.percent}
            onChange={(e) => {
              const n = Math.round(Number(e.target.value));
              if (Number.isFinite(n)) setProgram(Math.min(128, Math.max(1, n)));
            }}
          />
        </label>
        <span className={`${select.wrap} ${styles.targetSelect}`}>
          <select
            className={select.select}
            aria-label={`Program ${program} selects`}
            value={target}
            onChange={(e) => setTarget(e.target.value)}
          >
            <optgroup label="Scenes of the preset in use">
              {Array.from({ length: NUM_SCENES }, (_, i) => (
                <option key={i} value={`scene:${i}`}>
                  Scene {i + 1}
                </option>
              ))}
            </optgroup>
            <optgroup label="Presets">
              {presets.map((p) => (
                <option key={p.id} value={`preset:${p.id}`}>
                  {p.name}
                </option>
              ))}
            </optgroup>
          </select>
          <Icon name="down" size={14} className={select.chevron} />
        </span>
        <button type="submit" className={`${buttons.solid} ${buttons.small}`}>
          Assign
        </button>
      </form>
    </>
  );
}

// ---------------------------------------------------------------------------------------------
// Scenes

function Scenes({ state }: { state: LiveState }) {
  const specs = useParamStore((s) => s.specs);
  const { active, count, values, maxValues } = state.scenes;
  const [to, setTo] = useState(active === 0 ? 1 : 0);
  const others = Array.from({ length: count }, (_, i) => i).filter((i) => i !== active);
  const target = others.includes(to) ? to : (others[0] ?? 0);

  return (
    <>
      <p className={styles.note}>
        Every scene keeps which pedals are on, the EQ, Doubler, Drift and Fathom. Scenes are saved
        with the preset. A scene you have not used yet starts as a copy of the one you left.
      </p>
      <p className={`${styles.label} ${styles.subhead}`} id="scene-values-label">
        Knobs each scene keeps ({values.length} of {maxValues})
      </p>
      {values.length === 0 ? (
        <p className={styles.empty}>
          None yet. Right-click a knob and choose <b>Remember in scenes</b>.
        </p>
      ) : (
        <ul className={styles.list} aria-labelledby="scene-values-label">
          {values.map((id) => {
            const name = specs[id]?.name ?? id;
            return (
              <li key={id} className={styles.row}>
                <span className={styles.target}>{name}</span>
                <button
                  type="button"
                  className={`${buttons.ghost} ${buttons.small}`}
                  aria-label={`${name}: same in every scene`}
                  title="Keep one value for every scene"
                  data-row-action="same"
                  onClick={(e) =>
                    refocusAfter(
                      e.currentTarget,
                      liveActions.remember(id, false),
                      values.indexOf(id),
                      'select',
                    )
                  }
                >
                  Same in every scene
                </button>
              </li>
            );
          })}
        </ul>
      )}
      <div className={styles.copy}>
        <label className={styles.copyField}>
          <span className={styles.label}>Copy scene {active + 1} to</span>
          <span className={select.wrap}>
            <select
              className={select.select}
              value={target}
              onChange={(e) => setTo(Number(e.target.value))}
            >
              {others.map((i) => (
                <option key={i} value={i}>
                  Scene {i + 1}
                  {state.scenes.made[i] ? '' : ' (empty)'}
                </option>
              ))}
            </select>
            <Icon name="down" size={14} className={select.chevron} />
          </span>
        </label>
        <button
          type="button"
          className={`${buttons.solid} ${buttons.small}`}
          onClick={() => void liveActions.copyScene(target)}
        >
          Copy
        </button>
      </div>
    </>
  );
}
