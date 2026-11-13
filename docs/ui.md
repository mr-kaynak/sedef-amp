# Sedef Amp — Web UI

The editor is a React + TypeScript app in [`ui/`](../ui). Inside the plugin it runs in JUCE's
`WebBrowserComponent` (ADR-0003); in a normal browser it runs against an in-memory mock backend
with the real parameter schema, so every screen — including the tuner and the intonation
workbench — can be developed and demoed without an audio stack.

![Amp page](images/amp.png)

## Running it

```sh
cd ui
pnpm install
pnpm dev          # http://localhost:5173, mock backend
pnpm build        # type-checks, then writes dist/ (relative asset paths, no network URLs)
pnpm test         # Vitest + React Testing Library
pnpm lint         # ESLint (flat config), zero warnings allowed
pnpm typecheck    # tsc, strict
pnpm format       # Prettier
pnpm screenshots  # renders docs/images/*.png from dist/ (needs a build first)
pnpm captures     # trailer stills (3x) + brand assets into ../media/trailer/assets
```

`pnpm screenshots` drives headless Chromium through Playwright and fails on any console error or
warning, so it doubles as a smoke test. First run: `pnpm exec playwright install
chromium-headless-shell`.

### URL flags (mock only)

| Flag                                | Effect                                                                                                                                                                                                                                                                                                                |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `?section=pedals\|amp\|eq\|cab\|fx` | open a section                                                                                                                                                                                                                                                                                                        |
| `?overlay=tuner\|intonation`        | open the tuner or the intonation workbench                                                                                                                                                                                                                                                                            |
| `?preset=soft-focus`                | start from a factory preset (the id in its file, here Vibrato Swell Wash; default `nacre`, Glassy Chorus)           |
| `?set=pearlEnabled:1,pearlGain:0.4` | parameter values applied as part of that preset (no dirty dot)                                                                                                                                                                                                                                                        |
| `?board=gate,pearl,ripple`          | the pedalboard, as part of that preset (empty: no pedals)                                                                                                                                                                                                                                                             |
| `?freeze`                           | no simulation: static meters, pitch only from the demo hook                                                                                                                                                                                                                                                           |
| `?demo`                             | expose `window.__sedefDemo` (`pitch`, `silence`, `intonation`, `set`, `midi`) for scripted frames                                                                                                                                                                                                                             |
| `?grain=0`                          | hide the lacquer grain (much smaller PNGs)                                                                                                                                                                                                                                                                            |
| `?host=plugin\|standalone`          | present as a plugin (no metronome) or as the standalone app instead of the browser preview                                                                                                                                                                                                                            |
| `?errors=3.6,-2.4,1.1`              | 12th-fret errors in cents that the simulated guitar produces, per string                                                                                                                                                                                                                                              |
| `?fresh`                            | ignore UI state saved by earlier browser sessions                                                                                                                                                                                                                                                                     |
| `?input=silent\|quiet\|clipping`     | what the simulated audio interface delivers: nothing, peaks around −30 dBFS, or flat tops at full scale; without it the strums peak inside the input meter's zone ([input-level.md](ui/input-level.md#mock)) |
| `?midi=demo`                        | MIDI bindings, two program assignments and a preset saved with three scenes, for the live screenshots |
| `?update=available`                 | what the update checker finds: `current`, `available`, `skipped`, `error`, `noinstaller`, `mismatch`, `unsupported`; how the install goes: `declined` (password dialog cancelled), `installfail`, `elsewhere` (Install manually only), `installed` (by another window), `standalone`, `windows`, `scheduled`, `linux` |

### Inside the plugin

Release builds embed `ui/dist` in the binary and serve it by relative path. Development builds
can point the WebView at the Vite dev server with `SEDEF_UI_DEV_URL` (architecture §10), which
gives hot reload inside the host.

## Structure

