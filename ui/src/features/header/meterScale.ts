// SPDX-License-Identifier: AGPL-3.0-or-later
// The header meters' scale: −60 to +3 dBFS across the bars.
export const METER_FLOOR_DB = -60;
export const METER_CEIL_DB = 3;

/** Where a level sits along the bars, 0 to 1. */
export function meterPosition(db: number): number {
  return Math.min(1, Math.max(0, (db - METER_FLOOR_DB) / (METER_CEIL_DB - METER_FLOOR_DB)));
}
