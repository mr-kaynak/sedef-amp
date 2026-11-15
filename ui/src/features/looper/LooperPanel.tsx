// SPDX-License-Identifier: AGPL-3.0-or-later
// The looper (docs/looper.md): one big button that records, closes and plays, overdubs and plays,
// with the loop's ring round it and its state beside it; Stop, Undo, Redo, Clear; level,
// feedback and count-in. Space anywhere in it is the big button, L everywhere (useShortcuts).
import type { KeyboardEvent, ReactNode } from 'react';
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

/**
 * Stop, Undo, Redo, Clear. Marked unavailable (aria-disabled) rather than disabled, so each keeps
 * its place in the tab order and its menu (right-click, Shift+F10): a footswitch for Stop, Undo
 * or Clear can be learnt before there is anything to stop, undo or clear (docs/live.md §4.5).
 */
function LoopAction({
  enabled,
  learn,
  icon,
  filled = false,
  label,
  onPress,
  children,
}: {
  enabled: boolean;
  learn?: string;
  icon: IconName;
  filled?: boolean;
  label?: string;
  onPress: () => void;
  children: ReactNode;
}) {
  return (
    <button
      type="button"
      className={`${buttons.ghost} ${buttons.lead}`}
      aria-disabled={!enabled || undefined}
      data-learn={learn}
      aria-label={label}
      onClick={() => {
        if (enabled) onPress();
      }}
    >
      <Icon name={icon} size={14} filled={filled} stroke={ICON_STROKE_LABEL} />
      {children}
    </button>
  );
}

export function LooperPanel() {
  const rate = useAppStore((s) => s.session.sampleRate);
  const t = useLooper(rate);
  const clickOn = useParamValue('metronomeEnabled') >= 0.5;
  // The metronome may play drums instead of a click (docs/drums.md): the copy says which.
  const drums = Math.round(useParamValue('metronomeMode')) === 1;
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
            // Right-click or Shift+F10: learn a footswitch for it (docs/live.md §4.5).
            data-learn="looper:main"
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
          <p className={styles.hint}>{stateHint(t, clickOn, drums)}</p>
        </div>
      </div>

      <div className={styles.actions} role="group" aria-label="Loop">
        <LoopAction
          enabled={canStop}
          learn="looper:stop"
          icon="stop"
          filled
          onPress={() => looperPress('stop')}
        >
          Stop
        </LoopAction>
        <LoopAction
          enabled={t.canUndo}
          learn="looper:undo"
          icon="undo"
          label={t.undoIsClear ? 'Undo clear' : 'Undo the last overdub'}
          onPress={() => looperPress('undo')}
        >
          Undo
        </LoopAction>
        <LoopAction
          enabled={t.canRedo}
          icon="redo"
          label={t.undoIsClear ? 'Redo clear' : 'Redo the last overdub'}
          onPress={() => looperPress('redo')}
        >
          Redo
        </LoopAction>
        <LoopAction
          enabled={canClear}
          learn="looper:clear"
          icon="close"
          onPress={() => looperPress('clear')}
        >
          Clear
        </LoopAction>
      </div>

      <div className={styles.settings}>
        <Knob param="looperLevel" size="sm" label="Level" />
        <Knob param="looperFeedback" size="sm" label="Feedback" />
        <div className={styles.countIn}>
          <Toggle param="looperCountIn" label="Count-in" />
          <span className={styles.note}>
            {clickOn
              ? drums
                ? 'One bar of the groove first'
                : 'One bar of clicks first'
              : drums
                ? 'Only with the drums on'
                : 'Only with the click on'}
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
