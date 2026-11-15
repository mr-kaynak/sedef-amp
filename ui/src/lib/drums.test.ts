// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { bundledSchema } from '../params/schema';
import { FILL_LABELS, GROOVES, grooveFor, groovesFor } from './drums';
import { stepsFor } from './metronome';

describe('drum grooves', () => {
  it('are the drumsGroove parameter’s choices, in its order', () => {
    const spec = bundledSchema.find((p) => p.id === 'drumsGroove');
    expect(spec?.choices).toEqual(GROOVES.map((g) => g.name));
  });

  it('give every meter at least one groove, each with a feel', () => {
    for (let signature = 0; signature < 8; signature++) {
      expect(groovesFor(signature).length, `signature ${signature}`).toBeGreaterThan(0);
      expect(stepsFor(signature)).toBeGreaterThan(0);
    }
    for (const g of GROOVES) expect(g.feel.length).toBeGreaterThan(10);
  });

  it('play the meter’s first groove when the chosen one is written in another', () => {
    const fourFour = groovesFor(2);
    const sixEight = groovesFor(4);
    expect(grooveFor(fourFour[1]!.index, 2)).toBe(fourFour[1]!.index);
    expect(grooveFor(fourFour[1]!.index, 4)).toBe(sixEight[0]!.index);
  });

  it('label the fills the way drumsFills orders them', () => {
    const spec = bundledSchema.find((p) => p.id === 'drumsFills');
    expect(spec?.choices).toHaveLength(FILL_LABELS.length);
    expect(spec?.choices[0]).toBe('Off');
  });
});
