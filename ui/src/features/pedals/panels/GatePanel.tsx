// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../../controls/Knob';
import { Led } from '../../../controls/Led';
import { useParamValue } from '../../../params/useParam';
import { telemetry, useLiveSelector } from '../../../state/live';
import { Pedal } from '../Pedal';
import { GATE } from './finishes';
import styles from './Panels.module.css';

/** Open, closed or bypassed, in words as well as on the lamp. */
function GateState() {
  const enabled = useParamValue('gateEnabled') >= 0.5;
  const open = useLiveSelector(telemetry, (t) => t?.gateOpen ?? true);
  return (
    <span className={styles.gateState}>
      <Led on={enabled && open} size={7} />
      {enabled ? (open ? 'Open' : 'Closed') : 'Bypassed'}
    </span>
  );
}

export function GatePanel() {
  return (
    <Pedal
      name="Gate"
      kind="Noise gate"
      enableParam="gateEnabled"
      finish={GATE}
      variant="narrow"
      status={<GateState />}
    >
      <Knob param="gateThreshold" label="Threshold" />
    </Pedal>
  );
}
