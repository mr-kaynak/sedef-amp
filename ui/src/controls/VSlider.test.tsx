// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { getValue } from '../params/paramStore';
import { setupMock, type Harness } from '../test/harness';
import { VSlider } from './VSlider';

describe('VSlider', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock({ initialPresetId: null });
  });
  afterEach(() => h.dispose());

  it('follows the pointer one to one in a scaled window', () => {
    render(<VSlider param="eqLagoonBand1" height={180} />);
    const track = screen.getByRole('slider', { name: /Lagoon EQ/ });
    // The canvas is drawn at twice its layout size: 180 layout px take 360 screen px.
    Object.defineProperty(track, 'offsetHeight', { value: 180 });
    track.getBoundingClientRect = () => ({ height: 360, width: 40 }) as DOMRect;

    fireEvent.pointerDown(track, { button: 0, clientY: 400, pointerId: 1 });
    // 90 screen px is 45 layout px: a quarter of the 24 dB range.
    fireEvent.pointerMove(track, { clientY: 310, pointerId: 1 });
    fireEvent.pointerUp(track, { pointerId: 1 });
    expect(getValue('eqLagoonBand1')).toBeCloseTo(6);
  });
});
