// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../controls/Knob';
import { KNOB_UNITS, usePedalScale, type KnobWeight } from './density';

/** A knob on an enclosure: its size follows the pedal's scale and the knob's weight there. */
export function PedalKnob({
  param,
  label,
  weight = 'normal',
}: {
  param: string;
  label: string;
  weight?: KnobWeight;
}) {
  const scale = usePedalScale();
  return <Knob param={param} label={label} size={Math.round(KNOB_UNITS[weight] * scale)} />;
}
