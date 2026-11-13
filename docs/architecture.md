# Sedef Amp — Architecture

Status: living document. Owners: core maintainers. Decisions that shaped this
document are recorded as ADRs in [`docs/adr/`](adr/).

## 1. What we are building

Sedef Amp is an open-source guitar amp and effects suite that runs as a
VST3 / AU / CLAP plugin and as a standalone application. It ships a complete
rig in one window:

```
Input ─► Tuner tap ─► Input Gain ─► Transpose ─► Pedalboard: the pedals the player puts on it, in the
      order they choose; by default Gate ─► Trench (octave down) ─► Anemone (envelope filter)
      ─► Conch (wah) ─► Remora (fuzz) ─► Tide (comp) ─► Urchin (octave-up fuzz)
      ─► Sargasso (sustain fuzz) ─► Pearl (overdrive) ─► Kelp (mid overdrive) ─► Coral (distortion)
      ─► Beacon (vibe) ─► Gyre (phaser) ─► Jetty (flanger) ─► Ripple (modulation)
      ─► Breaker (treble booster)
      ─► Amp (Lagoon | Reef | Abyss) ─► Cab (2 mics + room)
      ─► 9-band EQ (one bank per amp) ─► Doubler ─► Drift (tape echo)
      ─► Fathom (plate | hall reverb) ─► Metronome (click | drums) ─► Output Gain ─► Output
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
| Loader (ours) | IR file decoding, resampling, FFT partitioning; .nam capture validation, building and level measurement | touch engine state |
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
- **Pedalboard → audio:** the board (an ordered, fixed-capacity list of pedal ids) goes
  through `core/RtMailbox.h`, a wait-free triple buffer in which the newest board wins. The
  engine picks it up at the start of a block ([The pedalboard](#the-pedalboard)).
- **Heavy objects → audio (IRs, captures):** built on the loader thread, published through
  `core/RtHandoff.h` (atomic pointer swap; the old object is reclaimed on the
  message thread, never freed on the audio thread). Captures use the same scheme with room for
  several retired models, so the newest always gets through ([nam.md](dsp/nam.md#5-real-time)).

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
4. Nonlinear stages run inside `core::Oversampler` (2× or 4×; minimum-phase
   IIR half-bands by default, linear-phase FIRs in the Linear phase setting,
   [dsp/oversampling.md](dsp/oversampling.md)). Modules pass `spec.oversampling`
   on and report the resulting latency, a whole number of samples.
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

- `Channel`: Input Gain → Transpose → pedalboard (`PedalRack`) → Amp → Cab. Mono in, mono or
  stereo out. The gate's sidechain is delayed by Transpose's latency, which the engine adds to
  the latency it reports while Transpose shifts ([transpose.md](dsp/transpose.md)).
- `PostChain`: EQ → Doubler → Drift → Fathom → Metronome (a click or the drums,
  [drums.md](drums.md)) → Output Gain. Stereo.

| Host layout | Mode | Graph |
|---|---|---|
| 1 in / 1 out | Mono | `Channel` → `PostChain` (folded to mono) |
| 1 in / 2 out | Mono → Stereo | `Channel` → `PostChain` |
| 2 in / 2 out, `stereoMode = Summed` | Summed | `InputFold` (L+R averaged, or the live side at unity when the other carries nothing) → `Channel` → `PostChain` |
| 2 in / 2 out, `stereoMode = Dual` | Dual | two `Channel`s (L, R) → `PostChain` |

The gate detector always listens to the raw input before input gain, so gating
follows the guitar's DI level regardless of input gain. On the board, that key is delayed by
the latency of the pedals in front of the Gate, so the detector and the audio stay lined up
wherever the Gate sits.

### The pedalboard

The player puts pedals on the board, takes them off and drags them into any order. The board
is state, not a parameter: it isn't automatable and has nothing in `ParameterList.inc`. Every
pedal keeps its own parameters (on/off and knobs) whether or not it is on the board.

**The registry.** `engine/include/sedef/board/PedalList.inc` is an X-macro list and the only
place a pedal is declared:

```cpp
SEDEF_PEDAL(id, name, category, rank, enable, Module, settings)
```

| Field | Meaning |
|---|---|
| `id` | Stable id, saved in sessions and presets. Never renamed. |
| `name` | Display name. |
| `category` | Dynamics, Drive, Fuzz, Filter, Modulation or Pitch. The picker groups by it. |
| `rank` | Default position. Add pedal puts a pedal after the last one on the board ranked at or below it. |
| `enable` | The pedal's on/off parameter. |
| `Module` | Its engine module, shaped like every other ([§5](#5-engine-contract)). |
| `settings` | An expression that builds `Module::Settings` from the snapshot. |

Consumers:

| Consumer | How |
|---|---|
| Engine | `PedalId` enum; `PedalRack` holds one `PedalSlot<Module>` per entry |
| Plugin | board ids in the session and presets (`plugin/src/BoardText.*`) |
| UI | `tools/sedef-params --pedals` dumps the list to `ui/src/generated/pedals.json`; a ctest (`ui-pedals-in-sync`) fails when it is stale. `ui/src/features/pedals/catalog.ts` adds each pedal's panel and its line for the picker. |

**The model.** `sedef::Board` (`board/Board.h`) is the ordered list of pedal ids on the board,
in a fixed-capacity array (`kNumPedals`). It keeps each pedal at most once. `Board::legacy()`
is the fixed chain the board replaced: Gate, Trench, Anemone, Conch, Remora, Tide, Urchin,
Sargasso, Pearl, Kelp, Coral, Beacon, Gyre, Jetty, Ripple, Breaker (0.3.0 had Gate, Tide, Pearl,
Coral and Ripple; the others joined the fixed chain before the board did).

**The Gate is a board pedal**, first by default. Players put a gate in different places (first,
to quiet the guitar; after the drives, to cut the noise they add) or leave it off, and keeping
it on the board costs nothing: its detector listens to the raw instrument wherever it sits, so
it closes on the guitar, not on the hiss in front of it.

**Saving.** The plug-in saves the board as a comma-separated id list (`board="gate,tide,…"`)
in its state's `Extra` element and in each preset. Snapshots (preset Undo, the browser's
audition) and A/B carry it too. A preset whose board differs from the loaded one counts as
edited.

- **Factory presets** each carry a board of only the pedals they switch on, in default order
  (`Board::switchedOn`), so the board shows what the preset uses.
- **A new instance and Init** have no file to carry a board: they open on the pedals the default
  parameters switch on, the Gate alone.
- **A session or user preset saved before the board** has none and gets `Board::legacyFor`: the
  whole legacy board, plus any newer pedal its parameters switch on, at its default position.
  Every pedal whose settings it saved is in view, whether it is on or off.

Both kinds of board play exactly what the fixed chain played. A board in default order with the
other pedals off sounds the same as the legacy board with those pedals switched off, because the
rack keeps the latency of the pedals left off where the fixed chain had it (below). The
`factory-preset-boards` ctest (`sedef-render --check-boards`) renders every factory preset on
its board and on the legacy board, in both oversampling modes, and requires each board to be the
pedals the preset switches on and the two renders to match sample for sample. Against main
before the board, every factory preset and a session with every pedal on render bit-identical at
44.1, 48, 96 and 192 kHz in both modes.

**The engine.** Each `Channel` has a `PedalRack`. It holds every pedal's module, prepared up
front, and processes them in its board's order. Pedals that aren't on the board are not
processed. A new board arrives from the message thread through the mailbox and is picked up at
the start of a block. `BoardSwitch` takes every chain's rack to it without clicks:

- **A pedal without latency joins or leaves** through its own 10 ms bypass fade. A leaving
  pedal is switched off and stays in the order until its fade has ended. A joining one comes in
  bypassed and fades in if it is on.
- **Anything else** (pedals that stay change their order, or a pedal with latency joins or
  leaves) happens under a duck. The board's input fades to silence over 6 ms (raised cosine).
  Its output gain is the input's gain the rack's latency earlier, the gain the signal leaving
  the pedals had when it went in: it reaches silence once the signal has left the pedals, the
  racks rewire there, and on the way back it opens only as the new signal arrives, so whatever
  the rewired pedals still held (filter tails, rerouted by the rewire) never gets out. Pedals
  that stay keep their state. A change of mind on the way down turns round smoothly, without a
  rewire.

**Latency** is constant whatever is on the board, so the host's delay compensation never
changes mid-session. The rig reports the latency of every pedal together (the oversampled
drives). A pedal with latency that isn't on the board still leaves its delay in the signal, as a
bypassed pedal would: in front of the first pedal on the board that comes after it in default
order, or at the end. A board in default order therefore delays every pedal's input exactly as
the fixed chain did with the other pedals off, which is what keeps the renders bit-identical. In
any other order the delays still add up to the same total, so the latency equals what the fixed
chain reported and sessions saved before the board keep their timing.

Measured by `[.board-measure]` (`engine/tests/board/BoardMeasure.cpp`): the legacy board turned
into Gate, Pearl, Tide, Coral, Ripple (Pearl moves in front of Tide, the six pedals that are off
leave) while a sine or a guitar plays, at 44.1–192 kHz, blocks of 1–4096 frames and in both
oversampling modes. "Excess" is the largest sample-to-sample step beyond what the signal itself
has, against its peak:

| Where | Mode | Dip | Below −1 dB | Swell | Excess step |
|---|---|---|---|---|---|
| The board's output | Low latency | −43 to −92 dB | 8–13 ms | ≤ +4.1 dB | −59 to −101 dB |
| | Linear phase | −43 dB to silence | 8–17 ms | ≤ +4.2 dB | −55 to −101 dB |
| The whole rig's output | Low latency | −9 to −28 dB | 9–21 ms | ≤ +4.7 dB | −53 to −105 dB |
| | Linear phase | −13 to −35 dB | 9–26 ms | ≤ +4.4 dB | −49 to −105 dB |
| A hard swap without the duck (for scale) | Low latency | −3 dB | | 0 dB | −15 dB |

The duck waits for the rack's latency, so in Low latency, where that is a few samples, it is
shorter, and the amp, cab and effects after the board fill more of it: the rig dips less. The
tests bound them: a dip of at most two fades plus the latency (and 2 ms), an excess step under
−50 dB at the board's output, and through the rig a slope at most 1.2× (guitar) or 1.6× (sine)
the signal's own (measured: up to 1.14× and 1.49×). CPU at 48 kHz, in ns per sample: every pedal
on the board and on, about 1010; Tide alone, 19; nothing on the board, 6 (the delays of the
pedals left off); every pedal on the board but switched off, 6.

**Later: a live mode.** A preset load sets the parameters and the board from the message
thread in one call; the audio thread can pick them up a block apart, which the duck and the
parameter smoothing cover. A live mode that must switch both on the same block can publish
them through one mailbox: `Board` is a small, trivially copyable value, so a board and a
parameter snapshot fit in one `RtMailbox` slot without allocating, and the engine already
applies a board only at a block boundary.

### Adding a pedal

1. **Engine.** Write the module in `engine/include/sedef/dsp/` and `engine/src/dsp/`, shaped
   like Pearl and Coral ([§5](#5-engine-contract)), with a `docs/dsp/<name>.md` page. If it
   needs the raw instrument as a key, give it a `process (io, key)` overload, as the Gate has.
2. **Parameters.** Append its parameters, an `<id>Enabled` switch first, to the end of
   `ParameterList.inc` (append only, [ADR-0004](adr/0004-single-parameter-schema.md)).
3. **Registry.** Append one `SEDEF_PEDAL(…)` line to `PedalList.inc` with a rank that puts it
   where it belongs by default (leave gaps). If it adds latency, the rig's reported latency
   grows by it for every board. `Board::legacy()` stays as it is: it is the fixed chain that
   sessions saved before the board played, and a new pedal was never in it.
4. **Generated tables.** Rebuild, then run `build/engine/tools/sedef-params` to regenerate
   `ui/src/generated/parameters.json` and `sedef-params --pedals` for
   `ui/src/generated/pedals.json`.
5. **UI.** Add its panel next to the others in `ui/src/features/pedals/` (a `<Pedal>` with rows
   of `<PedalKnob>` and a `<PedalSwitch>`; it renders on its own in any container), its box in
   `sizes.ts` (compact, regular or wide by what it carries, and the height its controls need),
   a line figure for its enclosure in `art.tsx`, and one entry in
   `ui/src/features/pedals/catalog.ts`: the panel, the kind of pedal and a one-line
   description for the picker. The board's layout test (`layout.test.ts`) fails if every pedal
   no longer fits two rows.
6. **Tests and docs.** The registry and layout tests check every pedal has a switch, a panel, a
   description and a box. Add the pedal to the README Pedals row, `docs/ui.md` and the CHANGELOG.

The default order (the ranks in `PedalList.inc`, which also give the legacy board) follows each
pedal's page ("Where it goes in the chain"); the player can change it on the board. In short:

- **Trench right after the gate.** An octave divider tracks best on the clean, uncompressed
  guitar: every drive in front of it would add harmonics for the tracker to mistake for the note,
  and the compressor would flatten the envelope that shapes the sub. The gate in front keeps
  noise from triggering it.
- **Anemone and Conch after Trench, before Remora.** The envelope filter reads the picking
  dynamics a fuzz or compressor would flatten, and a wah into a fuzz is the classic order.
- **Remora behind Trench and the two filters.** The two-transistor fuzz cleans up with the
  guitar's volume because its low input impedance loads the pickup. Remora models that with its
  own Guitar control, so the filters in front of it don't buffer the clean-up away; it still comes
  before the compressor and every drive. Trench goes ahead of it because a divider can't track a
  fuzzed note, and its subs into a fuzz are a classic pairing.
- **Urchin after Tide, before the drives.** Its rectifier needs a clean signal too (a clipped
  input folds into a near-constant level, not an octave), and a compressor in front evens out the
  notes and makes them sustain into the octave.
- **Kelp after Pearl, right before Coral and the amp.** A mid-hump overdrive is the pedal that
  tightens what follows it: its low cut and mid push shape what the distortion or a high-gain amp
  sees. Pearl, transparent, can push it.
- **Sargasso after Urchin, before the overdrives.** The sustain fuzz doesn't need the guitar's
  impedance, and a compressor in front evens out the attack it clips. It comes after Urchin,
  whose rectifier wants an unclipped note.
- **Beacon, Gyre and Jetty after the drives, before Ripple.** The vibe, phaser and flanger move
  the harmonics the drives make, so they come after them.
- **Breaker last, straight into the amp.** A treble booster shapes what a driven amp sees: it
  should get the whole board's output, and in front of a fuzz or overdrive it would be clipped
  flat.

Each oversampled pedal (Remora, Urchin, Sargasso, Pearl, Kelp, Coral, Breaker) adds its
oversampler's latency to the chain whether it is on or off, on the board or not (the rack keeps
the delay of the ones that aren't), so switching or moving pedals never changes the latency reported to the
host. With the default minimum-phase oversampling that is 5 samples per
stage at 44.1/48 kHz: 41 samples (0.85 ms) for the whole rig at 48 kHz, including the amp and the
capture stage. Settings > Latency > Linear phase brings back 0.4.0's linear-phase filters,
298 samples (6.2 ms). The setting is the `oversampling` parameter; changing it prepares the engine
again and reports the new latency to the host
([dsp/oversampling.md](dsp/oversampling.md)).

Selecting an amp also selects the matching cab and EQ bank while
`ampCabLink` is on. With the link off, `cabModel` picks the cab independently.
The EQ bank always follows the amp.

## 7. Modules

| Module | Character | Notes |
|---|---|---|
| Transpose | −12 to +12 semitones, polyphonic, before the pedals | attack-synchronous resampling grains on what a long-window partial model leaves, plus the model's partials resynthesised shifted; 3 ms down, 10 ms up, off and bit-exact at 0 ([transpose.md](dsp/transpose.md)) |
| Gate | Sidechained downward expander with hysteresis and hold | detector on raw input |
| Trench | Octave down, −1 and −2 octaves: Mono (analog-style divider) or Poly (chords) | Mono: NSDF note tracker, steered resonators, half-phase Schmitt trigger, flip-flops, polyBLEP; Poly: decimated complex filter bank, one voice per spectral peak, halved phase steps; no OS ([trench.md](dsp/trench.md)) |
| Remora | Two-transistor feedback fuzz, germanium / silicon | Ebers–Moll pair, guitar-volume clean-up, bias servo, 4× OS ([remora.md](dsp/remora.md)) |
| Tide | Fixed-threshold 3:1 compressor, tilt EQ, parallel blend | soft knee, program-dependent release |
| Urchin | Transformer-and-diodes octave-up fuzz | full-wave rectifier into an asymmetric clipper, 4× OS, ADAA ([urchin.md](dsp/urchin.md)) |
| Sargasso | Four-stage sustain fuzz, low-pass/high-pass tone blend | two cascaded clippers, 4× OS ([sargasso.md](dsp/sargasso.md)) |
| Pearl | Mid-hump overdrive with mid-boost | clipping in the feedback path, 4× OS |
| Kelp | Classic mid-hump overdrive | diode-law feedback clipping, 723 Hz tone stage, 4× OS, ADAA ([kelp.md](dsp/kelp.md)) |
| Coral | Hard-clipping distortion with low-pass "cut" | 4× OS |
| Ripple | Tremolo, chorus, vibrato | 0.69–7.19 Hz |
| Breaker | Germanium treble booster, Treble / Full range | resonant input network, Ebers–Moll stage, 4× OS ([breaker.md](dsp/breaker.md)) |
| Anemone | Envelope filter: state-variable LP/BP/HP swept by the picking envelope, up or down | 1 ms / 120 ms envelope, Q 0.7–16 ([anemone.md](dsp/anemone.md)) |
| Conch | Inductor wah, Manual or Auto (envelope) | 350 Hz–2.2 kHz, Q 10 → 1.6 heel to toe ([conch.md](dsp/conch.md)) |
| Beacon | Photocell vibe: four staggered stages, lopsided lamp LFO, Chorus or Vibrato | 0.3–8 Hz ([beacon.md](dsp/beacon.md)) |
| Gyre | 4- or 6-stage all-pass phaser with feedback | 0.1–8 Hz, ±2 octaves ([gyre.md](dsp/gyre.md)) |
| Jetty | Bucket-brigade flanger, ± feedback, clock-tracking band limit | 0.25–10 ms, 0.05–5 Hz ([jetty.md](dsp/jetty.md)) |
| Lagoon | Clean amp, glassy and wide | two triode stages, FMV tone stack |
| Reef | Crunch amp, three voicings | cascaded stages, British-style tone stack |
| Abyss | High-gain lead amp, bright and shift switches | four stages, shifted mid |
| Capture | A Neural Amp Modeler (.nam) file in place of the amp | NeuralAmpModelerCore, rate conversion, levelled to the amps ([nam.md](dsp/nam.md)) |
| Cab | Procedural speaker + cabinet + mic models, or user IR | two mics + room per cab |
| EQ | 9 bands (65 Hz–16 kHz, ±12 dB) + low/high cut | one bank per amp |
| Drift | Multi-head tape echo, wow/flutter, tape drive | host/tap/free sync |
| Fathom | Plate (Dattorro topology) and hall (FDN) | 1–60 s decay |
| Doubler | Mono → stereo double tracking | second take re-timed per note, 3–20 ms spread; onsets from the DI |

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
stereo processing. A capture replaces the amp's share with its own: a standard
WaveNet capture takes about 10% of an M2 core by itself ([nam.md](dsp/nam.md)).
