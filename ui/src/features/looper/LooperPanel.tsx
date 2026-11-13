// SPDX-License-Identifier: AGPL-3.0-or-later
// The looper (docs/looper.md): one big button that records, closes and plays, overdubs and plays,
// with the loop's ring round it and its state beside it; Stop, Undo, Redo, Clear; level,
// feedback and count-in. Space anywhere in it is the big button, L everywhere (useShortcuts).
import type { KeyboardEvent } from 'react';
import { useEffect } from 'react';
import buttons from '../../controls/Button.module.css';
import { Icon, ICON_STROKE_LABEL, type IconName } from '../../controls/Icon';
import { Knob } from '../../controls/Knob';
import { Toggle } from '../../controls/Toggle';
import { stepsFor } from '../../lib/metronome';
import { useParamValue } from '../../params/useParam';
import { useAppStore } from '../../state/appStore';
import { looperPress, openLooper } from './actions';
import { LoopRing } from './LoopRing';
import { mainAction, stateHint, stateTitle, toneOf } from './model';
import { useLooper, useLooperDetail } from './useLooper';
import styles from './LooperPanel.module.css';

const MAIN_ICONS: Record<ReturnType<typeof mainAction>['icon'], IconName> = {
  record: 'record',
  play: 'play',
  overdub: 'overdub',
  close: 'close',
};

/** Space presses the big button from anywhere in the panel but a button or a field. */
function onPanelKey(e: KeyboardEvent<HTMLDivElement>, disabled: boolean): void {
  if (e.key !== ' ' || e.altKey || e.metaKey || e.ctrlKey || e.repeat) return;
  const target = e.target as HTMLElement;
  if (target.closest('button, input, select, textarea, [contenteditable="true"]')) return;
  e.preventDefault();
  if (!disabled) looperPress('main');
}

export function LooperPanel() {
  const rate = useAppStore((s) => s.session.sampleRate);
  const t = useLooper(rate);
  const clickOn = useParamValue('metronomeEnabled') >= 0.5;
  const steps = stepsFor(Math.round(useParamValue('metronomeSignature')));
  const detail = useLooperDetail(rate, clickOn ? steps : null);
  const main = mainAction(t);
  const tone = toneOf(t.state);
  const hasLoop = t.state === 'playing' || t.state === 'overdubbing' || t.state === 'stopped';
  const canStop =
    t.state === 'armed' ||
    t.state === 'recording' ||
    t.state === 'closing' ||
    t.state === 'playing' ||
    t.state === 'overdubbing';
  const canClear =
    hasLoop || t.state === 'armed' || t.state === 'recording' || t.state === 'closing';

  // The first open reserves the memory, so the first press records at once.
  useEffect(() => openLooper(), []);

  return (
    <div className={styles.panel} data-tone={tone} onKeyDown={(e) => onPanelKey(e, main.disabled)}>
      <div className={styles.top}>
        <div className={styles.mainWrap}>
          <LoopRing size={84} stroke={4} rate={rate} className={styles.ring} />
          <button
            type="button"
            // Ignored while a take closes, so it doesn't give way under a press either.
            className={`${buttons.press} ${styles.main} ${main.disabled ? buttons.static : ''}`}
            data-autofocus
            data-action={main.icon}
            aria-label={main.label}
            aria-describedby="looper-state"
            // Never disabled, so it keeps the focus Space needs; ignored while a take closes.
            aria-disabled={main.disabled || undefined}
            onClick={() => {
              if (!main.disabled) looperPress('main');
            }}
          >
            <Icon
              name={MAIN_ICONS[main.icon]}
              size={26}
              filled={main.icon !== 'close' && main.icon !== 'overdub'}
              stroke={ICON_STROKE_LABEL}
            />
          </button>
        </div>
        <div className={styles.state} id="looper-state">
          <p className={styles.title}>
            <span className={styles.lamp} aria-hidden="true" />
            {stateTitle(t)}
          </p>
          {detail && <p className={styles.detail}>{detail}</p>}
          <p className={styles.hint}>{stateHint(t, clickOn)}</p>
        </div>
      </div>

      <div className={styles.actions} role="group" aria-label="Loop">
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.lead}`}
          disabled={!canStop}
          onClick={() => looperPress('stop')}
        >
          <Icon name="stop" size={14} filled stroke={ICON_STROKE_LABEL} />
          Stop
        </button>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.lead}`}
          disabled={!t.canUndo}
          aria-label={t.undoIsClear ? 'Undo clear' : 'Undo the last overdub'}
          onClick={() => looperPress('undo')}
        >
          <Icon name="undo" size={14} stroke={ICON_STROKE_LABEL} />
          Undo
        </button>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.lead}`}
          disabled={!t.canRedo}
          aria-label={t.undoIsClear ? 'Redo clear' : 'Redo the last overdub'}
          onClick={() => looperPress('redo')}
        >
          <Icon name="redo" size={14} stroke={ICON_STROKE_LABEL} />
          Redo
        </button>
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.lead}`}
          disabled={!canClear}
          onClick={() => looperPress('clear')}
        >
          <Icon name="close" size={14} stroke={ICON_STROKE_LABEL} />
          Clear
        </button>
      </div>

      <div className={styles.settings}>
        <Knob param="looperLevel" size="sm" label="Level" />
        <Knob param="looperFeedback" size="sm" label="Feedback" />
        <div className={styles.countIn}>
          <Toggle param="looperCountIn" label="Count-in" />
          <span className={styles.note}>
            {clickOn ? 'One bar of clicks first' : 'Only with the click on'}
          </span>
        </div>
      </div>

      <p className={styles.keys}>
        <kbd>L</kbd> or <kbd>Space</kbd> record, play, overdub <span aria-hidden="true">·</span>{' '}
        <kbd>
          <span aria-hidden="true">⇧</span>
          <span className="visually-hidden">Shift </span>L
        </kbd>{' '}
        stop
      </p>
    </div>
  );
}
