// SPDX-License-Identifier: AGPL-3.0-or-later
// The mods lane's pedals, each a self-contained panel that renders in any container. Listed in
// their recommended chain order (docs/dsp/conch.md et al.): Anemone and Conch before the drives,
// Beacon, Gyre and Jetty after them, before Ripple.
export { AnemonePedal } from './AnemonePedal';
export { BeaconPedal } from './BeaconPedal';
export { ConchPedal } from './ConchPedal';
export { GyrePedal } from './GyrePedal';
export { JettyPedal } from './JettyPedal';