```
ui/src/
  main.tsx              bootstrap: pick backend, handshake, render
  app/                  shell: Stage (scaling), App, SectionTabs, StatusBar, shortcuts,
                        connect.ts (wiring), persistence.ts (UI state), demo.ts (URL flags)
  bridge/
    protocol.ts         THE contract with plugin/src/bridge (versioned)
    backend.ts          Backend interface
    juceBackend.ts      WebView bridge implementation
    mockBackend.ts      in-memory implementation + mock/ (presets, telemetry, pitch simulation)
  params/               schema, normalisation (JUCE skew), formatting, store, sender, useParam
  state/                app/session, presets, UI, live channels (telemetry, pitch), pitch lease,
                        liveStore (scenes, MIDI bindings, program changes, learn)
  controls/             Knob, VSlider, Footswitch, Toggle, Segmented, ChoiceLever, ChoiceSelect,
                        Led, Popover, Icon
  features/
    header/             brand, preset bar with A/B compare, gain + level meters, the input
                        level verdict and its panel (inputLevel.ts, InputLevelChip)
    presets/            preset browser: library model (order, groups, search), the list, the strip
                        under it, Save as/Rename, session (audition, Esc, Undo); see ui/presets.md
    pedals/             the pedalboard: registry.ts (the engine's pedal table, generated), catalog.ts
                        (each pedal's panel and picker line), sizes.ts (each enclosure's box),
                        art.tsx (the figure painted on it), Pedal + PedalKnob + PedalSwitch, the
                        panels (panels/, mods/, *Pedal.tsx), layout.ts (where everything sits),
                        PedalBoard, PedalPicker (the Add pedal shelf), useBoardDrag; the board
                        lives in state/boardStore.ts
    amp/ eq/ cab/ fx/
    tuner/              needle + strobe gauge, overlay
    intonation/         math.ts, store, controller, wizard steps, saddle diagram
    metronome/ settings/
    updates/            status bar pill, Updates panel, release-notes Markdown (docs/updates.md)
    live/               playing live (docs/live.md): the scene bar and MIDI button (LiveBar), the
                        MIDI panel, the control menu (ControlMenu, menu.ts), the learn banner,
                        model.ts (the arithmetic the mock shares with the engine)
  lib/                  music (notes, cents), note values, metronome steps
  styles/               tokens.css, global.css, bundled fonts
```

## The bridge

All values cross the bridge in real units (dB, Hz, ms, 0–1 knobs, choice index, 0/1 switches),
as `protocol.ts` specifies. The UI depends only on this interface:

```ts
interface Backend {
  kind: "juce" | "mock";
  hello(): Promise<HelloResponse>;
  call(name, ...args): Promise<result>; // typed against NativeFunctions
  on(event, fn): () => void; // typed against Events
  simulator?: PitchSimulator; // mock only, see below
}
```

