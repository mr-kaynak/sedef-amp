// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { legalFileName, renamedPresetId, savedPresetId } from './presetFiles';

describe('preset file names', () => {
  it('names files the way the plugin does, never outside the preset folder', () => {
    expect(legalFileName(' Sunday: set? ')).toBe('Sunday set');
    expect(legalFileName('.hidden')).toBe('hidden');
    expect(legalFileName('Vol. 2. ')).toBe('Vol. 2');
    expect(savedPresetId('Riff', '..')).toBe('user:Riff.sedefpreset');
    expect(savedPresetId('...', 'Live')).toBe('user:Live/Untitled.sedefpreset');
    expect(renamedPresetId('user:Live/Old.sedefpreset', '..')).toBe(
      'user:Live/Untitled.sedefpreset',
    );
  });
});
