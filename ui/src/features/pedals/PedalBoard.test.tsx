// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Toasts } from '../../app/Toasts';
import type { MockOptions } from '../../bridge/mockBackend';
import { useBoardStore } from '../../state/boardStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { LEGACY_BOARD } from './registry';
import { PedalBoard } from './PedalBoard';

/** 0.3.0's pedals: most tests play this board. */
const FIVE = ['gate', 'tide', 'pearl', 'coral', 'ripple'];

/** Each pedal's box in the fake layout: one row, 220 px wide, 24 px apart. */
const WIDTH = 220;
const PITCH = WIDTH + 24;
const HEIGHT = 480;

const settle = () =>
  act(async () => {
    await flush();
    await new Promise((resolve) => setTimeout(resolve, 0));
  });

/** The pedals on screen, in order. */
const shown = () =>
  [...document.querySelectorAll<HTMLElement>('li[data-pedal]')].map((li) => li.dataset.pedal);
const board = () => useBoardStore.getState().pedals;
/** The board's announcements (the Toasts region is a status too). */
const status = () => {
  const found = screen.getAllByRole('status').filter((el) => el.closest('[data-board]') !== null);
  expect(found).toHaveLength(1);
  return found[0]!;
};
const handle = (name: string) => screen.getByRole('button', { name: `Move ${name}` });
const picker = () => screen.getByRole('dialog', { name: 'Add a pedal' });

/** Where a slot sits in the fake layout: its place in the list, as rendered. */
function slotIndex(el: Element): number {
  const li = el.closest('li[data-pedal]');
  if (!li) return -1;
  return [...(li.parentElement?.querySelectorAll('li[data-pedal]') ?? [])].indexOf(li);
}