`connect()` chooses `JuceBackend` when `window.__JUCE__.backend` exists (JUCE injects it before
the page loads) and `MockBackend` otherwise. `@juce-framework/webview` (1.0.0, matching JUCE
9.0.4's backend) is imported lazily: at import time it installs a placeholder `window.__JUCE__`
and warns when the global is missing, which must never happen in a browser.

Startup is one round trip: `sedef_hello(PROTOCOL_VERSION)` returns the schema, values, presets,
IR slots, the capture, saved UI state and session facts. A protocol mismatch shows a banner but
keeps going. After that the UI listens to `sedef:params`, `sedef:telemetry`, `sedef:pitch`,
`sedef:presets`, `sedef:ir`, `sedef:capture` and `sedef:board`, and to `sedef:session` for the
session facts that change after startup (sample rate, latency, layout: the latency moves with
Settings > Latency).

### Parameter flow

- `useParam(id)` returns value (real units), spec, normalised value, display text, `set`,
  `setNormalised`, `begin`/`end` (host gestures) and `reset` (double-click).
- Normalisation mirrors JUCE `NormalisableRange`: `norm = ((v − min) / (max − min))^skew`.
- Changes are optimistic: the store updates at once, and `sedef_setParam` calls are coalesced
  per task. `begin`/`end` flush pending values first, so hosts record whole gestures.
- Gestures are reference-counted per parameter (`params/sender.ts`): a wheel burst, a drag and a
  key press on one control can overlap and the host still sees one begin and one end. A drag or
  wheel burst ends the parameter it began on, also when the control unmounts mid-drag, is
  rebound to another parameter or loses pointer capture.
- While the user owns a parameter (mid-gesture or changed in the last 250 ms), host values for
  it are held back, so a control never jumps under the pointer. The plugin sends each change
  only once, so a held value is applied when ownership ends (a preset loaded right after a wheel
  turn still lands), unless the user changed the parameter after it arrived. A value that
  arrives while our own call for that parameter is still unanswered was produced before the host
  applied ours and is dropped.
- The wheel moves by the distance scrolled: 2 % of the range (one step for stepped parameters)
  per 100 px notch, line and page delta modes converted, small trackpad deltas accumulated, and
  the first event of a burst always moves a stepped control. Faders count drag distance in their
  own layout pixels, so the cap follows the pointer at any window scale.
- Components subscribe to one value each (Zustand selectors). An automation burst re-renders only
  the controls whose values changed.

### High-rate data

Telemetry (~30 Hz) and pitch (~60 Hz) never go through React state. They land in `LiveChannel`s;
meters, the tuner needle, the strobe and the compressor's reduction bar draw from them in
`requestAnimationFrame` through refs (`useLiveFrame`). Text that changes rarely — a note name, a
metronome step, the effective Drift time — uses `useLiveSelector`, which re-renders only when the
selected value changes.

Pitch analysis costs CPU in the plugin, so the tuner and the workbench each hold a lease
(`state/pitchLease.ts`); `sedef_tuner_setActive(false)` goes out when the last one is released.

### UI state

Section, window scale, the metronome pattern and the intonation profile with its results are
saved through `sedef_ui_setState` (debounced), so they travel with host sessions. Hydration
validates everything, because state may come from an older build.

## The mock backend

`MockBackend` holds values for all 204 parameters (from `src/generated/parameters.json`), the real
factory presets (`presets/factory/*.sedefpreset`, bundled at build time with `import.meta.glob`)
plus one user preset, IR slots, UI state (in `localStorage` when running in a browser), tap tempo,
and simulators. Preset handling mirrors `plugin/src/PresetManager.cpp`: ids are
`factory:<slug>` with the same slugify (of the file's `id`, else its name), global parameters (gains, transpose, stereo mode, tempo,
tuner, metronome, preset stepping) are never stored, loaded or counted as changes, missing values
fall back to the schema default, and "dirty" compares against the loaded preset with the same
tolerance. The mock starts on Glassy Chorus (`factory:nacre`).

- **Telemetry**: a player strumming every couple of seconds, shaped by input/output gain, the gate
  threshold and the compressor; metronome steps at the current tempo and signature; the effective
  Drift time for Free/Host/Tap sync.
- **Presets**: the plugin's snapshots (`browse`, `undo`), A/B slots, Recent, undo of a delete,
  save details and the per-preset `amp`/`cab`/`blocks` summary follow the same rules
  (`mock/describePreset.ts` mirrors `PresetManager::describe`).
- **Pitch**: a virtual player modelled on what the C++ `PitchTracker` reports
  (`docs/dsp/tuner.md`), at the editor's 60 Hz — plucks with an attack glide, a jittery reading,
  and settled detections at 200 per second from 0.25 s after the pluck whose last 0.4 s give
  `stable` (window full, spread under 0.5 cent), `stableHz` (mean), `stableStdCents` (sample
  standard deviation) and `stableCount`. The `display*` fields follow the engine's tuner display
  in outline (a new note shows 50 ms after its first reading, a re-pluck keeps the needle still,
  the reading is followed while the peg turns, the last value holds, settled if it was). With no
  intent it tunes the strings of a guitar one after another. The intonation workbench sets
  intents through the optional `simulator` hook (`tune` an open string, `fret` the 12th fret with
  a configurable error per string), so the whole flow is demoable in a browser.

- **Live** (`mock/liveSim.ts`): scenes, bindings, program changes and learn with the plug-in's
  rules (`LiveControl`, `LiveState`): scenes made on first visit, the dirty rules, presets and
  snapshots carrying scenes. `midi(status, data1, data2, time)` plays a controller into it and
  `live.advance(seconds)` moves its audio clock, so the learn window is testable.

Tests construct the mock with `autoStart: false` and drive time with `tickTelemetry()` /
`tickPitch()`, so they are deterministic.

## Trailer captures and brand assets

`pnpm build && pnpm captures` renders still frames of real UI states at 3x (3600 × 2280) into
`media/trailer/assets/shots/` (or `--out <dir>`), using `?freeze&demo&grain=0&host=standalone`
so frames in a sequence differ only in what the sequence animates. It also writes, from the same
sources the UI uses, `logo.svg` (the star mark), `wordmark.svg` and `lockup.svg` (outlined with
fontkit from the exact variable-font instances), and `fonts/` (the bundled woff2 files, their OFL
licences and a `fonts.css`). `node scripts/brand-assets.mjs [dir]` regenerates only the assets.

## Screens

Design canvas: 1200 × 760 CSS px, scaled uniformly to the window (`app/Stage.tsx`). The plugin
resizes its window to the canvas times the chosen scale (75–200 %, `sedef_ui_setScale`); in a
browser the canvas fits the window. Everything is vector or text, so it stays crisp at any scale.

| Screen     | Notes                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Header     | logo, preset bar (prev/next, category chip, name with dirty dot, A/B with copy, Save), input/output gain with peak meters and clip latches; the input meter marks the level the presets want and has the input level chip under it (No signal, Check level, Too quiet, Level good, Too hot), which opens the Input level panel ([input-level.md](ui/input-level.md)); the Doubler switch under the output meter; Tuner (T), Intonation, Metronome (standalone only), Settings                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| Presets    | browser over the rig ([details](ui/presets.md)): search, the library switch, one list of one-line rows in two columns under category headers, a strip with the note of the row under the pointer; click or arrows audition, Enter keeps, Esc goes back exactly; `[` `]` `P` `A` `B` |
| Tabs       | Pedals · Amp · EQ · Cab · FX, keys 1–5 and arrow keys; each tab shows what is switched on inside it. Then Transpose: a −/+ stepper ("−2 st"), and while it shifts the latency it adds and Back to 0. At the right end the scene bar (scenes 1–8, a radio group; made scenes are brighter and dotted; folded into one "Scene 3" button while Transpose shifts) and the MIDI button with its activity light ([live.md](live.md)) |
| Pedals     | the pedalboard ([below](#pedalboard)): the signal runs along a cable from the Guitar jack through the pedals, row by row, into the amp in use; every pedal is its own enclosure (three widths, its own knob layout and painted figure), drawn as large as the board's length allows, and all sixteen fit without scrolling; drag a pedal, or use the arrow keys or move buttons on it, to reorder; Remove with Undo; Add pedal opens a shelf with a picture of every pedal by category. Gate (threshold, open/closed lamp); Trench Mono/Poly, Remora Germanium/Silicon, Breaker Treble/Full, Conch Manual/Auto, Anemone mode and direction, Gyre stages and Beacon Chorus/Vibrato switches; Tide gain reduction, Ripple rate lamp, Beacon lamp and Gyre/Jetty sweep LEDs; playing hints on Trench (it follows the mode: "One note at a time" in Mono, "Chords and single notes" in Poly) and Urchin, read with their footswitch |
| Amp        | Lagoon/Reef/Abyss faceplates; Reef voice lever; Abyss bright/shift levers; amp/cab link; Capture: load or drop a .nam file, gear/author/model, trims, through-cab, interface level, recent, missing file                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| EQ         | the current amp's bank: 9 faders on a log axis under a live response curve, low/high cut, on/off, flatten                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| Cab        | cab picker; Mic A/B markers anywhere over the cone (drag or click to place, wheel or Alt-drag for distance, keys), off-axis stem, other speakers' map; type, position, distance, off-axis, level, pan, phase, room, custom IR                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| FX         | Drift (heads, Free/Host/Tap sync with tap button, time or note value, effective time, tape controls), Fathom (plate/hall, envelope sketch), Doubler (sketch of the two takes, spread, variation, width, mix)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| Tuner      | needle + strobe, note and octave, cents, Hz, "Tune up" hint, reference 420–460 Hz, mute; in tune within ±1 cent after 0.15 s, with hysteresis; holds the last reading ([tuner.md](dsp/tuner.md#3-display)); "Nothing is coming in" with Check input while the input level says No signal                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| Intonation | instrument → measure each string (tune open, capture 12th fret, result with saddle diagram) → summary with copy as text                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| Metronome  | play/stop, tempo knob + tap, a Click / Drums switch; Click: signature, sound, volume, step grid (off/normal/accent/soft, with a line saying what a click does), live beat; Drums: signature with the bar's beats lit, the grooves of the meter (name and feel, one tab stop), Intensity 1–5, Fill every, Level, Tone ([drums.md](drums.md)) |
| Settings   | Audio settings… (standalone app only: the app's audio device window); window size; Stereo input (sum to mono or two channels); Latency: Low latency or Linear phase, a global setting kept out of presets ([oversampling.md](dsp/oversampling.md)); updates; About with the session's rate and latency, which follow the Latency setting                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| Updates    | status bar pill when a newer release is out ("Update available"), while it updates ("Updating…") or once installed ("Restart to update"); panel with versions, last check, Check now, the automatic-check switch, release notes, Update now (download with progress and Cancel, then the OS installer's steps), Restart Sedef in the standalone app, Install manually and the releases page after a failure, Skip this version, Later ([updates.md](updates.md))                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| MIDI       | panel under the MIDI button: last message received, the format's limits (VST3: no program changes; AU: what the host sends), the standalone app's inputs; tabs Controls (learned controls with Toggle/Latch or range and Invert, Forget, Forget all with a confirmation, learn for scene and preset steps), Programs (channel, assignments to a preset or a scene, library order otherwise), Scenes (the knobs scenes keep, copy the scene in use) |
| Control menu | right-click, the context-menu key or Shift+F10 on a knob, switch, lever, scene button or preset arrow (anything with `data-param` or `data-learn`): Learn MIDI, Forget, Remember in scenes / Same in every scene, Copy scene N here; arrows, Enter, Escape. Learning shows a banner over the rig and outlines the control; Escape cancels |

### Pedalboard

![The pedalboard with the Add pedal shelf open](images/pedal-picker.png)

The board is the list of pedal ids in signal order (`state/boardStore.ts`), mirrored from the
plug-in: `hello.board`, `sedef_board_set(pedals)` (answers with the board it kept) and
`sedef:board` for changes made elsewhere (presets, A/B, preset Undo, host restores). Edits
apply at once; only the answer to the latest edit is taken, and a board older than the one held
(by `revision`) is ignored. A plug-in without the board leaves `supported` false: the page then
shows 0.3.0's pedals in their fixed order without the editing tools.

- **Pedals**: one family, sixteen enclosures. `sizes.ts` gives each its box in board units:
  compact (one column of controls: Gate, Breaker), regular, or wide (Trench, Remora, Anemone),
  and the height its controls need, so pedals stand at different heights on one bottom line.
  Each panel lays out its own rows (`PedalRow`), sizes its knobs by weight (`PedalKnob`: normal,
  big, or one hero knob) and carries a line figure after its name (`art.tsx`: rings for Ripple,
  a spiral for Conch, spines for Urchin). The figures are computed in code; nothing is traced
  or imported ([ADR-0006](adr/0006-clean-room-sources.md)). Lettering has a halo of the paint,
  so a figure's lines stop short of it. What stays the same on every pedal: the controls on
  top, the name and kind under them, then switches and hints, the lamp and the footswitch.
- **Scale**: a pedal draws at the scale its host gives it (`density.ts`), from 1 to 1.4 canvas
  pixels per board unit; the enclosure, its knobs, its name and its footswitch grow together,
  and the legends (knob labels, values, kinds, hints, switch labels) are 11 px at every scale,
  12 px for knob labels on a large pedal. A board that fits one row is drawn as large as the
  row allows (1.4 up to about five pedals, less for six to eight) and centred; a board that
  needs two rows is at 1. While the Add pedal shelf is open the board is at most 1.2, so all
  of it stays in view above the shelf.
- **Layout** (`layout.ts`): arithmetic over the boxes on the fixed 1200 px canvas, no
  measuring. Rows fill from the left and are evened out, so a ninth pedal does not sit alone
  under a row of eight. The widths are chosen so that all sixteen
  fit two rows, in any order, in the 588 px under the toolbar (`layout.test.ts` tries a couple
  of hundred orders); the page scrolls only if a later pedal breaks that.
- **Signal path**: a cable runs from the Guitar jack along each row at the height of the
  pedals' jacks, with an arrowhead in every gap, out past the row's end, back along the gap
  between rows and into the next one. It ends in a small head in the colours and name of the
  amp in use (Capture when a capture plays), which is a button to the Amp page. An empty board
  is the jack, a short cable and the amp.
- **Tools**: each pedal has its tools on its own top edge, on an ink strip that reads on any
  paint: the handle, then Move left, Move right and Remove. They show while the pointer or the
  keyboard focus is on the pedal, and always where there is no hover.
- **Drag**: press the handle, or the enclosure anywhere but on a control, and move 4 px. The
  pedal follows the pointer (canvas pixels, so the Stage's scale doesn't matter), the others
  stay put, and a nacre bar down the row marks the gap it would land in. When it lands, the
  pedals around it glide to their places. Near the scroller's top or bottom edge a scrolling
  board scrolls. Escape or a cancelled pointer puts it back.
- **Keyboard**: on the handle, arrow keys move the pedal one place, Home and End to either end;
  the move buttons do the same. Focus stays on the tool used (the handle when that button
  switches off at an end). A polite live region says where it went: "Pearl moved to 4 of 5,
  after Coral."
- **Remove** takes the pedal off at once and shows "Removed Pearl · Undo"; Undo puts it back
  where it was. The notice goes when a preset or A/B brings another board. Focus moves to the
  next pedal's handle, or to Add pedal when the board is empty.
- **Add pedal** opens a shelf under the board (a non-modal dialog), not a popover over it: a
  card for every pedal, by category, with the pedal itself drawn at 40 % of the smallest scale
  (the real panel,
  `inert` and hidden from assistive technology, so the picture always matches). Pointing at a
  card, with the pointer or the keyboard focus, shows a bar on the board where the pedal would
  land (its rank in the registry) and a line with what it does and where it goes; a pedal on
  the board already is marked "On the board", ringed on the board, and picking it shows it
  there. Adding announces the pedal's place and leaves the shelf open with focus on the card,
  so several pedals can go on in one visit. Arrow keys, Home and End walk the cards; Close,
  Escape or Add pedal closes the shelf, and focus goes back to Add pedal.
- **Registry**: `features/pedals/registry.ts` reads `generated/pedals.json`, which
  `sedef-params --pedals` dumps from the engine's `PedalList.inc`, and mirrors `sedef::Board`'s
  rules (add at rank, move, the legacy board for presets saved without one, only the pedals
  switched on for Init). Adding a pedal: [architecture.md](architecture.md#adding-a-pedal).
- **Mock**: the mock backend keeps a board per preset, snapshot and A/B side, counts it towards
  the dirty dot like the plug-in, and takes `?board=`. Factory presets bring the boards in their
  files; Init is the Gate alone, and the mock's user preset, which has no board, gets the legacy
  board.

### Input level

The input meter and the chip under it answer the first question after plugging in: is the
guitar coming in, at the level the presets were voiced for? `features/header/inputLevel.ts`
judges the telemetry the engine already sends (no engine change): the loudest peak of the last
8 s after the Input knob against the zone −12 to 0 dBFS, the last 3 s for clipping, 3 s of
digital silence for No signal. The panel says what to turn. The spec, with the reason for each
number: [ui/input-level.md](ui/input-level.md).

### EQ curve

`features/eq/response.ts` evaluates the same filters as `sedef::dsp::GraphicEq`: nine RBJ peaking
bands at 65 Hz–16 kHz with Q 1.41, 2nd-order Butterworth high-pass and low-pass (Q 1/√2) that are
bypassed at ≤ 20 Hz and ≥ 20 kHz, at 48 kHz, with the same frequency clamp as `Biquad.cpp`. Keep
the two in sync; the tests pin the known values (band gain at its centre, −3.01 dB at a cut's
corner, 12 dB/oct slopes).

### Intonation workbench

1. **Instrument**: 6-string standard or drop D, 7-string, 4- and 5-string bass, or a custom
   tuning (4–8 strings, any note B0–E5); scale length from 24.75 in to 35 in, or custom mm.
2. **Measure**, per string: tune the open string until it stays within ±1 cent for a second (the
   workbench advances by itself); fret the 12th fret and hold while the progress ring fills. The
   ring needs `PitchEvent.stable` without a break for 0.5 s from one pluck (same `onsetId`, and
   `stableCount` not falling back, which marks a restarted segment); the result is `stableHz`
   averaged over that half second. "Steady to ±x cents" is the larger of the tracker's detection
   scatter (`stableStdCents`) and the reading's movement over the hold. Holds are
   timed with the clock (`performance.now()`), not by counting events, so they do not depend on
   the event rate. Read the result.
3. **Summary** for all strings, re-measure or measure any of them, copy as text. A string picked
   from the summary has a "Back to summary" button, and its Next goes to the next string without
   a result, or back to the summary when every string has one.

The math (`features/intonation/math.ts`): octave error `c = 1200·log2(f12 / 2f0)` using the
measured open frequency, verdict in tune when |c| ≤ 1.5 cents, otherwise move the saddle back
(sharp) or forward (flat) by about `Δs = L·(2^(c/1200) − 1)`. Example: +4 cents on a 648 mm scale
is about +1.50 mm.

## Design

Sedef is Turkish for mother-of-pearl. The UI is deep-sea lacquer with nacre used only where
something is alive.

- **Palette** (`styles/tokens.css`): blue-green lacquer inks (`--ink-0` #061119 to `--ink-4`
  #22465c) that read as colour rather than tinted black; warm bone type (`--bone` #ede6d6, with
  three quieter steps); four pale interference colours for nacre (rose #f6dbe2, aqua #cdeeee,
  lilac #ddd6f6, gold #f2e8cb); amber for "needs attention" and coral for clipping.
- **Type**: Bodoni Moda italic (optical sizes) names every module, like a maker's script on a
  faceplate; Archivo carries the interface, with its width axis widened for engraved panel
  legends. Sentence case throughout. Both are bundled (`@fontsource-variable`), no network.
- **Signature**: the knob caps are nacre shells. The conic sheen turns with the knob while the
  specular highlight stays put, so light appears to travel across the shell as you turn it. The
  same nacre inlays the logo (an eight-pointed star, as in sedef inlay work), the in-tune note,
  active selections, the Abyss badge and the Pearl enclosure. Nothing else glows.
- **Hardware language**: pedals are painted enclosures with screws, chrome-bezel LEDs, hex-nut
  footswitches and a line figure each; amps are heads with grille cloth and an engraved plate (Lagoon is pale sea-glass,
  Reef is oxblood, Abyss is ink with a nacre badge).
- **Accessibility**: every control is reachable by keyboard with a visible focus ring; knobs and
  faders are ARIA sliders with real values and readable `aria-valuetext`; arrows, Page Up/Down,
  Home/End, Delete (reset), shift for fine control. Radio groups have one tab stop and arrow,
  Home and End keys (`controls/useRadioGroup.ts`). The preset browser has a full keyboard path
  (`/` or ⌘/Ctrl+F to search, arrows to audition, Enter, Esc, F for favourite, and `P`, `[`,
  `]`, `A`, `B` anywhere outside text fields, as is `T` for the tuner; see
  [ui/presets.md](ui/presets.md)); digit keys never switch sections while you type or while it is
  open, and toasts are a polite live region that never takes focus. The tuner announces a settled state ("E 2, in tune") through
  a polite live region once it has held 0.6 s, at most every 2 s, and says nothing while a note is
  starting or has ended (the next note is announced again, even in the same state); the state is
  also written ("In tune", "Tune up"), not only coloured. Text meets WCAG AA (4.5:1):
  `--bone-3` is the quietest text colour (`--bone-4` is for lines and fills only), and the
  enclosure legends' dim tone is strong enough on every paint. With `prefers-reduced-motion`,
  movement turns into a cross-fade rather than a cut: panels, sheets, popovers and toasts fade in
  without sliding, lever bats and switch thumbs jump, buttons don't scale when pressed, and the
  Ripple lamp stops blinking. Colour and opacity changes still fade. The strobe is hidden, and
  the needle keeps its damping because it carries information. Motion is gated where it is
  defined (`@media (prefers-reduced-motion: no-preference)`), never by zeroing durations
  globally.
- **Interaction states**: hover styles apply only on pointers that can hover
  (`@media (hover: hover)`), so a tap on a touchscreen never leaves a control looking selected,
  and a disabled button never lights up. A pressed button scales to 0.96 over 150 ms
  (`controls/Button.module.css`, `.press` for custom buttons, `.static` to opt out). Rows, rail
  items, cards and segments don't scale. A nacre fill fades in on a layer
  (`controls/NacreFill.module.css`), because a gradient cannot transition. Under forced colors
  (Windows High Contrast) shadows and gradients disappear, so every edge that is only a shadow
  ring also has a transparent border or outline, and selected states use `border-color` with a
  `Highlight` fallback.
- **Icons**: one stroke set on a 16-unit grid (`controls/Icon.tsx`). The `stroke` prop is the
  rendered line in CSS px at any size: 1.5px by default, 2px (`ICON_STROKE_LABEL`) beside the
  540–600 weight button labels. Check a dense glyph at its render size before giving it 2px (the
  keyboard stays at 1.5px). Icons are `currentColor`. An infrequent swap (copy to copied, play to
  stop) cross-fades through `controls/IconSwap.tsx`.

## Known gaps

- The window-size setting only resizes inside the plugin; in a browser the canvas always fits.
- Intonation keeps one instrument profile (with its results), not a library of named guitars.
- The mock has no host transport; Drift's Host sync assumes 120 bpm.
- The EQ screenshot shapes its curve with `?set=`; only a few factory presets move the EQ.
- Screenshots are full-fidelity PNGs (the lacquer grain does not compress well).
- A switched-off FX unit fades its body to 62 % opacity, which takes its quiet text below 4.5:1.
  Its controls still work, so this is a deliberate exception to the contrast rule above.
