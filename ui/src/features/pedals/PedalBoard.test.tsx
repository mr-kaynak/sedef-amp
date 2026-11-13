// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Toasts } from '../../app/Toasts';
import type { MockOptions } from '../../bridge/mockBackend';
import { useBoardStore } from '../../state/boardStore';
import { useUiStore } from '../../state/uiStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { gapAt, layoutBoard } from './layout';
import { PedalBoard } from './PedalBoard';

/** 0.3.0's pedals: most tests play this board. */
const FIVE = ['gate', 'tide', 'pearl', 'coral', 'ripple'];

/** Each pedal's box in the fake layout: one row, 220 px wide, 24 px apart. */
const WIDTH = 220;
const PITCH = WIDTH + 24;
const HEIGHT = 480;
/** The board's own numbers for FIVE, which is drawn large: the gap the drop bar sits in. */
const GAP = layoutBoard(FIVE).gap;

const settle = () =>
  act(async () => {
    await flush();
    await new Promise((resolve) => setTimeout(resolve, 0));
  });

/** The pedals on screen, in order, and the amp among them when a pedal stands after it. */
const drawn = () =>
  [...document.querySelectorAll<HTMLElement>('li[data-pedal]')].map((li) => li.dataset.pedal);
/** The pedals on screen, in order (the board as the store spells it). */
const shown = () => {
  const items = drawn();
  return items.at(-1) === 'amp' ? items.slice(0, -1) : items;
};
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

  it('shows the board in signal order, with its tools on each pedal', async () => {
    await start();
    const list = screen.getByRole('list', { name: 'Pedalboard, in signal order' });
    expect(shown()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple']);
    expect(screen.getByText('5 pedals')).toBeInTheDocument();
    const first = within(list).getAllByRole('listitem')[0]!;
    expect(within(first).getByRole('button', { name: 'Move Gate left' })).toBeDisabled();
    expect(within(first).getByRole('button', { name: 'Move Gate right' })).toBeEnabled();
    expect(handle('Ripple')).toHaveAccessibleDescription(
      'Drag to move it, or use the arrow keys. Modulation can go after the amp.',
    );
    // The amp is in the chain, last: modulation steps over it, a drive stops in front of it.
    expect(drawn()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple', 'amp']);
    expect(screen.getByRole('button', { name: 'Move Ripple after the amp' })).toBeEnabled();
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
      // Mariana carries its own board: the one pedal it switches on.
      expect(shown()).toEqual(['gate']);
      expect(screen.queryByText('Removed Ripple')).not.toBeInTheDocument();
    });

    it('adds a pedal from the picker at its default position and announces where it went', async () => {
      await start({ board: ['gate', 'tide', 'ripple'] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      const coral = within(picker()).getByRole('button', { name: 'Add Coral, distortion' });
      // What it does, and where it would go on this board.
      expect(coral).toHaveAccessibleDescription(
        'Hard clipping with a cut control for darker tones. Goes in front of the amp, after Tide.',
      );
      coral.focus();
      fireEvent.click(coral);
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'coral', 'ripple']);
      expect((await h.mock.hello()).board?.pedals).toEqual(['gate', 'tide', 'coral', 'ripple']);
      expect(status()).toHaveTextContent('Added Coral, 3 of 4 in front of the amp, after Tide.');
      // The shelf stays for the next pedal; the card keeps focus and now offers a second Coral.
      expect(coral).toHaveFocus();
      expect(coral).toHaveAccessibleName('Add another Coral, distortion');
      expect(coral).toHaveAccessibleDescription(
        'Hard clipping with a cut control for darker tones. Coral 2 goes in front of the amp, after Coral.',
      );
      fireEvent.click(within(picker()).getByRole('button', { name: 'Add Kelp, mid overdrive' }));
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'kelp', 'coral', 'ripple']);
      expect(status()).toHaveTextContent('Added Kelp, 3 of 5 in front of the amp, after Tide.');
    });

    it('adds a second copy next to the first, then shows both', async () => {
      await start({ board: ['gate', 'kelp', 'ripple'] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      const kelp = within(picker()).getByRole('button', {
        name: 'Add another Kelp, mid overdrive',
      });
      expect(within(kelp).getByText('On the board')).toBeInTheDocument();
      expect(within(kelp).getByText('Add another')).toBeInTheDocument();
      fireEvent.click(kelp);
      await settle();
      expect(shown()).toEqual(['gate', 'kelp', 'kelp#2', 'ripple']);
      expect((await h.mock.hello()).board?.pedals).toEqual(['gate', 'kelp', 'kelp#2', 'ripple']);
      expect(status()).toHaveTextContent('Added Kelp 2, 3 of 4 in front of the amp, after Kelp.');
      // The second copy is a pedal of its own: its name, its own switch and controls.
      expect(screen.getByRole('region', { name: 'Kelp 2 Mid overdrive' })).toBeInTheDocument();
      expect(handle('Kelp 2')).toBeInTheDocument();
      // Both on the board: the card shows them instead.
      expect(kelp).toHaveAccessibleName('Kelp, mid overdrive, both on the board. Show them');
      fireEvent.click(kelp);
      await settle();
      expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
      expect(handle('Kelp')).toHaveFocus();
    });

    it('adds modulation after the amp from its card', async () => {
      await start({ board: ['gate', 'tide'] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      const after = within(picker()).getByRole('button', { name: 'Add Gyre after the amp' });
      expect(after).toHaveAccessibleDescription('Goes after the amp, first.');
      // Only modulation has the button.
      expect(within(picker()).queryByRole('button', { name: 'Add Kelp after the amp' })).toBeNull();
      fireEvent.click(after);
      await settle();
      expect(drawn()).toEqual(['gate', 'tide', 'amp', 'gyre']);
      expect((await h.mock.hello()).board?.pedals).toEqual(['gate', 'tide', 'amp', 'gyre']);
      expect(status()).toHaveTextContent('Added Gyre, 1 of 1 after the amp, first.');
      // A second Gyre from the card's main button joins the first, after the amp.
      fireEvent.click(within(picker()).getByRole('button', { name: 'Add another Gyre, phaser' }));
      await settle();
      expect(drawn()).toEqual(['gate', 'tide', 'amp', 'gyre', 'gyre#2']);
    });

    it('holds seven drives and fuzzes at most, and says so on the cards', async () => {
      const drives = ['remora', 'urchin', 'sargasso', 'pearl', 'kelp', 'coral', 'breaker'];
      await start({ board: ['gate', ...drives] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      const kelp = within(picker()).getByRole('button', {
        name: 'Add another Kelp, mid overdrive',
      });
      expect(kelp).toHaveAttribute('aria-disabled', 'true');
      expect(kelp).toHaveAccessibleDescription(
        'Mid-hump drive, or a tight boost into a high-gain amp. The board holds 7 drives and fuzzes at most.',
      );
      expect(within(kelp).getByText('7 drives and fuzzes at most')).toBeInTheDocument();
      fireEvent.click(kelp);
      await settle();
      expect(shown()).toEqual(['gate', ...drives]);
      // Anything else still goes on.
      const gate = within(picker()).getByRole('button', { name: 'Add another Gate, noise gate' });
      expect(gate).not.toHaveAttribute('aria-disabled');
    });

    it('adds the first pedal from the empty board', async () => {
      await start({ board: [] });
      const add = screen.getAllByRole('button', { name: 'Add pedal' });
      fireEvent.click(add[add.length - 1]!);
      fireEvent.click(within(picker()).getByRole('button', { name: 'Add Gate, noise gate' }));
      await settle();
      expect(shown()).toEqual(['gate']);
      expect(status()).toHaveTextContent('Added Gate, 1 of 1 in front of the amp, first.');
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
        'Filter',
        'Modulation',
        'Pitch',
      ]);
      const names = (i: number) =>
        within(groups[i]!)
          .getAllByRole('button')
          .map((b) => b.getAttribute('aria-label'));
      // Modulation also adds after the amp.
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
      expect(names(3)).toEqual(['Add Anemone, envelope filter', 'Add Conch, wah']);
      expect(names(4)).toEqual([
        'Add Beacon, photocell vibe',
        'Add Beacon after the amp',
        'Add Gyre, phaser',
        'Add Gyre after the amp',
        'Add Jetty, flanger',
        'Add Jetty after the amp',
        'Add another Ripple, modulation',
        'Add another Ripple after the amp',
      ]);
      const dynamics = within(groups[0]!).getAllByRole('button');
      expect(dynamics.map((b) => b.getAttribute('aria-label'))).toEqual([
        'Add Gate, noise gate',
        'Add another Tide, compressor',
      ]);
      expect(within(dynamics[1]!).getByText('On the board')).toBeInTheDocument();
      expect(within(dynamics[0]!).getByText('Add')).toBeInTheDocument();
      // The first row takes focus, so the keyboard starts in the list.
      expect(dynamics[0]).toHaveFocus();
    });

    it('shows a pedal whose copies are both on the board instead of adding a third', async () => {
      await start({ board: [...FIVE, 'amp', 'ripple#2'] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      expect(
        within(picker()).getByRole('button', { name: 'Add another Ripple after the amp' }),
      ).toHaveAttribute('aria-disabled', 'true');
      fireEvent.click(
        within(picker()).getByRole('button', {
          name: 'Ripple, modulation, both on the board. Show them',
        }),
      );
      await settle();
      expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
      expect(drawn()).toEqual([...FIVE, 'amp', 'ripple#2']);
      expect(handle('Ripple')).toHaveFocus();
    });

    it('marks on the board where a pedal would land, and says what it does', async () => {
      await start({ board: ['gate', 'tide', 'ripple'] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      const landing = () => document.querySelector<HTMLElement>('li[data-landing]');
      // Gate, the first card, has focus: it is on the board, so it is ringed there, and its
      // second copy would land right after it.
      expect(document.querySelector('li[data-pedal="gate"]')).toHaveAttribute('data-marked');
      const second = gapAt(layoutBoard(['gate', 'tide', 'ripple'], false), 1)!;
      expect(landing()!.style.left).toBe(`${second.x}px`);
      expect(picker()).toHaveTextContent('Gate 2 goes in front of the amp, after Gate.');

      const coral = within(picker()).getByRole('button', { name: 'Add Coral, distortion' });
      act(() => coral.focus());
      // The board is smaller while the shelf is open; Coral would go between Tide and Ripple.
      const gap = gapAt(layoutBoard(['gate', 'tide', 'ripple'], false), 2)!;
      expect(landing()!.style.left).toBe(`${gap.x}px`);
      expect(landing()!.style.height).toBe(`${gap.h}px`);
      expect(document.querySelector('li[data-marked]')).toBeNull();
      expect(picker()).toHaveTextContent('Hard clipping with a cut control for darker tones.');
      expect(picker()).toHaveTextContent('Goes in front of the amp, after Tide.');

      // Pointing with the mouse does the same, and leaving the shelf clears the mark.
      fireEvent.pointerEnter(
        within(picker()).getByRole('button', { name: 'Add Trench, octave down' }).closest('li')!,
      );
      expect(picker()).toHaveTextContent('Goes in front of the amp, after Gate.');
      fireEvent.pointerLeave(picker());
      expect(landing()).toBeNull();

      // After the amp: past the amp, which the board draws last.
      const after = within(picker()).getByRole('button', { name: 'Add Gyre after the amp' });
      act(() => after.focus());
      const past = gapAt(layoutBoard(['gate', 'tide', 'ripple'], false), 4)!;
      expect(landing()!.style.left).toBe(`${past.x}px`);
      expect(picker()).toHaveTextContent('Goes after the amp, first.');
    });

    it('draws the board smaller while the shelf is open, so all of it stays in view', async () => {
      await start({ board: ['gate', 'tide', 'ripple'] });
      const unit = () =>
        screen.getByRole('region', { name: 'Tide Compressor' }).style.getPropertyValue('--u');
      expect(unit()).toBe('1.4px');
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      expect(unit()).toBe('1.2px');
      fireEvent.click(within(picker()).getByRole('button', { name: 'Close' }));
      expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
      expect(unit()).toBe('1.4px');
      expect(screen.getByRole('button', { name: 'Add pedal' })).toHaveFocus();
    });

    it('walks the cards with the arrow keys', async () => {
      await start({ board: ['tide'] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      const gate = within(picker()).getByRole('button', { name: 'Add Gate, noise gate' });
      expect(gate).toHaveFocus();
      fireEvent.keyDown(gate, { key: 'ArrowRight' });
      expect(
        within(picker()).getByRole('button', { name: 'Add another Tide, compressor' }),
      ).toHaveFocus();
      fireEvent.keyDown(document.activeElement!, { key: 'End' });
      expect(
        within(picker()).getByRole('button', { name: 'Add Trench, octave down' }),
      ).toHaveFocus();
      fireEvent.keyDown(document.activeElement!, { key: 'Home' });
      expect(gate).toHaveFocus();
    });

    it('keeps the pictures of the pedals out of reach of the keyboard and screen readers', async () => {
      await start({ board: ['tide'] });
      fireEvent.click(screen.getByRole('button', { name: 'Add pedal' }));
      // Only the cards and Close are controls: no knob or footswitch of a picture is exposed.
      expect(within(picker()).queryAllByRole('slider')).toHaveLength(0);
      expect(within(picker()).queryAllByRole('switch')).toHaveLength(0);
      // Sixteen cards, four of them with After the amp, and Close.
      expect(within(picker()).getAllByRole('button')).toHaveLength(21);
      for (const picture of picker().querySelectorAll('[inert]'))
        expect(picture).toHaveAttribute('aria-hidden', 'true');
      expect(picker().querySelectorAll('[inert]')).toHaveLength(16);
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
      expect(status()).toHaveTextContent('Pearl moved to 4 of 5 in front of the amp, after Coral.');
      expect(handle('Pearl')).toHaveFocus();

      fireEvent.keyDown(handle('Pearl'), { key: 'Home' });
      await settle();
      expect(shown()).toEqual(['pearl', 'gate', 'tide', 'coral', 'ripple']);
      expect(status()).toHaveTextContent('Pearl moved to 1 of 5 in front of the amp, first.');

      fireEvent.keyDown(handle('Pearl'), { key: 'End' });
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'coral', 'ripple', 'pearl']);

      // A drive stops in front of the amp: there the key does nothing, and says nothing new.
      fireEvent.keyDown(handle('Pearl'), { key: 'ArrowDown' });
      await settle();
      expect(shown()).toEqual(['gate', 'tide', 'coral', 'ripple', 'pearl']);
      expect(status()).toHaveTextContent(
        'Pearl moved to 5 of 5 in front of the amp, after Ripple.',
      );
      expect((await h.mock.hello()).board?.pedals).toEqual(board());
    });

    it('takes modulation across the amp and back from the keyboard', async () => {
      await start();
      handle('Ripple').focus();
      fireEvent.keyDown(handle('Ripple'), { key: 'ArrowRight' });
      await settle();
      expect(drawn()).toEqual(['gate', 'tide', 'pearl', 'coral', 'amp', 'ripple']);
      expect(status()).toHaveTextContent('Ripple moved to 1 of 1 after the amp, first.');
      expect(handle('Ripple')).toHaveFocus();
      // Its left button now says where it goes.
      expect(screen.getByRole('button', { name: 'Move Ripple in front of the amp' })).toBeEnabled();
      // Home and End stay on its side of the amp.
      fireEvent.keyDown(handle('Ripple'), { key: 'Home' });
      await settle();
      expect(drawn()).toEqual(['gate', 'tide', 'pearl', 'coral', 'amp', 'ripple']);
      fireEvent.click(screen.getByRole('button', { name: 'Move Ripple in front of the amp' }));
      await settle();
      expect(drawn()).toEqual(['gate', 'tide', 'pearl', 'coral', 'ripple', 'amp']);
      expect(status()).toHaveTextContent(
        'Ripple moved to 5 of 5 in front of the amp, after Coral.',
      );
      // Focus stays on the button used, which says where it goes now.
      expect(screen.getByRole('button', { name: 'Move Ripple left' })).toHaveFocus();
      expect(screen.getByRole('button', { name: 'Move Ripple after the amp' })).toBeEnabled();
      expect((await h.mock.hello()).board?.pedals).toEqual(FIVE);
    });

    it('moves a pedal with its move buttons, keeping focus on the button used', async () => {
      await start();
      fireEvent.click(screen.getByRole('button', { name: 'Move Tide left' }));
      await settle();
      expect(shown()).toEqual(['tide', 'gate', 'pearl', 'coral', 'ripple']);
      expect(status()).toHaveTextContent('Tide moved to 1 of 5 in front of the amp, first.');
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
    const bar = () =>
      document.querySelector<HTMLElement>('li[aria-hidden="true"]:not([data-landing])');

    it('drags a pedal into a gap, showing the drop bar there, and announces the move', async () => {
      await start();
      // Over the left half of Pearl: Ripple would land between Tide and Pearl.
      const el = drag('Ripple', 2 * PITCH + 40);
      expect(el.closest('li')).toHaveAttribute('data-dragging');
      expect(bar()).not.toBeNull();
      expect(bar()!.style.left).toBe(`${2 * PITCH - GAP / 2}px`);
      // Pedals differ in height: the bar spans the row.
      expect(bar()!.style.height).toBe(`${layoutBoard(FIVE).rows[0]!.height}px`);
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
      expect(status()).toHaveTextContent('Ripple moved to 3 of 5 in front of the amp, after Tide.');
    });

    it('drops a drive dragged past the amp in front of it, and says why', async () => {
      await start();
      // The amp is the sixth item (index 5); past it, Gate can't go.
      const el = drag('Gate', 6 * PITCH + 60);
      expect(bar()!.style.left).toBe(`${5 * PITCH - GAP / 2}px`);
      expect(screen.getByText('Only modulation goes after the amp')).toBeInTheDocument();
      fireEvent.pointerUp(el, { pointerId: 1 });
      await settle();
      expect(shown()).toEqual(['tide', 'pearl', 'coral', 'ripple', 'gate']);
      expect(screen.queryByText('Only modulation goes after the amp')).toBeNull();
    });

    it('drags modulation across the amp and back', async () => {
      await start();
      // Past the middle of the amp: Ripple lands after it.
      let el = drag('Ripple', 5 * PITCH + 150);
      expect(bar()!.style.left).toBe(`${5 * PITCH + WIDTH + GAP / 2}px`);
      expect(screen.queryByText('Only modulation goes after the amp')).toBeNull();
      fireEvent.pointerUp(el, { pointerId: 1 });
      await settle();
      expect(drawn()).toEqual(['gate', 'tide', 'pearl', 'coral', 'amp', 'ripple']);
      expect((await h.mock.hello()).board?.pedals).toEqual([
        'gate',
        'tide',
        'pearl',
        'coral',
        'amp',
        'ripple',
      ]);
      expect(status()).toHaveTextContent('Ripple moved to 1 of 1 after the amp, first.');
      // Back over Tide.
      el = drag('Ripple', PITCH + 40);
      fireEvent.pointerUp(el, { pointerId: 1 });
      await settle();
      expect(drawn()).toEqual(['gate', 'ripple', 'tide', 'pearl', 'coral', 'amp']);
      expect((await h.mock.hello()).board?.pedals).toEqual([
        'gate',
        'ripple',
        'tide',
        'pearl',
        'coral',
      ]);
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

    it('drags a pedal by its enclosure, but not by a control on it', async () => {
      await start();
      const slot = document.querySelector<HTMLElement>('li[data-pedal="ripple"]')!;
      const enclosure = screen.getByRole('region', { name: 'Ripple Modulation' });
      const from = 4 * PITCH + 100;
      fireEvent.pointerDown(enclosure, { button: 0, pointerId: 1, clientX: from, clientY: 200 });
      fireEvent.pointerMove(enclosure, { pointerId: 1, clientX: PITCH + 40, clientY: 200 });
      expect(slot).toHaveAttribute('data-dragging');
      fireEvent.pointerUp(enclosure, { pointerId: 1, clientX: PITCH + 40, clientY: 200 });
      await settle();
      expect(shown()).toEqual(['gate', 'ripple', 'tide', 'pearl', 'coral']);
      expect(status()).toHaveTextContent('Ripple moved to 2 of 5 in front of the amp, after Gate.');

      // A press on a knob turns the knob.
      const knob = screen.getByRole('slider', { name: 'Pearl Gain' });
      fireEvent.pointerDown(knob, {
        button: 0,
        pointerId: 2,
        clientX: 3 * PITCH + 50,
        clientY: 60,
      });
      fireEvent.pointerMove(knob, { pointerId: 2, clientX: 40, clientY: 60 });
      expect(document.querySelector('li[data-dragging]')).toBeNull();
      fireEvent.pointerUp(knob, { pointerId: 2, clientX: 40, clientY: 60 });
      await settle();
      expect(shown()).toEqual(['gate', 'ripple', 'tide', 'pearl', 'coral']);
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

  describe('the signal path', () => {
    it('runs through the amp in use, which opens the Amp page', async () => {
      await start({ board: ['gate', 'tide', 'amp', 'gyre'] });
      act(() => useUiStore.setState({ section: 'pedals' }));
      const amp = screen.getByRole('button', { name: 'The amp: Lagoon. Open the Amp page' });
      // In the list, between the two sides, as in the signal; Guitar before it, Out after.
      const list = screen.getByRole('list', { name: 'Pedalboard, in signal order' });
      const items = within(list).getAllByRole('listitem');
      expect(items.map((li) => li.dataset.pedal)).toEqual(['gate', 'tide', 'amp', 'gyre']);
      expect(items[2]).toContainElement(amp);
      // The amp is not a pedal: it has no tools and is not dragged.
      expect(within(items[2]!).queryByRole('button', { name: /^Move / })).toBeNull();
      expect(screen.getByText('Guitar').compareDocumentPosition(list)).toBe(
        Node.DOCUMENT_POSITION_FOLLOWING,
      );
      expect(list.compareDocumentPosition(screen.getByText('Out'))).toBe(
        Node.DOCUMENT_POSITION_FOLLOWING,
      );
      // The pedals after it stand on a band named for them.
      expect(screen.getByText('After the amp')).toBeInTheDocument();
      fireEvent.click(amp);
      expect(useUiStore.getState().section).toBe('amp');
    });

    it('names the amp the preset uses', async () => {
      await start();
      await act(async () => {
        await h.mock.call('sedef_presets_load', 'factory:mariana', false);
      });
      await settle();
      expect(
        screen.getByRole('button', { name: 'The amp: Abyss. Open the Amp page' }),
      ).toBeInTheDocument();
    });

    it('places each pedal where the layout says, large for a short board', async () => {
      await start();
      const layout = layoutBoard(FIVE);
      expect(layout.scale).toBe(1.4);
      for (const placed of layout.pedals) {
        const slot = document.querySelector<HTMLElement>(`li[data-pedal="${placed.id}"]`)!;
        expect(slot.style.left).toBe(`${placed.x}px`);
        expect(slot.style.top).toBe(`${placed.y}px`);
        expect(slot.style.width).toBe(`${placed.w}px`);
      }
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
