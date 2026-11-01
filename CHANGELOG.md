# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed

- **Intonation workbench:** the open-string step no longer waits for a reading held within one
  cent for a second, which a real plucked string never gives. Within 5 cents it moves on by
  itself; further off on the right note it says how far and offers **Use this tuning**. Both
  notes are now captured the same way from one pluck (one second from 0.6 s after it), which also
  fixes 12th-fret captures that restarted late on a wobbling note and read up to 1.5 cents off.

## [0.1.0] - 2026-11-01

The first public build: a complete guitar rig with a tuner and an intonation workbench.

### Added

- **Pedals:** noise gate, Tide (compressor with tilt EQ and parallel blend), Pearl (transparent
  overdrive with mid boost), Coral (distortion), Ripple (tremolo, chorus, vibrato) and polyphonic
  transpose (±12 semitones). Drive stages run oversampled with antiderivative anti-aliasing.
- **Amps:** Lagoon (glassy clean), Reef (British crunch, three voicings) and Abyss (tight lead),
  each with its own 9-band graphic EQ, low cut and high cut.
- **Cabs:** 1x12 open, 2x12 open and 4x12 closed, two mics per cab from ten mic models placed
  freely on the cone, room mics, and your own impulse responses (zero-latency convolution).
- **Time effects:** Doubler, Drift (multi-head tape echo with wow, flutter, tape drive and
  tempo sync) and Fathom (plate and hall reverbs).
- **Tools:** precision tuner, the intonation workbench (open string, 12th fret, saddle advice in
  millimetres for any scale length) and a metronome with odd meters (standalone).
- **Formats:** VST3, AU, CLAP and a standalone app for macOS (universal), Windows and Linux.
- **UI:** resizable web UI with keyboard control, screen reader labels and WCAG AA text contrast;
  preset browser with 13 factory presets balanced to -16 LUFS, user presets and favourites.
- **Mono, stereo and dual-mono** rigs, latency-aligned bypasses and host tempo sync.
- `sedef-render`, an offline renderer for presets, and loudness tooling for preset authors.

### Quality

- 202 engine tests on macOS, Windows and Linux, including a test that the audio thread never
  allocates, plus ASan, UBSan and ThreadSanitizer runs.
- Every CI build is checked with pluginval (strictness 8), clap-validator and auval, and a smoke
  test opens the editor in a real host and WebView on every OS.

[Unreleased]: https://github.com/mr-kaynak/sedef-amp/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/mr-kaynak/sedef-amp/releases/tag/v0.1.0
