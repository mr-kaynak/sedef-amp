// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Toasts } from '../../app/Toasts';
import { useShortcuts } from '../../app/useShortcuts';
import { presetActions, usePresetStore } from '../../state/presetStore';
import { useUiStore } from '../../state/uiStore';
import { flush, setupMock, type Harness } from '../../test/harness';
import { PresetBar } from '../header/PresetBar';
import { PresetBrowser } from './PresetBrowser';
import { useBrowserStore } from './session';

const SUNDAY = 'user:Live/Sunday set.sedefpreset';

function Shell() {
  useShortcuts();
  return (
    <>
      <PresetBar />
      <PresetBrowser />
      <Toasts />
    </>
  );
}

const presets = () => usePresetStore.getState().state;
/** Lets every queued bridge call, event and store update land (a macrotask drains them all). */
const settle = () =>
  act(async () => {
    await flush();
    await new Promise((resolve) => setTimeout(resolve, 0));
  });
const option = (name: string) => screen.getByRole('option', { name: new RegExp(`^${name}\\b`) });
const search = () => screen.getByRole('combobox', { name: 'Search presets' });
const dialog = () => screen.queryByRole('dialog', { name: 'Preset browser' });

async function openBrowser() {
  fireEvent.click(screen.getByRole('button', { name: /Open preset browser/ }));
  await settle();
}

