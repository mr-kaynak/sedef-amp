# Sedef Amp — Architecture

Status: living document. Owners: core maintainers. Decisions that shaped this
document are recorded as ADRs in [`docs/adr/`](adr/).

## 1. What we are building

Sedef Amp is an open-source guitar amp and effects suite that runs as a
VST3 / AU / CLAP plugin and as a standalone application. It ships a complete
rig in one window:

```
Input ─► Tuner tap ─► Transpose ─► Input Gain ─► Gate ─► Tide (comp)
      ─► Pearl (overdrive) ─► Coral (distortion) ─► Ripple (modulation)
      ─► Amp (Lagoon | Reef | Abyss) ─► Cab (2 mics + room)
      ─► 9-band EQ (one bank per amp) ─► Doubler ─► Drift (tape echo)
      ─► Fathom (plate | hall reverb) ─► Metronome ─► Output Gain ─► Output
```

On top of the rig it ships musician tools: a precision chromatic tuner and a
guided **intonation workbench** that tells a player which way, and roughly how
far, to move each saddle.

### Goals

- Sound and feel of a modern boutique amp sim: three amp voicings from glassy
  clean to saturated lead, studio-style cab mic'ing, lush time effects.
- Hard real-time safety. No allocation, locks, logging or I/O on the audio
  thread. Ever.
- One parameter schema used by DSP, host automation, presets, and UI.
- A UI that people enjoy: fast, resizable, keyboard accessible, beautiful.
- Engine code that a contributor can understand, test, and extend in isolation.

### Non-goals (v1)

- AAX. It requires a signed Avid SDK agreement that is incompatible with an
  AGPL distribution.
- Neural captures of commercial products. Models are built from public circuit
  literature and, later, from captures of hardware we own (see §9).
- Mobile (AUv3) builds. The architecture does not preclude them.

### Anti-goals

- Any clicks, zipper noise, or denormal CPU spikes while automating.
- A UI that blocks or stutters the audio thread.
- Code or data derived from decompiling another product (see ADR-0006).

## 2. Repository layout

```
engine/            Pure C++20 DSP library. No JUCE. Builds and tests on its own.
  include/sedef/
    core/          Primitives: smoothing, filters, delay lines, oversampling…
    params/        Parameter schema (single source of truth)
    dsp/           Effect and amp modules
    analysis/      Pitch detection, tuner, intonation measurement
    Engine.h       The rig: graph, routing, bypass, telemetry
  src/             Implementations
  tests/           Catch2 unit + property tests
plugin/            JUCE 9 shell: AudioProcessor, WebView editor, presets, bridge
ui/                React + TypeScript web UI, loaded inside the plugin WebView
tools/             Developer tools (parameter dump, render CLI)
presets/factory/   Factory presets (JSON)
docs/              Architecture, ADRs, DSP notes
```

The dependency direction is strictly `ui ─(bridge)─► plugin ─► engine`. The
engine never includes JUCE headers; the UI never assumes it runs inside a
plugin (it ships a mock backend for browser development).

## 3. Threads and data flow

| Thread | Owns | May not |
|---|---|---|
| Audio (host) | `Engine::process` | allocate, lock, wait, log, throw |
| Message (JUCE) | parameters, presets, WebView bridge, file dialogs | block on audio |
| Analysis (ours) | pitch detection for tuner and intonation | touch engine state |
| Loader (ours) | IR file decoding, resampling, FFT partitioning | touch engine state |
| Updates (ours) | update check and installer download ([updates.md](updates.md)) | touch engine or plugin state |

Communication:

- **Parameters → audio:** JUCE `AudioProcessorValueTreeState` raw atomics. At
  the start of each block the processor copies them into a
  `sedef::ParamSnapshot` (plain `std::array<float, kNumParams>`) and hands it
  to `Engine::process`. Modules derive their own settings from the snapshot and
  smooth internally.
- **Audio → analysis:** the engine writes the post-input-gain signal into a
  lock-free SPSC ring (`core/SpscRing.h`). The analysis thread drains it.
- **Audio → UI telemetry:** meters, gain reduction, gate state are atomics in
  `Engine::Telemetry`, sampled by a 30 Hz timer on the message thread.
- **Heavy objects → audio (IRs):** built on the loader thread, published through
  `core/RtHandoff.h` (atomic pointer swap; the old object is reclaimed on the
  message thread, never freed on the audio thread).

