<div align="center">

# Sedef Amp

**An open-source guitar amp and effects suite, with a tuner that teaches your guitar to play in tune.**

VST3 · AU · CLAP · Standalone — macOS, Windows, Linux

[![CI](https://github.com/mr-kaynak/sedef-amp/actions/workflows/ci.yml/badge.svg)](https://github.com/mr-kaynak/sedef-amp/actions/workflows/ci.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)

<img src="docs/images/amp.png" alt="Sedef Amp: the amp section" width="860">

</div>

*Sedef* (seh-DEF) is Turkish for mother-of-pearl: the shimmering inlay on fine instruments, and
the sound we're after. Glassy cleans, warm crunch, singing leads, and time effects that melt
around them.

## What's inside

| | |
|---|---|
| **Pedals** | **Tide** compressor (3:1, tilt EQ, parallel blend) · **Pearl** transparent overdrive with mid boost · **Coral** distortion · **Ripple** tremolo / chorus / vibrato · noise gate · ±12 semitone polyphonic transpose |
| **Amps** | **Lagoon** — glassy, wide clean · **Reef** — British crunch with three voicings · **Abyss** — tight, saturated lead with bright and mid-shift switches |
| **Cabs** | 1x12 open, 2x12 open, 4x12 closed · two mics per cab from ten mic models, freely placed on the cone · room mics · load your own IRs |
| **EQ** | 9-band graphic EQ with low/high cut, one bank per amp |
| **Post** | **Drift** multi-head tape echo with wow, flutter and tape saturation · **Fathom** plate and hall reverbs · double-tracking **Doubler** |
| **Tools** | precision tuner · **intonation workbench** · metronome with odd meters (standalone) |

<img src="docs/images/cab.png" alt="Cab section with movable microphones" width="49%"> <img src="docs/images/fx.png" alt="Tape echo and reverb" width="49%">

## The intonation workbench

A guitar can be perfectly tuned and still play out of tune higher up the neck. The fix is moving
each string's saddle, and Sedef tells you exactly which way and how far:

1. Pick your instrument and scale length (6/7-string guitar, 4/5-string bass, or custom).
2. Tune the open string. Sedef waits until it's steady within a cent.
3. Play the 12th fret. Sedef skips the pick attack, averages the sustained note, and reports
   the octave error to a tenth of a cent.
4. Get a clear answer: *"+3.8 cents sharp: move the saddle about 1.4 mm away from the neck."*

<img src="docs/images/intonation.png" alt="Intonation workbench showing a saddle recommendation" width="860">

The estimate uses `Δs ≈ L · (2^(c/1200) − 1)` for scale length `L` and octave error `c` cents.
See [docs/intonation.md](docs/intonation.md) for the method and its limits.

## Install

Download the zip for your OS from [Releases](https://github.com/mr-kaynak/sedef-amp/releases)
and copy the formats you use:

| OS | VST3 | AU | CLAP |
|---|---|---|---|
| macOS | `~/Library/Audio/Plug-Ins/VST3` | `~/Library/Audio/Plug-Ins/Components` | `~/Library/Audio/Plug-Ins/CLAP` |
| Windows | `C:\Program Files\Common Files\VST3` | — | `C:\Program Files\Common Files\CLAP` |
| Linux | `~/.vst3` | — | `~/.clap` |

Builds are not notarized yet, so macOS quarantines them after download. Clear the flag once:
`xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Sedef Amp.vst3"` (and the same for
the AU, CLAP and app). Details for every OS are in [docs/install.md](docs/install.md).

## Build from source

```sh
git clone https://github.com/mr-kaynak/sedef-amp.git && cd sedef-amp
cmake --preset release
cmake --build --preset release
```

Requirements: CMake ≥ 3.25, Ninja, a C++20 compiler, Node 22+ and pnpm (for the UI). JUCE and the
other dependencies are fetched automatically. Linux needs the usual JUCE packages plus
`libwebkit2gtk-4.1-dev` (see [.github/workflows/ci.yml](.github/workflows/ci.yml)).

DSP-only builds skip JUCE entirely and run the test suite in seconds:

```sh
cmake --preset engine && cmake --build --preset engine && ctest --preset engine
```

Render any preset offline without a DAW:

```sh
build/engine/tools/sedef-render --preset presets/factory/nacre.sedefpreset --out nacre.wav
build/engine/tools/sedef-render --in my-di.wav --set ampModel=2 --set abyssGain=0.8 --out lead.wav
```

## How it's built

```
ui/       React + TypeScript, runs inside the plugin's WebView (and in a browser with a mock backend)
plugin/   JUCE 9 shell: parameters, presets, IRs, bridge to the UI
engine/   Pure C++20 DSP library with no JUCE dependency, unit-tested on every OS
```

- One parameter schema drives the DSP, host automation, presets and UI
  ([ADR-0004](docs/adr/0004-single-parameter-schema.md)).
- Hard real-time rules: the audio thread never allocates, locks or waits; a test proves it
  ([ADR-0005](docs/adr/0005-real-time-safety.md)).
- Nonlinear stages run oversampled with latency-aligned bypasses, so switching pedals never
  shifts your timing ([ADR-0008](docs/adr/0008-oversampling.md)).
- Every DSP module documents the public research it's based on in [docs/dsp/](docs/dsp/).

Start with [docs/architecture.md](docs/architecture.md).

## Contributing

Issues and pull requests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first,
especially the clean-room policy: Sedef is built from public research and our own measurements,
never from other products' code or data.

## License

[AGPL-3.0-or-later](LICENSE). Sedef Amp uses JUCE under its AGPLv3 option. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
