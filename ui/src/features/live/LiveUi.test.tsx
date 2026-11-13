// SPDX-License-Identifier: AGPL-3.0-or-later
// Playing live from the UI (docs/live.md): the scene bar, a control's menu and learn mode, and
// the MIDI panel's bindings and program changes, against the mock.
import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { Knob } from '../../controls/Knob';
import { flush, setupMock, type Harness } from '../../test/harness';
import { useLiveStore } from '../../state/liveStore';
import { ControlMenu } from './ControlMenu';
import { DONE_SECONDS, LearnBanner } from './LearnBanner';
import { LiveBar } from './LiveBar';

const CC = 0xb0;

function Rig() {
  return (
    <>
      <LiveBar />
      <Knob param="pearlGain" />
      <ControlMenu />
      <LearnBanner />
    </>
  );
}

describe('Playing live', () => {
  let h: Harness | undefined;
  afterEach(() => {
    // Unmount first: resetting the stores under mounted components updates them outside act().
    cleanup();
    h?.dispose();
    h = undefined;
  });

  const midi = async (status: number, data1: number, data2: number, time?: number) => {
    await act(async () => {
      h!.mock.midi(status, data1, data2, time);
      await flush();
    });
  };
  /** Lets a timer queued by the component (refocusing after a removal) run. */
  const settle = async () => {
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
      await flush();
    });
  };
  const click = async (el: HTMLElement) => {
    await act(async () => {
      fireEvent.click(el);
      await flush();
    });
  };

  describe('the scene bar', () => {
    it('is one radio group of eight scenes; arrows and clicks switch at once', async () => {
      h = await setupMock();
      render(<Rig />);
      const group = screen.getByRole('radiogroup', { name: 'Scenes' });
      const scenes = within(group).getAllByRole('radio');
      expect(scenes.map((s) => s.getAttribute('aria-label'))).toEqual([
        'Scene 1',
        ...Array.from({ length: 7 }, (_, i) => `Scene ${i + 2}, empty`),
      ]);
      expect(scenes.map((s) => s.tabIndex)).toEqual([0, -1, -1, -1, -1, -1, -1, -1]);

      await click(scenes[2]!);
      expect(scenes[2]).toHaveAttribute('aria-checked', 'true');
      expect(h.mock.value('scene')).toBe(2);
      expect(scenes[2]).toHaveAccessibleName('Scene 3');

      scenes[2]!.focus();
      await act(async () => {
        fireEvent.keyDown(scenes[2]!, { key: 'ArrowRight' });
        await flush();
      });
      expect(scenes[3]).toHaveFocus();
      expect(h.mock.value('scene')).toBe(3);
    });

    it('folds into one button while Transpose shifts, for the room its note takes', async () => {
      h = await setupMock({ overrides: { transpose: -2 } });
      render(<Rig />);
      expect(screen.queryByRole('radiogroup', { name: 'Scenes' })).toBeNull();
      const chip = screen.getByRole('button', { name: 'Scene 1, choose a scene' });
      expect(chip).toHaveTextContent('Scene 1');
      await click(chip);
      const popover = screen.getByRole('dialog', { name: 'Scenes' });
      const group = within(popover).getByRole('radiogroup', { name: 'Scenes' });
      await click(within(group).getByRole('radio', { name: 'Scene 3, empty' }));
      expect(h.mock.value('scene')).toBe(2);
      expect(screen.getByRole('button', { name: /^Scene 3, choose a scene/ })).toHaveTextContent(
        'Scene 3',
      );
      // Back at 0, the eight are in the row again.
      await act(async () => {
        h!.mock.hostSet({ transpose: 0 });
        await flush();
      });
      expect(screen.getByRole('radiogroup', { name: 'Scenes' })).toBeInTheDocument();
    });

    it('follows a footswitch', async () => {
      h = await setupMock({ liveDemo: true });
      render(<Rig />);
      // The demo binds Next scene to CC 24 as a momentary switch.
      await midi(CC, 24, 127);
      expect(screen.getByRole('radio', { name: 'Scene 2' })).toHaveAttribute(
        'aria-checked',
        'true',
      );
    });
  });

  describe('a control’s menu', () => {
    it('learns a pedal for a knob: the banner follows, the knob is outlined', async () => {
      h = await setupMock();
      render(<Rig />);
      const knob = screen.getByRole('slider', { name: 'Pearl Gain' });
      fireEvent.contextMenu(knob, { button: 2, clientX: 40, clientY: 40 });
      const menu = screen.getByRole('menu', { name: 'Pearl Gain: MIDI and scenes' });
      const items = within(menu).getAllByRole('menuitem');
      expect(items.map((i) => i.textContent)).toEqual(['Learn MIDI', 'Remember in scenes']);
      expect(items[0]).toHaveFocus();

      await click(items[0]!);
      expect(screen.queryByRole('menu')).toBeNull();
      expect(knob).toHaveFocus();
      const banner = screen.getByRole('status');
      expect(banner).toHaveTextContent('Move a pedal or press a footswitch for Pearl Gain');
      expect(knob).toHaveAttribute('data-learning');

      await midi(CC, 11, 30);
      expect(banner).toHaveTextContent('Pearl Gain follows CC 11 · ch 1');
      expect(useLiveStore.getState().state?.bindings).toEqual([
        expect.objectContaining({ target: 'param:pearlGain', controller: 11 }),
      ]);

      await click(within(banner).getByRole('button', { name: 'Done' }));
      expect(banner).toBeEmptyDOMElement();
      expect(knob).not.toHaveAttribute('data-learning');
      // The pedal plays the knob now (learning only listened).
      await midi(CC, 11, 30);
      expect(h.mock.value('pearlGain')).toBeCloseTo(30 / 127, 6);

      // Bound now: the menu offers to learn again or forget it.
      fireEvent.contextMenu(knob, { button: 2, clientX: 40, clientY: 40 });
      expect(
        within(screen.getByRole('menu'))
          .getAllByRole('menuitem')
          .map((i) => i.textContent),
      ).toEqual(['Learn MIDI again', 'Forget CC 11 · ch 1', 'Remember in scenes']);
      await click(screen.getByRole('menuitem', { name: 'Forget CC 11 · ch 1' }));
      expect(useLiveStore.getState().state?.bindings).toEqual([]);
    });

    it('tells a momentary footswitch by its release, and goes by itself when done', async () => {
      h = await setupMock();
      render(<Rig />);
      fireEvent.contextMenu(screen.getByRole('radio', { name: 'Scene 2, empty' }), {
        button: 2,
      });
      await click(screen.getByRole('menuitem', { name: 'Learn MIDI' }));
      await midi(CC, 64, 127, 3);
      expect(screen.getByRole('status')).toHaveTextContent(
        'Scene 2 on CC 64 · ch 1. Checking whether the footswitch springs back',
      );
      await midi(CC, 64, 0, 3.1);
      expect(screen.getByRole('status')).toHaveTextContent(
        'Scene 2 follows CC 64 · ch 1: each press switches it',
      );
      // Its own timer dismisses it.
      await act(async () => {
        await new Promise((resolve) => setTimeout(resolve, DONE_SECONDS * 1000 + 50));
        await flush();
      });
      expect(screen.getByRole('status')).toBeEmptyDOMElement();
    }, 10_000);

    it('opens from the keyboard and closes with Escape; Escape also stops learning', async () => {
      h = await setupMock();
      render(<Rig />);
      const knob = screen.getByRole('slider', { name: 'Pearl Gain' });
      knob.focus();
      fireEvent.keyDown(knob, { key: 'F10', shiftKey: true });
      const menu = screen.getByRole('menu');
      const items = within(menu).getAllByRole('menuitem');
      expect(items[0]).toHaveFocus();
      fireEvent.keyDown(items[0]!, { key: 'ArrowDown' });
      expect(items[1]).toHaveFocus();
      fireEvent.keyDown(items[1]!, { key: 'ArrowDown' });
      expect(items[0]).toHaveFocus();
      fireEvent.keyDown(items[0]!, { key: 'Escape' });
      expect(screen.queryByRole('menu')).toBeNull();
      expect(knob).toHaveFocus();
      // Escape in the menu was the menu's: nothing was learning, nothing else happened.

      fireEvent.keyDown(knob, { key: 'F10', shiftKey: true });
      await click(screen.getByRole('menuitem', { name: 'Learn MIDI' }));
      expect(screen.getByRole('status')).toHaveTextContent('Move a pedal');
      await act(async () => {
        fireEvent.keyDown(knob, { key: 'Escape' });
        await flush();
      });
      expect(screen.getByRole('status')).toBeEmptyDOMElement();
      expect(useLiveStore.getState().state?.learn).toBeNull();
    });

    it('keeps a knob per scene, and copies the scene in use into another', async () => {
      h = await setupMock();
      render(<Rig />);
      const knob = screen.getByRole('slider', { name: 'Pearl Gain' });
      fireEvent.contextMenu(knob, { button: 2 });
      await click(screen.getByRole('menuitem', { name: 'Remember in scenes' }));
      expect(useLiveStore.getState().state?.scenes.values).toEqual(['pearlGain']);
      fireEvent.contextMenu(knob, { button: 2 });
      expect(screen.getByRole('menuitem', { name: 'Same in every scene' })).toBeInTheDocument();
      fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });

      // The scene in use has no "copy here".
      fireEvent.contextMenu(screen.getByRole('radio', { name: 'Scene 1' }), { button: 2 });
      expect(screen.queryByRole('menuitem', { name: /^Copy/ })).toBeNull();
      fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
      fireEvent.contextMenu(screen.getByRole('radio', { name: 'Scene 4, empty' }), {
        button: 2,
      });
      await click(screen.getByRole('menuitem', { name: 'Copy scene 1 here' }));
      expect(screen.getByRole('radio', { name: 'Scene 4' })).toBeInTheDocument();
    });

    it('leaves other elements’ menus alone', async () => {
      h = await setupMock();
      render(
        <>
          <input aria-label="Name" />
          <ControlMenu />
        </>,
      );
      const event = fireEvent.contextMenu(screen.getByRole('textbox'), { button: 2 });
      expect(event).toBe(true); // not prevented
      expect(screen.queryByRole('menu')).toBeNull();
    });
  });

  describe('the MIDI panel', () => {
    const openPanel = async () => {
      await click(screen.getByRole('button', { name: 'MIDI' }));
      return screen.getByRole('dialog', { name: 'MIDI' });
    };

    it('lists the learned controls and edits them', async () => {
      h = await setupMock({ liveDemo: true });
      render(<Rig />);
      const panel = await openPanel();
      expect(within(panel).getByText('Last received')).toBeInTheDocument();
      expect(panel).toHaveTextContent('CC 11 = 87 · ch 1');
      const list = within(panel).getByRole('list', { name: 'Learned controls' });
      const rows = within(list).getAllByRole('listitem');
      expect(rows.map((r) => r.firstElementChild?.textContent)).toEqual([
        'PearlCC 20 · ch 1',
        'DriftCC 21 · ch 1',
        'Conch PositionCC 11 · ch 1',
        'Output GainCC 7 · ch 1',
        'Next sceneCC 24 · ch 1',
        'Next presetCC 25 · ch 1',
        'Looper: record, play, overdubCC 26 · ch 1',
      ]);
      expect(panel).toHaveTextContent('7 of 64 learned');

      // A switch's mode.
      await click(within(rows[0]!).getByRole('radio', { name: 'Latch' }));
      expect(useLiveStore.getState().state?.bindings[0]?.mode).toBe('latch');

      // A knob's range, in percent of its travel; Invert swaps the ends.
      const low = within(rows[2]!).getByRole('spinbutton', {
        name: 'Conch Position at controller 0, percent of its travel',
      });
      fireEvent.change(low, { target: { value: '20' } });
      await act(async () => {
        fireEvent.blur(low);
        await flush();
      });
      expect(useLiveStore.getState().state?.bindings[2]).toMatchObject({ low: 0.2, high: 1 });
      await click(within(rows[2]!).getByRole('button', { name: 'Invert' }));
      expect(useLiveStore.getState().state?.bindings[2]).toMatchObject({ low: 1, high: 0.2 });
      expect(within(rows[2]!).getByRole('button', { name: 'Invert' })).toHaveAttribute(
        'aria-pressed',
        'true',
      );
      await midi(CC, 11, 127);
      expect(h.mock.value('conchPosition')).toBeCloseTo(0.2, 6);

      // Forget one: the focus moves to the next row's button, not to the page.
      await click(within(rows[1]!).getByRole('button', { name: 'Forget Drift (CC 21 · ch 1)' }));
      expect(useLiveStore.getState().state?.bindings).toHaveLength(6);
      await settle();
      expect(
        within(panel).getByRole('button', { name: 'Forget Conch Position (CC 11 · ch 1)' }),
      ).toHaveFocus();

      // Then all, after a confirmation that names the count.
      await click(within(panel).getByRole('button', { name: 'Forget all' }));
      const confirm = within(panel).getByRole('group', { name: 'Forget all 6 learned controls?' });
      expect(within(confirm).getByRole('button', { name: 'Keep' })).toHaveFocus();
      await click(within(confirm).getByRole('button', { name: 'Forget all' }));
      expect(useLiveStore.getState().state?.bindings).toEqual([]);
      expect(panel).toHaveTextContent('Right-click a knob, switch or scene (or focus it and press');
      await settle();
      expect(within(panel).getByRole('button', { name: /^Previous scene/ })).toHaveFocus();
    });

    it('learns a footswitch for scene and preset steps', async () => {
      h = await setupMock();
      render(<Rig />);
      const panel = await openPanel();
      const group = within(panel).getByRole('group', { name: 'Learn a footswitch for' });
      await click(within(group).getByRole('button', { name: /^Previous preset/ }));
      expect(screen.queryByRole('dialog')).toBeNull();
      expect(screen.getByRole('status')).toHaveTextContent(
        'Move a pedal or press a footswitch for Previous preset',
      );
      await midi(CC, 80, 0);
      expect(useLiveStore.getState().state?.bindings).toEqual([
        expect.objectContaining({ target: 'preset:previous', controller: 80, mode: 'latch' }),
      ]);
    });

    it('assigns program changes to presets and scenes', async () => {
      h = await setupMock();
      render(<Rig />);
      const panel = await openPanel();
      await click(within(panel).getByRole('tab', { name: 'Programs' }));
      expect(within(panel).getByRole('tab', { name: 'Programs' })).toHaveAttribute(
        'aria-selected',
        'true',
      );
      fireEvent.change(within(panel).getByRole('spinbutton'), { target: { value: '12' } });
      fireEvent.change(within(panel).getByRole('combobox', { name: 'Program 12 selects' }), {
        target: { value: 'scene:4' },
      });
      await click(within(panel).getByRole('button', { name: 'Assign' }));
      expect(useLiveStore.getState().state?.programs.assigned).toEqual([{ program: 12, scene: 4 }]);
      expect(within(panel).getByRole('list', { name: 'Assigned programs' })).toHaveTextContent(
        'Program 12Scene 5',
      );
      await midi(0xc0, 11, 0);
      expect(h.mock.value('scene')).toBe(4);

      await act(async () => {
        fireEvent.change(within(panel).getByRole('combobox', { name: 'Listen on' }), {
          target: { value: '3' },
        });
        await flush();
      });
      expect(useLiveStore.getState().state?.programs.channel).toBe(3);

      await click(within(panel).getByRole('button', { name: 'Unassign program 12 (Scene 5)' }));
      expect(useLiveStore.getState().state?.programs.assigned).toEqual([]);
    });

    it('says what a format cannot receive', async () => {
      h = await setupMock({ wrapperType: 'VST3', isStandalone: false });
      render(<Rig />);
      const panel = await openPanel();
      expect(panel).toHaveTextContent('The VST3 version receives controllers but no program');
      // The host routes MIDI to a plug-in: no input list.
      expect(within(panel).queryByRole('group', { name: 'Inputs' })).toBeNull();
    });
  });
});