## 4. Parameter schema

`engine/include/sedef/params/ParameterList.inc` is an X-macro list and the only
place a parameter is declared. Each entry has: stable string id, display name,
group, kind (`Float | Int | Bool | Choice`), range, default, skew, unit, choice
labels, and whether it is host-automatable.

Consumers:

| Consumer | How |
|---|---|
| Engine | `ParamId` enum indexes the `ParamSnapshot` array |
| Plugin | builds the APVTS parameter layout by iterating the table |
| Presets | serialise by string id; unknown ids ignored, missing ids default |
| UI | requests metadata over the bridge at startup; `tools/sedef-params` dumps the same table to `ui/src/generated/parameters.json` for the browser mock |

Values in the snapshot are always in **real units** (dB, Hz, ms, 0–1 for
unitless knobs, index for choices, 0/1 for bools).

IDs are a public contract: they appear in presets and host sessions. Never
rename or reorder an id; deprecate and add a new one instead.

## 5. Engine contract

Every DSP module follows the same shape so modules can be written, reviewed,
and tested independently:

```cpp
namespace sedef::dsp {
class Module {
public:
    struct Settings { /* real-unit values derived from the snapshot */ };

    void prepare (const ProcessSpec& spec);  // may allocate; not RT
    void reset() noexcept;                   // clears state; RT-safe
    void setSettings (const Settings&) noexcept; // once per block; RT-safe
    void process (AudioView io) noexcept;    // in place; RT-safe
    int  latencySamples() const noexcept;    // default 0
};
}
```

Rules:

1. `prepare` is the only place memory is allocated. Size buffers for the
   largest block and the highest sample rate settings can reach.
2. `setSettings` stores targets. Smoothing happens inside `process`, either per
   sample (`Smoothed<float>`) or per 32-sample sub-block for filter
   coefficients (`core/BlockSmoother.h`).
3. `process` must handle `numFrames == 0`, any block size up to the prepared
   maximum, and 1 or 2 channels.
4. Nonlinear stages run inside `core::Oversampler` (2× or 4×, linear-phase
   half-band FIR). Modules report the resulting latency.
5. Output must be finite for any finite input and any settings. Tests enforce
   this with randomised settings and full-scale noise.
6. Denormals are flushed by `ScopedNoDenormals` at the engine entry; modules
   additionally avoid producing them in feedback paths (`core/Math.h`
   `flushToZero`).

Enable/disable is not the module's concern. `Engine` wraps every module in a
`BypassFader` that crossfades over 10 ms and skips processing when fully
bypassed. Time-based effects (Drift, Fathom) take an `enabled` input of their
own instead, so their tails ring out ("spillover") after being switched off.

## 6. Signal routing and I/O modes

`Engine` builds the chain from two reusable halves:

- `Channel`: Transpose → Input Gain → Gate → Tide → Pearl → Coral → Ripple →
  Amp → Cab. Mono in, mono or stereo out.
- `PostChain`: EQ → Doubler → Drift → Fathom → Metronome → Output Gain. Stereo.

| Host layout | Mode | Graph |
|---|---|---|
| 1 in / 1 out | Mono | `Channel` → `PostChain` (folded to mono) |
| 1 in / 2 out | Mono → Stereo | `Channel` → `PostChain` |
| 2 in / 2 out, `stereoMode = Summed` | Summed | L+R (−6 dB) → `Channel` → `PostChain` |
| 2 in / 2 out, `stereoMode = Dual` | Dual | two `Channel`s (L, R) → `PostChain` |

The gate detector always listens to the raw input before input gain, so gating
follows the guitar's DI level regardless of input gain.

Selecting an amp also selects the matching cab and EQ bank while
`ampCabLink` is on. With the link off, `cabModel` picks the cab independently.
The EQ bank always follows the amp.

## 7. Modules