describe('PedalBoard', () => {
  let h: Harness;
  const start = async (options: MockOptions = {}) => {
    h = await setupMock({ board: FIVE, ...options });
    render(
      <>
        <div data-board>
          <PedalBoard />
        </div>
        <Toasts />
      </>,
    );
    await settle();
  };

  beforeEach(() => {
    // jsdom lays nothing out: give each pedal a box in one row, by its place in the list.
    vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (
      this: Element,
    ) {
      const i = this.matches('li[data-pedal]') ? slotIndex(this) : -1;
      const left = i < 0 ? 0 : i * PITCH;
      const width = i < 0 ? 0 : WIDTH;
      const height = i < 0 ? 0 : HEIGHT;
      return {
        left,
        top: 0,
        right: left + width,
        bottom: height,
        width,
        height,
        x: left,
        y: 0,
      } as DOMRect;
    });
    const offset = (pick: (i: number) => number) =>
      function (this: HTMLElement) {
        const i = this.matches('li[data-pedal]') ? slotIndex(this) : -1;
        return i < 0 ? 0 : pick(i);
      };
    vi.spyOn(HTMLElement.prototype, 'offsetLeft', 'get').mockImplementation(
      offset((i) => i * PITCH),
    );
    vi.spyOn(HTMLElement.prototype, 'offsetWidth', 'get').mockImplementation(offset(() => WIDTH));
    vi.spyOn(HTMLElement.prototype, 'offsetHeight', 'get').mockImplementation(offset(() => HEIGHT));
  });

  afterEach(async () => {
    await settle();
    cleanup();
    h.dispose();
    vi.restoreAllMocks();
  });

  it('shows the board in signal order, with a strip for each pedal', async () => {
    await start();
    const list = screen.getByRole('list', { name: 'Pedalboard, in signal order' });
    expect(shown()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple']);
    expect(screen.getByText('5 pedals')).toBeInTheDocument();
    const first = within(list).getAllByRole('listitem')[0]!;
    expect(within(first).getByRole('button', { name: 'Move Gate left' })).toBeDisabled();
    expect(within(first).getByRole('button', { name: 'Move Gate right' })).toBeEnabled();
    expect(handle('Ripple')).toHaveAccessibleDescription('Drag to move it, or use the arrow keys.');
    expect(screen.getByRole('button', { name: 'Move Ripple right' })).toBeDisabled();
  });

  describe('adding and removing', () => {
    it('removes a pedal, moves focus to its neighbour, and Undo puts it back where it was', async () => {
      await start();
      fireEvent.click(screen.getByRole('button', { name: 'Remove Pearl' }));
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'coral', 'ripple']);
      expect((await h.mock.hello()).board?.pedals).toEqual(['gate', 'tide', 'coral', 'ripple']);
      expect(handle('Coral')).toHaveFocus();
      expect(screen.getByText('Removed Pearl')).toBeInTheDocument();

      fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple']);
      expect((await h.mock.hello()).board?.pedals).toEqual(board());
      expect(screen.queryByText('Removed Pearl')).not.toBeInTheDocument();
    });

    it('focuses the Add pedal button when the last pedal leaves, and shows the empty board', async () => {
      await start({ board: ['tide'] });
      fireEvent.click(screen.getByRole('button', { name: 'Remove Tide' }));
      await settle();
      expect(screen.getByText('No pedals on the board')).toBeInTheDocument();
      expect(screen.getByText('0 pedals')).toBeInTheDocument();
      expect(screen.getAllByRole('button', { name: 'Add pedal' })[0]).toHaveFocus();
    });

    it('drops the Undo when a preset brings another board', async () => {
      await start();
      fireEvent.click(screen.getByRole('button', { name: 'Remove Ripple' }));
      await settle();
      expect(screen.getByText('Removed Ripple')).toBeInTheDocument();
      await act(async () => {
        await h.mock.call('sedef_presets_load', 'factory:mariana', false);
      });
      await settle();
      // Saved before the board: the fixed chain it replaced.
      expect(shown()).toEqual(LEGACY_BOARD);
      expect(screen.queryByText('Removed Ripple')).not.toBeInTheDocument();
    });

    it('adds a pedal from the picker at its default position and announces where it went', async () => {
      await start({ board: ['gate', 'tide', 'ripple'] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      const coral = within(picker()).getByRole('button', { name: 'Add Coral, distortion' });
      expect(coral).toHaveAccessibleDescription(
        'Hard clipping with a cut control for darker tones.',
      );
      fireEvent.click(coral);
      await settle();
      expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
      expect(shown()).toEqual(['gate', 'tide', 'coral', 'ripple']);
      expect((await h.mock.hello()).board?.pedals).toEqual(['gate', 'tide', 'coral', 'ripple']);
      expect(status()).toHaveTextContent('Added Coral, 3 of 4, after Tide.');
      expect(handle('Coral')).toHaveFocus();
    });

    it('adds the first pedal from the empty board', async () => {
      await start({ board: [] });
      const add = screen.getAllByRole('button', { name: 'Add pedal' });
      fireEvent.click(add[add.length - 1]!);
      fireEvent.click(within(picker()).getByRole('button', { name: 'Add Gate, noise gate' }));
      await settle();
      expect(shown()).toEqual(['gate']);
      expect(status()).toHaveTextContent('Added Gate, 1 of 1, first.');
    });
  });

  describe('the picker', () => {
    it('groups pedals by category and marks the ones on the board', async () => {
      await start({ board: ['tide', 'ripple'] });
      const add = screen.getByRole('button', { name: 'Add pedal' });
      expect(add).toHaveAttribute('aria-expanded', 'false');
      fireEvent.click(add);
      expect(add).toHaveAttribute('aria-expanded', 'true');
      const dialog = picker();
      const groups = within(dialog).getAllByRole('region');
      // In the registry's category order, each pedal in its default order.
      expect(groups.map((g) => g.getAttribute('aria-label'))).toEqual([
        'Dynamics',
        'Drive',
        'Fuzz',
        'Modulation',
        'Pitch',
      ]);
      const names = (i: number) =>
        within(groups[i]!)
          .getAllByRole('button')
          .map((b) => b.getAttribute('aria-label'));
      expect(names(1)).toEqual([
        'Add Pearl, overdrive',
        'Add Kelp, mid overdrive',
        'Add Coral, distortion',
        'Add Breaker, treble booster',
      ]);
      expect(names(2)).toEqual([
        'Add Remora, fuzz',
        'Add Urchin, octave-up fuzz',
        'Add Sargasso, sustain fuzz',
      ]);
      const dynamics = within(groups[0]!).getAllByRole('button');
      expect(dynamics.map((b) => b.getAttribute('aria-label'))).toEqual([
        'Add Gate, noise gate',
        'Tide, compressor, on the board. Show it',
      ]);
      expect(within(dynamics[1]!).getByText('On the board')).toBeInTheDocument();
      expect(within(dynamics[0]!).getByText('Add')).toBeInTheDocument();
      // The first row takes focus, so the keyboard starts in the list.
      expect(dynamics[0]).toHaveFocus();
    });

    it('shows a pedal that is on the board already instead of adding it twice', async () => {
      await start();
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      fireEvent.click(
        within(picker()).getByRole('button', { name: 'Ripple, modulation, on the board. Show it' }),
      );
      await settle();
      expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
      expect(shown()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple']);
      expect(handle('Ripple')).toHaveFocus();
    });

    it('closes on Escape and gives focus back to Add pedal', async () => {
      await start();
      const add = screen.getByRole('button', { name: 'Add pedal' });
      fireEvent.click(add);
      fireEvent.keyDown(picker(), { key: 'Escape' });
      expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
      expect(add).toHaveFocus();
    });
  });

  describe('keyboard reorder', () => {
    it('moves a pedal with the arrow keys on its handle and announces each move', async () => {
      await start();
      handle('Pearl').focus();
      fireEvent.keyDown(handle('Pearl'), { key: 'ArrowRight' });
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'coral', 'pearl', 'ripple']);
      expect(status()).toHaveTextContent('Pearl moved to 4 of 5, after Coral.');
      expect(handle('Pearl')).toHaveFocus();

      fireEvent.keyDown(handle('Pearl'), { key: 'Home' });
      await settle();
      expect(shown()).toEqual(['pearl', 'gate', 'tide', 'coral', 'ripple']);
      expect(status()).toHaveTextContent('Pearl moved to 1 of 5, first.');

      fireEvent.keyDown(handle('Pearl'), { key: 'End' });
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'coral', 'ripple', 'pearl']);

      // At the end of the board the key does nothing, and says nothing new.
      fireEvent.keyDown(handle('Pearl'), { key: 'ArrowDown' });
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'coral', 'ripple', 'pearl']);
      expect(status()).toHaveTextContent('Pearl moved to 5 of 5, after Ripple.');
      expect((await h.mock.hello()).board?.pedals).toEqual(board());
    });

    it('moves a pedal with its move buttons, keeping focus on the button used', async () => {
      await start();
      fireEvent.click(screen.getByRole('button', { name: 'Move Tide left' }));
      await settle();
      expect(shown()).toEqual(['tide', 'gate', 'pearl', 'coral', 'ripple']);
      expect(status()).toHaveTextContent('Tide moved to 1 of 5, first.');
      // Tide is first now: its left button switched off, so focus lands on its handle.
      expect(handle('Tide')).toHaveFocus();

      fireEvent.click(screen.getByRole('button', { name: 'Move Gate right' }));
      await settle();
      expect(shown()).toEqual(['tide', 'pearl', 'gate', 'coral', 'ripple']);
      expect(screen.getByRole('button', { name: 'Move Gate right' })).toHaveFocus();
    });
  });

  describe('pointer reorder', () => {
    /** Presses a pedal's handle and drags it to (x, y) on the board, in a few steps. */
    function drag(name: string, x: number, y = 100) {
      const el = handle(name);
      const from = slotIndex(el) * PITCH + 14;
      fireEvent.pointerDown(el, { button: 0, pointerId: 1, clientX: from, clientY: 14 });
      fireEvent.pointerMove(el, { pointerId: 1, clientX: (from + x) / 2, clientY: y });
      fireEvent.pointerMove(el, { pointerId: 1, clientX: x, clientY: y });
      return el;
    }
    const bar = () => document.querySelector<HTMLElement>('li[aria-hidden="true"]');

    it('drags a pedal into a gap, showing the drop bar there, and announces the move', async () => {
      await start();
      // Over the left half of Pearl: Ripple would land between Tide and Pearl.
      const el = drag('Ripple', 2 * PITCH + 40);
      expect(el.closest('li')).toHaveAttribute('data-dragging');
      expect(bar()).not.toBeNull();
      expect(bar()!.style.left).toBe(`${2 * PITCH - 12}px`);
      expect(bar()!.style.height).toBe(`${HEIGHT}px`);
      fireEvent.pointerUp(el, { pointerId: 1, clientX: 2 * PITCH + 40, clientY: 100 });
      await settle();
      expect(bar()).toBeNull();
      expect(shown()).toEqual(['gate', 'tide', 'ripple', 'pearl', 'coral']);
      expect((await h.mock.hello()).board?.pedals).toEqual([
        'gate',
        'tide',
        'ripple',
        'pearl',
        'coral',
      ]);
      expect(status()).toHaveTextContent('Ripple moved to 3 of 5, after Tide.');
    });

    it('drops after the last pedal past the end of the board', async () => {
      await start();
      const el = drag('Gate', 5 * PITCH + 60);
      expect(bar()!.style.left).toBe(`${4 * PITCH + WIDTH + 12}px`);
      fireEvent.pointerUp(el, { pointerId: 1 });
      await settle();
      expect(shown()).toEqual(['tide', 'pearl', 'coral', 'ripple', 'gate']);
    });

    it('puts the pedal back on Escape', async () => {
      await start();
      const el = drag('Ripple', 40);
      expect(bar()).not.toBeNull();
      fireEvent.keyDown(window, { key: 'Escape' });
      expect(bar()).toBeNull();
      expect(el.closest('li')).not.toHaveAttribute('data-dragging');
      fireEvent.pointerUp(el, { pointerId: 1 });
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple']);
    });

    it('treats a press without movement as a click, not a drag', async () => {
      await start();
      const el = handle('Pearl');
      fireEvent.pointerDown(el, { button: 0, pointerId: 1, clientX: 500, clientY: 14 });
      fireEvent.pointerMove(el, { pointerId: 1, clientX: 502, clientY: 15 });
      expect(bar()).toBeNull();
      fireEvent.pointerUp(el, { pointerId: 1, clientX: 502, clientY: 15 });
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple']);
      expect(status()).toHaveTextContent('');
    });

    it('ends the drag without a move when the pointer is cancelled', async () => {
      await start();
      const el = drag('Ripple', 40);
      fireEvent.pointerCancel(el, { pointerId: 1 });
      await settle();
      expect(bar()).toBeNull();
      expect(shown()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple']);
    });
  });

  it('shows the fixed board without editing against a plug-in that predates the board', async () => {
    await start();
    act(() => useBoardStore.setState({ supported: false }));
    expect(shown()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple']);
    expect(screen.queryByRole('button', { name: 'Add pedal' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /^Move / })).not.toBeInTheDocument();
  });
});
