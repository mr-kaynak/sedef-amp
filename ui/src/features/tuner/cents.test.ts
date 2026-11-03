// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { spokenState } from './cents';

describe('spokenState', () => {
  it('names the note, whether it is in tune, and what to do', () => {
    expect(spokenState(40, 'in-tune')).toBe('E 2, in tune');
    expect(spokenState(61, 'sharp')).toBe('C sharp 4, sharp, tune down');
    expect(spokenState(45, 'flat')).toBe('A 2, flat, tune up');
  });
});
