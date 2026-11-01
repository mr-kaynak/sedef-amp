# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Preset browser:** a panel over the rig with a library rail (all, favourites, recent, factory,
  user), categories, tag chips, search across names, notes, tags, amps and blocks, and sorting by
  name, recency or category. Rows show the amp, the switched-on chain, a one-line note, tags and
  the author.
- **Audition:** a click or the arrow keys play a preset while the browser stays open; Enter or a
  double-click keeps it, and Esc restores exactly the sound you had when it opened, unsaved edits
  included.
- **Undo:** loading over unsaved edits (from the browser or prev/next) and deleting a user preset
  offer Undo in a toast (also ⌘/Ctrl+Z).
- **A/B compare** next to the preset bar, with copy between the slots and an edit marker per slot.
- **Saving:** ⌘/Ctrl+S saves (Save as for factory presets), ⌘/Ctrl+Shift+S is Save as. Save as
  takes a category (or a new one), tags with suggestions and an optional note, and warns inline
  before replacing a preset.
- Presets may carry a one-line `description`; every factory preset has one, and the factory
  categories and tags follow one vocabulary (`presets/README.md`).

### Changed

- Bridge protocol 2: presets report their amp, cab, active blocks and description; Recent, A/B,
  snapshots and undoing a delete are new native functions.
- Factory presets: Mariana and Coral Fuzz moved to a new High Gain category, Morning Ritual to
  Crunch. Sounds and levels are unchanged.

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
