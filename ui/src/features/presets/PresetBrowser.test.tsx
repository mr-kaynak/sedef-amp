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

  /** An unsaved edit on top of Glassy Chorus; returns every value as it stands. */
  async function editNacre() {
    await act(async () => {
      await h.mock.call('sedef_setParam', 'lagoonVolume', 0.81);
      await h.mock.call('sedef_setParam', 'rippleSpeed', 1.2345);
      await flush();
    });
    expect(presets().dirty).toBe(true);
    return (await h.mock.hello()).values;
  }

  it('shows one-line rows under category headers and starts on the playing preset with the search focused', async () => {
    render(<Shell />);
    await openBrowser();
    const nacre = option('Glassy Chorus');
    expect(nacre).toHaveAttribute('aria-selected', 'true');
    expect(nacre.getAttribute('aria-label')).toContain('Tide → Ripple → Lagoon 1x12 → Fathom');
    expect(nacre.getAttribute('aria-label')).toMatch(/Glassy/);
    expect(within(nacre).getByText('Lagoon')).toBeInTheDocument();
    // One line per preset: name and chain; the note and the tags are in the strip under the list.
    expect(nacre.textContent).not.toContain('compressor');
    const strip = within(screen.getByRole('complementary', { name: 'Preset details' }));
    expect(strip.getByText('compressor')).toBeInTheDocument();
    expect(within(nacre).queryByText(/lightly squeezed/)).toBeNull();
    expect(screen.getByRole('group', { name: 'Clean' })).toContainElement(nacre);
    expect(search()).toHaveFocus();
    expect(search()).toHaveAttribute('aria-activedescendant', nacre.id);
  });

  it('auditions on click, and Esc brings back the exact sound, edits included', async () => {
    const before = await editNacre();
    render(<Shell />);
    await openBrowser();
    expect(
      screen.getByRole('button', { name: /Back to Glassy Chorus with your edits/ }),
    ).toBeVisible();

    fireEvent.click(option('Modern Metal Rhythm'));
    await settle();
    expect(h.mock.value('ampModel')).toBe(2);
    expect(presets().currentName).toBe('Modern Metal Rhythm');
    expect(dialog()).toBeInTheDocument();

    fireEvent.keyDown(search(), { key: 'ArrowDown' });
    await settle();
    expect(presets().currentName).toBe('Octave Down Chug');

    fireEvent.keyDown(search(), { key: 'Escape' });
    await settle();
    expect(dialog()).toBeNull();
    expect((await h.mock.hello()).values).toEqual(before);
    expect(presets()).toMatchObject({ currentName: 'Glassy Chorus', dirty: true, recent: [] });
    expect(screen.getByRole('status')).toHaveTextContent('Back to Glassy Chorus');
    expect(screen.getByRole('button', { name: /Open preset browser/ })).toHaveFocus();
  });

  it('keeps with Enter, records it in Recent, and offers Undo for the edits it replaced', async () => {
    const before = await editNacre();
    render(<Shell />);
    await openBrowser();
    // Glassy Chorus, then Jazz Warm, Soft Phaser, Surf Tremolo.
    for (let i = 0; i < 3; i++) fireEvent.keyDown(search(), { key: 'ArrowDown' });
    fireEvent.keyDown(search(), { key: 'Enter' });
    await settle();
    expect(dialog()).toBeNull();
    expect(presets()).toMatchObject({
      currentName: 'Surf Tremolo',
      recent: ['factory:tidewater'],
    });
    expect(screen.getByRole('status')).toHaveTextContent('Loaded Surf Tremolo');

    fireEvent.click(screen.getByRole('button', { name: /Undo/ }));
    await settle();
    expect((await h.mock.hello()).values).toEqual(before);
    expect(presets()).toMatchObject({ currentName: 'Glassy Chorus', dirty: true });
  });

  it('keeps on double-click; a clean sound needs no Undo', async () => {
    render(<Shell />);
    await openBrowser();
    fireEvent.click(option('Thick Mids'));
    fireEvent.doubleClick(option('Thick Mids'));
    await settle();
    expect(dialog()).toBeNull();
    expect(presets()).toMatchObject({ currentName: 'Thick Mids', recent: ['factory:brine'] });
    expect(screen.getByRole('status')).toBeEmptyDOMElement();
  });

  it('groups by category, filters by library and search, and lists Recent newest first', async () => {
    render(<Shell />);
    await openBrowser();
    const names = () =>
      screen.getAllByRole('option').map((o) => o.getAttribute('aria-label')?.split(/[,.]/)[0]);
    const groups = () =>
      within(screen.getByRole('listbox', { name: 'Presets' }))
        .queryAllByRole('group')
        .map((g) => g.getAttribute('aria-labelledby'))
        .map((id) => document.getElementById(id ?? '')?.textContent);
    const library = within(screen.getByRole('radiogroup', { name: 'Library' }));

    // Clean to ambient, then the user's own folders; previous and next step in the same order.
    expect(groups()).toEqual(['Clean', 'Crunch', 'High Gain', 'Lead', 'Ambient', 'Live']);
    expect(names().slice(0, 3)).toEqual(['Auto-Wah Funk', 'Country Snap', 'Dry Studio']);
    expect(names()).toEqual(presets().items.map((p) => p.name));
    expect(names().length).toBeGreaterThanOrEqual(41);

    fireEvent.click(library.getByRole('radio', { name: /Favourites/ }));
    expect(names()).toEqual(['Sunday set']);
    fireEvent.click(library.getByRole('radio', { name: /Recent/ }));
    expect(screen.queryAllByRole('option')).toHaveLength(0);
    expect(screen.getByText('Presets you pick show up here.')).toBeInTheDocument();
    fireEvent.click(library.getByRole('radio', { name: /User/ }));
    expect(names()).toEqual(['Sunday set']);

    fireEvent.click(library.getByRole('radio', { name: /All/ }));
    fireEvent.change(search(), { target: { value: 'abyss drift hall' } });
    expect(names()).toEqual(['Singing Echo', 'Dark Drone']);
    expect(groups()).toEqual(['Lead', 'Ambient']);
    expect(screen.getByText('2 matches')).toBeInTheDocument();
    fireEvent.change(search(), { target: { value: 'no such sound' } });
    expect(screen.getByText('No presets match.')).toBeInTheDocument();
    fireEvent.click(screen.getAllByRole('button', { name: 'Clear search' })[0]!);
    expect(names().length).toBeGreaterThanOrEqual(41);

    // Recent: what was picked, newest first, without headers; each row names its category.
    await act(() => presetActions.load('factory:brine'));
    await act(() => presetActions.load('factory:undertow'));
    await settle();
    fireEvent.click(library.getByRole('radio', { name: /Recent/ }));
    expect(names()).toEqual(['Dark Drone', 'Thick Mids']);
    expect(groups()).toEqual([]);
    expect(within(option('Thick Mids')).getByText('Crunch')).toBeInTheDocument();
  });

  it('plays a filtered result with Enter from the search field', async () => {
    render(<Shell />);
    await openBrowser();
    fireEvent.change(search(), { target: { value: 'rockabilly' } });
    fireEvent.keyDown(search(), { key: 'Enter' });
    await settle();
    expect(dialog()).toBeNull();
    expect(presets().currentName).toBe('Rockabilly Slapback');
  });

  it('moves through the list from the listbox, and F toggles the favourite', async () => {
    render(<Shell />);
    await openBrowser();
    const list = screen.getByRole('listbox', { name: 'Presets' });
    list.focus();
    fireEvent.keyDown(list, { key: 'Home' });
    await settle();
    expect(presets().currentName).toBe('Auto-Wah Funk');
    fireEvent.keyDown(list, { key: 'End' });
    await settle();
    expect(presets().currentName).toBe('Sunday set');
    fireEvent.keyDown(list, { key: 'ArrowUp' });
    await settle();
    expect(presets().currentName).toBe('Vibrato Swell Wash');
    fireEvent.keyDown(list, { key: 'f' });
    await settle();
    expect(presets().items.find((p) => p.name === 'Vibrato Swell Wash')?.favorite).toBe(true);
    expect(list).toHaveAttribute('aria-activedescendant', option('Vibrato Swell Wash').id);
  });

  it('steps with prev/next and offers Undo when that dropped edits', async () => {
    const before = await editNacre();
    render(<Shell />);
    fireEvent.click(screen.getByRole('button', { name: 'Next preset' }));
    await settle();
    expect(presets().currentName).toBe('Jazz Warm');
    expect(screen.getByRole('status')).toHaveTextContent('Loaded Jazz Warm');
    fireEvent.keyDown(window, { key: 'z', ctrlKey: true });
    await settle();
    expect((await h.mock.hello()).values).toEqual(before);
    expect(presets().currentName).toBe('Glassy Chorus');

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
    expect(presets().currentName).toBe('Glassy Chorus');

    await act(() => presetActions.load('factory:riptide'));
    await act(async () => {
      await h.mock.call('sedef_setParam', 'abyssGain', 0.91);
      await flush();
    });
    expect(slot('B')).toHaveAccessibleName('B: Singing Echo, edited');

    fireEvent.click(slot('A'));
    await settle();
    expect(slot('A')).toBeChecked();
    expect(h.mock.value('ampModel')).toBe(0);
    expect(slot('B')).toHaveAccessibleName('B: Singing Echo, edited');

    fireEvent.keyDown(slot('A'), { key: 'ArrowRight' });
    await settle();
    expect(h.mock.value('abyssGain')).toBe(0.91);
    expect(presets()).toMatchObject({ currentName: 'Singing Echo', dirty: true });

    fireEvent.click(screen.getByRole('button', { name: 'Copy B to A' }));
    await settle();
    expect(slot('A')).toHaveAccessibleName('A: Singing Echo, edited');
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
    expect(name).toHaveValue('Glassy Chorus');
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

  it('describes the highlighted preset under the list, or the row under the pointer', async () => {
    render(<Shell />);
    await openBrowser();
    const pane = within(screen.getByRole('complementary', { name: 'Preset details' }));
    expect(pane.getByRole('heading', { name: 'Glassy Chorus' })).toBeInTheDocument();
    expect(pane.getByText(/^Glassy, lightly squeezed clean/)).toBeInTheDocument();
    expect(pane.getByText('1x12 Open Back')).toBeInTheDocument();
    expect(pane.getByText('Playing')).toBeInTheDocument();
    // Factory presets can be saved as a copy, not renamed or deleted.
    expect(pane.getByRole('button', { name: 'Save as…' })).toBeInTheDocument();
    expect(pane.queryByRole('button', { name: 'Delete' })).toBeNull();

    // Pointing at a row shows its note without loading it; leaving the list goes back.
    fireEvent.pointerEnter(option('Dark Drone'));
    expect(pane.getByRole('heading', { name: 'Dark Drone' })).toBeInTheDocument();
    expect(pane.getByText(/12-second hall/)).toBeInTheDocument();
    expect(pane.getByText('Click to audition')).toBeInTheDocument();
    expect(presets().currentName).toBe('Glassy Chorus');
    fireEvent.pointerLeave(screen.getByRole('listbox', { name: 'Presets' }).parentElement!);
    expect(pane.getByRole('heading', { name: 'Glassy Chorus' })).toBeInTheDocument();

    fireEvent.click(option('Surf Tremolo'));
    await settle();
    expect(pane.getByRole('heading', { name: 'Surf Tremolo' })).toBeInTheDocument();
    fireEvent.click(pane.getByRole('button', { name: /^Use & close/ }));
    await settle();
    expect(dialog()).toBeNull();
    expect(presets().currentName).toBe('Surf Tremolo');
  });

  it('has single-key shortcuts that never fire while typing, and lists them', async () => {
    render(<Shell />);
    const key = (k: string, target: Element | Window = window) =>
      fireEvent.keyDown(target, { key: k });

    key(']');
    await settle();
    expect(presets().currentName).toBe('Jazz Warm');
    key('[');
    await settle();
    expect(presets().currentName).toBe('Glassy Chorus');

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
    expect(presets()).toMatchObject({ currentName: 'Glassy Chorus', compare: { active: 'A' } });

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
    expect(load.mock.calls.map(([id]) => id)).toEqual(['factory:classic-stack']);
    expect(presets()).toMatchObject({
      currentName: 'Classic Stack',
      recent: ['factory:classic-stack'],
    });

    load.mockClear();
    await openBrowser();
    fireEvent.click(option('Singing Echo'));
    fireEvent.click(option('Singing Echo'));
    fireEvent.doubleClick(option('Singing Echo'));
    await settle();
    expect(load).toHaveBeenCalledTimes(1);
    expect(presets().currentName).toBe('Singing Echo');
  });

  it('lands on the picked preset when answers arrive out of order', async () => {
    cleanup();
    h.dispose();
    h = await setupMock({ jitter: { maxMs: 10, seed: 7 } });
    render(<Shell />);
    await openBrowser();
    for (let i = 0; i < 4; i++) fireEvent.keyDown(search(), { key: 'ArrowDown' });
    fireEvent.click(option('Modern Metal Rhythm'));
    fireEvent.keyDown(search(), { key: 'ArrowUp' });
    fireEvent.keyDown(search(), { key: 'Enter' });
    await act(() => new Promise((resolve) => setTimeout(resolve, 120)));
    const truth = await h.mock.call('sedef_presets_list');
    expect(truth).toMatchObject({ currentName: 'Max Gain Wall' });
    expect(presets()).toMatchObject({ currentName: 'Max Gain Wall', recent: truth.recent });
  });

  it('offers no Undo across A/B slots', async () => {
    await editNacre();
    render(<Shell />);
    const slot = (s: 'A' | 'B') => screen.getByRole('radio', { name: new RegExp(`^${s}:`) });
    await openBrowser();
    fireEvent.click(option('Singing Echo'));
    fireEvent.click(slot('B'));
    await settle();
    expect(dialog()).toBeNull();
    expect(presets()).toMatchObject({ currentName: 'Singing Echo', compare: { active: 'B' } });
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
    // The first switch copied the live sound, so A holds Singing Echo as it was then.
    expect(presets()).toMatchObject({ currentName: 'Singing Echo', dirty: false });
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
    fireEvent.click(option('Singing Echo'));
    await settle();
    fireEvent.keyDown(search(), { key: 's', metaKey: true });
    await settle();
    expect(save).not.toHaveBeenCalled();
    expect(screen.getByRole('status')).toHaveTextContent(
      'Choose first: Enter keeps Singing Echo, Esc goes back to Sunday set with your edits. Then save.',
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
    expect(presets().currentName).toBe('Glassy Chorus');
    expect(screen.getByRole('status')).toHaveTextContent('Deleted Sunday set');
    await openBrowser();
    fireEvent.keyDown(search(), { key: 'Escape' });
    expect(screen.getByRole('status')).toHaveTextContent('Deleted Sunday set');

    fireEvent.click(screen.getByRole('button', { name: /Undo/ }));
    await settle();
    expect(presets().items.some((p) => p.id === SUNDAY)).toBe(true);
    expect(presets().currentName).toBe('Glassy Chorus');
  });

  it('follows a rename of the preset Esc goes back to, edits and all', async () => {
    await act(async () => {
      await presetActions.load(SUNDAY);
      await h.mock.call('sedef_setParam', 'lagoonVolume', 0.81);
      await flush();
    });
    render(<Shell />);
    await openBrowser();
    fireEvent.click(option('Singing Echo'));
    fireEvent.click(option('Sunday set'));
    await settle();
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    const input = screen.getByRole('textbox', { name: 'New name' });
    fireEvent.change(input, { target: { value: 'Sunday late' } });
    fireEvent.submit(input);
    await settle();
    fireEvent.click(option('Singing Echo'));
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