describe('PresetBrowser', () => {
  let h: Harness;
  beforeEach(async () => {
    h = await setupMock();
  });
  afterEach(async () => {
    await settle();
    cleanup(); // unmount before the stores reset
    h.dispose();
  });

  /** An unsaved edit on top of Nacre; returns every value as it stands. */
  async function editNacre() {
    await act(async () => {
      await h.mock.call('sedef_setParam', 'lagoonVolume', 0.81);
      await h.mock.call('sedef_setParam', 'rippleSpeed', 1.2345);
      await flush();
    });
    expect(presets().dirty).toBe(true);
    return (await h.mock.hello()).values;
  }

  it('shows rich rows and starts on the playing preset with the search focused', async () => {
    render(<Shell />);
    await openBrowser();
    const nacre = option('Nacre');
    expect(nacre).toHaveAttribute('aria-selected', 'true');
    expect(nacre.getAttribute('aria-label')).toContain('Tide → Ripple → Lagoon 1x12 → Fathom');
    expect(nacre.getAttribute('aria-label')).toMatch(/Glassy/);
    expect(within(nacre).getByText('Lagoon')).toBeInTheDocument();
    expect(within(nacre).getByText('chorus')).toBeInTheDocument();
    expect(search()).toHaveFocus();
    expect(search()).toHaveAttribute('aria-activedescendant', nacre.id);
  });

  it('auditions on click, and Esc brings back the exact sound, edits included', async () => {
    const before = await editNacre();
    render(<Shell />);
    await openBrowser();
    expect(screen.getByRole('button', { name: /Back to Nacre with your edits/ })).toBeVisible();

    fireEvent.click(option('Mariana'));
    await settle();
    expect(h.mock.value('ampModel')).toBe(2);
    expect(presets().currentName).toBe('Mariana');
    expect(dialog()).toBeInTheDocument();

    fireEvent.keyDown(search(), { key: 'ArrowDown' });
    await settle();
    expect(presets().currentName).toBe('Morning Ritual');

    fireEvent.keyDown(search(), { key: 'Escape' });
    await settle();
    expect(dialog()).toBeNull();
    expect((await h.mock.hello()).values).toEqual(before);
    expect(presets()).toMatchObject({ currentName: 'Nacre', dirty: true, recent: [] });
    expect(screen.getByRole('status')).toHaveTextContent('Back to Nacre');
    expect(screen.getByRole('button', { name: /Open preset browser/ })).toHaveFocus();
  });

  it('keeps with Enter, records it in Recent, and offers Undo for the edits it replaced', async () => {
    const before = await editNacre();
    render(<Shell />);
    await openBrowser();
    // Nacre, then Riptide, Sargasso, Silk Road.
    for (let i = 0; i < 3; i++) fireEvent.keyDown(search(), { key: 'ArrowDown' });
    fireEvent.keyDown(search(), { key: 'Enter' });
    await settle();
    expect(dialog()).toBeNull();
    expect(presets()).toMatchObject({ currentName: 'Silk Road', recent: ['factory:silk-road'] });
    expect(screen.getByRole('status')).toHaveTextContent('Loaded Silk Road');

    fireEvent.click(screen.getByRole('button', { name: /Undo/ }));
    await settle();
    expect((await h.mock.hello()).values).toEqual(before);
    expect(presets()).toMatchObject({ currentName: 'Nacre', dirty: true });
  });

  it('keeps on double-click; a clean sound needs no Undo', async () => {
    render(<Shell />);
    await openBrowser();
    fireEvent.click(option('Brine'));
    fireEvent.doubleClick(option('Brine'));
    await settle();
    expect(dialog()).toBeNull();
    expect(presets()).toMatchObject({ currentName: 'Brine', recent: ['factory:brine'] });
    expect(screen.getByRole('status')).toBeEmptyDOMElement();
  });

  it('filters by library, category, tags and search, and sorts', async () => {
    render(<Shell />);
    await openBrowser();
    const names = () =>
      screen.getAllByRole('option').map((o) => o.getAttribute('aria-label')?.split(/[,.]/)[0]);
    const rail = within(screen.getByRole('navigation', { name: 'Preset library' }));
    const sort = within(screen.getByRole('radiogroup', { name: 'Sort' }));

    fireEvent.click(rail.getByRole('radio', { name: /Favourites/ }));
    expect(names()).toEqual(['Sunday set']);
    fireEvent.click(rail.getByRole('radio', { name: /Recent/ }));
    expect(screen.queryAllByRole('option')).toHaveLength(0);
    expect(screen.getByText('Presets you pick show up here.')).toBeInTheDocument();
    // Recent reads newest first.
    expect(sort.getByRole('radio', { name: 'Recent' })).toBeChecked();

    fireEvent.click(rail.getByRole('radio', { name: /Factory/ }));
    fireEvent.click(rail.getByRole('radio', { name: /^Ambient/ }));
    expect(names()).toEqual(['Soft Focus', 'Undertow']);
    fireEvent.click(rail.getByRole('radio', { name: 'All categories' }));
    fireEvent.click(screen.getByRole('button', { name: 'delay', pressed: false }));
    fireEvent.click(screen.getByRole('button', { name: 'hall', pressed: false }));
    expect(names()).toEqual(['Riptide', 'Soft Focus', 'Undertow']);
    fireEvent.change(search(), { target: { value: 'abyss drift' } });
    expect(names()).toEqual(['Riptide', 'Undertow']);
    expect(screen.getByText('2 presets')).toBeInTheDocument();

    fireEvent.click(screen.getAllByRole('button', { name: 'Clear filters' })[0]!);
    fireEvent.click(sort.getByRole('radio', { name: 'Category' }));
    expect(names()[0]).toBe('Soft Focus'); // Ambient comes first
  });

  it('plays a filtered result with Enter from the search field', async () => {
    render(<Shell />);
    await openBrowser();
    fireEvent.change(search(), { target: { value: 'slapback' } });
    fireEvent.keyDown(search(), { key: 'Enter' });
    await settle();
    expect(dialog()).toBeNull();
    expect(presets().currentName).toBe('Harbor Lights');
  });

  it('moves through the list from the listbox, and F toggles the favourite', async () => {
    render(<Shell />);
    await openBrowser();
    const list = screen.getByRole('listbox', { name: 'Presets' });
    list.focus();
    fireEvent.keyDown(list, { key: 'Home' });
    await settle();
    expect(presets().currentName).toBe('Bioluminescence');
    fireEvent.keyDown(list, { key: 'End' });
    await settle();
    expect(presets().currentName).toBe('Undertow');
    fireEvent.keyDown(list, { key: 'f' });
    await settle();
    expect(presets().items.find((p) => p.name === 'Undertow')?.favorite).toBe(true);
    expect(list).toHaveAttribute('aria-activedescendant', option('Undertow').id);
  });

  it('steps with prev/next and offers Undo when that dropped edits', async () => {
    const before = await editNacre();
    render(<Shell />);
    fireEvent.click(screen.getByRole('button', { name: 'Next preset' }));
    await settle();
    expect(presets().currentName).toBe('Tidewater');
    expect(screen.getByRole('status')).toHaveTextContent('Loaded Tidewater');
    fireEvent.keyDown(window, { key: 'z', ctrlKey: true });
    await settle();
    expect((await h.mock.hello()).values).toEqual(before);
    expect(presets().currentName).toBe('Nacre');

    // Without edits there is nothing to undo.
    await act(() => presetActions.load('factory:nacre'));
    fireEvent.click(screen.getByRole('button', { name: 'Previous preset' }));
    await settle();
    expect(screen.getByRole('status')).toBeEmptyDOMElement();
  });

  it('compares A and B, each slot keeping its own preset and edits', async () => {
    render(<Shell />);
    const slot = (s: 'A' | 'B') => screen.getByRole('radio', { name: new RegExp(`^${s}:`) });
    expect(slot('B')).toHaveAccessibleName(/empty/);
    fireEvent.click(slot('B'));
    await settle();
    expect(slot('B')).toBeChecked();
    expect(presets().currentName).toBe('Nacre');

    await act(() => presetActions.load('factory:riptide'));
    await act(async () => {
      await h.mock.call('sedef_setParam', 'abyssGain', 0.91);
      await flush();
    });
    expect(slot('B')).toHaveAccessibleName('B: Riptide, edited');

    fireEvent.click(slot('A'));
    await settle();
    expect(slot('A')).toBeChecked();
    expect(h.mock.value('ampModel')).toBe(0);
    expect(slot('B')).toHaveAccessibleName('B: Riptide, edited');

    fireEvent.keyDown(slot('A'), { key: 'ArrowRight' });
    await settle();
    expect(h.mock.value('abyssGain')).toBe(0.91);
    expect(presets()).toMatchObject({ currentName: 'Riptide', dirty: true });

    fireEvent.click(screen.getByRole('button', { name: 'Copy B to A' }));
    await settle();
    expect(slot('A')).toHaveAccessibleName('A: Riptide, edited');
  });

  it('validates Save as inline: name clashes, new categories, tags and a note', async () => {
    const save = vi.spyOn(presetActions, 'save');
    render(<Shell />);
    await openBrowser();
    fireEvent.click(screen.getByRole('button', { name: 'Save as…' }));
    // Queries stay inside the sheet: the whole browser makes role queries slow in jsdom.
    const sheet = within(screen.getByRole('group', { name: 'Save as a user preset' }));
    const name = sheet.getByRole('textbox', { name: 'Name' });
    expect(name).toHaveFocus();
    expect(name).toHaveValue('Nacre');
    expect(sheet.getByText(/A factory preset has this name too/)).toBeInTheDocument();

    fireEvent.change(name, { target: { value: 'sunday SET' } });
    fireEvent.change(sheet.getByRole('combobox', { name: 'Category' }), {
      target: { value: 'Live' },
    });
    expect(sheet.getByText(/is already in User \/ Live\. Saving replaces it\./)).toBeVisible();
    expect(sheet.getByRole('button', { name: 'Replace “Sunday set”' })).toBeEnabled();

    fireEvent.change(sheet.getByRole('combobox', { name: 'Category' }), {
      target: { value: '__new-category__' },
    });
    const newCategory = sheet.getByRole('textbox', { name: 'New category' });
    expect(newCategory).toHaveFocus();
    expect(sheet.getByRole('button', { name: 'Save preset' })).toBeDisabled();
    fireEvent.change(newCategory, { target: { value: 'Worship' } });
    fireEvent.change(name, { target: { value: 'Pads' } });

    for (const t of ['chorus', 'compressor', 'plate'])
      fireEvent.click(sheet.getByRole('button', { name: `Remove tag ${t}` }));
    const tag = sheet.getByRole('textbox', { name: 'Add a tag' });
    fireEvent.change(tag, { target: { value: 'Swell' } });
    fireEvent.keyDown(tag, { key: 'Enter' });
    const suggested = sheet.getByRole('group', { name: 'Suggested tags' });
    fireEvent.click(within(suggested).getByRole('button', { name: 'hall' }));
    fireEvent.change(sheet.getByRole('textbox', { name: /Note/ }), {
      target: { value: 'Big room pads' },
    });
    fireEvent.click(sheet.getByRole('button', { name: 'Save preset' }));
    await settle();

    expect(save).toHaveBeenCalledWith('Pads', 'Worship', {
      tags: ['swell', 'hall'],
      description: 'Big room pads',
    });
    const saved = presets().items.find((p) => p.id === presets().currentId);
    expect(saved).toMatchObject({ name: 'Pads', folder: 'Worship', tags: ['swell', 'hall'] });
    expect(screen.getByRole('status')).toHaveTextContent('Saved Pads to User / Worship');
    // Back in the list, and Esc now stays on the saved preset.
    expect(search()).toHaveFocus();
    fireEvent.keyDown(search(), { key: 'Escape' });
    await settle();
    expect(presets().currentName).toBe('Pads');
  }, 15_000);

  it('saves with ⌘S: in place for a user preset, through Save as for a factory one', async () => {
    const save = vi.spyOn(presetActions, 'save');
    render(<Shell />);
    fireEvent.keyDown(window, { key: 's', ctrlKey: true });
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveFocus();
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Name' }), { key: 'Escape' });
    expect(dialog()).toBeNull(); // opened to save, so cancelling leaves the browser too

    await act(() => presetActions.load(SUNDAY));
    fireEvent.keyDown(window, { key: 's', metaKey: true });
    await settle();
    expect(save).toHaveBeenCalledWith('Sunday set', 'Live');
    expect(screen.getByRole('status')).toHaveTextContent('Saved Sunday set');
    expect(presets().items.find((p) => p.id === SUNDAY)?.tags).toEqual(['worship', 'delay']);

    fireEvent.keyDown(window, { key: 'S', ctrlKey: true, shiftKey: true });
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveValue('Sunday set copy');
  });

  it('deletes a user preset at once, with Undo', async () => {
    await act(() => presetActions.load(SUNDAY));
    render(<Shell />);
    await openBrowser();
    fireEvent.click(screen.getByRole('button', { name: 'Delete' }));
    await settle();
    expect(presets().items.some((p) => p.id === SUNDAY)).toBe(false);
    expect(screen.getByRole('status')).toHaveTextContent('Deleted Sunday set');
    fireEvent.click(screen.getByRole('button', { name: /Undo/ }));
    await settle();
    expect(presets().items.some((p) => p.id === SUNDAY)).toBe(true);
    expect(presets().currentId).toBe(SUNDAY);
  });

  it('opens with / or ⌘F, and digit keys never switch sections behind it', async () => {
    render(<Shell />);
    useUiStore.setState({ section: 'amp' });
    fireEvent.keyDown(window, { key: '/' });
    await settle();
    expect(search()).toHaveFocus();
    fireEvent.keyDown(search(), { key: '1' });
    const list = screen.getByRole('listbox', { name: 'Presets' });
    list.focus();
    fireEvent.keyDown(list, { key: '3' });
    expect(useUiStore.getState().section).toBe('amp');

    fireEvent.keyDown(list, { key: 'f', ctrlKey: true });
    expect(search()).toHaveFocus();
    fireEvent.keyDown(search(), { key: 'Escape' });
    expect(dialog()).toBeNull();
    fireEvent.keyDown(window, { key: '3' });
    expect(useUiStore.getState().section).toBe('eq');
  });

  it('details the highlighted preset: chain by stage, tags that filter, and its actions', async () => {
    render(<Shell />);
    await openBrowser();
    const pane = within(screen.getByRole('complementary', { name: 'Preset details' }));
    expect(pane.getByRole('heading', { name: 'Nacre' })).toBeInTheDocument();
    expect(pane.getByText('Clean · Sedef')).toBeInTheDocument();
    const chain = Object.fromEntries(
      pane.getAllByRole('term').map((t) => [t.textContent, t.nextElementSibling?.textContent]),
    );
    expect(chain).toEqual({
      Pedals: 'Tide → Ripple',
      Amp: 'Lagoon',
      Cab: '1x12 Open Back',
      FX: 'Fathom',
    });
    // Factory presets can be saved as a copy, not renamed or deleted.
    expect(pane.getByRole('button', { name: 'Save as…' })).toBeInTheDocument();
    expect(pane.queryByRole('button', { name: 'Delete' })).toBeNull();

    fireEvent.click(pane.getByRole('button', { name: 'plate', pressed: false }));
    expect(useBrowserStore.getState().filter.tags).toEqual(['plate']);

    fireEvent.click(option('Tidewater'));
    await settle();
    expect(pane.getByRole('heading', { name: 'Tidewater' })).toBeInTheDocument();
    fireEvent.click(pane.getByRole('button', { name: /^Use & close/ }));
    await settle();
    expect(dialog()).toBeNull();
    expect(presets().currentName).toBe('Tidewater');
  });

  it('has single-key shortcuts that never fire while typing, and lists them', async () => {
    render(<Shell />);
    const key = (k: string, target: Element | Window = window) =>
      fireEvent.keyDown(target, { key: k });

    key(']');
    await settle();
    expect(presets().currentName).toBe('Tidewater');
    key('[');
    await settle();
    expect(presets().currentName).toBe('Nacre');

    key('b');
    await settle();
    expect(presets().compare.active).toBe('B');
    key('a');
    await settle();
    expect(presets().compare.active).toBe('A');

    key('p');
    await settle();
    expect(dialog()).toBeInTheDocument();
    for (const k of ['p', '[', ']', 'b', '3']) key(k, search());
    await settle();
    expect(dialog()).toBeInTheDocument();
    expect(presets()).toMatchObject({ currentName: 'Nacre', compare: { active: 'A' } });

    fireEvent.click(screen.getByRole('button', { name: 'Shortcuts' }));
    const list = within(screen.getByRole('group', { name: 'Shortcuts' }));
    expect(list.getByText('Previous or next preset')).toBeInTheDocument();
    expect(list.getByText('Compare slot A or B')).toBeInTheDocument();
    // Esc closes the list first, then the browser.
    fireEvent.keyDown(search(), { key: 'Escape' });
    expect(screen.queryByRole('group', { name: 'Shortcuts' })).toBeNull();
    expect(dialog()).toBeInTheDocument();
    key('p', screen.getByRole('listbox', { name: 'Presets' }));
    expect(dialog()).toBeNull();
  });

  it('saves from the header: Save as for a factory preset, in place for a user one', async () => {
    const save = vi.spyOn(presetActions, 'save');
    render(<Shell />);
    expect(screen.getByRole('button', { name: /Open preset browser/ })).toHaveTextContent(
      /^Clean\s*Factory/,
    );
    fireEvent.click(screen.getByRole('button', { name: 'Save as a user preset' }));
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveFocus();
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Name' }), { key: 'Escape' });

    await act(() => presetActions.load(SUNDAY));
    fireEvent.click(screen.getByRole('button', { name: 'Save Sunday set' }));
    await settle();
    expect(save).toHaveBeenCalledWith('Sunday set', 'Live');
  });

  it('keeps focus inside the browser when it switches between the list and a form', async () => {
    render(<Shell />);
    await openBrowser();
    fireEvent.click(screen.getByRole('button', { name: 'Save as…' }));
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(useBrowserStore.getState().mode).toBe('list');
    expect(search()).toHaveFocus();
  });

  it('refuses a rename onto another preset in the same folder, and says why', async () => {
    await act(async () => {
      await h.mock.call('sedef_presets_save', 'Encore', 'Live');
      await presetActions.load(SUNDAY);
      await flush();
    });
    const rename = vi.spyOn(presetActions, 'rename');
    render(<Shell />);
    await openBrowser();
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    const input = screen.getByRole('textbox', { name: 'New name' });
    expect(input).toHaveFocus();

    fireEvent.change(input, { target: { value: 'encore' } });
    expect(screen.getByRole('alert')).toHaveTextContent('already called “encore”');
    expect(input).toHaveAttribute('aria-invalid', 'true');
    fireEvent.submit(input);
    expect(rename).not.toHaveBeenCalled();

    fireEvent.change(input, { target: { value: 'Sunday best' } });
    expect(screen.queryByRole('alert')).toBeNull();
    fireEvent.submit(input);
    await settle();
    expect(rename).toHaveBeenCalledWith(SUNDAY, 'Sunday best');
    expect(presets().currentName).toBe('Sunday best');
    expect(option('Sunday best')).toHaveAttribute('aria-selected', 'true');
  });

  it('sends one load per row however fast the keys come, and keeps the last one', async () => {
    const load = vi.spyOn(presetActions, 'load');
    render(<Shell />);
    await openBrowser();
    for (let i = 0; i < 7; i++) fireEvent.keyDown(search(), { key: 'ArrowDown' });
    fireEvent.keyDown(search(), { key: 'Enter' });
    await settle();
    // Keys faster than the plugin answers: only the newest waiting load is sent.
    expect(load.mock.calls.map(([id]) => id)).toEqual(['factory:undertow']);
    expect(presets()).toMatchObject({ currentName: 'Undertow', recent: ['factory:undertow'] });

    load.mockClear();
    await openBrowser();
    fireEvent.click(option('Riptide'));
    fireEvent.click(option('Riptide'));
    fireEvent.doubleClick(option('Riptide'));
    await settle();
    expect(load).toHaveBeenCalledTimes(1);
    expect(presets().currentName).toBe('Riptide');
  });

  it('lands on the picked preset when answers arrive out of order', async () => {
    cleanup();
    h.dispose();
    h = await setupMock({ jitter: { maxMs: 10, seed: 7 } });
    render(<Shell />);
    await openBrowser();
    for (let i = 0; i < 4; i++) fireEvent.keyDown(search(), { key: 'ArrowDown' });
    fireEvent.click(option('Mariana'));
    fireEvent.keyDown(search(), { key: 'ArrowUp' });
    fireEvent.keyDown(search(), { key: 'Enter' });
    await act(() => new Promise((resolve) => setTimeout(resolve, 120)));
    const truth = await h.mock.call('sedef_presets_list');
    expect(truth).toMatchObject({ currentName: 'Harbor Lights' });
    expect(presets()).toMatchObject({ currentName: 'Harbor Lights', recent: truth.recent });
  });

  it('offers no Undo across A/B slots', async () => {
    await editNacre();
    render(<Shell />);
    const slot = (s: 'A' | 'B') => screen.getByRole('radio', { name: new RegExp(`^${s}:`) });
    await openBrowser();
    fireEvent.click(option('Riptide'));
    fireEvent.click(slot('B'));
    await settle();
    expect(dialog()).toBeNull();
    expect(presets()).toMatchObject({ currentName: 'Riptide', compare: { active: 'B' } });
    expect(screen.queryByRole('button', { name: /Undo/ })).toBeNull();

    // An Undo from prev/next goes too: its snapshot belongs to the slot left behind.
    await act(async () => {
      await h.mock.call('sedef_setParam', 'abyssGain', 0.91);
      await flush();
    });
    fireEvent.keyDown(window, { key: ']' });
    await settle();
    expect(screen.getByRole('button', { name: /Undo/ })).toBeInTheDocument();
    fireEvent.click(slot('A'));
    await settle();
    expect(screen.queryByRole('button', { name: /Undo/ })).toBeNull();
    // The first switch copied the live sound, so A holds Riptide as it was then.
    expect(presets()).toMatchObject({ currentName: 'Riptide', dirty: false });
  });

  it('keeps ⌘S out of other text fields and held keys, and asks first while auditioning', async () => {
    const save = vi.spyOn(presetActions, 'save');
    await act(() => presetActions.load(SUNDAY));
    render(
      <>
        <Shell />
        <input aria-label="Other field" />
      </>,
    );
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Other field' }), {
      key: 's',
      metaKey: true,
    });
    fireEvent.keyDown(window, { key: 's', metaKey: true, repeat: true });
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Other field' }), {
      key: 'f',
      metaKey: true,
    });
    await settle();
    expect(save).not.toHaveBeenCalled();
    expect(dialog()).toBeNull();

    // In the browser over unsaved edits, it is not clear which sound ⌘S means.
    await act(async () => {
      await h.mock.call('sedef_setParam', 'lagoonVolume', 0.81);
      await flush();
    });
    await openBrowser();
    fireEvent.click(option('Riptide'));
    await settle();
    fireEvent.keyDown(search(), { key: 's', metaKey: true });
    await settle();
    expect(save).not.toHaveBeenCalled();
    expect(screen.getByRole('status')).toHaveTextContent(
      'Choose first: Enter keeps Riptide, Esc goes back to Sunday set with your edits. Then save.',
    );
    expect(dialog()).toBeInTheDocument();
  });

  it("keeps a delete's Undo through Esc and the next session", async () => {
    render(<Shell />);
    await openBrowser();
    fireEvent.click(option('Sunday set'));
    await settle();
    fireEvent.click(screen.getByRole('button', { name: 'Delete' }));
    await settle();
    expect(screen.getByRole('status')).toHaveTextContent('Deleted Sunday set');

    fireEvent.keyDown(search(), { key: 'Escape' });
    await settle();
    expect(presets().currentName).toBe('Nacre');
    expect(screen.getByRole('status')).toHaveTextContent('Deleted Sunday set');
    await openBrowser();
    fireEvent.keyDown(search(), { key: 'Escape' });
    expect(screen.getByRole('status')).toHaveTextContent('Deleted Sunday set');

    fireEvent.click(screen.getByRole('button', { name: /Undo/ }));
    await settle();
    expect(presets().items.some((p) => p.id === SUNDAY)).toBe(true);
    expect(presets().currentName).toBe('Nacre');
  });

  it('follows a rename of the preset Esc goes back to, edits and all', async () => {
    await act(async () => {
      await presetActions.load(SUNDAY);
      await h.mock.call('sedef_setParam', 'lagoonVolume', 0.81);
      await flush();
    });
    render(<Shell />);
    await openBrowser();
    fireEvent.click(option('Riptide'));
    fireEvent.click(option('Sunday set'));
    await settle();
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    const input = screen.getByRole('textbox', { name: 'New name' });
    fireEvent.change(input, { target: { value: 'Sunday late' } });
    fireEvent.submit(input);
    await settle();
    fireEvent.click(option('Riptide'));
    await settle();

    fireEvent.keyDown(search(), { key: 'Escape' });
    await settle();
    expect(presets()).toMatchObject({
      currentId: 'user:Live/Sunday late.sedefpreset',
      currentName: 'Sunday late',
      dirty: true,
    });
    expect(h.mock.value('lagoonVolume')).toBe(0.81);
    expect(screen.getByRole('status')).toHaveTextContent('Back to Sunday late');
  });
});