| Module | Character | Notes |
|---|---|---|
| Gate | Sidechained downward expander with hysteresis and hold | detector on raw input |
| Tide | Fixed-threshold 3:1 compressor, tilt EQ, parallel blend | soft knee, program-dependent release |
| Pearl | Mid-hump overdrive with mid-boost | clipping in the feedback path, 4× OS |
| Coral | Hard-clipping distortion with low-pass "cut" | 4× OS |
| Ripple | Tremolo, chorus, vibrato | 0.69–7.19 Hz |
| Lagoon | Clean amp, glassy and wide | two triode stages, FMV tone stack |
| Reef | Crunch amp, three voicings | cascaded stages, Marshall-style stack |
| Abyss | High-gain lead amp, bright and shift switches | four stages, shifted mid |
| Cab | Procedural speaker + cabinet + mic models, or user IR | two mics + room per cab |
| EQ | 9 bands (65 Hz–16 kHz, ±12 dB) + low/high cut | one bank per amp |
| Drift | Multi-head tape echo, wow/flutter, tape drive | host/tap/free sync |
| Fathom | Plate (Dattorro topology) and hall (FDN) | 1–60 s decay |
| Doubler | Mono → stereo double tracking | second take re-timed per note, 3–20 ms spread; onsets from the DI |
| Transpose | ±12 semitone polyphonic shifter | WSOLA-style splicing |

Each module has a page in [`docs/dsp/`](dsp/) describing its model, the public
references it is based on, and its test coverage.

## 8. Analysis: tuner and intonation workbench

`analysis/PitchDetector` implements the McLeod Pitch Method (normalised square
difference function, key-maximum peak picking, parabolic interpolation) on a
decimated signal. Target accuracy is ±0.1 cent on a steady tone.

`analysis/PitchTracker` adds onset detection, attack skipping (a plucked string
reads sharp for the first ~150 ms), median filtering and a stability window.
It publishes a reading with mean and standard error.

The intonation workbench (UI-driven) walks the player through each string:

1. Tune the open string to pitch (stable within ±1 cent for one second).
2. Fret the 12th fret; capture a stable reading.
3. Report the octave error and the saddle adjustment
   `Δs ≈ L · (2^(c/1200) − 1)` (scale length `L`, error `c` cents, positive =
   move the saddle away from the neck), with direction and millimetres.

The math lives in `ui/src/features/intonation/math.ts` (tested); measurement
lives in C++.

## 9. Sound design approach

All voicings come from public sources: circuit-analysis papers (Yeh & Smith
tone stacks, Dattorro plate), textbook filter design (RBJ cookbook), and
measurements of hardware we own. Each module documents its references.

Roadmap item: optional Neural Amp Modeler (`.nam`) loading for user captures,
running through the same cab/EQ/FX chain.

## 10. Plugin shell

- JUCE 9 `AudioProcessor`, formats VST3, AU, CLAP (clap-juce-extensions),
  Standalone.
- Editor is a `WebBrowserComponent`. Release builds serve the UI from a zip
  embedded with `juce_add_binary_data`; development builds can point at the
  Vite dev server (`SEDEF_UI_DEV_URL`).
- Bridge protocol (`plugin/src/bridge/`, `ui/src/bridge/`), versioned:
  - Native functions: `getSchema`, `setParam`, `beginGesture`, `endGesture`,
    `presets.*`, `ir.load`, `ir.clear`, `tuner.setActive`, `ui.getState`,
    `ui.setState`.
  - Events: `params` (batched changes, 60 Hz max), `telemetry` (30 Hz),
    `pitch` (tuner/intonation readings), `presets` (list/current changed).
- State: APVTS XML plus a `<Sedef>` child for non-parameter state (IR paths,
  UI state). Presets are JSON files.
- Updates (`plugin/src/update/`, [updates.md](updates.md)): `update.*` functions
  and an `update` event. One service per process checks GitHub at most once a
  day, only once an editor's page asks; its settings live in a per-user
  `update.json`, never in the plugin state. `SEDEF_UPDATE_CHECK=OFF` compiles
  it out.

## 11. Quality gates

| Gate | Where |
|---|---|
| Engine unit/property tests (Catch2) | CI on macOS, Windows, Linux |
| ASan + UBSan engine test run | CI Linux |
| `pluginval` strictness 8 on VST3 | CI macOS |
| UI typecheck, lint, Vitest | CI |
| `clang-format` check | CI |
| Real-time safety | code review + `Engine` allocation test using a counting allocator |

## 12. Performance budget

At 48 kHz, 64-sample blocks, Apple M1: full rig (Abyss, 2 mics + room, Drift,
Fathom hall, Ripple) under 8% of one core. Mono-only modes must not pay for
stereo processing.
