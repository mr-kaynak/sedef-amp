// SPDX-License-Identifier: AGPL-3.0-or-later
import { Knob } from '../../controls/Knob';
import { KNOB_SIZE, usePedalDensity, type KnobWeight } from './density';

/** A knob on an enclosure: its size follows the board's density and the knob's weight there. */
export function PedalKnob({
  param,
  label,
  weight = 'normal',
}: {
  param: string;
  label: string;
  weight?: KnobWeight;
}) {
  const density = usePedalDensity();
  return <Knob param={param} label={label} size={KNOB_SIZE[density][weight]} />;
}
