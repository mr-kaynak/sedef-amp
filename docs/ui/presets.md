# Presets: browser, audition, A/B and saving

Status: implemented. This note records the patterns we adopted for the preset experience, why,
and the exact interaction spec. Code: `ui/src/features/presets/`, the preset bar in
`ui/src/features/header/PresetBar.tsx`, and `plugin/src/PresetManager.cpp`.

![Preset browser](../images/presets.png)

## What we looked at

We studied a few well-liked libraries for interaction patterns only (no code or assets):

- **[TONE3000](https://www.tone3000.com)**, a community library of amp and pedal captures. Its
  in-plugin browser opens on _Trending_, _Recently used_, _Favorites_ and _Created_; gear-type
  filters (amp + cab, amp head, cab, pedal…) narrow the catalogue; tags are, in its own sharing
  guidelines, what search, filtering and discovery run on. Every entry says which gear it is.
  ([plugin guide](https://www.tone3000.com/guides/tone3000-plugin),
  [sharing guidelines](https://www.tone3000.com/guides/tone-sharing-guidelines))
- **GarageBand's Library**: categories on the left, patches on the right, a search field.
  Selecting a patch _auditions_ it: you play through it straight away, before deciding. Save
  and Delete sit at the bottom, and a deleted patch cannot be recovered.
  ([Library](https://support.apple.com/guide/garageband/gbnddf60c8c8/mac),
  [choose a patch](https://support.apple.com/guide/garageband/gbndbd9d9b33/mac))
- Plugin preset browsers in general: star favourites, tag filters, an A/B compare pair in the
  header, and a "back to where I was" when browsing goes nowhere.
- A separate prototype exploration of this UI (built from our own screenshots) contributed the
  three-pane layout with a detail pane, the hint that teaches auditioning, `#tag` rows, a
  "playing" lamp on the loaded row, single-key shortcuts and Save beside the preset bar. We kept
  our own palette, type and hardware language.

## What we adopted, and why

| Pattern                                                                                                | Why                                                                                                                                                                                    | Where                                   |
| ------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| A large browser over the rig instead of a popover                                                      | Choosing a sound is a mode of its own. A 400 px popover could only show names; the panel shows what each preset _is_. The header stays live (gain, meters, A/B, tuner).                | `PresetBrowser.tsx`, `App.tsx`          |
| Click to audition (GarageBand)                                                                         | Guitarists choose by ear. One click, or ↑/↓, and the preset plays while the list stays put.                                                                                            | `session.ts` `audition`                 |
| Esc returns exactly to where you were                                                                  | Auditioning is only free if the way back is certain, including unsaved tweaks. The footer says so: "Esc Back to Nacre with your edits".                                                | plugin snapshots, `session.ts` `cancel` |
| Undo instead of confirmations                                                                          | A dialog interrupts and trains people to click through. Loads that drop edits and deletions are instant and offer Undo in a toast (also ⌘/Ctrl+Z).                                     | `Toasts.tsx`, `session.ts`              |
| Library and categories in a rail, tags as chips (TONE3000 sections and filters, GarageBand categories) | Two independent questions: _whose_ presets (all, favourites, recent, factory, user) and _what kind_ (Clean, Crunch, High Gain, Lead, Ambient, your own folders). Tags cut across both. | `LibraryRail`, `library.ts`             |
| Rows that describe the sound (TONE3000 names the gear)                                                 | Amp badge in the amp's own finish, the switched-on chain (`Tide → Ripple → Lagoon 1x12 → Fathom`), a one-line note, tags, category and author. You can read a rig without loading it.  | `PresetList.tsx`, `PresetInfo`          |
| A/B compare in the preset bar                                                                          | Comparing two settings by ear is the most common reason to keep two versions of a sound.                                                                                               | `PresetBar.tsx`, plugin `compareSwitch` |
| Save as with a category, tags and a note                                                               | Your presets should be as findable as the factory ones.                                                                                                                                | `PresetForm.tsx`                        |

Not adopted: ratings, download counts and comments (there is no community behind a local
library), audio previews (the real engine plays the real preset at once, which is better), cards
in a grid (rows scan faster at 1200 × 760), and nested folders beyond one level (the plugin
stores one folder per user preset).

## Interaction spec

### Opening and closing

- Open: click the preset name or press `P`, press `/` or ⌘/Ctrl+F (focus goes to search), or
  ⌘/Ctrl+S on a factory preset (opens straight into Save as). On open the plugin takes a
  **browse snapshot**.
- The panel covers the section tabs and the rig (they are `inert` while it is open):
  - left, the rail: the title, Library and Categories with counts;
  - centre, search (its placeholder teaches "↑↓ audition · ↵ keep") and sort, tag chips with
    "Clear filters", the count, and the list; the current preset starts in the middle,
    highlighted with a nacre edge and a lit "Playing" lamp ("· edited" in amber when dirty);
  - right, the detail pane for the highlighted preset: "Category · Author", the name in Bodoni
    italic, Playing and Factory/User, the note, the chain as a definition list (Pedals, Amp, Cab,
    FX), tags (a click filters by that tag), the favourite toggle, and at the bottom **Use &
    close** (or **Load preset** when it is not the one playing), Save, Save as…, and for your
    own presets Rename and Delete;
  - the footer: a hint box, "Click or ↑↓ to audition — the browser stays open. Esc returns to
    Nacre", where the Esc part is itself the Back button; New; the preset folder (plugin only);
    and **Shortcuts**, which lists every key.
- **Keep** (close and keep what plays): Enter, double-click a row, **Use & close**, `P` or a click
  on the preset name, or opening the tuner or intonation workbench.
- **Back** (close and restore the browse snapshot): Esc or the Esc button in the hint. If nothing
  was auditioned, it just closes. A toast confirms "Back to Nacre". With the shortcut list open,
  Esc closes the list first.
- If the sound you opened on had unsaved edits and you keep something else, a toast says
  "Loaded Riptide" with **Undo**, which restores the browse snapshot, edits included.

### Auditioning

| Input               | In the search field                                                                 | In the list                           |
| ------------------- | ----------------------------------------------------------------------------------- | ------------------------------------- |
| Click a row         | plays it                                                                            | plays it                              |
| Double-click a row  | plays and keeps it                                                                  | plays and keeps it                    |
| ↑ / ↓               | plays the previous / next visible preset                                            | same                                  |
| Page Up / Page Down | first / last                                                                        | first / last                          |
| Home / End          | caret                                                                               | first / last                          |
| Enter               | keeps the highlighted preset, or the first match if the playing one is filtered out | keeps                                 |
| F                   | types                                                                               | toggles the favourite                 |
| Esc                 | back and close (a form open on top: back to the list)                               | same                                  |
| Digits 1–5          | type                                                                                | do nothing (the sections are covered) |

The search field is a combobox that points at the list (`aria-activedescendant`), so the arrows
work while typing. Every word of the query must match the name, category, tags, note, author,
amp, cab or a block name ("abyss drift" finds high-gain presets with echo).

Prev/next in the preset bar while the browser is open are auditions too (Esc still goes back).
With the browser closed, prev/next on an edited sound keep an **undo snapshot** and show
"Loaded Tidewater · Undo".

### Filtering and sorting

- Library: All presets, Favourites, Recent, Factory, User, each with a count.
- Categories: the folders of the presets in the chosen library, with counts. Changing library
  keeps the category when it still exists there.
- Tag chips: the tags of what library, category and search leave, most common first; the first
  twelve show, the rest behind "n more". Several chips narrow to presets carrying all of them.
- Sort: Name, Recent (picked most recently first) or Category. Choosing the Recent library
  switches the sort to Recent.
- **Recent** holds presets you _picked_: kept from the browser, loaded directly or saved. Auditions
  and prev/next steps don't count. It lives next to the favourites in `settings.json` (12 at
  most), so it spans sessions and plugin instances.

### The preset bar

Previous and next, then the preset: a category chip and Factory/User (or "Unsaved") above the
name and its amber dirty dot. Next to it, one inset group holds the A/B slots, the copy button
and **Save** (the floppy icon turns amber while there are unsaved edits; for a factory preset it
opens Save as).

### A/B compare

Two slots next to the preset bar. The active slot is what you hear; the plugin holds the other.

- The first switch to B copies A, so B starts identical; after that each slot keeps its own
  preset, edits and dirty state. An amber dot marks a slot with unsaved edits, and each slot's
  tooltip names its preset.
- The copy button copies the active slot over the other one ("Copy A to B" / "Copy B to A").
- Loading and saving act on the active slot. Switching while the browser is open keeps what the
  browser auditioned first, so Esc never crosses slots.
- A host restoring a session (or a project reload) resets A/B; the slots don't travel with the
  session.

### Saving

- ⌘/Ctrl+S on a user preset saves it in place, keeping its tags and note ("Saved Sunday set").
  On a factory preset or Init it opens Save as. ⌘/Ctrl+Shift+S always opens Save as.
- **Save as** opens as a sheet over the list: name; category (an existing folder, none, or "New
  category…"); tags with suggestions from the tags already in use (Enter or comma adds, Backspace
  removes the last); and an optional one-line note (140 characters), stored as `description`.
  It starts from the playing preset's name, category, tags and note ("… copy" for a user preset).
- Clashes show inline before anything is written: a user preset with that name in that category
  ("“Sunday set” is already in User / Live. Saving replaces it.", and the button becomes
  "Replace “Sunday set”"); a factory preset with the same name (a note only, as yours lives under
  User); a new category without a name disables saving.
- After a save inside the browser, the saved preset becomes the state Esc returns to.
- **Delete** (user presets) happens at once with an Undo toast; the plugin keeps the file's text
  and writes it back on Undo, so the id, favourite and Recent entry survive.
- **Rename** (user presets) refuses a name another preset in the folder already has, inline.

### What the plugin does

`PresetManager` (message thread) owns the library and three kinds of state for the editor:

- `PresetInfo` per preset, computed when the library is scanned: `amp`, `cab` (the amp's own while
  the amp/cab link is on), `blocks` (switched-on blocks in signal order; the EQ only when the
  amp's bank is on and not flat) and `description`, all with defaults applied.
- **Snapshots** (`browse`, `undo`) and the **A/B** slot: every per-preset parameter, the current
  preset and the values "dirty" compares against. Restoring is exact: an unchanged parameter is
  left alone, a changed one gets the normalised value (searched a few ULPs around the stored one)
  that lands on the stored float, so a session saved after "audition, then Esc" is byte for byte
  the session saved before.
- A host `setStateInformation` (any thread) only bumps an atomic generation after replacing the
  values; the message thread drops snapshots and A/B taken before it on its next preset call, so
  nothing from an old session is ever restored over a new one.

The browser mock (`ui/src/bridge/mockBackend.ts`) implements the same rules, and the host smoke
test drives the real bridge in a real WebView: audition, restore and compare must give back
every value exactly, and the saved state must not change (`plugin/smoke/HostSmoke.cpp`).

### Accessibility

- The browser is a labelled dialog; the rail is two radio groups; the detail pane is a labelled
  complementary region whose chain is a definition list; tag chips are toggle buttons;
  the list is a listbox whose options carry the whole row as their name ("Nacre, playing. Tide →
  Ripple → Lagoon 1x12 → Fathom. Glassy, lightly squeezed clean… Factory Clean. tags chorus,
  compressor, plate"). Rows hold no focusable controls; the star is a mouse shortcut for F and
  for the detail pane's favourite button.
- A/B is a radio group (arrow keys switch) with the slot's preset and edit state in its name.
- Toasts are a polite live region, never take focus, stay 8 s (4 s without an action) and pause
  while hovered or focused.
- Focus moves into forms and back to the search field, and to the preset bar when the browser
  closes. `prefers-reduced-motion` turns the panel, sheet and toast animations off.

## Known gaps

- MIDI or host preset switching (the automatable previous/next switches) does not take an undo
  snapshot; edits made before such a switch are not recoverable.
- A/B slots and snapshots live in memory: they survive closing the editor, not a project reload.
- Recent is per machine (it lives with the favourites), not per project.
