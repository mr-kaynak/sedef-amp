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
```

`pnpm screenshots` drives headless Chromium through Playwright and fails on any console error or
warning, so it doubles as a smoke test. First run: `pnpm exec playwright install
chromium-headless-shell`.

### URL flags (mock only)

| Flag | Effect |
| --- | --- |
| `?section=pedals\|amp\|eq\|cab\|fx` | open a section |
| `?overlay=tuner\|intonation` | open the tuner or the intonation workbench |
| `?preset=night-swim` | start from a factory preset (slug) |
| `?host=plugin` | behave like a plugin (no metronome) instead of the standalone app |
| `?errors=3.6,-2.4,1.1` | 12th-fret errors in cents that the simulated guitar produces, per string |
| `?fresh` | ignore UI state saved by earlier browser sessions |

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
  state/                app/session, presets, UI, live channels (telemetry, pitch), pitch lease
  controls/             Knob, VSlider, Footswitch, Toggle, Segmented, ChoiceLever, ChoiceSelect,
                        Led, Popover, Icon
  features/
    header/             brand, preset bar + browser, gain + level meters
    pedals/ amp/ eq/ cab/ fx/
    tuner/              needle + strobe gauge, overlay
    intonation/         math.ts, store, controller, wizard steps, saddle diagram
    metronome/ settings/
  lib/                  music (notes, cents), note values, metronome steps
  styles/               tokens.css, global.css, bundled fonts
```

## The bridge

All values cross the bridge in real units (dB, Hz, ms, 0–1 knobs, choice index, 0/1 switches),
as `protocol.ts` specifies. The UI depends only on this interface:

```ts
interface Backend {
  kind: 'juce' | 'mock';
  hello(): Promise<HelloResponse>;
  call(name, ...args): Promise<result>;   // typed against NativeFunctions
  on(event, fn): () => void;               // typed against Events
  simulator?: PitchSimulator;              // mock only, see below
}
```

`connect()` chooses `JuceBackend` when `window.__JUCE__.backend` exists (JUCE injects it before
the page loads) and `MockBackend` otherwise. `@juce-framework/webview` (1.0.0, matching JUCE
9.0.4's backend) is imported lazily: at import time it installs a placeholder `window.__JUCE__`
and warns when the global is missing, which must never happen in a browser.

Startup is one round trip: `sedef_hello(PROTOCOL_VERSION)` returns the schema, values, presets,
IR slots, saved UI state and session facts. A protocol mismatch shows a banner but keeps going.
After that the UI listens to `sedef:params`, `sedef:telemetry`, `sedef:pitch`, `sedef:presets`
and `sedef:ir`.

### Parameter flow

- `useParam(id)` returns value (real units), spec, normalised value, display text, `set`,
  `setNormalised`, `begin`/`end` (host gestures) and `reset` (double-click).
- Normalisation mirrors JUCE `NormalisableRange`: `norm = ((v − min) / (max − min))^skew`.
- Changes are optimistic: the store updates at once, and `sedef_setParam` calls are coalesced
  per task. `begin`/`end` flush pending values first, so hosts record whole gestures.
- The host echoes our own changes back. Echoes for a parameter that is mid-gesture or changed in
  the last 250 ms are ignored, so a knob never jumps backwards under the pointer.
- Components subscribe to one value each (Zustand selectors). An automation burst re-renders only
  the controls whose values changed.

### High-rate data

Telemetry (~30 Hz) and pitch (~50 Hz) never go through React state. They land in `LiveChannel`s;
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

`MockBackend` holds values for all 137 parameters (from `src/generated/parameters.json`), six
factory presets and one user preset with full save/rename/delete/favourite/step semantics, IR
slots, UI state (in `localStorage` when running in a browser), tap tempo, and simulators:

- **Telemetry**: a player strumming every couple of seconds, shaped by input/output gain, the gate
  threshold and the compressor; metronome steps at the current tempo and signature; the effective
  Drift time for Free/Host/Tap sync.
- **Pitch**: a virtual player modelled on what the C++ `PitchTracker` reports — plucks with a sharp
  attack, settling, jitter, decay, and a stability window with mean and standard error. With no
  intent it tunes the strings of a guitar one after another. The intonation workbench sets
  intents through the optional `simulator` hook (`tune` an open string, `fret` the 12th fret with
  a configurable error per string), so the whole flow is demoable in a browser.

Tests construct the mock with `autoStart: false` and drive time with `tickTelemetry()` /
`tickPitch()`, so they are deterministic.

## Screens

Design canvas: 1200 × 760 CSS px, scaled uniformly to the window (`app/Stage.tsx`). The plugin
resizes its window to the canvas times the chosen scale (75–200 %, `sedef_ui_setScale`); in a
browser the canvas fits the window. Everything is vector or text, so it stays crisp at any scale.

| Screen | Notes |
| --- | --- |
| Header | logo, preset bar (prev/next, dirty dot, browser with search, Factory/User/Favourites, save, save as, rename, delete, new), input/output gain with peak meters and clip latches, Tuner, Intonation, Metronome (standalone only), Settings |
| Tabs | Pedals · Amp · EQ · Cab · FX, keys 1–5 and arrow keys; each tab shows what is switched on inside it |
| Pedals | Gate (threshold, open/closed lamp), Transpose, Tide/Pearl/Coral/Ripple enclosures with LEDs and footswitches, Tide gain reduction, Ripple rate lamp |
| Amp | Lagoon/Reef/Abyss faceplates; Reef 3-way voice lever; Abyss bright/shift levers; amp/cab link |
| EQ | the current amp's bank: 9 faders on a log axis under a live response curve, low/high cut, on/off, flatten |
| Cab | cab picker (shows the link), speaker with draggable Mic A/B markers (sideways = position, up/down = distance ring), per-mic type for the current cab, level, pan, phase, room, custom IR load/clear |
| FX | Drift (heads, Free/Host/Tap sync with tap button, time or note value, the effective time from telemetry, tape controls), Fathom (plate/hall with an envelope sketch), Doubler |
| Tuner | needle + strobe, note and octave, cents, Hz, reference 420–460 Hz, mute |
| Intonation | instrument → measure each string (tune open, capture 12th fret, result with saddle diagram) → summary with copy as text |
| Metronome | play/stop, tempo knob + tap, signature, sound, volume, step grid (off/normal/accent/soft), live beat |

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
   workbench advances by itself); fret the 12th fret and hold while the progress ring fills from
   stable readings (`PitchEvent.stable`, `stableHz`, `stableStdCents`, 40 stable detections, standard
   error ≤ 0.6 cents); read the result.
3. **Summary** for all strings, re-measure any of them, copy as text.

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
- **Hardware language**: pedals are painted enclosures with screws, chrome-bezel LEDs and hex-nut
  footswitches; amps are heads with grille cloth and an engraved plate (Lagoon is pale sea-glass,
  Reef is oxblood, Abyss is ink with a nacre badge).
- **Accessibility**: every control is reachable by keyboard with a visible focus ring; knobs and
  faders are ARIA sliders with real values and readable `aria-valuetext`; arrows, Page Up/Down,
  Home/End, Delete (reset), shift for fine control. `prefers-reduced-motion` disables transitions
  and the strobe; the needle keeps its damping because it carries information.

## Known gaps

- The window-size setting only resizes inside the plugin; in a browser the canvas always fits.
- Intonation keeps one instrument profile (with its results), not a library of named guitars.
- The mock has no host transport; Drift's Host sync assumes 120 bpm.
- Screenshots are full-fidelity PNGs (the lacquer grain does not compress well).
