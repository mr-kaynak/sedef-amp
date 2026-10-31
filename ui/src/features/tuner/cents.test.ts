// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { spokenReading } from './cents';

describe('spokenReading', () => {
  it('names the note and whether it is in tune, sharp or flat', () => {
    expect(spokenReading(40, 0.4)).toBe('E 2, in tune');
    expect(spokenReading(61, 12)).toBe('C sharp 4, sharp');
    expect(spokenReading(45, -1.5)).toBe('A 2, flat');
  });
});
