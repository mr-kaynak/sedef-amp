// SPDX-License-Identifier: AGPL-3.0-or-later
// Where the notes go besides the host: Sedef MIDI's own virtual port, an existing MIDI output (an
// IAC bus, an interface, a loopback port), or nowhere.
import { useId } from 'react';
import { Icon } from '../../controls/Icon';
import select from '../../controls/ChoiceSelect.module.css';
import { setDestination, useMidiStore } from '../store';
import styles from './PortPicker.module.css';

export function PortPicker({ standalone }: { standalone: boolean }) {
  const ports = useMidiStore((s) => s.ports);
  const hintId = useId();
  if (!ports) return null;

  const known =
    ports.selected === 'off' ||
    ports.selected === 'virtual' ||
    ports.outputs.some((o) => o.id === ports.selected);
  const hint = ports.status
    ? ports.status
    : ports.selected === 'off'
      ? standalone
        ? 'Notes go nowhere: pick a port.'
        : 'Notes go to the host only.'
      : ports.selected === 'virtual'
        ? `Other apps list “${ports.virtualName}” as a MIDI input.`
        : ports.openName
          ? `Sending to ${ports.openName}.`
          : 'Opening…';

  return (
    <div className={styles.picker}>
      <span className={select.wrap}>
        <select
          className={`${select.select} ${styles.select}`}
          aria-label={standalone ? 'Send notes to' : 'Also send notes to'}
          aria-describedby={hintId}
          value={ports.selected}
          onChange={(e) => void setDestination(e.target.value)}
        >
          <option value="off">{standalone ? 'Nowhere' : 'Host only'}</option>
          {ports.virtualSupported && (
            <option value="virtual">{`Virtual port: ${ports.virtualName}`}</option>
          )}
          {ports.outputs.length > 0 && (
            <optgroup label="MIDI outputs">
              {ports.outputs.map((o) => (
                <option key={o.id} value={o.id}>
                  {o.name}
                </option>
              ))}
            </optgroup>
          )}
          {!known && <option value={ports.selected}>Disconnected output</option>}
        </select>
        <Icon name="down" size={14} className={select.chevron} />
      </span>
      <p
        id={hintId}
        className={styles.hint}
        data-error={ports.status ? '' : undefined}
        role={ports.status ? 'alert' : undefined}
      >
        {hint}
      </p>
    </div>
  );
}
