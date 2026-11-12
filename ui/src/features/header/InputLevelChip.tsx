// SPDX-License-Identifier: AGPL-3.0-or-later
// The verdict under the input meter, and the panel that says what to turn
// (docs/ui/input-level.md).
import { useCallback, useRef } from 'react';
import buttons from '../../controls/Button.module.css';
import { Knob } from '../../controls/Knob';
import { Led } from '../../controls/Led';
import { Popover } from '../../controls/Popover';
import { useAppStore } from '../../state/appStore';
import { invoke } from '../../state/backendRef';
import { uiActions, useUiStore } from '../../state/uiStore';
import { INPUT_LEVEL, levelAdvice, useInputLevel, type InputLevelState } from './inputLevel';
import { LevelMeter } from './LevelMeter';
import { meterPosition } from './meterScale';
import styles from './InputLevelChip.module.css';

const CHIP: Record<InputLevelState, { text: string; lamp: 'off' | 'nacre' | 'amber' | 'coral' }> = {
  none: { text: 'No signal', lamp: 'off' },
  waiting: { text: 'Check level', lamp: 'off' },
  quiet: { text: 'Too quiet', lamp: 'amber' },
  good: { text: 'Level good', lamp: 'nacre' },
  hot: { text: 'Too hot', lamp: 'coral' },
  clip: { text: 'Too hot', lamp: 'coral' },
};

const HEADLINE: Record<InputLevelState, string> = {
  none: 'No signal',
  waiting: 'Check your level',
  quiet: 'Too quiet',
  good: 'Level good',
  hot: 'Too hot',
  clip: 'Your interface is clipping',
};

/** The scale under the panel's meter, in dBFS. */
const SCALE_DB = [-48, -24, -12, -6, 0];

const signed = (db: number) => (db > 0 ? `+${db}` : db < 0 ? `−${-db}` : '0');

export function InputLevelPanel() {
  const { state, loudestDb } = useInputLevel();
  const standalone = useAppStore((s) => s.session.isStandalone);
  const zone = {
    left: `${meterPosition(INPUT_LEVEL.quietDb) * 100}%`,
    width: `${(meterPosition(INPUT_LEVEL.hotDb) - meterPosition(INPUT_LEVEL.quietDb)) * 100}%`,
  };
  return (
    <div className={styles.panel}>
      <h3 className={styles.title}>Input level</h3>
      <div className={styles.meter}>
        {/* The target over the bars: where the loudest strums should reach. */}
        <div className={styles.target} style={zone} aria-hidden="true">
          Good
        </div>
        <LevelMeter which="input" label="Input" zones size="lg" withClip={false} />
        <div className={styles.scale} aria-hidden="true">
          {SCALE_DB.map((db) => (
            <span key={db} style={{ left: `${meterPosition(db) * 100}%` }}>
              {signed(db)}
            </span>
          ))}
        </div>
      </div>
      <div className={styles.verdict}>
        {/* Announced when the verdict changes, so a knob turned by ear says when it is right. */}
        <div role="status">
          <p className={styles.headline} data-state={state}>
            <Lamp state={state} size={8} />
            {HEADLINE[state]}
          </p>
          <p className={styles.advice}>{levelAdvice(state, standalone)}</p>
        </div>
        {loudestDb !== null && (
          <p className={styles.loudest}>
            Loudest peak, last 8 s: <span className={styles.number}>{signed(loudestDb)} dB</span>
          </p>
        )}
      </div>
      <div className={styles.controls}>
        <Knob param="inputGain" size="md" label="Input" />
        <p className={styles.how}>
          Strum hard. Set your interface&rsquo;s gain so the peaks land in Good, then fine-tune with
          Input.
        </p>
      </div>
      {standalone && (
        <div className={styles.foot}>
          <button
            type="button"
            className={`${buttons.solid} ${buttons.small}`}
            onClick={() => void invoke('sedef_audioSettings')}
          >
            Audio settings…
          </button>
          <span className={styles.footNote}>Your interface, its input and buffer size</span>
        </div>
      )}
    </div>
  );
}

function Lamp({ state, size }: { state: InputLevelState; size: number }) {
  const lamp = CHIP[state].lamp;
  return <Led on={lamp !== 'off'} tone={lamp === 'off' ? 'nacre' : lamp} size={size} />;
}

/** The chip under the input meter; it opens the Input level panel. */
export function InputLevelChip() {
  const state = useInputLevel((s) => s.state);
  const open = useUiStore((s) => s.popover === 'input');
  const ref = useRef<HTMLButtonElement>(null);
  const close = useCallback(() => uiActions.setPopover(null), []);
  const { text } = CHIP[state];
  return (
    <div className={styles.anchor}>
      <button
        ref={ref}
        type="button"
        className={`${buttons.press} ${styles.chip}`}
        data-state={state}
        aria-haspopup="dialog"
        aria-expanded={open}
        title="Input level: how to set it"
        onClick={() => uiActions.togglePopover('input')}
      >
        <Lamp state={state} size={5} />
        {text}
        <span className="visually-hidden">, input level</span>
      </button>
      <Popover
        open={open}
        onClose={close}
        anchorRef={ref}
        label="Input level"
        className={styles.popover}
      >
        <InputLevelPanel />
      </Popover>
    </div>
  );
}
