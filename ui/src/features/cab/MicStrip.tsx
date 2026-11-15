// SPDX-License-Identifier: AGPL-3.0-or-later
import { ChoiceSelect } from '../../controls/ChoiceSelect';
import { Icon } from '../../controls/Icon';
import { Knob } from '../../controls/Knob';
import { Toggle } from '../../controls/Toggle';
import buttons from '../../controls/Button.module.css';
import { useParamSpec, useParamValue } from '../../params/useParam';
import { useAppStore } from '../../state/appStore';
import { invoke } from '../../state/backendRef';
import styles from './MicStrip.module.css';

const NOT_FOUND = 'The IR file was not found';

function IrSlot({ mic, cab }: { mic: 0 | 1; cab: number }) {
  const slot = useAppStore((s) => s.ir.slots.find((x) => x.mic === mic && x.cab === cab));
  // A session's IR that couldn't be loaded keeps its path; the slot says where it was and
  // offers to find it (docs/robustness.md, section 4).
  const missing = slot?.name ? null : (slot?.missing ?? null);
  const load = async () => {
    const ir = await invoke('sedef_ir_load', mic, cab);
    if (ir) useAppStore.setState({ ir });
  };
  const clear = async () => {
    const ir = await invoke('sedef_ir_clear', mic, cab);
    if (ir) useAppStore.setState({ ir });
  };
  return (
    <div className={styles.ir} data-missing={missing ? '' : undefined}>
      <Icon name={missing ? 'info' : 'file'} size={14} />
      {missing ? (
        <span className={styles.irMissing}>
          <span className={styles.irMissingTitle}>{missing.fileName} is missing</span>
          <span className={styles.irMissingText}>
            {missing.reason === NOT_FOUND
              ? `Not found at ${missing.path}.`
              : `${missing.reason}: ${missing.path}.`}{' '}
            Dyn Classic plays until you locate it.
          </span>
        </span>
      ) : (
        <span className={styles.irName}>
          {slot?.name ? (
            <>
              {slot.name}
              {slot.durationMs ? (
                <span className={styles.irMeta}> {Math.round(slot.durationMs)} ms</span>
              ) : null}
            </>
          ) : (
            <span className={styles.irEmpty}>No impulse response loaded</span>
          )}
        </span>
      )}
      <button
        type="button"
        className={`${buttons.solid} ${buttons.small}`}
        onClick={() => void load()}
      >
        {missing ? 'Locate…' : slot?.name ? 'Replace…' : 'Load…'}
      </button>
      {(slot?.name || missing) && (
        <button
          type="button"
          className={`${buttons.ghost} ${buttons.small}`}
          onClick={() => void clear()}
        >
          Clear
        </button>
      )}
    </div>
  );
}

/** Channel strip for one mic on the current cab. */
export function MicStrip({ mic, cab }: { mic: 'A' | 'B'; cab: number }) {
  const typeId = `mic${mic}Cab${cab + 1}Type`;
  const type = Math.round(useParamValue(typeId));
  const choices = useParamSpec(typeId).choices;
  const isCustom = choices[type] === 'Custom IR';
  const enabled = useParamValue(`mic${mic}Enabled`) >= 0.5;

  return (
    <section
      className={styles.strip}
      data-mic={mic}
      data-off={!enabled || undefined}
      aria-label={`Mic ${mic}`}
    >
      <header className={styles.head}>
        <span className={styles.badge} aria-hidden="true">
          {mic}
        </span>
        <h3 className={styles.title}>Mic {mic}</h3>
        <Toggle param={`mic${mic}Enabled`} label="On" />
        <Toggle param={`mic${mic}Phase`} label="Invert phase" />
      </header>
      <ChoiceSelect param={typeId} ariaLabel={`Mic ${mic} type`} className={styles.type} />
      {isCustom && (
        <>
          <IrSlot mic={mic === 'A' ? 0 : 1} cab={cab} />
          <p className={styles.irNote}>
            The impulse response holds its own position and distance. Off-axis still turns the mic.
          </p>
        </>
      )}
      <div className={styles.knobs}>
        <Knob param={`mic${mic}Position`} label="Position" size="sm" disabled={isCustom} />
        <Knob param={`mic${mic}Distance`} label="Distance" size="sm" disabled={isCustom} />
        <Knob param={`mic${mic}OffAxis`} label="Off-axis" size="sm" />
        <Knob param={`mic${mic}Level`} label="Level" size="sm" />
        <Knob param={`mic${mic}Pan`} label="Pan" size="sm" />
        <div className={styles.room}>
          <Toggle param={`room${mic}Enabled`} label="Room" />
          <Knob param={`room${mic}Level`} label="Room level" size="sm" />
        </div>
      </div>
    </section>
  );
}
