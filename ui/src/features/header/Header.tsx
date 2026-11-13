// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback, useRef } from 'react';
import type { ReactNode } from 'react';
import { Icon, ICON_STROKE_LABEL } from '../../controls/Icon';
import { Knob } from '../../controls/Knob';
import { Popover } from '../../controls/Popover';
import buttons from '../../controls/Button.module.css';
import { useAppStore } from '../../state/appStore';
import { uiActions, useUiStore } from '../../state/uiStore';
import { MetronomePanel } from '../metronome/MetronomePanel';
import { RecorderButton } from '../recorder/RecorderButton';
import { RecorderPanel } from '../recorder/RecorderPanel';
import { useRecorderStore } from '../recorder/store';
import { presetBrowser } from '../presets/session';
import { SettingsPanel } from '../settings/SettingsPanel';
import { UpdateSlot } from '../updates/UpdateSlot';
import { Brand } from './Brand';
import { DoublerSwitch } from './DoublerSwitch';
import { InputLevelChip } from './InputLevelChip';
import { LevelMeter } from './LevelMeter';
import { PresetBar } from './PresetBar';
import styles from './Header.module.css';

function GainStrip({
  param,
  which,
  label,
  below,
}: {
  param: string;
  which: 'input' | 'output';
  label: string;
  /** Hangs under the meter, in the knob label's row, without moving the meter. */
  below?: ReactNode;
}) {
  return (
    <div className={styles.gain}>
      <Knob param={param} size="sm" label={label} hideValue />
      <div className={styles.meterSlot}>
        <LevelMeter which={which} label={label} zones={which === 'input'} />
        {below && <div className={styles.below}>{below}</div>}
      </div>
    </div>
  );
}

export function Header() {
  const overlay = useUiStore((s) => s.overlay);
  const popover = useUiStore((s) => s.popover);
  const standalone = useAppStore((s) => s.session.isStandalone);
  const metroRef = useRef<HTMLButtonElement>(null);
  const recorderRef = useRef<HTMLButtonElement>(null);
  const recorder = useRecorderStore((s) => s.state !== null);
  const settingsRef = useRef<HTMLButtonElement>(null);
  const closePopover = useCallback(() => uiActions.setPopover(null), []);

  return (
    <header className={styles.header}>
      <Brand />
      <PresetBar />
      <div className={styles.io}>
        <GainStrip param="inputGain" which="input" label="Input" below={<InputLevelChip />} />
        <GainStrip param="outputGain" which="output" label="Output" below={<DoublerSwitch />} />
      </div>
      <nav className={styles.tools} aria-label="Tools">
        <UpdateSlot />
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.lead}`}
          aria-pressed={overlay === 'tuner'}
          aria-keyshortcuts="T"
          title="Tuner (T)"
          onClick={() => {
            presetBrowser.keep();
            uiActions.toggleOverlay('tuner');
          }}
        >
          <Icon name="fork" stroke={ICON_STROKE_LABEL} />
          Tuner
        </button>
        {/* In the standalone app the transport takes the room its label had. */}
        <button
          type="button"
          className={`${buttons.ghost} ${standalone ? buttons.icon : buttons.lead}`}
          aria-pressed={overlay === 'intonation'}
          aria-label={standalone ? 'Intonation' : undefined}
          title={standalone ? 'Intonation' : undefined}
          onClick={() => {
            presetBrowser.keep();
            uiActions.toggleOverlay('intonation');
          }}
        >
          <Icon name="ruler" stroke={ICON_STROKE_LABEL} />
          {!standalone && 'Intonation'}
        </button>
        {standalone && (
          // What keeps time or holds your playing, together (docs/recorder.md, section 1).
          <div className={styles.transport} role="group" aria-label="Transport">
            <div className={styles.anchor}>
              <button
                ref={metroRef}
                type="button"
                className={`${buttons.ghost} ${buttons.icon}`}
                aria-label="Metronome"
                title="Metronome"
                aria-haspopup="dialog"
                aria-expanded={popover === 'metronome'}
                onClick={() => uiActions.togglePopover('metronome')}
              >
                <Icon name="metronome" />
              </button>
              <Popover
                open={popover === 'metronome'}
                onClose={closePopover}
                anchorRef={metroRef}
                label="Metronome"
                className={styles.rightPopover}
              >
                <MetronomePanel />
              </Popover>
            </div>
            {recorder && (
              <div className={styles.anchor}>
                <RecorderButton
                  ref={recorderRef}
                  expanded={popover === 'recorder'}
                  onToggle={() => uiActions.togglePopover('recorder')}
                />
                <Popover
                  open={popover === 'recorder'}
                  onClose={closePopover}
                  anchorRef={recorderRef}
                  label="Recorder"
                  className={styles.rightPopover}
                >
                  <RecorderPanel />
                </Popover>
              </div>
            )}
          </div>
        )}
        <div className={styles.anchor}>
          <button
            ref={settingsRef}
            type="button"
            className={`${buttons.ghost} ${buttons.icon}`}
            aria-label="Settings"
            title="Settings"
            aria-haspopup="dialog"
            aria-expanded={popover === 'settings'}
            onClick={() => uiActions.togglePopover('settings')}
          >
            <Icon name="sliders" />
          </button>
          <Popover
            open={popover === 'settings'}
            onClose={closePopover}
            anchorRef={settingsRef}
            label="Settings"
            className={styles.rightPopover}
          >
            <SettingsPanel />
          </Popover>
        </div>
      </nav>
    </header>
  );
}
