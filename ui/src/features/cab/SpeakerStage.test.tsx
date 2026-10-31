// SPDX-License-Identifier: AGPL-3.0-or-later
import { fireEvent, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { setupMock, type Harness } from '../../test/harness';
import { SpeakerStage } from './SpeakerStage';

describe('SpeakerStage', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(() => h.dispose());

  it('closes both host gestures of a mic drag cut short by a section switch', () => {
    const call = vi.spyOn(h.mock, 'call');
    const { unmount } = render(<SpeakerStage cabLabel="4x12" />);
    const marker = screen.getByRole('slider', { name: /Mic A placement/ });
    fireEvent.pointerDown(marker, { button: 0, clientX: 100, clientY: 100, pointerId: 1 });
    unmount();
    const gestures = call.mock.calls
      .filter(([name]) => String(name).includes('Gesture'))
      .map(([name, id]) => `${String(name).replace('sedef_', '')} ${String(id)}`);
    expect(gestures).toEqual([
      'beginGesture micAPosition',
      'beginGesture micADistance',
      'endGesture micAPosition',
      'endGesture micADistance',
    ]);
  });
});
